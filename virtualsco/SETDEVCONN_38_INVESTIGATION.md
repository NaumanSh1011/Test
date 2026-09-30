# Investigation handoff: setDeviceConnectionState(AVAILABLE) fails with -38 (INVALID_OPERATION) before reaching the HAL

**Owner: whoever has the AOSP + Samsung policy-manager source. The `virtualsco/` HAL side is confirmed
OK as of `137770f`.** This is Option 1 (root-cause the reject). Option 2 (switch the device to a
`TYPE_BUS`, empty-connection auto-attached type) is being pursued in parallel and would make this moot —
coordinate before investing heavily here.

## Goal
Make the virtual BT-SCO device (`IModule/virtual`, `AUDIO_DEVICE_OUT/IN_BLUETOOTH_SCO_HEADSET`,
MAC `02:56:41:00:00:01`) appear in `AudioManager.getDevices()` so the app can pin voice-communication to
it. A `CONNECTION_BT_SCO` port is never auto-attached, so the plan was to report it AVAILABLE via
`AudioSystem.setDeviceConnectionState`. That call is rejected with -38.

## Device / build under test
- **SM-A566B (Exynos 1580), Android 15, Samsung retail build.** Tested under `setenforce 0` (SELinux is
  not the cause).
- Module binary is **`137770f`** — verified on device (md5 matches; `ModuleVirtualSco::populateConnectedDevicePort`
  override present). So the base-Module "must override populateConnectedDevicePort" rejection is NOT in play.
- Module enumerates in `dumpsys media.audio_policy`: `Handle N "virtual"`,
  `BT SCO Virtual` (`OUT_BLUETOOTH_SCO_HEADSET @ 02:56:41:00:00:01`) +
  `BT SCO Virtual Mic` (`IN_… @ same`), PCM16 mono 16 kHz.

## Exact failure
Captured with `logcat -s AHAL_VirtualScoModule AudioPolicyManager APM_AudioPolicyManager AudioSystem-JNI AudioFlinger DeviceHalAidl`.

Caller: a **privileged app holding `MODIFY_AUDIO_SETTINGS`**, calling (reflection via HiddenApiBypass):
```java
AudioSystem.setDeviceConnectionState(
    new AudioDeviceAttributes(ROLE_OUTPUT, TYPE_BLUETOOTH_SCO, "02:56:41:00:00:01"),
    DEVICE_STATE_AVAILABLE /*1*/, AUDIO_FORMAT_DEFAULT /*0*/);
// and the same with ROLE_INPUT
```
Log:
```
E AudioSystem-JNI: Command failed for android_media_AudioSystem_setDeviceConnectionState: -38   (both roles)
I StrategyRoutePinner: setDeviceConnectionState(role=2/1, SCO @02:56:41:00:00:01, AVAILABLE) = 1   (1 = generic JNI error)
# NO "AHAL_VirtualScoModule: populateConnectedDevicePort" anywhere -> APM rejected BEFORE calling the HAL
# during routing (~7 s later), APM logs:
V APM_AudioPolicyManager: getDeviceConnectionState() undeclared device, type 00000004, address:   (0x4 = OUT_BLUETOOTH_SCO_HEADSET)
V APM_AudioPolicyManager: getDeviceConnectionState() undeclared device, type 00000008, address:   (0x8 = OUT_BLUETOOTH_SCO_CARKIT)
```
Net: **status = -38 INVALID_OPERATION, HAL never invoked, device never in `getDevices()`** → the app's
pinner logs `no SCO output device in AudioManager.getDevices` and falls back to telephony taps (agent
plays out the speaker instead of the call uplink).

## Ruled out
- **HAL** — `137770f` accepts connects, but it's never called.
- **Caller / permission / root** — gate is `MODIFY_AUDIO_SETTINGS` (app has it); a denial would be `-1`,
  not `-38`. uid-0 / audioserver take the identical post-gate path, so root won't change `-38`.
- **SELinux** — tested permissive; only unrelated permissive property-read AVCs.

## Questions to resolve (with the tree)
1. In `AudioPolicyManager::setDeviceConnectionStateInt` (r36), which branch returns **INVALID_OPERATION
   (-38) for an OUT device `type=BLUETOOTH_SCO_HEADSET` + explicit address WITHOUT calling the HAL**
   (`connectExternalDevice`)? "could not find HW module for device" (declared-device lookup miss), an
   early state check, or a BT-specific branch?
2. Why is our declared device not matched? Note the `undeclared device … address: <empty>` logs — the
   framework queries BT-SCO with an **empty** address. **Strongest lead:** does APM's BT-SCO connect/lookup
   match by type + empty address (the BT stack assigns the real MAC), so our **hard-coded MAC
   `02:56:41:00:00:01` makes the port unmatchable** via this path? If so, must a BT-SCO device be
   declared/connected with an empty address?
3. **Samsung custom policy manager:** does this build load a vendor `libaudiopolicymanager*.so` / custom
   `AudioPolicyManager` rather than AOSP's? (Check audioserver's loaded libs / audio-policy props.) If so,
   `-38` may originate in Samsung code AOSP source won't explain — inspect their path.
4. Does the AOSP path even **support** connecting a HAL-declared BT-SCO device via
   `setDeviceConnectionState`, or is BT-SCO connection state exclusively owned by the BT stack? (Earlier
   analysis expected it to bind to our module; `-38` contradicts that — reconcile.)

## Fast diagnostics
- Raise APM verbosity / add a log at each `-38` return in `setDeviceConnectionStateInt` to print the
  branch that fires for our device.
- Try the connect with an **empty address** (`AudioDeviceAttributes(ROLE_OUTPUT, TYPE_BLUETOOTH_SCO, "")`)
  — if APM then matches/accepts, Q2 is confirmed (and the app loses MAC-based disambiguation from the
  vendor's dead SCO port — note that).
- Confirm the loaded policy-manager library (AOSP vs Samsung custom).

## Acceptance
Identify the exact `-38` branch and a change (HAL config, module, or the connect-call shape) that makes
APM **reach `connectExternalDevice` on the `virtual` module** and surface the device in
`AudioManager.getDevices()` as `TYPE_BLUETOOTH_SCO @ 02:56:41:00:00:01` (out + in).
