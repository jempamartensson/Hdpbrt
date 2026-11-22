# Compare hdPbrt material renders with Typhoon references, image by image.
#
#   python3 compare_images.py REF_DIR OUT_DIR
#
# Both directories hold <name>.pfm. Reports the ratio of mean pixel values
# and the RMS error relative to the reference's mean, after 4x4 downsampling
# (which averages out sampling noise but keeps wrong lobes, colors and
# textures). Rough guide: < 0.1 matches, 0.1-0.25 close, > 0.25 differs.
import sys, glob, os
import numpy as np
from compare import read_pfm


def down(a, k=4):
    h, w, c = a.shape
    return a[:h // k * k, :w // k * k].reshape(h // k, k, w // k, k, c).mean((1, 3))


def main():
    ref_dir, out_dir = sys.argv[1], sys.argv[2]
    counts = {'match': 0, 'close': 0, 'differs': 0, 'missing': 0}
    for ref in sorted(glob.glob(os.path.join(ref_dir, '*.pfm'))):
        name = os.path.basename(ref)[:-4]
        out = os.path.join(out_dir, name + '.pfm')
        if not os.path.exists(out):
            counts['missing'] += 1
            print('%-62s missing' % name)
            continue
        a = down(read_pfm(ref)[..., :3])
        b = down(read_pfm(out)[..., :3])
        rel = np.sqrt(((a - b) ** 2).mean()) / max(a.mean(), 1e-6)
        verdict = 'match' if rel < 0.1 else 'close' if rel < 0.25 else 'differs'
        counts[verdict] += 1
        print('%-62s mean ratio %6.3f   rel RMSE %6.3f   %s' %
              (name, b.mean() / max(a.mean(), 1e-9), rel, verdict))
    print('  '.join('%s %d' % kv for kv in counts.items()))


if __name__ == '__main__':
    main()
