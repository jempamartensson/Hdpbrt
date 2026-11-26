#!/bin/bash
# True displacement (pbrt patch 39): hdPbrt vs Karma CPU (husk) on
# make_displacement_scene.py's scene.
#
#   OUT=<work dir> tests/displacement/run_displacement.sh
#
# HYTHON (default "rez env houdini-22 hdpbrt -- hython"), HUSK_ENV (default
# "rez env houdini-22 --"), IMGTOOL, SPP (default 64). Prints the silhouette
# difference (pixels of the upper image whose object/background classification
# differs; the background is Karma's black, hdPbrt's dome) and the mean of the
# displaced ground in front (ratio hdPbrt / Karma).
set -euo pipefail
here=$(cd "$(dirname "$0")" && pwd)
: "${OUT:?}"
HYTHON=${HYTHON:-"rez env houdini-22 hdpbrt -- hython"}
HUSK_ENV=${HUSK_ENV:-"rez env houdini-22 --"}
IMGTOOL=${IMGTOOL:-imgtool}
SPP=${SPP:-64}
mkdir -p "$OUT/karma" "$OUT/hdpbrt"
$HYTHON "$here/make_displacement_scene.py" "$OUT" "$IMGTOOL" > "$OUT/make.log" 2>&1
( cd "$OUT" && $HUSK_ENV husk -R BRAY_HdKarma --pixel-samples "$SPP" -o karma/karma.exr \
    displacement.usda > husk.log 2>&1 )
"$IMGTOOL" convert --channels R,G,B "$OUT/karma/karma.exr" --outfile "$OUT/karma/karma.pfm"
( cd "$OUT" && $HYTHON "$here/../typhoon/render_hdpbrt.py" displacement.usda 1:1 "$OUT/hdpbrt" \
    "pbrt:pixelSamples=$SPP" > hdpbrt.log 2>&1 )
python3 - "$OUT/karma/karma.pfm" "$OUT/hdpbrt/displacement-hdpbrt.0001.pfm" <<'PY'
import sys, numpy as np
def pfm(p):
    with open(p, 'rb') as f:
        f.readline(); w, h = map(int, f.readline().split()); f.readline()
        return np.frombuffer(f.read(), dtype='<f4').reshape(h, w, 3)[::-1]
k, h = pfm(sys.argv[1]), pfm(sys.argv[2])
def objects(a):
    sky = a[5, 5]
    return np.abs(a - sky).sum(2) > 0.05 * max(sky.sum(), 1e-3)
top = slice(0, 200)
mk, mh = objects(k)[top], objects(h)[top]
print('silhouette: karma %d hdPbrt %d object pixels, %d differ (%.2f%%)'
      % (mk.sum(), mh.sum(), (mk ^ mh).sum(), 100.0 * (mk ^ mh).sum() / max(mk.sum(), 1)))
g = (slice(380, 420), slice(100, 540))
print('ground: karma', k[g].mean((0, 1)).round(4), 'hdPbrt', h[g].mean((0, 1)).round(4),
      'ratio', (h[g].mean((0, 1)) / k[g].mean((0, 1))).round(3))
PY
