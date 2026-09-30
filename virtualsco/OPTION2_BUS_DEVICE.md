# Option 2: make the virtual device a `TYPE_BUS` (empty-connection) device

**Scope: `virtualsco/` HAL only (mainly `VirtualScoConfiguration.cpp`). The app side is handled
separately (the pinner is made dual-type: it matches the virtual device by address across
`TYPE_BLUETOOTH_SCO` — legacy HIDL fleet — OR `TYPE_BUS` — this AIDL build).**

## Why
The BT-SCO connect path is a dead end on this stack: `setDeviceConnectionState` returns `-38` and — with
the addressless template — the connect binds by type to the vendor's own BT-SCO port instead of `virtual`
(see `SETDEVCONN_38_INVESTIGATION.md`). A **`TYPE_BUS` device has an *empty* connection type**, so:
- the framework **auto-attaches it at boot** → it appears in `AudioManager.getDevices()` with **no
  `setDeviceConnectionState` call at all** (no `-38`, no fan-out dependency);
- an empty-connection device is attached to **its own module**, so it **cannot be stolen** by the
  vendor's BT-SCO port.

This removes the entire connect problem class.

## Change (`VirtualScoConfiguration.cpp`)
In `createScoDeviceExt(...)` (or equivalent), for the two device ports:
- **Device type → BUS:** `deviceExt.device.type.type = AudioDeviceType::OUT_BUS` for the output port and
  the input-BUS equivalent for the input port. (Confirm the exact enum names in the tree —
  `aidl/android/media/audio/common/AudioDeviceType.aidl`. Output is `OUT_BUS`; use the corresponding
  input BUS type. These must map to `AudioDeviceInfo.TYPE_BUS` at the app layer.)
- **Connection → empty:** remove `deviceExt.device.type.connection = CONNECTION_BT_SCO`; leave it unset /
  empty (`AudioDeviceDescription.connection == ""`). This is what makes it auto-attach.
- **Keep the address** `02:56:41:00:00:01` on the device port (the app matches by address to distinguish
  it from any vendor BUS device). An attached device *can* carry a fixed address (unlike the BT-SCO
  connect case), so keep it here.
- **Keep `profiles` on the port** (PCM16 mono 16 kHz) — attached device with fixed profiles, as in
  `0bbcc28`/`231eb19`. `connectedProfiles` is not needed (no external connect).

Everything else (mix ports, routes, the `va_server` socket streams, the ap3a/V2 build, packaging) is
unchanged. The `137770f` connect-accept override becomes unused (harmless to leave).

## Acceptance (on device, no real Bluetooth, no `setDeviceConnectionState` call)
- `dumpsys media.audio_policy`: the `virtual` module lists a BUS out + BUS in device at
  `02:56:41:00:00:01`, shown as **available** (attached), not just declared.
- `AudioManager.getDevices(GET_DEVICES_OUTPUTS)` includes a **`TYPE_BUS`** device at that address at boot
  (this is what the app pinner will match); `_INPUTS` the matching input.
- Then the app pins voice-communication to it via `setPreferredDeviceForStrategy` (it already holds
  `MODIFY_AUDIO_ROUTING`) and the socket streams carry audio.

## The one behavioral unknown to verify after it builds
That routing `USAGE_VOICE_COMMUNICATION` playback/capture to a BUS device actually taps a third-party
VoIP app's audio (BUS is a generic routing sink, so it should — but confirm `out_write`/`in_read` on the
`va_server` socket during a WhatsApp call).

## Implemented (HAL side)
`VirtualScoConfiguration.cpp`: the two device ports are now `OUT_BUS` / `IN_BUS` with an empty
connection, static PCM16 mono 16 kHz profiles, and the fixed address. Rebuilt incrementally with
`aosp_arm64-ap3a-userdebug`; NEEDED unchanged.

**One deviation from the spec above — the address is declared as an `id` string, not `mac`:**
`AudioDeviceAddress::id("02:56:41:00:00:01")`. For an empty-connection device the framework converts
legacy address strings back to AIDL as the `id` variant (`suggestDeviceAddressTag` only picks `mac` for
BT/wireless connections), and `Hal2AidlMapper` matches devices exactly (type + address incl. the variant).
A `mac`-tagged address would round-trip as `id` and stop matching when the framework later creates
patches / opens streams on the device. The app-visible `AudioDeviceInfo.getAddress()` string is the same
`02:56:41:00:00:01` either way.
