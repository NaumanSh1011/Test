#!/usr/bin/env bash
#
# Build the virtual SCO AIDL audio module inside an AOSP (or BSP) tree and collect the artifacts.
#
# Because the module only depends on stable, upstream AIDL (android.hardware.audio.core-V*-ndk) and
# AOSP's default audio HAL impl (libaudioserviceexampleimpl), a PLAIN AOSP checkout at the right
# Android version builds an artifact usable across AIDL devices at that interface version — you do
# NOT need each device's vendor BSP.
#
# Usage (on the Linux AOSP host):
#     AOSP_ROOT=/path/to/aosp ./build_in_aosp.sh [OUTDIR]
# Optional overrides:
#     LUNCH=aosp_arm64-trunk_staging-userdebug   # generic arm64; or your device's target
#     DEST_REL=vendor/aicaller/virtualsco        # where to stage the module inside the tree
#
# Output: OUTDIR (default ./artifacts next to this script) gets the service binary, the .rc and
# VINTF .xml, and a build-info.txt recording the detected android.hardware.audio.core version + arch
# — that version is what customize.sh templates into the deployed VINTF fragment.
#
# NOTE: this builds only the service BINARY. SELinux (sepolicy/) and the rc/vintf files are deployed
# per-device by magisk/audiopolicy/customize.sh at flash time, not baked into an AOSP image, so this
# script does not touch BOARD_VENDOR_SEPOLICY_DIRS.

set -uo pipefail

MODULE_SRC="$(cd "$(dirname "$0")" && pwd)"
OUT="${1:-$MODULE_SRC/artifacts}"
LUNCH="${LUNCH:-aosp_arm64-trunk_staging-userdebug}"
DEST_REL="${DEST_REL:-vendor/aicaller/virtualsco}"
TARGET="android.hardware.audio.service-aidl.virtualsco"

if [ -z "${AOSP_ROOT:-}" ] || [ ! -d "$AOSP_ROOT/build/soong" ]; then
    echo "ERROR: set AOSP_ROOT to a synced AOSP/BSP checkout (must contain build/soong)." >&2
    exit 2
fi

DEST="$AOSP_ROOT/$DEST_REL"
echo "== staging module -> $DEST =="
mkdir -p "$DEST/sepolicy"
cp -f "$MODULE_SRC"/Android.bp "$MODULE_SRC"/VirtualScoConfiguration.cpp \
      "$MODULE_SRC"/VirtualScoConfiguration.h "$MODULE_SRC"/main_virtual.cpp \
      "$MODULE_SRC"/virtualsco.rc "$MODULE_SRC"/virtualsco.xml "$DEST"/
cp -f "$MODULE_SRC"/sepolicy/* "$DEST/sepolicy"/

CORE_DIR="$AOSP_ROOT/hardware/interfaces/audio/aidl/aidl_api/android.hardware.audio.core"
CORE_VERS="$(ls "$CORE_DIR" 2>/dev/null | grep -E '^[0-9]+$' | sort -n | tr '\n' ' ')"
echo "== android.hardware.audio.core frozen versions in tree: ${CORE_VERS:-<not found>} =="

echo "== building $TARGET (lunch=$LUNCH) =="
cd "$AOSP_ROOT"
set +u
source build/envsetup.sh
lunch "$LUNCH" || { echo "lunch '$LUNCH' failed — pass a valid LUNCH= target" >&2; exit 3; }
set -u
if ! m "$TARGET"; then
    echo "ERROR: build of $TARGET failed (see Soong output above)." >&2
    exit 4
fi

echo "== collecting artifacts -> $OUT =="
mkdir -p "$OUT"
BIN="$(find "$AOSP_ROOT/out" -type f -name "$TARGET" -path "*/vendor/bin/hw/*" -print 2>/dev/null | head -1)"
if [ -z "$BIN" ]; then
    echo "ERROR: built binary not found under out/*/vendor/bin/hw/$TARGET" >&2
    exit 5
fi
cp -f "$BIN" "$OUT"/
cp -f "$MODULE_SRC"/virtualsco.rc "$MODULE_SRC"/virtualsco.xml "$OUT"/

{
    echo "target: $TARGET"
    echo "lunch: $LUNCH"
    echo "audio.core frozen versions: ${CORE_VERS:-unknown}"
    echo "binary: $BIN"
    echo -n "arch: "; readelf -h "$BIN" 2>/dev/null | sed -n 's/.*Machine: *//p'
    echo "built: $(date -u +%FT%TZ)"
} > "$OUT/build-info.txt"

echo "== done =="
cat "$OUT/build-info.txt"
echo
echo "Copy $OUT back to the AICaller host (scp/adb) and hand it to the deploy step."
