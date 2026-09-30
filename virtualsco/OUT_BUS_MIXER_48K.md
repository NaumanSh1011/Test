# Fix: OUT_BUS opens as a DIRECT 16 kHz output → third-party VoIP playback can't route to it

**Scope: `virtualsco/` HAL only (`VirtualScoConfiguration.cpp` + `StreamVirtualSco.{cpp,h}`). App side is
handled separately.**

## What we proved on device (SM-A566B, permissive, live WhatsApp call, far side talking)

From `dumpsys media.audio_flinger` sampled every 300 ms during the call:

- **Enumeration + input both work.** The `virtual` module is loaded; `IN_BUS @ 02:56:41:00:00:01` is a
  **fast/mixer capture** (`AudioIn_*`, `Fast capture thread: yes`, `readErrors=0`). **WhatsApp
  (uid com.whatsapp) actively records its mic from our `IN_BUS`** — the framework resamples its 48 kHz
  record to our 16 kHz HAL. The uplink path is solid.
- **The output is broken.** `OUT_BUS`'s AudioFlinger thread is:
  ```
  Output thread ... name AudioOut_3D ... type 1 (DIRECT)
    Sample rate: 16000 Hz
    Output devices: 0x1000000 (AUDIO_DEVICE_OUT_BUS)
  ```
  It opened as a **DIRECT** output at **16 kHz mono** — because the `virtual output` mix port advertises
  a single `PCM_16_BIT / 16000 / mono` profile. **DIRECT outputs do not resample.** WhatsApp's VoIP
  playback is **48 kHz**, so it can't be placed on this output:
  - `openOutputStream` on our module is **never called** during the call (only `openInputStream` is).
  - The only track on `OUT_BUS` is **our own app's**, and it stays in **standby (0 active)** the entire
    call (it writes silence in SCO mode).
  - **WhatsApp has no active playback track anywhere**, so its far-end audio never reaches `OUT_BUS`.

**Consequence:** the downlink tap (`StreamVirtualSco` OUT → `va_server_push_downlink`) only ever sees
silence → the agent hears nothing → it invokes its End-Call tool ~3 s in → the call drops. This is the
"call drops after 3 seconds / agent audio heard nowhere" symptom. The **input side is fine**; only the
output endpoint is the blocker.

## Root cause

A mix port whose only profile is `16000 / mono` forces AudioFlinger to open the output as **DIRECT**
(no mixer, no resampler). A DIRECT output requires the client's format to match exactly, so a 48 kHz VoIP
playback client cannot attach — the framework does not insert a resampler for DIRECT outputs.

The **input** side avoids this because a capture endpoint opens as a fast/normal record that resamples
the client to the HAL rate; the **output** side does not get that for free with a single narrow profile.

## Change

Goal: make `OUT_BUS` (and `IN_BUS`, for symmetry and future-proofing) a **normal MIXER endpoint that
advertises the standard 48 kHz** rate, so the framework mixer/resampler places any VoIP client on it and
resamples to a fixed rate — while the **va_server socket stays 16 kHz mono** (the app/protocol side is
unchanged: `NativeAudioBridge` negotiates 16 kHz / 320-sample frames).

### 1. `VirtualScoConfiguration.cpp` — widen the mix port profiles
On the **output mix port** (`virtual output`) and the **input mix port** (`virtual input`), advertise
the rate the framework actually produces for VoIP so the endpoint is opened as a MIXER, not DIRECT:

- Add **48000** (keep 16000 too if you like, but 48000 is the one WhatsApp/WebRTC uses). A single 48000
  profile is the simplest thing that makes it a mixer output.
- Consider advertising **stereo** on the output mix port as well (WebRTC output is often stereo);
  the device port can stay mono.
- Leave the **device ports** (`OUT_BUS` / `IN_BUS`) as they are (auto-attached BUS, address
  `02:56:41:00:00:01`) — this change is about the **mix port** profiles that decide the framework thread
  type, not the device.
- Do **not** set `AUDIO_OUTPUT_FLAG_DIRECT`. If a flag is needed to force a normal mixer path, use the
  default (`AUDIO_OUTPUT_FLAG_NONE`) / primary-like mixer flags; the intent is a resampling MIXER output.

Confirm on device afterwards: `dumpsys media.audio_flinger` shows the `OUT_BUS` thread as
`type 0 (MIXER)` (not DIRECT), and during a call **`openOutputStream` is called on the `virtual` module**
with an active track owned by `com.whatsapp`.

### 2. `StreamVirtualSco.cpp` — convert between the mixer rate and the 16 kHz socket
The HAL stream now runs at the mix-port rate (48 kHz, possibly stereo), but `va_server` frames are
**16 kHz mono, 320 samples/20 ms**. Add conversion in `transfer()`:

- **Output (downlink):** the buffer AudioFlinger writes is now 48 kHz (stereo?). Before
  `va_server_push_downlink`, **downmix to mono and resample 48 kHz → 16 kHz** so exactly 16 kHz mono
  PCM16 goes to the socket. (AudioFlinger already resampled every client to the thread rate, so you only
  do 48→16 once here.)
- **Input (uplink):** `va_server_pull_uplink` returns 16 kHz mono; **upsample 16 kHz → 48 kHz** (and
  duplicate to stereo if the input mix port is stereo) to fill the buffer AudioFlinger reads.
- Keep the existing wall-clock pacing and the silence-on-underrun behaviour. Pick frame counts so the
  16 kHz socket frame (320 samples) maps cleanly (e.g. 48 kHz burst of 960 samples ↔ 320 samples @16 k).
- A simple linear/polyphase resampler is fine (voice band); `libaudioutils` (already a dependency) has
  resampler helpers if you prefer not to hand-roll.

Everything else (va_server bridge, socket protocol, 16 kHz app side, ap3a/V2 build, packaging,
auto-attached BUS device) is unchanged.

## Acceptance (on device, live WhatsApp call)

1. `dumpsys media.audio_flinger`: the `OUT_BUS` thread is `type 0 (MIXER)`, not DIRECT.
2. During a call with the far side talking: **`openOutputStream: ... virtual`** fires, and the `OUT_BUS`
   thread shows an **active** track owned by **com.whatsapp** (the human's voice is now on our device).
3. `AHAL_VirtualScoStream` shows non-silent frames going to `va_server_push_downlink`; the app's capture
   RMS (`ElevenLabsDirectCapture ... SCO`) is non-zero → the agent hears the human.
4. The call **no longer drops at ~3 s** (the agent gets real downlink audio and stays engaged); the far
   side hears the agent via the already-working `IN_BUS` uplink.

## Notes
- The app pinner and socket are unchanged; only the HAL endpoint format changes.
- If a single 48 kHz mixer profile still opens DIRECT on this device, try the AOSP `r_submix` mix-port
  shape (48 kHz stereo, `AUDIO_OUTPUT_FLAG_NONE`) as the reference — r_submix is the canonical software
  MIXER endpoint and is known to open non-direct.

---

## Decision: implemented as **stereo 16 kHz**, not 48 kHz

**What was built:** the playback side (`virtual output` mix port + `OUT_BUS` device port) advertises
**PCM16 stereo 16 kHz**; the capture side stays **PCM16 mono 16 kHz** (already working).
`StreamVirtualSco::transfer()` downmixes the stereo playback buffer to mono before
`va_server_push_downlink` (and would duplicate mono across channels on a multi-channel input). No
resampler anywhere; the socket stays 16 kHz mono / 320-sample frames.

**Why stereo 16 kHz is expected to work — the DIRECT thread was caused by *mono*, not by 16 kHz.**
In this tree (android-15.0.0_r36):

1. `AudioFlinger::openOutput_l` (`frameworks/av/services/audioflinger/AudioFlinger.cpp` ~l.3151) creates a
   **DIRECT** thread if the output has the DIRECT flag **or** the HAL format isn't a valid mixer sink
   format **or** the channel mask isn't a valid mixer sink mask; otherwise a **MIXER** thread.
2. `IAfThreadBase::isValidPcmSinkChannelMask` (`Threads.cpp` ~l.287) rejects fewer than 2 channels:
   `if (channelCount < FCC_2 // mono is not supported at this time`. Our mix port was mono → invalid sink
   mask → DIRECT. The sample rate plays no part in this decision.
3. PCM16 is a valid sink format, and our mix port has no DIRECT flag, so with a **stereo** mask the
   condition is false → **MIXER** thread.
4. A MIXER thread runs at whatever rate the HAL stream is opened at and **resamples every track to the
   thread rate** — that's what lets WhatsApp's 48 kHz VoIP playback attach to a 16 kHz output (the same way
   the capture side already resamples WhatsApp's 48 kHz record to our 16 kHz input).

**Why not 48 kHz:** it would also produce a MIXER thread (stereo is what matters), but it adds a
48↔16 kHz resampler in `transfer()` in both directions — more code, CPU and latency — for no benefit:
the socket and the app side are 16 kHz voice anyway, and AudioFlinger's mixer already does the one
resampling step we need (48 → 16 kHz on playback). With 16 kHz on both ends of the HAL, `transfer()` only
changes channel count.

**Residual risks to verify on device:**
- `dumpsys media.audio_flinger`: the `OUT_BUS` thread should now be `type 0 (MIXER)` at `16000 Hz`,
  channel mask stereo. `logcat -s AHAL_VirtualScoStream` logs `init: output 16000 Hz, 2 ch, …`.
- Whether APM actually places WhatsApp's VoIP track on this output depends on routing/pinning
  (`setPreferredDeviceForStrategy`), not on the thread type — the MIXER thread removes the format
  blocker only. Acceptance items 2–4 above still apply.
- A mixer thread at 16 kHz band-limits playback to 8 kHz — fine for voice (the downlink is voice-only).
- If on this device the output still opens DIRECT, fall back to the spec's 48 kHz stereo shape (r_submix
  reference) and add the 48→16 kHz conversion.
