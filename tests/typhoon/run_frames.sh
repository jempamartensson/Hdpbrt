#!/bin/bash
# Render the frames of one of these Typhoon comparison scenes with Typhoon
# (cached in $OUT/ref) and hdPbrt, and compare image by image (compare_images.py).
#
#   TYPHOON=<openusd-typhoon checkout, built> OUT=<work dir> \
#   tests/typhoon/run_frames.sh <scene.usda> <number of frames>
#
# PIXI, IMGTOOL and HYTHON as in run_materials.sh; SPP (default 256). Direct
# lighting only (the scenes set ty:maxBounces 0; hdPbrt gets pbrt:maxDepth 1).
# TY_ENV: extra environment for Typhoon's usdrender (e.g. VAR=value).
set -euo pipefail
here=$(cd "$(dirname "$0")" && pwd)
: "${TYPHOON:?}" "${OUT:?}"
scene=$(cd "$(dirname "$1")" && pwd)/$(basename "$1"); frames=$2
stem=$(basename "${scene%.usda}")
PIXI=${PIXI:-pixi}
HYTHON=${HYTHON:-"rez env houdini-22 hdpbrt -- hython"}
IMGTOOL=${IMGTOOL:-imgtool}
SPP=${SPP:-256}
mkdir -p "$OUT/typhoon" "$OUT/ref" "$OUT/hdpbrt" "$OUT/tmp"
for f in $(seq 1 "$frames"); do
  name=$stem.$(printf %02d "$f")
  if [ ! -f "$OUT/ref/$name.pfm" ]; then
    ( cd "$TYPHOON" && env ${TY_ENV:-} "$PIXI" run usdrender "$scene" -r Embree -f "$f" --outputRoot "$OUT/typhoon/$f" \
        -s "{settings}.ty:convergedSamplesPerPixel = $SPP" > "$OUT/typhoon/$f.log" 2>&1 ) \
      || echo "typhoon failed: frame $f"
    exr=$(find "$OUT/typhoon/$f" -name '*.exr' | head -1)
    [ -n "$exr" ] && "$IMGTOOL" convert --channels R,G,B "$exr" --outfile "$OUT/ref/$name.pfm"
  fi
done
HDPBRT_KEEP_TMP=1 HDPBRT_TMPDIR="$OUT/tmp" $HYTHON "$here/render_hdpbrt.py" "$scene" "1:$frames" \
  "$OUT/hdpbrt/frames" pbrt:maxDepth=1 pbrt:pixelSamples=$SPP > "$OUT/hdpbrt.log" 2>&1 \
  || echo "hdpbrt failed, see $OUT/hdpbrt.log"
for f in $(seq 1 "$frames"); do
  src="$OUT/hdpbrt/frames/$stem-hdpbrt.$(printf %04d "$f").pfm"
  [ -f "$src" ] && cp "$src" "$OUT/hdpbrt/$stem.$(printf %02d "$f").pfm"
done
python3 "$here/compare_images.py" "$OUT/ref" "$OUT/hdpbrt"
