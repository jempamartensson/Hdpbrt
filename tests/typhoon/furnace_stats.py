# White-furnace check for the materials_extra/furnace scenes: a unit sphere in
# a uniform white dome, so lossless glass should vanish (radiance 1).
#
#   python3 furnace_stats.py OUT_DIR [name ...]     (OUT as for run_materials.sh)
#
# For each <name>.pfm in OUT/ref (Typhoon) and OUT/hdpbrt prints the mean over
# the ball and a radial profile (centre to rim, 10 rings).
import sys, glob, os
import numpy as np
from compare import read_pfm


def stats(path):
    a = read_pfm(path).mean(2)
    h = a.shape[0]
    yy, xx = np.mgrid[:h, :h]
    r = np.hypot(yy - h / 2 + .5, xx - h / 2 + .5) / (0.42 * h)  # ~1 at the rim
    rings = [a[(r >= b) & (r < b + 0.1)].mean() for b in np.arange(0, 1, 0.1)]
    return a[r < 0.83].mean(), rings


def main():
    out = sys.argv[1]
    names = sys.argv[2:] or sorted(os.path.basename(p)[:-4]
                                   for p in glob.glob(os.path.join(out, 'ref', '*.pfm')))
    for n in names:
        for who, label in (('ref', 'typhoon'), ('hdpbrt', 'hdpbrt')):
            p = os.path.join(out, who, n + '.pfm')
            if not os.path.exists(p):
                print('%-24s %-7s missing' % (n, label))
                continue
            m, rings = stats(p)
            print('%-24s %-7s ball %.3f   rings %s' % (n, label, m, ' '.join('%.3f' % v for v in rings)))


if __name__ == '__main__':
    main()
