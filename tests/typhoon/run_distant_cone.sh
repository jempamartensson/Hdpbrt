#!/bin/bash
# DistantLight cone vs Typhoon: renders tests/typhoon/distant_cone.usda (12 frames:
# angle sweep with normalize on, then normalize off) with Typhoon (cached in
# $OUT/ref) and hdPbrt, and compares image by image (compare_images.py).
#
#   TYPHOON=<openusd-typhoon checkout, built> OUT=<work dir> tests/typhoon/run_distant_cone.sh
#
# PIXI, IMGTOOL and HYTHON as in run_materials.sh. Direct lighting only
# (ty:maxBounces 0 = pbrt:maxDepth 1).
set -euo pipefail
here=$(cd "$(dirname "$0")" && pwd)
: "${TYPHOON:?}" "${OUT:?}"
PIXI=${PIXI:-pixi}
HYTHON=${HYTHON:-"rez env houdini-22 hdpbrt -- hython"}
IMGTOOL=${IMGTOOL:-imgtool}
SPP=${SPP:-256}
FRAMES=${FRAMES:-12}
scene="$here/distant_cone.usda"
mkdir -p "$OUT/typhoon" "$OUT/ref" "$OUT/hdpbrt" "$OUT/tmp"
for f in $(seq 1 "$FRAMES"); do
  name=distant_cone.$(printf %02d "$f")
  if [ ! -f "$OUT/ref/$name.pfm" ]; then
    ( cd "$TYPHOON" && "$PIXI" run usdrender "$scene" -r Embree -f "$f" --outputRoot "$OUT/typhoon/$f" \
        -s "{settings}.ty:convergedSamplesPerPixel = $SPP" > "$OUT/typhoon/$f.log" 2>&1 ) \
      || echo "typhoon failed: frame $f"
    exr=$(find "$OUT/typhoon/$f" -name '*.exr' | head -1)
    [ -n "$exr" ] && "$IMGTOOL" convert --channels R,G,B "$exr" --outfile "$OUT/ref/$name.pfm"
  fi
done
HDPBRT_KEEP_TMP=1 HDPBRT_TMPDIR="$OUT/tmp" $HYTHON "$here/render_hdpbrt.py" "$scene" "1:$FRAMES" \
  "$OUT/hdpbrt/frames" pbrt:maxDepth=1 pbrt:pixelSamples=$SPP > "$OUT/hdpbrt.log" 2>&1 \
  || echo "hdpbrt failed, see $OUT/hdpbrt.log"
for f in $(seq 1 "$FRAMES"); do
  src="$OUT/hdpbrt/frames/distant_cone-hdpbrt.$(printf %04d "$f").pfm"
  [ -f "$src" ] && cp "$src" "$OUT/hdpbrt/distant_cone.$(printf %02d "$f").pfm"
done
python3 "$here/compare_images.py" "$OUT/ref" "$OUT/hdpbrt"
