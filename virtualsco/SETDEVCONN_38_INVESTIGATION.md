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

## Findings (android-15.0.0_r36 AOSP sources) — root cause found, fix built

**Q1 — the `-38` branch.** It is **not** the declared-device lookup: the connect path calls
`HwModuleCollection::getDeviceDescriptor(..., allowToCreate=true)`, which creates a device instead of
returning null, so "could not find HW module" can't fire. It is the HAL-connect step in
`AudioPolicyManager::setDeviceConnectionStateInt`: `broadcastDeviceConnectionState(CONNECTED)` fails →
`mAvailableOutputDevices.remove` → `return INVALID_OPERATION`. The capture should contain (E, tag
`APM_AudioPolicyManager`) `Error -22 while setting connected state …` and
`setDeviceConnectionStateInt() device … connection failed`.

**Q2 — why our device isn't matched (the actual reject).** The reject is one layer below APM, in the
framework's `Hal2AidlMapper` (`frameworks/av/media/libaudiohal/impl/Hal2AidlMapper.cpp`):
- `AudioFlinger::setDeviceConnectedState` fans the connect out to **every** HAL module (success if any
  accepts).
- `Hal2AidlMapper::setDevicePortConnectedState` looks up the **template** port with the requested
  address **reset to empty** (`// Reset the device address to find the "template" port.`), then
  `findPort()` → `audioDeviceMatches()` compares the **whole** `AudioDevice` (type **and** address).
- Our template ports declared the MAC, so they never matched → the mapper returns `BAD_VALUE` (logged
  only at D, "normal" because it's asked in every module) and **never calls `connectExternalDevice`** →
  no module accepts → APM `-38`. That's why `populateConnectedDevicePort` never logs.

So yes: an external (BT) template port must be declared **without an address**; the address comes with
the connect request and the HAL copies it onto the connected port
(`Module::connectExternalDevice`: `connectedDevicePort.device.address = inputDevicePort.device.address`).

**Q3 — Samsung policy manager.** Not needed to explain `-38` (AOSP alone does). But the
`getDeviceConnectionState() undeclared device, type 00000004, address: <empty>` lines are **not**
explained by AOSP: with an empty address that lookup matches any declared device of the type, and ours
was declared. Worth confirming which policy-manager library audioserver loads on the A566B.

**Q4 — is connecting a HAL-declared BT-SCO device supported?** Yes. It's exactly the Bluetooth stack's
path (AudioService → `AudioSystem.setDeviceConnectionState` → APM → mapper → addressless template →
`connectExternalDevice`). No BT-specific ownership/validation exists in the native path (APM,
AudioFlinger, libaudiohal); BtHelper is AudioService-only and a direct `AudioSystem` call bypasses it.
Caller identity only matters at the `MODIFY_AUDIO_SETTINGS` gate.

### Fix (built, commit below): addressless SCO template ports
`VirtualScoConfiguration.cpp` no longer sets `AudioDeviceAddress::mac` on the two SCO device ports. Keep
calling `setDeviceConnectionState(AudioDeviceAttributes(ROLE_…, TYPE_BLUETOOTH_SCO,
"02:56:41:00:00:01"), AVAILABLE, 0)` — the connected device still surfaces at that MAC. Expected on
device: `AHAL_VirtualScoModule: populateConnectedDevicePort: connecting …` for both roles, return `0`,
device in `getDevices()`.

**Caveat — module binding.** With no declared port carrying the MAC, APM can't bind the connect to a
module by address; it creates a dynamic device attached to the **first module that declares
`OUT/IN_BLUETOOTH_SCO_HEADSET`** (`HwModuleCollection::createDevice` → `getModuleForDeviceType`). The HAL
connect itself still reaches `virtual` (fan-out), but APM opens outputs/patches on the module the
device is attached to.
- If `virtual` is the **only** module declaring BT-SCO types → full fix.
- If the vendor module declares a SCO port too (likely — the "dead SCO port") and precedes `virtual`,
  the device binds to the **vendor** module and audio won't reach our socket. Check
  `dumpsys media.audio_policy` (which modules list `…BLUETOOTH_SCO_HEADSET`, module order) and, after
  connecting, which module the `02:56:41:00:00:01` device is attached to. In that case Option 2
  (`TYPE_BUS`, empty connection → auto-attached, per-module) is the cleaner route.
- Not recommended: keeping a MAC'd port (for binding) **plus** an addressless template. The connect
  succeeds, but later patch/stream setup resolves the device via `findPort()` by address, which returns
  the lowest-id match — the never-connected MAC'd port — and is likely to fail.

## Acceptance
Identify the exact `-38` branch and a change (HAL config, module, or the connect-call shape) that makes
APM **reach `connectExternalDevice` on the `virtual` module** and surface the device in
`AudioManager.getDevices()` as `TYPE_BLUETOOTH_SCO @ 02:56:41:00:00:01` (out + in).

---

## Retest after 0bbcc28 (addressless) — the "module binding" caveat is realized → going Option 2

On SM-A566B (0bbcc28 verified on device; virtual device now `{…BLUETOOTH_SCO_HEADSET, @:}` empty address;
stock `libaudiopolicymanagerdefault.so`):
- `setDeviceConnectionState(…AVAILABLE)` still returns **-38**, and the verbose capture
  (`Hal2AidlMapper:V DeviceHalAidl:V AHAL_VirtualScoModule:V AudioFlinger:V AudioSystem-JNI:V`) shows
  **no `setDevicePortConnectedState` / `connectExternalDevice` / `populateConnectedDevicePort` for our
  module at all** — only the primary module's routine call patches. The connect never reaches `virtual`.
- This matches the **"Caveat — module binding"** above: with an addressless template, APM binds the
  connect by type to the vendor's BT-SCO port (which precedes `virtual`), and it fails there (`-38`)
  rather than reaching us.

Per the recommendation in that caveat, **we are switching primary effort to Option 2 (`TYPE_BUS`,
empty-connection auto-attached, per-module)** — see `OPTION2_BUS_DEVICE.md`. It needs no
`setDeviceConnectionState` (auto-attached at boot), and an empty-connection device is attached to *its
own* module, so it can't be stolen by the vendor's BT-SCO port. This `-38` line of investigation is
parked unless Option 2 also hits a wall.
