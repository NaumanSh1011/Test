# Decision: run the OUT_BUS mixer at 48 kHz (downconvert to the 16 kHz socket in the HAL)

Follows OUT_BUS_MIXER_DEBUG_DATA.md (`94cfff0`) and the debug plan (`d82e09c`). **Recommend building the
48 kHz-sink variant now** — we have direct on-device evidence it's the fix.

## Decisive evidence (SM-A566B, permissive, live WhatsApp calls)

1. **The 16 kHz OUT_BUS MIXER consumes nothing.** In the valid pinned call, `OUT_BUS` opened as
   `type 0 (MIXER)` @ **16000 Hz**, and **both** active playback tracks sat at `Server=0` for the whole
   call — WhatsApp's 48 kHz track *and* our own 16 kHz render. The thread wrote to the HAL
   (`Frames written` advancing) but mixed no track → silence → downlink `rms=0.0`.
2. **No downsampling resampler is ever created for that thread.** With `AudioMixer` at INFO, the only
   `create resampler` lines during a call are `dst 48000` and `dst 32000` (other threads). There is
   **never** `create resampler src 48000, dst 16000` — the one WhatsApp's 48 kHz track would need to mix
   into the 16 kHz OUT_BUS thread. It's simply never set up.
3. **A 48 kHz mixer mixes the exact same WhatsApp track fine.** In a later call (routing had fallen back
   to the earpiece), WhatsApp's 48 kHz playback landed on the **primary `AudioOut_D` MIXER @ 48000 Hz**,
   and there its `Server` advances normally (`0x1AF40`) alongside our render — full mixing, no stall.

Conclusion: the blocker is specific to the **16 kHz OUT_BUS mixer thread** (down-resampling a 48 kHz VoIP
track into a 16 kHz sink is not happening on this vendor stack). Running the sink at **48 kHz** — the rate
the VoIP client already uses, and a rate we've proven this device's mixer handles — sidesteps it.

## Change (as your OUT_BUS_MIXER_48K.md §"Why not 48 kHz" fallback, now the primary path)

`VirtualScoConfiguration.cpp`:
- Output mix port + `OUT_BUS` device port → **PCM16 STEREO 48000 Hz** (keep stereo — mono still forces
  DIRECT via `isValidPcmSinkChannelMask`).
- Input side: keep what works (WhatsApp's mic already mixes/records fine on `IN_BUS`); if simplest to keep
  symmetric, 48 kHz is fine there too, else leave input mono 16 kHz.

`StreamVirtualSco.cpp` `transfer()`:
- **Output (downlink):** buffer is now 48 kHz stereo. Downmix to mono **and resample 48 kHz → 16 kHz**
  before `va_server_push_downlink` (socket stays 16 kHz mono / 320-sample frames).
- **Input (uplink):** `va_server_pull_uplink` returns 16 kHz mono; **resample 16 kHz → 48 kHz** (and
  duplicate to stereo) to fill the read buffer (only if the input port is moved to 48 kHz).
- Keep wall-clock pacing + silence-on-underrun. `libaudioutils` has a resampler; voice-band linear is
  fine. Map frame counts cleanly (960 @48k ↔ 320 @16k = 20 ms).

## Acceptance (live WhatsApp call, far side talking)
- `OUT_BUS` thread is `type 0 (MIXER)` @ **48000 Hz**.
- WhatsApp's playback track on it shows **`Server` advancing** (not frozen at 0).
- `ElevenLabsDirectCapture … SCO: rms` non-zero → agent hears the human; call no longer drops on silence.
- Far side hears the agent via the `IN_BUS` uplink.

## Test-procedure note (not a HAL issue)
On this Samsung device the module only enumerates into APM after a **permissive audioserver restart**
(magisk-domain SELinux). We also observed that an **extra** mid-session `audioserver` restart can drop the
BUS device out of `AudioManager.getDevices()`, so the app pinner logs "no virtual tap device" and degrades
(agent plays out the speaker; no tap). For a clean test: reboot → `setenforce 0` → one `killall
audioserver` → confirm the pin succeeds (`StrategyRoutePinner: pinned … out=true in=true`) → then call.
AudioMixer resampler lines are INFO level, so no extra restart for verbose is needed.

---

## Implemented: 48 kHz output + explicit 20 ms buffers (combined change)

**Built as a combined change**, because the debug data (`94cfff0`) showed our own **16 kHz** render
(same rate as the sink, no resampler needed) also never drained. So the sample rate alone doesn't
explain the stall, and the "48 kHz mixes fine" evidence came from the vendor's primary output (different
HAL, different buffering). The one anomaly specific to our thread was its buffer geometry, so both are
addressed:

1. **`VirtualScoConfiguration.cpp`:** playback mix port + `OUT_BUS` → **PCM16 stereo 48 kHz** (stereo still
   required for a MIXER thread). Capture unchanged: PCM16 mono 16 kHz (works).
2. **`ModuleVirtualSco::getNominalLatencyMs()` → 20 ms** (was the base `Module`'s placeholder 5 ms).
3. **`StreamVirtualSco::transfer()` (output):** stereo → mono downmix, then a 31-tap Hamming-windowed-sinc
   low-pass (cutoff 7.2 kHz) + **decimate by 3 (48 → 16 kHz)**, with filter state carried across calls so
   any HAL frame count works. Host-tested with the identical algorithm, fed in uneven chunks: exactly
   16000 out samples per second, passband gain 0.998 @ 1 kHz / 1.001 @ 3.4 kHz, stopband −57.6 dB @ 10 kHz,
   −66 dB @ 20 kHz. Input path unchanged (16 kHz mono, 1:1 with the socket).

### Trace of `HAL frame count: 10` (android-15.0.0_r36)
- AudioFlinger: `mFrameCount = getBufferSize() / getFrameSize()` (`Threads.cpp` ~l.3225). Both come from
  the same `StreamContextAidl` (`StreamHalAidl.cpp` `getBufferSize` / `getFrameSize`), i.e.
  `HAL frame count` == the HAL descriptor's **`bufferSizeFrames`**.
- The HAL fills that from `StreamContext::getBufferSizeInFrames()` (`Stream.cpp` l.70) — the FMQ size in
  frames, which is what our driver logged (`buffer 80 frames`).
- The framework requests the buffer as the patch's `minimumStreamBufferSizeFrames`
  (`Hal2AidlMapper.cpp` ~l.907), which the default HAL computes from `getNominalLatencyMs()` — **5 ms** by
  default (`Module.cpp` l.375: "Arbitrary value. Implementations must override this method") → 5 ms @
  16 kHz = **80 frames**.
- So stock AOSP would print `HAL frame count: 80`. **The `10` (= 80 / 8) isn't produced by this tree** —
  either the device's audioserver differs from AOSP here (Samsung modifies audioserver components), or
  the value came from another thread block. Please paste the raw `OUT_BUS` thread block from
  `dumpsys media.audio_flinger` with this build.
- Regardless, a 5 ms buffer is a placeholder that real HALs override; with the 20 ms override the HAL
  now computes **1024 frames** for the 48 kHz output (`calculateBufferSizeFrames` rounds >512 frames at
  ≥44.1 kHz up to a power of two) and **320 frames** for the 16 kHz input.

### What to check on device with this build
- `AHAL_VirtualScoStream: init: output 48000 Hz, 2 ch, buffer 1024 frames, decimation 3` and
  `init: input 16000 Hz, 1 ch, buffer 320 frames, decimation 1`.
- `OUT_BUS` thread: `type 0 (MIXER)` @ 48000 Hz, stereo; its `HAL frame count` / `Normal frame count`
  (expect 1024-ish, not 10).
- WhatsApp's playback track (and our render) on it: **`Server` advancing**.
- Downlink `rms` non-zero while the far side talks; call doesn't drop.
