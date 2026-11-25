# Stats for run_distant_cone.sh: per frame, the sun's energy seen in the mirror
# sphere (sum over its screen disk: with direct lighting only, the mirror shows
# nothing but the sun) and the relative RMSE of the rest of the image (shadow
# penumbra, rough highlight) after 4x4 downsampling.
#
#   python3 cone_stats.py OUT
import sys, os, glob
import numpy as np
from compare import read_pfm

# Camera and mirror sphere of distant_cone.usda.
EYE = np.array([0, 4.5, 8.0])
AXES = np.array([[1, 0, 0], [0, 0.903278, -0.429057], [0, 0.429057, 0.903278]])  # x, y, z rows
TAN_HALF = 18.0 / 35.0
MIRROR, RADIUS = np.array([1.3, 0.6, 1.2]), 0.6


def project(p, w, h):
    c = AXES @ (p - EYE)
    x, y = c[0] / (-c[2] * TAN_HALF), c[1] / (-c[2] * TAN_HALF)
    return (x + 1) / 2 * w, (1 - y) / 2 * h, RADIUS / (-c[2] * TAN_HALF) / 2 * w


def down(a, k=4):
    h, w, c = a.shape
    return a[:h // k * k, :w // k * k].reshape(h // k, k, w // k, k, c).mean((1, 3))


def main():
    out = sys.argv[1]
    print('%-16s %10s %10s %7s   %s' % ('frame', 'sun ty', 'sun hd', 'ratio', 'rest: mean ratio / rel RMSE'))
    for ref in sorted(glob.glob(os.path.join(out, 'ref', '*.pfm'))):
        name = os.path.basename(ref)[:-4]
        hd = os.path.join(out, 'hdpbrt', name + '.pfm')
        if not os.path.exists(hd):
            continue
        a, b = read_pfm(ref)[..., :3], read_pfm(hd)[..., :3]
        h, w = a.shape[:2]
        cx, cy, r0 = project(MIRROR, w, h)
        r = int(np.ceil(r0)) + 2
        x0, x1, y0, y1 = int(cx) - r, int(cx) + r + 1, int(cy) - r, int(cy) + r + 1
        # The sun: pixels inside the sphere's disk (1 px in from its edge).
        yy, xx = np.mgrid[:h, :w]
        disk = np.hypot(xx + 0.5 - cx, yy + 0.5 - cy) < r0 - 1
        sa, sb = a[disk].sum(), b[disk].sum()
        mask = np.ones((h, w), bool)
        mask[y0:y1, x0:x1] = False
        ma, mb = a * mask[..., None], b * mask[..., None]
        da, db = down(ma), down(mb)
        rel = np.sqrt(((da - db) ** 2).mean()) / max(da.mean(), 1e-6)
        print('%-16s %10.2f %10.2f %7.3f   %.3f / %.3f' %
              (name, sa, sb, sb / max(sa, 1e-9), db.mean() / max(da.mean(), 1e-9), rel))


if __name__ == '__main__':
    main()
