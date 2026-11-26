#!/bin/bash
# hdPbrt's ACEScg working space vs Karma CPU (husk) under Houdini's ACES cg-config.
#
#   OUT=<work dir> tests/aces/run_aces.sh
#
# HYTHON (default "rez env houdini-22 hdpbrt -- hython"), HUSK_ENV (default
# "rez env houdini-22 --"), OCIO_CG (default $HFS/packages/ocio/cg-config-*.ocio).
# Prints Karma's and hdPbrt's mean centre color per scene (see CLAUDE.md,
# "Working color space ACEScg", for the expected agreement).
set -euo pipefail
here=$(cd "$(dirname "$0")" && pwd)
: "${OUT:?work dir}"
HYTHON=${HYTHON:-"rez env houdini-22 hdpbrt -- hython"}
HUSK_ENV=${HUSK_ENV:-"rez env houdini-22 --"}
OCIO_CG=${OCIO_CG:-$(ls /opt/hfs22.0.508/packages/ocio/cg-config-*.ocio | head -1)}
mkdir -p "$OUT/karma" "$OUT/hdpbrt"
$HYTHON "$here/make_aces_scenes.py" "$OUT" > /dev/null
cat > "$OUT/husk.sh" <<EOS
#!/bin/bash
cd "$OUT"
for s in albedo_*.usda light_*.usda; do
  [ -f karma/\${s%.usda}.exr ] || OCIO="$OCIO_CG" husk -R BRAY_HdKarma --res 100 100 -f 1 \
      -c /World/cam --pixel-samples 64 -o karma/\${s%.usda}.exr \$s > karma/\${s%.usda}.log 2>&1
done
EOS
chmod +x "$OUT/husk.sh"
$HUSK_ENV "$OUT/husk.sh"
for s in "$OUT"/albedo_*.usda "$OUT"/light_*.usda; do
  n=$(basename "${s%.usda}"); tmp="$OUT/hdpbrt/tmp_$n"; rm -rf "$tmp"; mkdir -p "$tmp"
  HDPBRT_KEEP_TMP=1 HDPBRT_TMPDIR="$tmp" $HYTHON "$here/../render_settings.py" "$tmp/out.png" "$s" 1 \
      pbrt:pixelSamples=256 > "$tmp.log" 2>&1
  cp "$(ls -t "$tmp"/hdpbrt-*/render_*.pfm | head -1)" "$OUT/hdpbrt/$n.pfm"
done
cd "$OUT"
printf '%-16s %-26s %s\n' scene "Karma (cg-config)" hdPbrt
for k in karma/*.exr; do
  n=$(basename "${k%.exr}")
  kv=$($HYTHON "$here/read_center.py" "$k" 2>/dev/null | tail -1 | cut -d' ' -f2-)
  hv=$(python3 "$here/read_center.py" "hdpbrt/$n.pfm" | cut -d' ' -f2-)
  printf '%-16s %-26s %s\n' "$n" "$kv" "$hv"
done
