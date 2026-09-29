# Next step: make the virtual SCO device appear in `AudioManager.getDevices()`

Status as of commit `137770f`. The HAL side is done; what remains is a **framework-side trigger**
that reports the virtual SCO devices as connected at boot. This lives outside the `virtualsco/`
build (Magisk module and/or app).

## 1. What we established (from the android-15.0.0_r36 sources)

| Finding | Where |
|---|---|
| The framework treats an AIDL device port as **attached** (always available) **only if its `connection` is empty** — the one exception is the remote-submix input. `CONNECTION_BT_SCO` is non-empty, so our ports can never be attached, whatever their `profiles` / `connectedProfiles`. This is why `231eb19` (profiles on the port) and `8df1e0d` (no `connectedProfiles`) cannot fix visibility. | `frameworks/av/services/audiopolicy/common/managerdefinitions/src/AudioPolicyConfig.cpp` (`loadFromAidl`, ~l.117) |
| r_submix is not "attached by config" either: the framework itself calls `connectExternalDevice` for the submix input at init. | `frameworks/av/media/libaudiohal/impl/Hal2AidlMapper.cpp` (`initialize`, ~l.727) |
| A non-attached device becomes available only via **`AudioPolicyService::setDeviceConnectionState(AVAILABLE)`** → APM → HAL `IModule.connectExternalDevice`. Normally the Bluetooth stack drives this through AudioService. | `AudioPolicyInterfaceImpl.cpp` (`setDeviceConnectionState`) |
| That call is gated on **`MODIFY_AUDIO_SETTINGS`** (or the audioserver uid); root/system uids pass. | same, ~l.206 |
| The base HAL `Module::populateConnectedDevicePort` **rejected** any connect on a port with a non-empty connection. **Fixed in `137770f`**: `ModuleVirtualSco` overrides it and accepts the connect, keeping the static PCM16 mono 16 kHz profile. | `hardware/interfaces/audio/aidl/default/Module.cpp` (~l.1739) |
| AudioService only re-applies **its own** device inventory after an audioserver restart (`AudioDeviceInventory.onRestoreDevices`). Our device isn't in it, so the connection is **lost on every audioserver restart** and must be re-issued. | `frameworks/base/services/core/java/com/android/server/audio/AudioDeviceInventory.java` |
| The vendor HAL service itself **cannot** issue the connect: it's a vendor process, and `media.audio_policy` is a framework-only binder service (Treble/SELinux). | — |

## 2. The call to make

For both directions, at MAC `02:56:41:00:00:01`:

```java
// android.media.AudioSystem — @UnsupportedAppUsage, hidden
AudioSystem.setDeviceConnectionState(
        new AudioDeviceAttributes(AudioDeviceAttributes.ROLE_OUTPUT,
                AudioDeviceInfo.TYPE_BLUETOOTH_SCO, "02:56:41:00:00:01"),   // @SystemApi ctor
        AudioSystem.DEVICE_STATE_AVAILABLE,   // 1
        AudioSystem.AUDIO_FORMAT_DEFAULT);    // 0
// ...and the same with AudioDeviceAttributes.ROLE_INPUT
```

APM then calls our HAL's `connectExternalDevice` for the matching template port, and the device
shows up in `AudioManager.getDevices()` as `TYPE_BLUETOOTH_SCO` (in + out).

## 3. Recommended approach: a root helper started by the Magisk module

**Why Magisk:** the virtualsco HAL binary itself is deployed through the Magisk audiopolicy module,
so a helper started from the same module adds **no new dependency**. It also avoids the app-side
hidden-API problem: `setDeviceConnectionState` is `@UnsupportedAppUsage` and the only usable
`AudioDeviceAttributes` constructors are `@SystemApi`, which a regular app may be blocked from
reflecting into on Android 15. A process started with `app_process` as root isn't subject to
app hidden-API enforcement and passes the permission check as uid 0.

**Shape:**

1. A tiny Java helper (`classes.dex` in the module), e.g. `ScoConnect.main()`, that makes the two
   calls in §2 (via reflection is fine) and prints the return codes (`0` = `AUDIO_STATUS_OK`).
2. `service.sh` (late-start, runs after boot):
   ```sh
   # wait until audioserver has loaded the virtual module
   until dumpsys media.audio_policy 2>/dev/null | grep -q '02:56:41:00:00:01'; do sleep 2; done
   CLASSPATH=$MODDIR/sco-connect.dex app_process /system/bin ScoConnect
   ```
3. **Re-issue after audioserver restarts:** loop in `service.sh` watching the audioserver pid
   (`pidof audioserver`) and re-run the helper when it changes (see §1 — the connection is not
   restored by AudioService).
4. Log to logcat (tag e.g. `ScoConnect`) so failures are visible.

**Fallback / complement: the app.** Before pinning, the app can check `getDevices()` for the SCO
device; if it's missing, it should *ask the helper to reconnect* (e.g. via `su -c` running the same
`app_process` command) rather than call the hidden API itself.

## 4. Caveats to check on the devices

- **Samsung + Magisk (SM-A566B):** Samsung devices are known to block or break Magisk under their
  enforcement (Knox / bootloader-lock / SELinux enforcing policy). If Magisk can't run on a device,
  **the virtualsco HAL can't be deployed there either** — this trigger doesn't change that
  requirement. Where Magisk does run, the helper runs in the `magisk` SELinux domain; if a
  `binder_call` from it to `audioserver` is denied (`avc: denied` in logcat), add a
  `sepolicy.rule` allow for it in the module.
- **Which module takes the device:** if the vendor's own audio module also declares BT-SCO device
  types, APM could bind the connection to it instead of `virtual`. After connecting, confirm in
  `dumpsys media.audio_policy` that the `02:56:41:00:00:01` device is available under the
  `virtual` module handle.
- **AudioService is unaware of this device.** `getDevices()` reads from audio policy, so visibility
  is fine; but if the app's pinner routes via `AudioManager.setCommunicationDevice()` with a
  `TYPE_BLUETOOTH_SCO` device, AudioService's Bluetooth SCO handling may try to start SCO through
  the (absent) Bluetooth stack. Check how `StrategyRoutePinner` pins and watch logcat for BT SCO
  errors when it does.
- **Timing:** the helper must run after the virtual module is registered and loaded by audioserver
  (hence the `dumpsys` wait loop).

## 5. Acceptance (unchanged)

At boot, with no real Bluetooth device:

- `AudioManager.getDevices(GET_DEVICES_OUTPUTS)` includes `TYPE_BLUETOOTH_SCO` at
  `02:56:41:00:00:01`, and `GET_DEVICES_INPUTS` the matching input;
- the pinner logs success instead of `no SCO output device in AudioManager.getDevices`;
- `logcat -s AHAL_VirtualScoModule` shows `populateConnectedDevicePort: connecting ...` for both
  ports (the HAL accepting the connect);
- after `killall audioserver`, the device reappears once the helper re-runs.
