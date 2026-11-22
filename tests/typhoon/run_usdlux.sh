#!/bin/bash
# Render Typhoon's usdlux regression suite with hdPbrt and compare it with
# Typhoon's reference images.
#
#   SUITE=<typhoon-test-suite> REFS=<dir with usdlux/reference/*.exr> \
#   OUT=<work dir> tests/typhoon/run_usdlux.sh
#
# Needs hython with hdPbrt (HYTHON, default: rez env houdini-22 hdpbrt --
# hython) and pbrt's imgtool (IMGTOOL) to convert the EXR references.
# Direct lighting only (pbrt:maxDepth=1), like the suite's ty:maxBounces = 0.
set -euo pipefail
here=$(cd "$(dirname "$0")" && pwd)
: "${SUITE:?typhoon-test-suite checkout}" "${REFS:?extracted usdlux reference archive}" "${OUT:?work dir}"
HYTHON=${HYTHON:-"rez env houdini-22 hdpbrt -- hython"}
IMGTOOL=${IMGTOOL:-imgtool}
SPP=${SPP:-64}
SCENES=${SCENES:-"distant:1:35 rect:1:63 sphere:1:63 disk:1:63 cylinder:1:63 dome:1:1 iesLibPreview:1:30 visibleRect:1:10"}
mkdir -p "$OUT/ref" "$OUT/hdpbrt" "$OUT/tmp"
for exr in "$REFS"/usdlux/reference/*.exr; do
  pfm="$OUT/ref/$(basename "${exr%.exr}").pfm"
  [ -f "$pfm" ] || "$IMGTOOL" convert --channels R,G,B "$exr" --outfile "$pfm"
done
names=()
for sc in $SCENES; do
  name=${sc%%:*}; frames=${sc#*:}; names+=("$name")
  HDPBRT_KEEP_TMP=1 HDPBRT_TMPDIR="$OUT/tmp" $HYTHON "$here/render_hdpbrt.py" \
    "$SUITE/usdlux/$name.usda" "$frames" "$OUT/hdpbrt" pbrt:maxDepth=1 pbrt:pixelSamples=$SPP \
    > "$OUT/$name.log" 2>&1 || { echo "$name: render failed, see $OUT/$name.log"; }
done
python3 "$here/compare.py" "$OUT/ref" "$OUT/hdpbrt" "${names[@]}"
