# Virtual BT-SCO AIDL audio module (Exynos / Unisoc-AIDL / any AIDL audio HAL)

The AIDL counterpart of `native/hal` (the legacy `.so` + XML overlay). Devices on the **AIDL audio
HAL** (`Config source: AIDL HAL`, e.g. SM-A566B / Exynos 1580, newer Unisoc on Android 13+) do not
load a legacy `.so` and do not read an XML policy source, so the virtual SCO device is provided as a
standalone **AIDL `IModule/virtual`** service instead.

## Why this is device-generic (built once, not per tree)

`android.hardware.audio.core` is a **stable, versioned AIDL interface** — identical on every
AOSP-based device. We build against the tree's frozen `android.hardware.audio.core-V*-ndk`, and
stable-AIDL backward compatibility means a module built at the base version is usable by newer
frameworks. Per-device variation (interface version, SELinux domain, VINTF) is handled at **flash
time** by `customize.sh`, not by rebuilding. So this is compiled once from upstream/BSP and shipped
everywhere; there is no per-AOSP-tree fork.

The framework discovers modules via `getDeclaredInstances("android.hardware.audio.core.IModule")`,
so simply **declaring** `IModule/virtual` (VINTF fragment) makes `AudioPolicyManager` enumerate it
next to the vendor's own modules — the AIDL analogue of adding an `<xi:include>` module to the
legacy policy XML. We never modify the vendor's HAL or manifest.

## What's here

| File | Role |
|---|---|
| `VirtualScoConfiguration.{cpp,h}` | Declares the virtual device: BT-SCO out + in at `02:56:41:00:00:01`, PCM16 mono 16 kHz, two mix ports, two routes. Modelled 1:1 on AOSP's `getRSubmixConfiguration()`. |
| `main_virtual.cpp` | Registers `IModule/virtual`, reusing AOSP's complete `Module` impl. **Spike:** `Type::STUB` (silent streams). |
| `Android.bp` | `cc_binary` reusing `libaudioserviceexampleimpl` + `aidlaudioservice_defaults` from `hardware/interfaces/audio/aidl/default`. |
| `virtualsco.xml` | VINTF fragment declaring `IModule/virtual` (version templated per device). |
| `virtualsco.rc` | init service. |
| `sepolicy/` | `hal_audio_virtualsco` domain (hal_audio server role) + file/service contexts. |

## Build (once, in an AOSP/BSP tree)

1. Copy this directory into the tree, e.g. `vendor/aicaller/virtualsco/`, and add its `sepolicy/` to
   `BOARD_VENDOR_SEPOLICY_DIRS`.
2. `m android.hardware.audio.service-aidl.virtualsco`
3. Collect the artifacts: the service binary
   (`.../vendor/bin/hw/android.hardware.audio.service-aidl.virtualsco`), `virtualsco.rc`,
   `virtualsco.xml`, and the compiled sepolicy (or the `.te`/contexts for the target's policy).

## Deploy (dynamic, via the Magisk module)

`magisk/audiopolicy/customize.sh` already detects the HAL model. Extend it so that when
`Config source: AIDL HAL`:
- install the service binary → `/vendor/bin/hw/`, `virtualsco.rc` → `/vendor/etc/init/`,
  `virtualsco.xml` → `/vendor/etc/vintf/manifest/`;
- **template the VINTF `<version>`** to the `android.hardware.audio.core` version the device
  advertises (from the device manifest / `lshal`), so one artifact fits V1/V2/V3 devices;
- label the binary `hal_audio_virtualsco_exec` (or, if the compiled policy can't be injected, fall
  back to `hal_audio_default_exec` to run in the existing audio domain) and add the `/virtual`
  service-context + any sepolicy rules;
- on the **legacy/HIDL** branch, keep shipping `native/hal`'s `.so` + XML overlay as today.

## Verify — the enumeration spike (make-or-break)

After flashing + reboot:

```
adb shell dumpsys media.audio_policy | grep -iE '"virtual"|BT SCO Virtual|02:56:41'
adb shell lshal | grep -i 'audio.core.IModule/virtual'
```

- **PASS:** the `virtual` module and its BT-SCO device appear, and `StrategyRoutePinner` (app side)
  can pin voice-communication to `02:56:41:00:00:01` (`out=true in=true`). → proceed to real streams.
- **FAIL:** module not enumerated / not routable → the vendor framework restricts modules; stop and
  reassess (this is exactly what the spike exists to find out cheaply).

## After a PASS — real streams

Replace `Type::STUB` with a virtual module type whose `IStreamIn`/`IStreamOut` are backed by the
**reused** `va_server`/`va_ring`/`va_protocol` bridge (same socket + jitter buffers as `native/hal`):
stream `write` → SCO downlink (human→agent), stream `read` → SCO uplink (agent→human). The app's
`sco/` package and `StrategyRoutePinner` are unchanged. Then extend the sepolicy here with the
abstract-socket rules noted in `sepolicy/hal_audio_virtualsco.te`.
