# hython tests/displacement/make_procedural_scene.py <outdir> [imgtool]
# Procedural displacement test scene (pbrt patch 41): MaterialX graphs with no
# images, so the noise must match Karma's. A row of catmullClark spheres:
# noise3d, fractal3d, worleynoise3d, cellnoise3d (steps) of the object-space
# position, and a vector displacement from noise3d_vector3; a ground grid,
# moved off the origin, displaced by fractal3d of the world-space position.
# A second row exercises nodes compiled through their stdlib nodegraphs
# (unifiednoise3d, place2d + trianglewave, range, rotate3d + separate3) and an
# image inside a procedural graph (tiledimage x noise).
import math, os, struct, subprocess, sys
from pxr import Gf, Sdf, Usd, UsdGeom, UsdLux, UsdRender, UsdShade

out = os.path.abspath(sys.argv[1])
imgtool = sys.argv[2] if len(sys.argv) > 2 else 'imgtool'
os.makedirs(out, exist_ok=True)
# Stripes image for the tiledimage sphere.
N = 128
with open(os.path.join(out, 'stripes.pfm'), 'wb') as f:
    f.write(b'Pf\n%d %d\n-1\n' % (N, N))
    for y in range(N):
        f.write(struct.pack('<%df' % N, *[0.5 + 0.5 * math.cos(2 * math.pi * 4 * (x + 0.5) / N)
                                          for x in range(N)]))
subprocess.run([imgtool, 'convert', os.path.join(out, 'stripes.pfm'), '--outfile',
                os.path.join(out, 'stripes.exr')], check=True)
stage = Usd.Stage.CreateNew(os.path.join(out, 'procedural.usda'))
UsdGeom.SetStageUpAxis(stage, UsdGeom.Tokens.y)
UsdGeom.SetStageMetersPerUnit(stage, 1)
UsdGeom.Xform.Define(stage, '/World')
cam = UsdGeom.Camera.Define(stage, '/World/cam')
cam.CreateFocalLengthAttr(35); cam.CreateHorizontalApertureAttr(36); cam.CreateVerticalApertureAttr(24)
cx = UsdGeom.XformCommonAPI(cam)
cx.SetTranslate(Gf.Vec3d(0, 1.8, 6.5)); cx.SetRotate(Gf.Vec3f(-7, 0, 0))
dome = UsdLux.DomeLight.Define(stage, '/World/dome')
dome.CreateIntensityAttr(0.6); dome.CreateColorAttr(Gf.Vec3f(0.7, 0.8, 1.0))
sun = UsdLux.DistantLight.Define(stage, '/World/sun')
sun.CreateIntensityAttr(2.5); sun.CreateAngleAttr(1)
UsdGeom.XformCommonAPI(sun).SetRotate(Gf.Vec3f(-50, 35, 0))

F, V3, V2 = Sdf.ValueTypeNames.Float, Sdf.ValueTypeNames.Vector3f, Sdf.ValueTypeNames.Float2


def uv_sphere(path, center, radius, nu=24, nv=12):
    pts, counts, idx, st = [], [], [], []
    for j in range(nv + 1):
        th = math.pi * j / nv
        for i in range(nu):
            ph = 2 * math.pi * i / nu
            pts.append(Gf.Vec3f(radius * math.sin(th) * math.cos(ph),
                                radius * math.cos(th), -radius * math.sin(th) * math.sin(ph)))
    for j in range(nv):
        for i in range(nu):
            a, b = j * nu + i, j * nu + (i + 1) % nu
            c, d = (j + 1) * nu + (i + 1) % nu, (j + 1) * nu + i
            counts.append(4); idx += [a, d, c, b]
            u0, u1 = i / nu, (i + 1) / nu
            v0, v1 = 1 - j / nv, 1 - (j + 1) / nv
            st += [Gf.Vec2f(u0, v0), Gf.Vec2f(u0, v1), Gf.Vec2f(u1, v1), Gf.Vec2f(u1, v0)]
    m = UsdGeom.Mesh.Define(stage, path)
    m.CreatePointsAttr(pts); m.CreateFaceVertexCountsAttr(counts); m.CreateFaceVertexIndicesAttr(idx)
    m.CreateSubdivisionSchemeAttr('catmullClark')
    UsdGeom.PrimvarsAPI(m).CreatePrimvar('st', Sdf.ValueTypeNames.TexCoord2fArray,
                                         UsdGeom.Tokens.faceVarying).Set(st)
    UsdGeom.XformCommonAPI(m).SetTranslate(Gf.Vec3d(*center))
    return m


def grid(path, size, n, offset):
    pts, counts, idx, st = [], [], [], []
    for j in range(n + 1):
        for i in range(n + 1):
            pts.append(Gf.Vec3f(size * (i / n - 0.5), 0, size * (j / n - 0.5)))
            st.append(Gf.Vec2f(i / n, 1 - j / n))
    for j in range(n):
        for i in range(n):
            a = j * (n + 1) + i
            counts.append(4); idx += [a, a + n + 1, a + n + 2, a + 1]
    m = UsdGeom.Mesh.Define(stage, path)
    m.CreatePointsAttr(pts); m.CreateFaceVertexCountsAttr(counts); m.CreateFaceVertexIndicesAttr(idx)
    m.CreateSubdivisionSchemeAttr('none')
    UsdGeom.PrimvarsAPI(m).CreatePrimvar('st', Sdf.ValueTypeNames.TexCoord2fArray,
                                         UsdGeom.Tokens.vertex).Set(st)
    UsdGeom.XformCommonAPI(m).SetTranslate(Gf.Vec3d(*offset))
    return m


class Graph:
    def __init__(self, path, color):
        self.path = path
        self.mat = UsdShade.Material.Define(stage, path)
        surf = self.node('surf', 'ND_standard_surface_surfaceshader')
        surf.CreateInput('base_color', Sdf.ValueTypeNames.Color3f).Set(Gf.Vec3f(*color))
        surf.CreateInput('specular_roughness', F).Set(0.4)
        self.mat.CreateSurfaceOutput('mtlx').ConnectToSource(
            surf.CreateOutput('out', Sdf.ValueTypeNames.Token))
        self.n = 0

    def node(self, name, nid):
        s = UsdShade.Shader.Define(stage, '%s/%s' % (self.path, name))
        s.CreateIdAttr(nid)
        return s

    def add(self, nid, out_type, **inputs):
        """inputs: name=(type, value or (shader, outtype))"""
        self.n += 1
        s = self.node('n%d' % self.n, nid)
        for k, (t, v) in inputs.items():
            i = s.CreateInput(k, t)
            if isinstance(v, tuple) and isinstance(v[0], UsdShade.Shader):
                i.ConnectToSource(v[0].GetOutput('out') or v[0].CreateOutput('out', v[1]))
            else:
                i.Set(v)
        s.CreateOutput('out', out_type)
        return (s, out_type)

    def displace(self, src, scale, vector=False):
        t = V3 if vector else F
        d = self.add('ND_displacement_vector3' if vector else 'ND_displacement_float',
                     Sdf.ValueTypeNames.Token, displacement=(t, src), scale=(F, scale))
        self.mat.CreateDisplacementOutput('mtlx').ConnectToSource(d[0].GetOutput('out'))
        return self.mat


def scaled_position(g, freq, space=None):
    kw = {} if space is None else {'space': (Sdf.ValueTypeNames.String, space)}
    p = g.add('ND_position_vector3', V3, **kw)
    return g.add('ND_multiply_vector3FA', V3, in1=(V3, p), in2=(F, freq))


def bind(prim, mat):
    UsdShade.MaterialBindingAPI.Apply(prim.GetPrim()).Bind(mat)


xs = [-2.6, -1.3, 0.0, 1.3, 2.6]
# 1 noise3d
g = Graph('/World/mtl/noise', (0.8, 0.35, 0.1))
n = g.add('ND_noise3d_float', F, position=(V3, scaled_position(g, 3.0)))
bind(uv_sphere('/World/s_noise', (xs[0], 0.9, 0), 0.5), g.displace(n, 0.25))
# 2 fractal3d
g = Graph('/World/mtl/fractal', (0.3, 0.6, 0.3))
n = g.add('ND_fractal3d_float', F, position=(V3, scaled_position(g, 2.5)),
          octaves=(Sdf.ValueTypeNames.Int, 4))
bind(uv_sphere('/World/s_fractal', (xs[1], 0.9, 0), 0.5), g.displace(n, 0.2))
# 3 worley
g = Graph('/World/mtl/worley', (0.3, 0.45, 0.8))
n = g.add('ND_worleynoise3d_float', F, position=(V3, scaled_position(g, 4.0)))
bind(uv_sphere('/World/s_worley', (xs[2], 0.9, 0), 0.5), g.displace(n, -0.2))
# 4 cellnoise (steps)
g = Graph('/World/mtl/cell', (0.7, 0.7, 0.3))
n = g.add('ND_cellnoise3d_float', F, position=(V3, scaled_position(g, 4.0)))
bind(uv_sphere('/World/s_cell', (xs[3], 0.9, 0), 0.5), g.displace(n, 0.12))
# 5 vector noise
g = Graph('/World/mtl/vnoise', (0.7, 0.35, 0.7))
n = g.add('ND_noise3d_vector3', V3, position=(V3, scaled_position(g, 2.0)))
bind(uv_sphere('/World/s_vnoise', (xs[4], 0.9, 0), 0.5), g.displace(n, 0.3, vector=True))
# Second row.
I, S = Sdf.ValueTypeNames.Int, Sdf.ValueTypeNames.String
xs2 = [-2.0, -1.0, 0.0, 1.0, 2.0]
y2 = 2.15
# 6 unifiednoise3d (perlin, freq 3)
g = Graph('/World/mtl/unified', (0.8, 0.5, 0.2))
n = g.add('ND_unifiednoise3d_float', F, freq=(V3, Gf.Vec3f(3, 3, 3)), type=(I, 0))
bind(uv_sphere('/World/s_unified', (xs2[0], y2, 0), 0.4), g.displace(n, 0.3))
# 7 place2d (rotated, scaled uv) -> separate2 -> trianglewave
g = Graph('/World/mtl/place', (0.5, 0.7, 0.8))
uv = g.add('ND_texcoord_vector2', V2)
pl = g.add('ND_place2d_vector2', V2, texcoord=(V2, uv), rotate=(F, 30.0),
           scale=(V2, Gf.Vec2f(0.08, 0.12)))
sep = g.add('ND_separate2_vector2', F, **{'in': (V2, pl)})
sep[0].CreateOutput('outx', F)
tw = g.node('tw', 'ND_trianglewave_float')
tw.CreateInput('in', F).ConnectToSource(sep[0].GetOutput('outx'))
tw.CreateOutput('out', F)
bind(uv_sphere('/World/s_place', (xs2[1], y2, 0), 0.4), g.displace((tw, F), 0.25))
# 8 range (gamma) of noise3d
g = Graph('/World/mtl/range', (0.6, 0.8, 0.4))
n = g.add('ND_noise3d_float', F, position=(V3, scaled_position(g, 4.0)))
r = g.add('ND_range_float', F, **{'in': (F, n), 'inlow': (F, -0.4), 'inhigh': (F, 0.4),
                                  'gamma': (F, 2.0), 'doclamp': (Sdf.ValueTypeNames.Bool, True)})
bind(uv_sphere('/World/s_range', (xs2[2], y2, 0), 0.4), g.displace(r, 0.15))
# 9 rotate3d -> separate3.outx -> sin
g = Graph('/World/mtl/rotate', (0.8, 0.4, 0.5))
p = g.add('ND_position_vector3', V3)
rt = g.add('ND_rotate3d_vector3', V3, **{'in': (V3, p), 'amount': (F, 45.0),
                                         'axis': (V3, Gf.Vec3f(0, 0, 1))})
sp = g.add('ND_separate3_vector3', F, **{'in': (V3, rt)})
sp[0].CreateOutput('outx', F)
mul = g.node('freq', 'ND_multiply_float')
mul.CreateInput('in1', F).ConnectToSource(sp[0].GetOutput('outx'))
mul.CreateInput('in2', F).Set(18.0)
mul.CreateOutput('out', F)
sn = g.add('ND_sin_float', F, **{'in': (F, (mul, F))})
bind(uv_sphere('/World/s_rotate', (xs2[3], y2, 0), 0.4), g.displace(sn, 0.06))
# 10 tiledimage x noise3d
g = Graph('/World/mtl/imgnoise', (0.6, 0.6, 0.75))
im = g.add('ND_tiledimage_float', F, file=(Sdf.ValueTypeNames.Asset, './stripes.exr'),
           uvtiling=(V2, Gf.Vec2f(2, 1)))
n = g.add('ND_noise3d_float', F, position=(V3, scaled_position(g, 3.0)), pivot=(F, 0.5))
m = g.add('ND_multiply_float', F, in1=(F, im), in2=(F, n))
bind(uv_sphere('/World/s_imgnoise', (xs2[4], y2, 0), 0.4), g.displace(m, 0.25))
# ground: fractal3d of the world position (the grid is moved off the origin)
g = Graph('/World/mtl/ground', (0.45, 0.45, 0.42))
n = g.add('ND_fractal3d_float', F, position=(V3, scaled_position(g, 1.5, 'world')),
          octaves=(Sdf.ValueTypeNames.Int, 3))
bind(grid('/World/ground', 9, 8, (0.37, 0, -0.81)), g.displace(n, 0.35))

s = UsdRender.Settings.Define(stage, '/Render/settings')
s.CreateCameraRel().SetTargets([cam.GetPath()])
s.CreateResolutionAttr(Gf.Vec2i(640, 427))
stage.GetRootLayer().Save()
print('wrote', stage.GetRootLayer().realPath)
