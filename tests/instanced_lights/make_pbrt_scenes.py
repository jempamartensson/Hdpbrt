# python3 tests/instanced_lights/make_pbrt_scenes.py <outdir>
# pbrt-only scenes for instanced and moving emitters (pbrt patch 47), in pairs
# that must render alike (run_instanced_lights.sh compares them):
#   plain / inst          an emissive quad as a shape / in ObjectBegin + one instance
#   plain2 / inst2        two instances (rotation, non-uniform scale) / the same
#                         quads pre-transformed
#   moving2 / s00..s15    an instance under ActiveTransform / the average of 16
#                         static renders at evenly spaced times
#   movingN / n00..n15    the same with hdPbrt's N-key "hdpbrtmotionxforms"
#   pvplain / pvinst      a primvar-textured emitter (patch 46) / instanced
# A floor and a glossy sphere catch the light (light sampling and MIS).
import os, struct, sys
import numpy as np

out = os.path.abspath(sys.argv[1])
os.makedirs(out, exist_ok=True)


def head(film, spp=256):
    return ('LookAt 0 3 4  0 0 0  0 1 0\n'
            'Camera "perspective" "float fov" 50 "float shutteropen" 0 "float shutterclose" 1\n'
            'Sampler "zsobol" "integer pixelsamples" %d\n'
            'Integrator "volpath" "integer maxdepth" 4\n'
            'Film "rgb" "integer xresolution" 160 "integer yresolution" 120 "string filename" "%s.pfm"\n'
            'WorldBegin\n' % (spp, film))


FLOOR = ('AttributeBegin\nMaterial "diffuse" "rgb reflectance" [0.5 0.5 0.5]\n'
         'Shape "bilinearmesh" "point3 P" [-3 0 -3 3 0 -3 -3 0 3 3 0 3] "integer indices" [0 1 2 3]\n'
         'AttributeEnd\nAttributeBegin\n'
         'Material "coateddiffuse" "rgb reflectance" [0.3 0.6 0.3] "float roughness" 0.1\n'
         'Shape "sphere" "float radius" 0.4\nAttributeEnd\n')
QUAD = np.array([[-0.3, 1.2, -0.3], [0.3, 1.2, -0.3], [0.3, 1.2, 0.3], [-0.3, 1.2, 0.3]])
EMIT = 'AreaLightSource "diffuse" "rgb L" [6 6 6]\nMaterial "diffuse" "rgb reflectance" [0 0 0]\n'


def quad(p):
    return ('Shape "trianglemesh" "point3 P" [%s] "integer indices" [0 1 2 0 2 3]\n'
            % ' '.join('%.9g' % v for v in np.asarray(p).flatten()))


PROTO = 'ObjectBegin "lamp"\n' + EMIT + quad(QUAD) + 'ObjectEnd\n'


def write(name, body, spp=256):
    open(os.path.join(out, name + '.pbrt'), 'w').write(head(name, spp) + FLOOR + body)


def mat(m):  # pbrt Transform takes the matrix column by column
    return '[%s]' % ' '.join('%.9g' % v for v in m.T.flatten())


def T(t):
    m = np.eye(4); m[:3, 3] = t; return m


def S(s):
    return np.diag([s[0], s[1], s[2], 1.0])


def R(axis, deg):
    a = np.radians(deg); c, s = np.cos(a), np.sin(a); m = np.eye(4)
    i, j = {'x': (1, 2), 'z': (0, 1)}[axis]
    m[i, i] = c; m[i, j] = -s; m[j, i] = s; m[j, j] = c
    return m


write('plain', 'AttributeBegin\n' + EMIT + quad(QUAD) + 'AttributeEnd\n')
write('inst', PROTO + 'AttributeBegin\nObjectInstance "lamp"\nAttributeEnd\n')

M = [T([-1.2, 0.2, 0]) @ S([2.0, 1.0, 0.5]),
     T([1.0, 1.4, 0.3]) @ R('x', 25) @ R('z', -15) @ S([0.7, 1.0, 1.6]) @ T([0, -1.2, 0])]
body = ''
for m in M:
    p = (m @ np.c_[QUAD, np.ones(4)].T).T[:, :3]
    body += 'AttributeBegin\n' + EMIT + quad(p) + 'AttributeEnd\n'
write('plain2', body)
write('inst2', PROTO + ''.join('AttributeBegin\nTransform %s\nObjectInstance "lamp"\nAttributeEnd\n'
                               % mat(m) for m in M))


def tx(x):
    return mat(T([x, 0, 0]))


write('moving2', PROTO + 'AttributeBegin\nActiveTransform StartTime Transform %s '
      'ActiveTransform EndTime Transform %s ActiveTransform All\nObjectInstance "lamp"\n'
      'AttributeEnd\n' % (tx(-1), tx(1)), 512)
keys = [-1, 0.6, 1]  # at times 0, 0.5, 1
write('movingN', PROTO + 'AttributeBegin\nAttribute "shape" "float hdpbrtmotionxforms" [%s] '
      '"float hdpbrtmotiontimes" [0 0.5 1]\nObjectInstance "lamp"\nAttributeEnd\n'
      % ' '.join(tx(x)[1:-1] for x in keys), 512)
K = 16
for k in range(K):
    t = (k + 0.5) / K
    xs = -1 + 2 * t
    xn = -1 + 1.6 * t / 0.5 if t < 0.5 else 0.6 + 0.4 * (t - 0.5) / 0.5
    write('s%02d' % k, PROTO + 'AttributeBegin\nTransform %s\nObjectInstance "lamp"\nAttributeEnd\n'
          % tx(xs), 32)
    write('n%02d' % k, PROTO + 'AttributeBegin\nTransform %s\nObjectInstance "lamp"\nAttributeEnd\n'
          % tx(xn), 32)

# Primvar-textured emitter: a downward quad with a red -> blue vertex primvar.
open(os.path.join(out, 'lamp.ply'), 'w').write(
    'ply\nformat ascii 1.0\nelement vertex 4\nproperty float x\nproperty float y\nproperty float z\n'
    'element face 2\nproperty list uchar int vertex_indices\nend_header\n'
    '-0.5 1 -0.5\n0.5 1 -0.5\n0.5 1 0.5\n-0.5 1 0.5\n3 0 1 2\n3 0 2 3\n')
open(os.path.join(out, 'grad.bin'), 'wb').write(struct.pack('12f', 2, 0, 0, 2, 0, 0, 0, 0, 2, 0, 0, 2))
TEX = 'Texture "em" "spectrum" "hdpbrtprimvar" "string primvar" "Le" "float default" [0 0 0]\n'
LAMP = ('AreaLightSource "diffuse" "rgb L" [1 1 1] "string hdpbrtltexture" "em"\n'
        'Material "diffuse" "rgb reflectance" [0 0 0]\n'
        'Shape "plymesh" "string filename" "lamp.ply" "string hdpbrtprimvars" ["Le" "3" "grad.bin"]\n')
write('pvplain', TEX + 'AttributeBegin\nTranslate 0.5 0.3 0\n' + LAMP + 'AttributeEnd\n')
write('pvinst', TEX + 'ObjectBegin "plamp"\n' + LAMP + 'ObjectEnd\n'
      'AttributeBegin\nTranslate 0.5 0.3 0\nObjectInstance "plamp"\nAttributeEnd\n')
print('wrote', out)
