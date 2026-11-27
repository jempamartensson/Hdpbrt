#!/bin/bash
# True displacement (pbrt patch 39): hdPbrt vs Karma CPU (husk) on
# make_displacement_scene.py's scene (height maps), make_vector_scene.py's
# (vector displacement), make_procedural_scene.py's (MaterialX noise and
# nodegraph-implemented nodes, no images but one) and
# make_primvar_motion_scene.py's (primvars in graphs; a displaced deforming
# mesh with motion blur, frame 1).
#
#   OUT=<work dir> tests/displacement/run_displacement.sh
#
# HYTHON (default "rez env houdini-22 hdpbrt -- hython"), HUSK_ENV (default
# "rez env houdini-22 --"), IMGTOOL, SPP (default 64). Prints the silhouette
# difference (pixels of the upper image whose object/background classification
# differs; the background is Karma's black, hdPbrt's dome) and the mean of the
# displaced ground in front (ratio hdPbrt / Karma); for the vector scene the
# silhouette of the sphere and the mushrooms' caps; for the procedural scene
# both rows of spheres and the ground's horizon; for primvar_motion the
# primvar spheres' silhouette and the blur's coverage profile (fraction of the
# static sphere's color over the background, along three rows).
set -euo pipefail
here=$(cd "$(dirname "$0")" && pwd)
: "${OUT:?}"
HYTHON=${HYTHON:-"rez env houdini-22 hdpbrt -- hython"}
HUSK_ENV=${HUSK_ENV:-"rez env houdini-22 --"}
IMGTOOL=${IMGTOOL:-imgtool}
SPP=${SPP:-64}
mkdir -p "$OUT/karma" "$OUT/hdpbrt"
$HYTHON "$here/make_displacement_scene.py" "$OUT" "$IMGTOOL" > "$OUT/make.log" 2>&1
$HYTHON "$here/make_vector_scene.py" "$OUT" "$IMGTOOL" > "$OUT/make_vector.log" 2>&1
$HYTHON "$here/make_procedural_scene.py" "$OUT" "$IMGTOOL" > "$OUT/make_procedural.log" 2>&1
$HYTHON "$here/make_primvar_motion_scene.py" "$OUT" > "$OUT/make_primvar_motion.log" 2>&1
for scene in displacement vector procedural primvar_motion; do
  ( cd "$OUT" && $HUSK_ENV husk -R BRAY_HdKarma --frame 1 --pixel-samples "$SPP" -o karma/$scene.exr \
      $scene.usda > husk-$scene.log 2>&1 )
  "$IMGTOOL" convert --channels R,G,B "$OUT/karma/$scene.exr" --outfile "$OUT/karma/$scene.pfm"
  ( cd "$OUT" && $HYTHON "$here/../typhoon/render_hdpbrt.py" $scene.usda 1:1 "$OUT/hdpbrt" \
      "pbrt:pixelSamples=$SPP" > hdpbrt-$scene.log 2>&1 )
  python3 - "$OUT/karma/$scene.pfm" "$OUT/hdpbrt/$scene-hdpbrt.0001.pfm" $scene <<'PY'
import sys, numpy as np
def pfm(p):
    with open(p, 'rb') as f:
        f.readline(); w, h = map(int, f.readline().split()); f.readline()
        return np.frombuffer(f.read(), dtype='<f4').reshape(h, w, 3)[::-1]
k, h = pfm(sys.argv[1]), pfm(sys.argv[2])
vector = sys.argv[3] == 'vector'
rows = {'displacement': 200, 'vector': 215, 'procedural': 280,
        'primvar_motion': 190}[sys.argv[3]]
def objects(a):
    sky = a[5, 630]
    return np.abs(a - sky).sum(2) > 0.05 * max(sky.sum(), 1e-3)
top = slice(0, rows)
mk, mh = objects(k)[top], objects(h)[top]
print('%s silhouette: karma %d hdPbrt %d object pixels, %d differ (%.2f%%)'
      % (sys.argv[3], mk.sum(), mh.sum(), (mk ^ mh).sum(), 100.0 * (mk ^ mh).sum() / max(mk.sum(), 1)))
if sys.argv[3] == 'primvar_motion':
    def coverage(a, y):
        sky, obj = a[y, 630], a[225:245, 470:490].reshape(-1, 3).mean(0)
        return ((a[y] - sky) * (obj - sky)).sum(1) / ((obj - sky) ** 2).sum()
    xs = slice(150, 400)
    ck = np.concatenate([coverage(k, y)[xs] for y in (215, 235, 255)])
    ch = np.concatenate([coverage(h, y)[xs] for y in (215, 235, 255)])
    print('motion blur coverage: karma mean %.3f hdPbrt %.3f, correlation %.3f'
          % (ck.mean(), ch.mean(), np.corrcoef(ck, ch)[0, 1]))
if sys.argv[3] == 'displacement':
    g = (slice(380, 420), slice(100, 540))
    print('ground: karma', k[g].mean((0, 1)).round(4), 'hdPbrt', h[g].mean((0, 1)).round(4),
          'ratio', (h[g].mean((0, 1)) / k[g].mean((0, 1))).round(3))
PY
done
