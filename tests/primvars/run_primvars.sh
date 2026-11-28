#!/bin/bash
# Varying primvars in surface shading: hdPbrt vs Karma CPU (husk) on
# make_primvar_scene.py's scene.
#
#   OUT=<work dir> tests/primvars/run_primvars.sh
#
# HYTHON (default "rez env houdini-22 hdpbrt -- hython"), HUSK_ENV (default
# "rez env houdini-22 --"), IMGTOOL, SPP (default 128). Prints, per object,
# the mean RGB of its box (hdPbrt / Karma) and the relative RMSE of the box
# downsampled 4x4 (noise); and the same for a few patches whose colors come
# from one primvar value (grid corners, checker faces, top/bottom of the
# roughness sphere).
set -euo pipefail
here=$(cd "$(dirname "$0")" && pwd)
: "${OUT:?}"
HYTHON=${HYTHON:-"rez env houdini-22 hdpbrt -- hython"}
HUSK_ENV=${HUSK_ENV:-"rez env houdini-22 --"}
IMGTOOL=${IMGTOOL:-imgtool}
SPP=${SPP:-128}
mkdir -p "$OUT/karma" "$OUT/hdpbrt"
$HYTHON "$here/make_primvar_scene.py" "$OUT" > "$OUT/make.log" 2>&1
( cd "$OUT" && $HUSK_ENV husk -R BRAY_HdKarma --frame 1 --pixel-samples "$SPP" -o karma/primvars.exr \
    primvars.usda > husk.log 2>&1 )
"$IMGTOOL" convert --channels R,G,B "$OUT/karma/primvars.exr" --outfile "$OUT/karma/primvars.pfm"
( cd "$OUT" && $HYTHON "$here/../typhoon/render_hdpbrt.py" primvars.usda 1:1 "$OUT/hdpbrt" \
    "pbrt:pixelSamples=$SPP" > hdpbrt.log 2>&1 )
python3 - "$OUT/karma/primvars.pfm" "$OUT/hdpbrt/primvars-hdpbrt.0001.pfm" \
    "$OUT/primvars_boxes.json" <<'PY'
import json, sys, numpy as np
def pfm(p):
    with open(p, 'rb') as f:
        f.readline(); w, h = map(int, f.readline().split()); f.readline()
        return np.frombuffer(f.read(), dtype='<f4').reshape(h, w, 3)[::-1]
k, h = pfm(sys.argv[1]), pfm(sys.argv[2])
boxes = json.load(open(sys.argv[3]))
def down(a):
    hh, ww = a.shape[0] // 4 * 4, a.shape[1] // 4 * 4
    return a[:hh, :ww].reshape(hh // 4, 4, ww // 4, 4, 3).mean((1, 3))
print('%-15s %-22s %-22s %-20s %s' % ('object', 'karma', 'hdPbrt', 'ratio', 'rel RMSE'))
for name, (x0, y0, x1, y1) in boxes.items():
    kb, hb = k[y0:y1, x0:x1], h[y0:y1, x0:x1]
    mk, mh = kb.mean((0, 1)), hb.mean((0, 1))
    dk, dh = down(kb), down(hb)
    rmse = np.sqrt(((dh - dk) ** 2).mean()) / max(dk.mean(), 1e-6)
    print('%-15s %-22s %-22s %-20s %.3f' % (name, mk.round(3), mh.round(3),
                                            (mh / np.maximum(mk, 1e-6)).round(3), rmse))
def patch(name, fx, fy, r=3):
    x0, y0, x1, y1 = boxes[name]
    x, y = int(x0 + fx * (x1 - x0)), int(y0 + fy * (y1 - y0))
    return (k[y - r:y + r + 1, x - r:x + r + 1].mean((0, 1)),
            h[y - r:y + r + 1, x - r:x + r + 1].mean((0, 1)))
print('patches:')
for name, fx, fy in (('unbound_grid', 0.12, 0.12), ('unbound_grid', 0.88, 0.88),
                     ('unbound_grid', 0.88, 0.12), ('mtlx_cd', 0.5, 0.5),
                     ('mtlx_cd', 0.25, 0.3), ('uniform_tint', 0.5, 0.5),
                     ('uniform_tint', 0.6, 0.5), ('rough_metal', 0.5, 0.2),
                     ('rough_metal', 0.5, 0.85), ('preview_fade', 0.15, 0.5),
                     ('preview_fade', 0.85, 0.5), ('implicit_const', 0.5, 0.5),
                     ('missing', 0.5, 0.5)):
    pk, ph = patch(name, fx, fy)
    print('  %-15s (%.2f, %.2f) karma %s hdPbrt %s' % (name, fx, fy, pk.round(3), ph.round(3)))
PY

# Curves, points and implicit prims (pbrt shape primvars).
$HYTHON "$here/make_curve_point_scene.py" "$OUT" > "$OUT/make_curves.log" 2>&1
( cd "$OUT" && $HUSK_ENV husk -R BRAY_HdKarma --frame 1 --pixel-samples "$SPP" \
    -o karma/curves_points.exr curves_points.usda > husk_curves.log 2>&1 )
"$IMGTOOL" convert --channels R,G,B "$OUT/karma/curves_points.exr" --outfile "$OUT/karma/curves_points.pfm"
( cd "$OUT" && $HYTHON "$here/../typhoon/render_hdpbrt.py" curves_points.usda 1:1 "$OUT/hdpbrt" \
    "pbrt:pixelSamples=$SPP" > hdpbrt_curves.log 2>&1 )
python3 - "$OUT/karma/curves_points.pfm" "$OUT/hdpbrt/curves_points-hdpbrt.0001.pfm" \
    "$OUT/curves_points_patches.json" <<'PY'
import json, sys, numpy as np
def pfm(p):
    with open(p, 'rb') as f:
        f.readline(); w, h = map(int, f.readline().split()); f.readline()
        return np.frombuffer(f.read(), dtype='<f4').reshape(h, w, 3)[::-1]
k, h = pfm(sys.argv[1]), pfm(sys.argv[2])
print('curves/points patches (value: the primvar there; ratio hdPbrt / Karma):')
for label, x, y, col in json.load(open(sys.argv[3])):
    pk = k[y - 2:y + 3, x - 2:x + 3].mean((0, 1)); ph = h[y - 2:y + 3, x - 2:x + 3].mean((0, 1))
    print('  %-20s value %-18s karma %-22s hdPbrt %-22s ratio %s'
          % (label, np.round(col, 2), pk.round(3), ph.round(3), (ph / np.maximum(pk, 1e-4)).round(2)))
PY
