# Virtual-audio bridge core (shared with the HIDL `.so`)

These files are the socket bridge the virtual audio HAL uses to exchange PCM with the app-side
client over the abstract Unix socket `@virtual_audio`. They are the SAME sources that build AICaller's
HIDL `audio.virtual.default.so`, copied here so the AIDL `virtualsco` build is self-contained (the
Linux/AOSP build only sees this repo).

- `include/va_protocol.h` — wire protocol (socket path, `va_hello` handshake, 16 kHz mono int16 frames).
- `include/va_server.h` + `va_server.cpp` — listening socket, accept/rx/tx threads, two non-blocking
  SPSC rings. Audio callbacks call only `va_server_pull_uplink()` / `va_server_push_downlink()`.
- `include/va_ring.h` + `va_ring.cpp` — the SPSC ring buffer.

If the protocol changes, keep these in sync with AICaller's `native/` and `NativeAudioBridge.kt`.
