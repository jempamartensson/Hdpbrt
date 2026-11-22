# Compare hdPbrt renders with Typhoon reference images, frame by frame.
#
#   python3 compare.py REF_DIR OUT_DIR SCENE [SCENE ...]
#
# REF_DIR holds <scene>-embree.NNNN.pfm (Typhoon references converted from
# EXR, e.g. with pbrt's `imgtool convert --channels R,G,B`), OUT_DIR holds
# <scene>-hdpbrt.NNNN.pfm (render_hdpbrt.py). A frame matches when the mean
# pixel values agree within 5%, or both images are black.
import sys, glob, os
import numpy as np


def read_pfm(path):
    with open(path, 'rb') as f:
        kind = f.readline().strip()
        w, h = map(int, f.readline().split())
        scale = float(f.readline())
        data = np.frombuffer(f.read(), dtype='<f4' if scale < 0 else '>f4')
    return data.reshape(h, w, 3 if kind == b'PF' else 1)[::-1].astype(np.float64)


def main():
    ref_dir, out_dir, scenes = sys.argv[1], sys.argv[2], sys.argv[3:]
    total = ok_total = 0
    for scene in scenes:
        ok, off = 0, []
        for ref in sorted(glob.glob(os.path.join(ref_dir, scene + '-embree.*.pfm'))):
            frame = ref.split('.')[-2]
            out = os.path.join(out_dir, '%s-hdpbrt.%s.pfm' % (scene, frame))
            if not os.path.exists(out):
                off.append('%s:missing' % int(frame))
                continue
            a = read_pfm(ref)[..., :3].mean()
            b = read_pfm(out)[..., :3].mean()
            if (a < 1e-5 and b < 1e-5) or (a > 0 and abs(b / a - 1) < 0.05):
                ok += 1
            else:
                off.append('%d:%.3f' % (int(frame), b / a if a > 0 else float('inf')))
        n = ok + len(off)
        total += n
        ok_total += ok
        print('%-14s %3d/%3d match   %s' % (scene, ok, n, ' '.join(off)))
    print('%-14s %3d/%3d match' % ('total', ok_total, total))


if __name__ == '__main__':
    main()
