#!/bin/bash
# Instanced and moving emitters (pbrt patch 47).
#
#   OUT=<work dir> PBRT=<pbrt> tests/instanced_lights/run_instanced_lights.sh
#
# 1. pbrt only (make_pbrt_scenes.py): pairs that must agree (plain vs
#    instanced emitter, transformed instances vs pre-transformed meshes,
#    motion vs the average of static renders, primvar emission instanced).
# 2. Through hdPbrt (HYTHON, default "rez env houdini-22 hdpbrt -- hython"):
#    a PointInstancer of 100 emissive quads (plus a moving emitter) vs the
#    same quads as separate meshes; and the time of 100000 instances.
# Prints mean and region ratios (1 = equal, noise ~0.1-0.3%).
set -euo pipefail
here=$(cd "$(dirname "$0")" && pwd)
: "${OUT:?}"
PBRT=${PBRT:-pbrt}
HYTHON=${HYTHON:-"rez env houdini-22 hdpbrt -- hython"}
mkdir -p "$OUT/pbrt" "$OUT/hdpbrt"
python3 "$here/make_pbrt_scenes.py" "$OUT/pbrt" > /dev/null
( cd "$OUT/pbrt" && for f in *.pbrt; do "$PBRT" --quiet "$f"; done )
python3 - "$OUT/pbrt" <<'PY'
import glob, os, sys, numpy as np
d = sys.argv[1]
def rd(n):
    b = open(os.path.join(d, n + '.pfm'), 'rb').read(); p = b.split(b'\n', 3)
    w, h = map(int, p[1].split())
    return np.frombuffer(p[3], dtype='<f4').reshape(h, w, 3)[::-1].astype(float)
def regions(a, b, label):
    h, w = a.shape[:2]
    out = ['%-22s mean %.4f' % (label, b.mean() / a.mean())]
    for n, (ys, xs) in {'floor': (slice(int(h * .6), h), slice(0, w)),
                        'sphere': (slice(int(h * .5), int(h * .7)), slice(int(w * .4), int(w * .6)))}.items():
        out.append('%s %.4f' % (n, b[ys, xs].mean() / a[ys, xs].mean()))
    print('  '.join(out))
regions(rd('plain'), rd('inst'), 'instance / plain')
regions(rd('plain2'), rd('inst2'), 'transformed / plain')
regions(np.mean([rd('s%02d' % k) for k in range(16)], 0), rd('moving2'), 'ActiveTransform / avg')
regions(np.mean([rd('n%02d' % k) for k in range(16)], 0), rd('movingN'), 'N keys / avg')
regions(rd('pvplain'), rd('pvinst'), 'primvar instance')
PY
cd "$OUT"
for args in "100" "100 flat" "100000"; do
  $HYTHON "$here/make_instanced_lights_scene.py" "$OUT" $args > /dev/null
done
for f in instanced_lights_100 instanced_lights_100_flat instanced_lights_100000; do
  start=$(date +%s.%N)
  $HYTHON "$here/../typhoon/render_hdpbrt.py" $f.usda 1:1 "$OUT/hdpbrt" "pbrt:pixelSamples=128" > $f.log 2>&1
  echo "$f: $(echo "$(date +%s.%N) - $start" | bc) s"
done
python3 - "$OUT/hdpbrt" <<'PY'
import sys, numpy as np
def pfm(p):
    with open(p, 'rb') as f:
        f.readline(); w, h = map(int, f.readline().split()); f.readline()
        return np.frombuffer(f.read(), dtype='<f4').reshape(h, w, 3)[::-1].astype(float)
a = pfm(sys.argv[1] + '/instanced_lights_100_flat-hdpbrt.0001.pfm')
b = pfm(sys.argv[1] + '/instanced_lights_100-hdpbrt.0001.pfm')
k = 16
def down(x):
    hh, ww = x.shape[0] // k * k, x.shape[1] // k * k
    return x[:hh, :ww].reshape(hh // k, k, ww // k, k, 3).mean((1, 3))
print('hdPbrt PointInstancer / separate meshes: mean %.4f, 16x16-block rel RMSE %.4f'
      % (b.mean() / a.mean(), np.sqrt(((down(a) - down(b)) ** 2).mean()) / down(a).mean()))
PY
