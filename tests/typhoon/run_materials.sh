#!/bin/bash
# Render the material fixtures of Typhoon's test suite with Typhoon (as clean
# references: firefly/caustics clamps off, caustics on, 256 spp) and with hdPbrt,
# then compare.
#
#   SUITE=<typhoon-test-suite> TYPHOON=<openusd-typhoon checkout, built> OUT=<work dir> \
#   tests/typhoon/run_materials.sh [group ...]     (default: pbr open_pbr standard_surface)
#
# The shaderball scenes need the suite's materials/_assets/wg-assets submodule
# (only full_assets/StandardShaderBall is used). PIXI (pixi binary), IMGTOOL and
# HYTHON as in run_usdlux.sh. Typhoon's renders are cached in $OUT/typhoon.
set -euo pipefail
here=$(cd "$(dirname "$0")" && pwd)
: "${SUITE:?}" "${TYPHOON:?}" "${OUT:?}"
PIXI=${PIXI:-pixi}
HYTHON=${HYTHON:-"rez env houdini-22 hdpbrt -- hython"}
IMGTOOL=${IMGTOOL:-imgtool}
SPP=${SPP:-256}
groups=${*:-pbr open_pbr standard_surface}
mkdir -p "$OUT/typhoon" "$OUT/ref" "$OUT/hdpbrt" "$OUT/tmp"
for g in $groups; do
  for usd in "$SUITE"/materials/$g/*.usda; do
    name=$g--$(basename "${usd%.usda}")
    if [ ! -f "$OUT/ref/$name.pfm" ]; then
      ( cd "$TYPHOON" && "$PIXI" run usdrender "$usd" -r Embree --outputRoot "$OUT/typhoon/$name" \
          -s '{settings}.ty:fireflyClampThreshold = 0.0' -s '{settings}.ty:causticsClampThreshold = 0.0' \
          -s '{settings}.ty:enableCaustics = 1' -s "{settings}.ty:convergedSamplesPerPixel = $SPP" \
          > "$OUT/typhoon/$name.log" 2>&1 ) || echo "typhoon failed: $name"
      exr=$(find "$OUT/typhoon/$name" -name '*.exr' | head -1)
      [ -n "$exr" ] && "$IMGTOOL" convert --channels R,G,B "$exr" --outfile "$OUT/ref/$name.pfm"
    fi
    stem=$(basename "${usd%.usda}")
    HDPBRT_KEEP_TMP=1 HDPBRT_TMPDIR="$OUT/tmp" $HYTHON "$here/render_hdpbrt.py" "$usd" 1:1 \
      "$OUT/hdpbrt/$g" pbrt:maxDepth=16 pbrt:pixelSamples=$SPP > "$OUT/hdpbrt/$name.log" 2>&1 \
      && mv "$OUT/hdpbrt/$g/$stem-hdpbrt.0001.pfm" "$OUT/hdpbrt/$name.pfm" || echo "hdpbrt failed: $name"
  done
done
python3 "$here/compare_images.py" "$OUT/ref" "$OUT/hdpbrt"
