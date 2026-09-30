# Debug data for OUT_BUS_MIXER_DEBUG_PLAN.md — collected on SM-A566B (permissive, live WhatsApp call)

Answers your cross-questions from captured `dumpsys media.audio_flinger` samples (300 ms apart) during a
talking-far-side call. **One item (fresh AudioMixer verbose logcat, Q6) is still pending — the device
dropped off USB before I could capture it; will add when it's back.**

## KEY FINDING that reframes H1: *neither* active track is consumed — the mixer mixes nothing

On the `OUT_BUS` MIXER thread, **both** active playback tracks sit at `Server = 0` for the whole call,
while the HAL output keeps writing:

```
 Id Active Client  ST Flags  Fmt      ChnMask  SRate Usg  Server   FrmCnt FrmRdy  Underruns   (owner)
 56   yes   6926    0 0x000  PCM16    0x1(mono) 48000  2   00000000  1928   1928      0        com.whatsapp
 60   yes   3840    0 0x001  PCM16    0x1(mono) 16000  2   00000000   644    644    322        com.propgo.aicaller (our render)
 58   no    3840    0 0x600  PCM16    0x1(mono) 16000  2   00000000    10     10      0        our app (paused)
```
- **Our own render track (id 60) is 16 kHz — the sink rate, no resampling — and it is ALSO not consumed
  (`Server = 0`), with `FrmRdy = 644` (full) and 322 underruns.** So this is **not** the 48→16 readiness
  threshold (H1): a same-rate, ready track isn't mixed either.
- The HAL output stream is running: `Standby: no`, `Last write occurred: 9 ms`, **`Frames written: 41520`**
  — i.e. the thread writes ~continuously but the frames are **silence** (nothing mixed in).
- Net at the app: `ElevenLabsDirectCapture … SCO: rms=0.0` for the entire call → agent hears nothing →
  End-Call → drop.

So the question isn't "why is WhatsApp's 48 kHz track starved" but **"why does this MIXER thread consume
none of its ready tracks and emit silence."**

## Thread geometry (Q1–Q3)

`OUT_BUS` output thread `AudioOut_3D`, `type 0 (MIXER)`:
- **Sample rate: 16000 Hz**, Channel mask `0x3` (stereo), Mixer channel mask `0x3`.
- **HAL frame count: 10**  (← very small; from the default-Module HAL stream)
- **Normal frame count: 320**
- **`No FastMixer`**; `Fast track availMask=0xfe`; `Standby delay ns=3000000000`.
- `Threadloop write latency: ave≈10.6 ms`.
- HAL side (`AHAL_VirtualScoStream: init:`): `output 16000 Hz, 2 ch, buffer 80 frames`;
  `input 16000 Hz, 1 ch, buffer 80 frames`.

So `desiredFrames ≈ 3 × 320 + margin ≈ 960` for WhatsApp's 48 kHz track, and its `FrmRdy = 1928 ≥ 960`,
so the readiness gate (H1) should pass — consistent with the reframing above.

## Tracks (Q2, Q4, Q5)
- WhatsApp (id 56): normal track (not in a fast list — there is no FastMixer), `48000/mono/PCM16`,
  flags `0x000`, `FrmCnt=1928`, `Server=0`/`FrmRdy=1928` frozen across 3 samples (~6 s).
- Our render (id 60): `16000/mono/PCM16`, flags `0x001`, `FrmCnt=644`, `Server=0`/`FrmRdy=644`,
  **Underruns=322** — the mixer *did* touch it early (underruns accrued while empty) but `Server` never
  advanced once it filled.

## Mix-port / routing (Q8)
- `virtual output` mix port: `maxOpenCount 1 / curOpenCount 1` → exactly one output open. Confirms your
  assessment that the 1/1 caps aren't the blocker (and matches: the tracks *are* on the thread).

## Our render on OUT_BUS in SCO mode (Q9)
- It is a silent (zeroed) `USAGE_VOICE_COMMUNICATION` track (the app zeroes the render buffer in SCO mode
  and injects the agent via the SCO uplink instead). It does not need to be on `OUT_BUS`.
- **But E1 is likely moot:** since our own track is *also* not consumed, removing it won't make WhatsApp's
  drain — the mixer is consuming nothing regardless of contention.

## Pending
- **Q6 (the decisive one): fresh `AudioMixer`/`AudioFlinger` verbose logcat during the call** —
  `cannot create track`, `AudioMixer: getTrackName`, `track disabled due to underrun`, `invalidate`,
  effect-chain warnings. I had `setprop log.tag.AudioMixer V` / `AudioFlinger V` queued when the device
  dropped; will capture on reconnect. This should show directly whether the tracks fail to get an
  `AudioMixer` slot (which the "both tracks Server=0, thread emits silence" pattern points to).

## Revised hypotheses for you to weigh
- **H2 — tracks never get an AudioMixer slot / the thread emits silence.** Fits: both ready tracks
  `Server=0`, thread writes but mixes nothing. Would show as `AudioMixer cannot create track` or the
  thread mixing 0 active tracks despite the track list. Possible causes on a vendor-injected software
  MIXER: an effect-chain/format quirk, or the very small `HAL frame count: 10` (20-byte HAL buffer)
  interacting badly with the normal mixer's 320-frame block.
- **H3 — HAL buffer too small.** `HAL frame count: 10` (≈0.6 ms) is tiny for a normal mixer expecting to
  write `Normal frame count = 320`. If the default `Module` derives an undersized HAL buffer for our
  stream, the mixer/thread write path may misbehave (the 322 underruns on our own track hint at chronic
  timing trouble). Worth checking what buffer size the HAL advertises vs what the MixerThread wants, and
  forcing a ~320-frame (20 ms) HAL buffer.

Given both tracks stall regardless of rate, I'd prioritise the fresh AudioMixer log (Q6) and the HAL
buffer-size check (H3) over the 48↔16 resampler path.
