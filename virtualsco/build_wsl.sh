#!/usr/bin/env bash
#
# One-shot AOSP build of the virtual SCO AIDL module for WSL / Ubuntu (any Linux x86-64).
#
#   In WSL:  git clone https://github.com/NaumanSh1011/Test.git
#            cd Test/virtualsco
#            ./build_wsl.sh
#
# Full manifest (no curation) — with ~250 GB this Just Works like a normal AOSP build, avoiding all
# the partial-tree analysis pain. Idempotent: re-running does an incremental sync + incremental build.
#
# Requirements:
#   * Run on the LINUX filesystem (under $HOME, ext4) — NOT /mnt/c (case-insensitive + slow; breaks AOSP).
#   * ~250 GB free on that filesystem, 16 GB+ RAM (set it in C:\Users\<you>\.wslconfig, then `wsl --shutdown`).
#   * sudo (for apt) and internet.
#
# Override any of these via env, e.g.:  AOSP_BRANCH=android-16.0.0_r1 ./build_wsl.sh
set -uo pipefail

AOSP_ROOT="${AOSP_ROOT:-$HOME/aosp}"
AOSP_BRANCH="${AOSP_BRANCH:-android-15.0.0_r36}"
# Android 15 release config: RELEASE_AIDL_USE_UNFROZEN=false, so the unfrozen latest interfaces
# report the last frozen version (audio.core V2 / bluetooth.audio V4 — the fleet floor).
# trunk_staging would build the unfrozen V3/V5 instead, which A15 devices can't load.
LUNCH="${LUNCH:-aosp_arm64-ap3a-userdebug}"
JOBS="${JOBS:-$(nproc)}"
TARGET="android.hardware.audio.service-aidl.virtualsco"
DEST_REL="vendor/aicaller/virtualsco"
MODULE_SRC="$(cd "$(dirname "$0")" && pwd)"
ARTIFACTS="${ARTIFACTS:-$MODULE_SRC/artifacts}"

say(){ printf '\n\033[1m=== %s ===\033[0m\n' "$*"; }
die(){ echo "ERROR: $*" >&2; exit 1; }

# 0. Sanity checks.
[ "$(uname -s)" = "Linux" ] || die "Run on Linux/WSL, not $(uname -s)."
case "$AOSP_ROOT" in
  /mnt/[a-z]/*) die "AOSP_ROOT=$AOSP_ROOT is on a Windows drive (case-insensitive + slow). Use a path under \$HOME." ;;
esac
say "Config"
echo "  AOSP_ROOT=$AOSP_ROOT   BRANCH=$AOSP_BRANCH   LUNCH=$LUNCH   JOBS=$JOBS"
echo "  free on target fs: $(df -h "$(dirname "$AOSP_ROOT")" | awk 'NR==2{print $4}')"

# 1. Dependencies + repo.
say "Installing build dependencies (sudo apt)"
sudo apt-get update
sudo apt-get install -y --no-install-recommends \
  git-core gnupg flex bison build-essential zip curl zlib1g-dev libc6-dev-i386 \
  lib32z1-dev libgl1-mesa-dev libxml2-utils xsltproc unzip fontconfig python3 python-is-python3 rsync
if ! command -v repo >/dev/null 2>&1; then
  mkdir -p "$HOME/bin"
  curl -s https://storage.googleapis.com/git-repo-downloads/repo > "$HOME/bin/repo"
  chmod a+x "$HOME/bin/repo"
  export PATH="$HOME/bin:$PATH"
fi
git config --global user.name  >/dev/null 2>&1 || git config --global user.name  "ci"
git config --global user.email >/dev/null 2>&1 || git config --global user.email "ci@example.com"
git config --global color.ui false

# 2. Sync the full manifest (long the first time; incremental afterwards).
# googlesource rate-limits high concurrency (HTTP 429), so keep sync parallelism modest and retry —
# repo sync is resumable, so each retry continues where the last stopped.
SYNC_JOBS="${SYNC_JOBS:-4}"
say "repo init + full sync ($AOSP_BRANCH, -j$SYNC_JOBS) — first run downloads ~100+ GB, be patient"
mkdir -p "$AOSP_ROOT"; cd "$AOSP_ROOT"
repo init -u https://android.googlesource.com/platform/manifest -b "$AOSP_BRANCH" --partial-clone --depth=1 --no-tags
synced=0
for attempt in 1 2 3 4 5 6 7 8; do
  if repo sync -c --no-clone-bundle --optimized-fetch --force-sync --prune \
       --retry-fetches=3 -j"$SYNC_JOBS"; then
    synced=1; break
  fi
  echo ">>> sync attempt $attempt failed (likely HTTP 429 rate-limit); resuming in 60s…"
  sleep 60
done
[ "$synced" = 1 ] || die "repo sync still failing after retries. Lower it further: SYNC_JOBS=2 ./build_wsl.sh (it resumes)."
echo "sync done; tree size: $(du -sh "$AOSP_ROOT" 2>/dev/null | cut -f1)"

# 3. Stage the module into the tree (source only; scripts/artifacts excluded).
say "Staging module -> $AOSP_ROOT/$DEST_REL"
mkdir -p "$AOSP_ROOT/$DEST_REL"
rsync -a --delete \
  --exclude 'artifacts/' --exclude 'build_wsl.sh' --exclude 'build_in_aosp.sh' \
  "$MODULE_SRC"/ "$AOSP_ROOT/$DEST_REL"/

# 4. Build just our target.
say "lunch $LUNCH && m $TARGET"
cd "$AOSP_ROOT"
set +u; source build/envsetup.sh; set -u
lunch "$LUNCH" || die "lunch '$LUNCH' failed (try LUNCH=aosp_arm64-userdebug on a release tag)."
m "$TARGET" || die "build failed — see the Soong/ninja error above."

# 5. Collect artifacts.
say "Collecting artifacts -> $ARTIFACTS"
mkdir -p "$ARTIFACTS"
BIN="$(find "$AOSP_ROOT/out" -type f -name "$TARGET" -path '*/vendor/bin/hw/*' 2>/dev/null | head -1)"
[ -n "$BIN" ] || BIN="$(find "$AOSP_ROOT/out" -type f -name "$TARGET" 2>/dev/null | head -1)"
[ -n "$BIN" ] || die "built binary not found under out/."
cp -f "$BIN" "$ARTIFACTS"/
cp -f "$MODULE_SRC/virtualsco.rc" "$MODULE_SRC/virtualsco.xml" "$ARTIFACTS"/ 2>/dev/null || true
{
  echo "target: $TARGET"
  echo "branch: $AOSP_BRANCH"
  echo "binary: $BIN"
  echo -n "arch: ";  readelf -h "$BIN" 2>/dev/null | sed -n 's/.*Machine:  *//p'
  echo -n "size: ";  du -h "$BIN" | cut -f1
  echo "built: $(date -u +%FT%TZ)"
} | tee "$ARTIFACTS/build-info.txt"

say "DONE"
echo "Copy the artifacts to your Mac (they go into the Magisk module, not the APK):"
echo "  $ARTIFACTS/  ->  $TARGET, virtualsco.rc, virtualsco.xml, build-info.txt"
