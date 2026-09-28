# Building the Virtual SCO AIDL Audio Module from AOSP

This document explains **what this build is for**, **where we started**, **the problems we hit and
how we debugged them**, **the way forward**, **what the build script does**, and **the final
outcome**. It is the companion to `build_wsl.sh` and the `virtualsco/` module in this repo.

---

## 1. What we're trying to achieve

AICaller bridges a phone call's audio to an AI voice agent (over WebRTC/LiveKit). To tap and inject
call audio without a real Bluetooth headset, it uses a **virtual Bluetooth-SCO audio device**: the
app pins the call's voice route to that virtual device, reads the far end from it, and writes the
agent's voice back into it.

There are **two Android audio-HAL worlds**, and the tap has to be built differently for each:

| HAL world | Devices | How the virtual SCO device is provided |
|---|---|---|
| **Legacy / HIDL** (file-based audio policy) | MediaTek (e.g. SM-A055F / MT6768, SM-A045F / MT6765), older Unisoc, Android ≤14 | A legacy `.so` HAL + an XML policy overlay (`native/hal` + the Magisk `audiopolicy` module). **Already working.** |
| **AIDL audio HAL** (`Config source: AIDL HAL`) | Exynos (e.g. SM-A566B / Exynos 1580), newer Unisoc, Android 15+ | An **AIDL `IModule/virtual` service** — this is what we're building here. |

The legacy `.so` cannot be loaded on an AIDL-HAL device (no `hw_get_module` path, no XML policy
source). So for the AIDL world we need a small **standalone vendor service** that registers
`android.hardware.audio.core.IModule/virtual`, exposing a virtual BT-SCO in/out device at MAC
`02:56:41:00:00:01`. The framework discovers modules by their declared AIDL instance name, so simply
declaring `IModule/virtual` in a VINTF fragment makes the policy manager pick it up alongside the
vendor's own modules — the AIDL analogue of adding a `<module>` to the legacy policy XML.

**Design goal:** one device-agnostic binary, built once from **stable, upstream AOSP** (stable AIDL
is backward-compatible, so a build against one interface version runs on that version and newer),
deployed per-device via the Magisk module. It is **not** shipped in the APK.

---

## 2. What we started with

- The **module source** (`virtualsco/`):
  - `VirtualScoConfiguration.{cpp,h}` — declares the virtual SCO ports/routes (modelled 1:1 on
    AOSP's `r_submix` software device).
  - `main_virtual.cpp` — registers `IModule/virtual`, **reusing AOSP's complete default `Module`
    implementation** (`libaudioserviceexampleimpl`); we only supply the config. For the first
    milestone (the "enumeration spike") it's created as `Type::STUB` (silent streams).
  - `Android.bp`, `virtualsco.xml` (VINTF), `virtualsco.rc` (init), `sepolicy/`.
- A key constraint: `android.hardware.audio.core` AIDL stubs are **not in the NDK**, so the module
  can only be compiled inside an **AOSP/Soong tree**. We can't cross-compile it with the NDK the way
  the legacy `.so` is built.
- To avoid needing a local Linux box, we first tried building on a **GitHub Actions free runner**
  with a **curated, partial AOSP sync** (only the projects we thought the target needed), keeping
  disk under the runner's limit.

---

## 3. Issues we hit, and how we debugged them

The partial-tree approach turned into a long chain of build-environment problems. Each was diagnosed
from the failing step's log and fixed by adding a project to sync, pruning a module, or changing the
build mode.

| # | Symptom | Root cause | Fix |
|---|---|---|---|
| 1 | `repo sync <list>` aborted on the first unknown project | `repo sync` fails hard on any name not in the manifest | Validate names via `repo manifest`, then batch-sync only valid ones |
| 2 | Filter skipped **everything** | `repo list` is empty before the first sync | Switched validation to `repo manifest` (works right after `repo init`) |
| 3 | `actions/cache` never restored | A synced AOSP subset (multi-GB `prebuilts/clang`) exceeds GitHub's 10 GB cache limit | Dropped caching; kept the sync fast with a single batch |
| 4 | `board_config.mk: VNDK version 30 not found` | Product config needs the versioned VNDK snapshots | Added `prebuilts/vndk/v30…v34` |
| 5 | `rbcrun: external/starlark-go … no such file` | Config runner needs the Starlark Go project | Added `external/starlark-go` |
| 6 | `afdo_profiles.mk: No such file` | Needs the PGO/AFDO profiles | Added `toolchain/pgo-profiles` |
| 7 | `bootstrap blueprint: undefined module "kernel-config-soong-rules" / "spdx-tools-*"` | Soong-plugin bootstrap needs those plugin projects (not covered by `SOONG_ALLOW_MISSING_DEPENDENCIES`) | Added `kernel/configs`, `external/spdx-tools` |
| 8 | `unrecognized module type "hidl_interface" / "xsd_config"` | The plugins defining those module types weren't synced | Added `system/tools/hidl`, `system/tools/xsdc` |
| 9 | `soong_filesystem_creator … testkey_rsa2048.pem does not exist` | The product **filesystem generator** needs an AVB key | Added `external/avb` |
| 10 | `panic in GenerateBuildActions … compatibility_matrix` | VINTF matrix generator nil-panics on a partial tree | Pruned `hardware/interfaces/compatibility_matrices`; tried `banchan` (worse — it analyzes module-SDK exports that also panic) and reverted to `lunch`; dropped speculative `prebuilts/runtime` |
| 11 | **Dozens** of `panic … "-java" … android_common` across `frameworks/native`, `hardware/interfaces`, test libs | **The wall:** Soong analyzes **every parsed module**, not just our target's deps; the Java/platform pieces are incomplete in a partial tree, so many modules nil-panic | Not fixable by pruning (too many, across required projects) → pivot |
| 12 | Full-manifest sync **failed on storage** | Full AOSP (~110–140 GB) + `out/` exceeds the free runner's ~118 GB disk | Move to a bigger disk (WSL / self-hosted / larger runner) |

**The core lesson:** Soong generates the *whole* build graph before building a single target, so it
must be able to **analyze every module in the synced tree**. A partial tree therefore produces an
endless series of analysis errors and panics from unrelated product/Java modules. The clean solution
is a **complete tree**, which just needs enough disk.

Useful levers we built along the way (kept in the workflow history): `SOONG_ALLOW_MISSING_DEPENDENCIES`
(tolerates missing *module* deps, but **not** missing source files, unrecognized types, or panics),
a manifest-validated batch sync, and a `PRUNE` list for product modules that break analysis but we
don't build.

---

## 4. The way forward

Build against the **full manifest** on a machine with enough disk — then it's just a normal AOSP
build of one module, with none of the partial-tree pain.

Chosen path: **WSL2 on Windows with ~250 GB allocated.**

- Full AOSP (~150–200 GB with `out/`) fits comfortably.
- The tree lives on the WSL **ext4** filesystem (under `$HOME`), **never** `/mnt/c` (case-insensitive
  + slow, which breaks AOSP).
- The tree **persists** between runs, so after the first build, rebuilds are incremental (minutes).

Alternatives if WSL isn't available: a **self-hosted Linux runner** (free, persistent) or a **GitHub
larger runner** (paid, zero setup). All produce the same binary.

---

## 5. Purpose of the build script (`build_wsl.sh`)

A one-shot, idempotent build for WSL/Ubuntu (or any Linux x86-64). It:

1. **Sanity-checks** the environment (Linux; refuses to build on a `/mnt/<drive>` Windows path).
2. **Installs** the AOSP build dependencies and the `repo` tool.
3. **`repo init` + full sync** of the manifest (default `android-15.0.0_r36`), with **HTTP-429
   handling** — modest concurrency (`-j4`), `--retry-fetches`, and an 8-attempt resume loop, since
   `repo sync` is resumable.
4. **Stages** the `virtualsco/` module into the tree at `vendor/aicaller/virtualsco`.
5. **Builds** just the target: `lunch` + `m android.hardware.audio.service-aidl.virtualsco`.
6. **Collects artifacts** into `virtualsco/artifacts/`: the service binary, `virtualsco.rc`,
   `virtualsco.xml`, and a `build-info.txt` (branch, arch, size).

Everything is env-overridable (`AOSP_BRANCH`, `LUNCH`, `JOBS`, `SYNC_JOBS`, `AOSP_ROOT`). Re-running
does an incremental sync + incremental build.

```bash
# In WSL:
git clone https://github.com/NaumanSh1011/Test.git
cd Test/virtualsco
./build_wsl.sh
# first run ~1–2 h; artifacts land in virtualsco/artifacts/
```

---

## 6. Final outcome

The build produces a small **vendor binary**:

- `android.hardware.audio.service-aidl.virtualsco` (~1–4 MB), plus `virtualsco.rc` and
  `virtualsco.xml`.
- It is deployed via the **Magisk audiopolicy module** (to `/vendor/bin/hw/`), **not** bundled in the
  APK — so it adds **0 bytes** to the app. (The app's only SCO payload is the ~260 KB JNI socket
  client, shared by the HIDL and AIDL paths.)

Next steps once the binary exists:

1. **Enumeration spike** — flash it and confirm `IModule/virtual` and its BT-SCO device appear in
   `dumpsys media.audio_policy`, and that the app's route pinner can pin voice-communication to it.
   This is the make-or-break test of the whole AIDL approach on a real vendor stack (Samsung/Exynos).
2. **Real streams** — if the spike passes, replace the stub streams with **socket-backed** streams
   reusing the same bridge core (`va_server`/`va_ring`) as the legacy `.so`, giving two-way call
   audio over the virtual SCO device.

End state: AICaller can bridge call audio on **both** HAL worlds — the legacy `.so` for MediaTek/HIDL
devices and this AIDL `IModule/virtual` for Exynos/newer-Unisoc/Android-15+ devices — from one
codebase, selected per device at flash time.
