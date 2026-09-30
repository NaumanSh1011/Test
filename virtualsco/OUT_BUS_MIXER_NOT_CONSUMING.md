# Follow-up: OUT_BUS is now a MIXER, WhatsApp's playback routes to it — but the mixer never consumes it

**Scope: `virtualsco/` HAL config (`VirtualScoConfiguration.cpp`). Follows OUT_BUS_MIXER_48K.md (8414ff8).**

## Progress since the stereo-16k fix (verified on SM-A566B, permissive, live WhatsApp call, far side talking)

The stereo-16k change **worked** for what it targeted:

- `dumpsys media.audio_flinger`: the `OUT_BUS` output thread is now **`type 0 (MIXER)`**, 16000 Hz,
  channel mask `0x3` (stereo). `AHAL_VirtualScoStream: init: output 16000 Hz, 2 ch`. (Was DIRECT before.)
- **WhatsApp's playback now routes onto our device.** `com.whatsapp` (its pid) has an **active** playback
  track on the `OUT_BUS` MIXER thread — `48000 Hz, mono, usage VOICE_COMMUNICATION`. Its mic is on
  `IN_BUS` as before. So both directions of the third-party call are now pointed at the virtual device.

## The remaining bug: the mixer does not consume WhatsApp's playback track

On the `OUT_BUS` MIXER thread, WhatsApp's track is **active but never drained**, for the whole call:

```
  Id Active Client ...  Fmt  ChnMask  SRate ST Usg CT ...  Server FrmCnt FrmRdy ...
  56    yes   6926  ... 0x1  0x1     48000  0   2  0  ... 00000000  1928   1928   A   (com.whatsapp)
```
- `Server = 00000000` and `FrmReady = 1928` (= full track buffer) are **frozen across every sample** —
  the mixer is not pulling frames from this track.
- Consequence A: the HAL output stream (which *is* being written — `Frames written` advances) carries only
  our own app's silent render, so `va_server_push_downlink` sends silence. The app's capture reads
  `ElevenLabsDirectCapture ... SCO: rms=0.0` for the entire call → the agent hears nothing → it invokes
  End-Call → the call drops (now at ~6–10 s instead of ~3 s, but still drops).
- Consequence B: WhatsApp's playout AudioTrack fills (1928/1928) and can't write more, so its own audio
  pipeline stalls — likely the direct cause of the call teardown.

The **input side does resample fine** — WhatsApp's 48 kHz mic record is consumed on the `IN_BUS`
RecordThread and resampled to our 16 kHz HAL (`readErrors=0`). So 48↔16 kHz conversion works for capture;
only the **playback mixer** fails to consume the 48 kHz track.

## Prime suspect: the mix port's stream-count caps

`VirtualScoConfiguration.cpp` builds both mix ports with:
```cpp
createPortMixExt(1, 1)   // maxOpenStreamCount = 1, maxActiveStreamCount = 1
```
`dumpsys media.audio_policy` shows the `virtual output` mix port as `maxOpenCount: 1; maxActiveCount: 1`.
Our own app's VOICE_COMMUNICATION render is also pinned to `OUT_BUS` (it's a silent, zeroed track in SCO
mode) and appears to hold the single active slot, so WhatsApp's playback track is added to the thread but
starved.

## Asks for the build agent

1. **Raise the output mix port's `maxOpenStreamCount` / `maxActiveStreamCount`** (e.g. 8/8, or match a
   known-good software mixer endpoint) so the MIXER thread can carry multiple concurrent tracks
   (our app's render + the tapped app's playback). This is the cheapest thing to try first.
   - Do the same on the input mix port if two capture clients (our app + WhatsApp) need to coexist on
     `IN_BUS` — the capture already shows 2 active input tracks, so confirm it isn't similarly capped.
2. If bumping the counts doesn't make the mixer consume the 48 kHz track, investigate **why the mixer
   allocates no resampler / skips a track whose rate (48 kHz) differs from the sink (16 kHz)** on this
   output, given the capture path resamples the symmetric case fine. Compare the track's mixer state
   (`AudioMixer`/`Track::getNextBuffer`) for a resample-needed track on a 16 kHz sink.
3. Consider whether the **sink rate should just be 48 kHz** after all (the earlier OUT_BUS_MIXER_48K.md
   alternative): if the mixer consumes same-rate tracks reliably but struggles with the 48→16 resample on
   the output, running the thread at 48 kHz (no per-track resample for the 48 kHz VoIP client) and doing
   the single 48→16 downconvert in `StreamVirtualSco::transfer` sidesteps it. Only pursue if (1)/(2)
   don't resolve it.

## Acceptance (on device, live WhatsApp call, far side talking)

- On the `OUT_BUS` MIXER thread, WhatsApp's playback track's **`Server` counter advances** (frames are
  consumed) and `FrmReady` fluctuates rather than sitting full.
- `ElevenLabsDirectCapture ... SCO: rms` is **non-zero** while the far side talks → the agent hears the
  human.
- The call **no longer drops** on its own; the far side hears the agent via the working `IN_BUS` uplink.

## Notes
- Everything else is confirmed working: enumeration, BUS auto-attach, `IN_BUS` capture (WhatsApp mic +
  our uplink injection), MIXER thread creation, WhatsApp playback routing to `OUT_BUS`. This is the last
  known blocker.
- App side already skips the telephony tap in SCO mode (ADM source = MIC), so the only playback tracks on
  `OUT_BUS` are our silent render + the tapped app — no extra contenders for the slot.
