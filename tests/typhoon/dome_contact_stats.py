# Analytic check for dome_contact.usda: a diffuse sphere (albedo 0.8, radius 1,
# centre (0, 1, 0)) on a 1000 x 1000 floor under a uniform dome of radiance 1,
# direct lighting only. The floor hides the sky below the horizon, so the
# sphere's radiance is 0.8 * (1 + n.y) / 2 (frame 1); without the floor 0.8
# (frame 2). Compares camera-facing sphere pixels, binned by n.y.
#
#   python3 dome_contact_stats.py <ref dir> <hdpbrt dir>     (run_frames.sh's OUT/ref, OUT/hdpbrt)
import sys
import numpy as np

def rpfm(p):
    f = open(p, 'rb'); f.readline(); w, h = map(int, f.readline().split()); s = float(f.readline())
    return np.frombuffer(f.read(), dtype='<f4' if s < 0 else '>f4').reshape(h, w, 3)[::-1].mean(2)

W, t = 200, 18 / 50  # resolution; tan(half fov) = (aperture / 2) / focal length
yy, xx = np.mgrid[0:W, 0:W]
d = np.stack([((xx + .5) / W * 2 - 1) * t, (1 - (yy + .5) / W * 2) * t, -np.ones((W, W))], -1)
d /= np.linalg.norm(d, axis=-1, keepdims=True)
o, c = np.array([0, 1, 6.]), np.array([0, 1, 0.])
b = (d * (o - c)).sum(-1); disc = b * b - ((o - c) @ (o - c) - 1)
n = o + d * (-b - np.sqrt(np.maximum(disc, 0)))[..., None] - c
ny = n[..., 1]
inner = (disc > 0) & (-(n * d).sum(-1) > 0.5)  # facing the camera, away from the silhouette
for frame in (1, 2):
    names = [f'{sys.argv[i]}/dome_contact.{frame:02d}.pfm' for i in (1, 2)]
    ty, hp = rpfm(names[0]), rpfm(names[1])
    pred = 0.8 * (1 + ny) / 2 if frame == 1 else np.full_like(ny, 0.8)
    print(f'frame {frame} ({"floor" if frame == 1 else "no floor"}): n.y bin, analytic, ref (ratio), hdPbrt (ratio)')
    for lo in np.arange(-0.8, 0.8, 0.2):
        m = inner & (ny >= lo) & (ny < lo + 0.2)
        a = pred[m].mean()
        print('  %5.1f..%4.1f  %.4f  %.4f (%.3f)  %.4f (%.3f)' %
              (lo, lo + .2, a, ty[m].mean(), ty[m].mean() / a, hp[m].mean(), hp[m].mean() / a))
