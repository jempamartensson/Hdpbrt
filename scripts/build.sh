#!/bin/bash
# Build pbrt-v4 (with the hdPbrt patches) and the hdPbrt delegate.
#
#   scripts/build.sh                 # Houdini flavor only
#   BLENDER=1 scripts/build.sh       # also the Blender 5.3 flavor (see docs/BUILD_NOTES.md)
#
# Variables: WORK (default ./_work: pbrt source + build dirs), PREFIX (default ./install),
# HFS (default /opt/hfs22.0), J (parallel jobs, default 8).
# For the Blender flavor: BLENDER_LIB, USD_SOURCE (OpenUSD v26.08 source tree), TBB_INCLUDE
# (oneTBB 2022.3 include dir), PYTHON_INCLUDE (Python 3.13 headers, e.g. Houdini's).
#
# On a memory-tight machine run it inside a cap, e.g.
#   systemd-run --user --scope -p MemoryMax=64G -p MemorySwapMax=0 scripts/build.sh
set -euo pipefail
ROOT=$(cd "$(dirname "$0")/.." && pwd)
WORK=${WORK:-$ROOT/_work}
PREFIX=${PREFIX:-$ROOT/install}
HFS=${HFS:-/opt/hfs22.0}
J=${J:-8}
PBRT_COMMIT=20e41fd
mkdir -p "$WORK"

# 1. pbrt-v4 at a known commit + patches (parent-PID watchdog, depth, id and alpha channels, deformation and N-key motion blur)
if [ ! -d "$WORK/pbrt-v4/.git" ]; then
  git clone --recursive https://github.com/mmp/pbrt-v4.git "$WORK/pbrt-v4"
  git -C "$WORK/pbrt-v4" checkout -b hdpbrt $PBRT_COMMIT
  git -C "$WORK/pbrt-v4" submodule update --init --recursive
  git -C "$WORK/pbrt-v4" am "$ROOT"/patches/*.patch
fi
cmake -G Ninja -S "$WORK/pbrt-v4" -B "$WORK/build/pbrt" -DCMAKE_BUILD_TYPE=Release \
      -DCMAKE_POSITION_INDEPENDENT_CODE=ON -DPBRT_BUILD_NATIVE_EXECUTABLE=ON
ninja -C "$WORK/build/pbrt" -j$J pbrt_exe imgtool

# 2. hdPbrt for Houdini 22
cmake -G Ninja -S "$ROOT" -B "$WORK/build/hdpbrt-houdini" -DHDPBRT_FLAVOR=houdini \
      -DHFS="$HFS" -DCMAKE_INSTALL_PREFIX="$PREFIX/houdini"
ninja -C "$WORK/build/hdpbrt-houdini" -j$J install
flavors=houdini

# 3. hdPbrt for Blender 5.3 (optional)
if [ "${BLENDER:-0}" = 1 ]; then
  cmake -G Ninja -S "$ROOT" -B "$WORK/build/hdpbrt-blender" -DHDPBRT_FLAVOR=blender \
        -DBLENDER_LIB="${BLENDER_LIB:-/opt/blender-5.3.0-linux-x64/lib}" \
        -DUSD_SOURCE="$USD_SOURCE" -DUSD_GENERATED="$ROOT/third_party/usd-blender-generated" \
        -DTBB_INCLUDE="$TBB_INCLUDE" \
        -DPYTHON_INCLUDE="${PYTHON_INCLUDE:-$HFS/python/include/python3.13}" \
        -DCMAKE_INSTALL_PREFIX="$PREFIX/blender"
  ninja -C "$WORK/build/hdpbrt-blender" -j$J install
  flavors="houdini blender"
fi

# pbrt + imgtool next to each plugin (the delegate looks there first)
for f in $flavors; do
  ln -sfn "$WORK/build/pbrt/pbrt" "$PREFIX/$f/pbrt"
  ln -sfn "$WORK/build/pbrt/imgtool" "$PREFIX/$f/imgtool"
done
echo "built OK: $PREFIX"
