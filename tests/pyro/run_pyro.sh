#!/bin/bash
# Karma pyro shader: hdPbrt vs Karma CPU (husk) on make_pyro_scene.py's scene.
#
#   OUT=<work dir> tests/pyro/run_pyro.sh
#
# HYTHON (default "rez env houdini-22 hdpbrt -- hython"), HUSK_ENV (default
# "rez env houdini-22 --"), IMGTOOL, SPP (hdPbrt, default 256). Karma renders
# are cached in $OUT/karma; delete them to re-render. Prints per-frame channel
# means and ratios (hdPbrt / Karma).
set -euo pipefail
here=$(cd "$(dirname "$0")" && pwd)
: "${OUT:?}"
HYTHON=${HYTHON:-"rez env houdini-22 hdpbrt -- hython"}
HUSK_ENV=${HUSK_ENV:-"rez env houdini-22 --"}
IMGTOOL=${IMGTOOL:-imgtool}
SPP=${SPP:-256}
mkdir -p "$OUT/karma" "$OUT/hdpbrt"
[ -f "$OUT/pyro_scene.usda" ] || $HYTHON "$here/make_pyro_scene.py" "$OUT" > "$OUT/make.log" 2>&1
if [ ! -f "$OUT/karma/karma.0005.exr" ]; then
  cat > "$OUT/husk.sh" <<'EOF'
#!/bin/bash
cd "$(dirname "$0")"
exec husk -R BRAY_HdKarma --res 240 240 -f 1 -n 5 -c /World/camera --pixel-samples 64 \
  -o 'karma/karma.$F4.exr' pyro_scene.usda
EOF
  chmod +x "$OUT/husk.sh"
  $HUSK_ENV "$OUT/husk.sh" > "$OUT/husk.log" 2>&1
fi
for i in 1 2 3 4 5; do
  "$IMGTOOL" convert --channels R,G,B "$OUT/karma/karma.000$i.exr" --outfile "$OUT/karma/k$i.pfm"
done
$HYTHON "$here/../typhoon/render_hdpbrt.py" "$OUT/pyro_scene.usda" 1:5 "$OUT/hdpbrt" \
  pbrt:maxDepth=9 pbrt:pixelSamples=$SPP > "$OUT/hdpbrt.log" 2>&1
python3 - "$OUT" <<'EOF'
import sys
import numpy as np

def rpfm(p):
    f = open(p, 'rb'); f.readline(); w, h = map(int, f.readline().split()); s = float(f.readline())
    return np.frombuffer(f.read(), dtype='<f4' if s < 0 else '>f4').reshape(h, w, 3)

out = sys.argv[1]
names = ['smoke', 'fire, ramp', 'fire, blackbody', 'tinted smoke x3', 'smoke + fire']
for i in range(1, 6):
    k = rpfm(f'{out}/karma/k{i}.pfm').mean((0, 1))
    o = rpfm(f'{out}/hdpbrt/pyro_scene-hdpbrt.{i:04d}.pfm').mean((0, 1))
    print('%d %-16s karma %s  hdPbrt %s  ratio %s' % (i, names[i - 1], np.round(k, 4),
          np.round(o, 4), np.round(o / np.maximum(k, 1e-9), 3)))
EOF
