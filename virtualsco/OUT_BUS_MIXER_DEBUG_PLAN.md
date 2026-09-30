# Debug plan: why the OUT_BUS mixer never consumes WhatsApp's 48 kHz track

**Answers `OUT_BUS_MIXER_NOT_CONSUMING.md` (`85de61d`). No HAL change is built yet — please collect the
data below first, so the next build targets the actual cause.** Source references are
android-15.0.0_r36 (`frameworks/av/services/audioflinger/Threads.cpp`, `MixerThread::prepareTracks_l`).

## Assessment of the `maxActiveCount = 1` suspect: unlikely

The mix-port caps count **HAL streams / outputs**, not tracks inside one output:
- HAL: `Module::openOutputStream` checks `maxOpenStreamCount` against `mStreams.count(portId)` — streams
  opened on that mix port (`hardware/interfaces/audio/aidl/default/Module.cpp` ~l.308).
- APM: `maxActiveCount` is used by `IOProfile::canOpenNewIo()` / `canStartNewIo()` — opened/started
  **outputs**.

Our app's render and WhatsApp's track share **one** MIXER thread = one output = one HAL stream, so the
1/1 caps don't limit them. It also doesn't fit the evidence: if the cap blocked WhatsApp, its track
wouldn't be on the thread in state `A` (active) at all. Bumping to 8/8 is harmless, but isn't expected to
fix this.

## Leading hypothesis (H1): the track can never reach the mixer's readiness threshold

In `MixerThread::prepareTracks_l`, a normal track is mixed only if
`framesReady >= minFrames && track->isReady() && !isPaused() && !isTerminated()`, where

```
desiredFrames = sourceFramesNeededWithTimestretch(trackRate /*48000*/, mNormalFrameCount,
                                                  threadRate /*16000*/, speed)
              + mAudioMixer->getUnreleasedFrames(track)
minFrames     = desiredFrames   if the previous mix round had ready tracks (MIXER_TRACKS_READY)
              = 1               otherwise
```

For a 48 kHz track on a 16 kHz sink, `desiredFrames ≈ 3 × mNormalFrameCount + resampler margin`.
Our own app's (silent) render on `OUT_BUS` is mixed every round → the thread is `MIXER_TRACKS_READY` →
WhatsApp's track is held to `minFrames = desiredFrames`. **If `desiredFrames` > the track's buffer
(`FrmCnt` 1928), it is never ready — `FrmRdy` sits at 1928 and `Server` stays 0, exactly as observed.**
That requires `mNormalFrameCount` ≳ ~640 frames (≳ 40 ms at 16 kHz), which is plausible because our HAL
stream buffer size comes from the default `Module` and may be large for a 16 kHz stream.

Why capture is fine: the RecordThread uses a different pull model (it reads from the HAL and pushes into
each client's buffer), so it has no equivalent readiness gate.

## Cross-questions (please answer from the device, live call, far side talking)

**Thread geometry — the key numbers for H1** (`adb shell dumpsys media.audio_flinger`, `OUT_BUS` thread):
1. `HAL frame count`, `Normal frame count`, `Sample rate`, `Latency`/`HAL latency` of the `OUT_BUS`
   MIXER thread.
2. For WhatsApp's track: `FrmCnt`, and if printed, the buffer size / `BufSize` and `Flags` columns.
   Is it listed under the normal tracks or under **Fast tracks** (`F`-prefixed id / fast track list)?
3. Does the thread report a FastMixer (`Fast mixer`/`FastMixer` section)? If so, what are its
   `frameCount` and `sampleRate`?

**Track state:**
4. The full dumpsys row for WhatsApp's track (all columns, especially state `ST`, `Flags`, `Underruns`,
   `Presented`, `Server`) sampled 3–4 times ~1 s apart.
5. The same row for **our own app's** render track on `OUT_BUS`. Is it mixed (`Server` advancing)?

**Logs during the call:**
6. Any of these in `adb logcat -s AudioFlinger AudioFlinger::PlaybackThread`:
   `AudioMixer cannot create track`, `attached to effect but no chain found`,
   `track(...) disabled due to underrun`, `invalidate`.
7. `AHAL_VirtualScoStream: init:` lines for the output (rate / channels / `buffer N frames`) — the
   HAL-side buffer size that drives `HAL frame count`.

**Routing context:**
8. How many outputs are open on the `virtual output` mix port in `dumpsys media.audio_policy` (to
   confirm the 1/1 caps aren't being hit — should be exactly 1)?
9. Does our app still need its own render track on `OUT_BUS` in SCO mode (it only writes silence)?

## Discriminating experiments (fast, no HAL build)

- **E1 — remove our silent render from `OUT_BUS`.** Don't start (or route elsewhere) the app's own
  VOICE_COMMUNICATION render during the call. With no other ready track, the thread isn't
  `MIXER_TRACKS_READY`, so `minFrames = 1` and WhatsApp's track should start draining.
  **If `Server` starts advancing → H1 confirmed.**
- **E2 — isolate the resample.** From a test app, play a `USAGE_VOICE_COMMUNICATION` AudioTrack routed to
  `OUT_BUS` (`setPreferredDevice`) at **16 kHz**, then at **48 kHz**, with the app's own render also
  playing. 16 kHz drains but 48 kHz doesn't → the 48→16 readiness threshold (H1) is the blocker.
- **E3 — perfetto/atrace.** Record with the `audio` atrace category during the call: the per-track
  framesReady counter (`AUDIO_TRACE_PREFIX_AUDIO_TRACK_NRDY` + track suffix) and the thread's mix
  activity show directly whether WhatsApp's track is considered and rejected every round.

## What each outcome would lead to (HAL side)

| Finding | HAL change |
|---|---|
| H1 confirmed (large `Normal frame count`) | Make the HAL output stream's buffer small (≈ 20 ms = 320 frames at 16 kHz) so `desiredFrames` fits WhatsApp's buffer; **or** run the output at **48 kHz** so WhatsApp's track needs no resampling (`desiredFrames ≈ mNormalFrameCount`) and downconvert 48→16 kHz in `StreamVirtualSco::transfer` (the `OUT_BUS_MIXER_48K.md` alternative). |
| Track is FAST / on a FastMixer | Fast tracks need the thread's rate; running the output at 48 kHz (or ensuring no FastMixer) addresses it. |
| `AudioMixer cannot create track` / effect-chain warning | Channel/format/effect issue — report the exact line; config change depends on it. |
| None of the above (E1 drains nothing either) | Capture E3 trace + full dumpsys; revisit. |
| (for completeness) 1/1 caps really hit (item 8 shows >1 output wanted) | Raise to 8/8. |
