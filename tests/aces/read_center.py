# python3 read_center.py image.(exr|pfm) ... : mean RGB of the central 40% of each image.
import sys
import numpy as np
def read(path):
    if path.endswith('.pfm'):
        with open(path, 'rb') as f:
            f.readline(); w, h = map(int, f.readline().split()); s = float(f.readline())
            return np.frombuffer(f.read(), '<f4' if s < 0 else '>f4').reshape(h, w, 3)
    import OpenImageIO as oiio
    i = oiio.ImageInput.open(path); px = i.read_image('float'); i.close()
    return px[..., :3]
for p in sys.argv[1:]:
    d = read(p); h, w = d.shape[:2]
    c = d[int(h * .3):int(h * .7), int(w * .3):int(w * .7)].reshape(-1, 3).mean(0)
    print('%s %.4f %.4f %.4f' % (p, *c))
