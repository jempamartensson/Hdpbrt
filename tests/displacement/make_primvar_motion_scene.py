# hython tests/displacement/make_primvar_motion_scene.py <outdir>
# Primvars in displacement graphs and displaced deforming meshes (motion
# blur), render at frame 1 (shutter 0..1 = frames 1..2):
# - three spheres share one material, noise3d x geompropvalue "amp": two with
#   constant primvars amp 0.15 / 0.45, one without (the node's default 0:
#   undisplaced);
# - a sphere with a vertex primvar "mask" (0 below, 1 above the equator)
#   times fractal3d;
# - a sphere whose vertex vector3 primvar "bulge" is the vector displacement
#   (along N on its right side);
# - a displaced sphere that deforms over the shutter (stretches and moves to
#   the right); karma:object:geosamples 2 so Karma blurs it too.
import math, os, sys
from pxr import Gf, Sdf, Usd, UsdGeom, UsdLux, UsdRender, UsdShade, Vt

out = os.path.abspath(sys.argv[1])
os.makedirs(out, exist_ok=True)
stage = Usd.Stage.CreateNew(os.path.join(out, 'primvar_motion.usda'))
stage.SetStartTimeCode(1)
stage.SetEndTimeCode(2)
UsdGeom.SetStageUpAxis(stage, UsdGeom.Tokens.y)
UsdGeom.SetStageMetersPerUnit(stage, 1)
UsdGeom.Xform.Define(stage, '/World')
cam = UsdGeom.Camera.Define(stage, '/World/cam')
cam.CreateFocalLengthAttr(35); cam.CreateHorizontalApertureAttr(36); cam.CreateVerticalApertureAttr(24)
cam.CreateShutterOpenAttr(0); cam.CreateShutterCloseAttr(1)
cx = UsdGeom.XformCommonAPI(cam)
cx.SetTranslate(Gf.Vec3d(0, 1.4, 7.0)); cx.SetRotate(Gf.Vec3f(-6, 0, 0))
dome = UsdLux.DomeLight.Define(stage, '/World/dome')
dome.CreateIntensityAttr(0.6); dome.CreateColorAttr(Gf.Vec3f(0.7, 0.8, 1.0))
sun = UsdLux.DistantLight.Define(stage, '/World/sun')
sun.CreateIntensityAttr(2.5); sun.CreateAngleAttr(1)
UsdGeom.XformCommonAPI(sun).SetRotate(Gf.Vec3f(-50, 35, 0))

F, V3 = Sdf.ValueTypeNames.Float, Sdf.ValueTypeNames.Vector3f


def sphere_points(radius, nu, nv):
    pts = []
    for j in range(nv + 1):
        th = math.pi * j / nv
        for i in range(nu):
            ph = 2 * math.pi * i / nu
            pts.append(Gf.Vec3f(radius * math.sin(th) * math.cos(ph),
                                radius * math.cos(th), -radius * math.sin(th) * math.sin(ph)))
    return pts


def uv_sphere(path, center, radius, nu=24, nv=12):
    pts = sphere_points(radius, nu, nv)
    counts, idx, st = [], [], []
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
        self.n += 1
        s = self.node('n%d' % self.n, nid)
        for k, (t, v) in inputs.items():
            i = s.CreateInput(k, t)
            if isinstance(v, tuple):
                i.ConnectToSource(v[0].GetOutput('out'))
            else:
                i.Set(v)
        s.CreateOutput('out', out_type)
        return (s, out_type)

    def displace(self, src, scale, vector=False):
        d = self.add('ND_displacement_vector3' if vector else 'ND_displacement_float',
                     Sdf.ValueTypeNames.Token, displacement=(V3 if vector else F, src),
                     scale=(F, scale))
        self.mat.CreateDisplacementOutput('mtlx').ConnectToSource(d[0].GetOutput('out'))
        return self.mat


def scaled_position(g, freq):
    p = g.add('ND_position_vector3', V3)
    return g.add('ND_multiply_vector3FA', V3, in1=(V3, p), in2=(F, freq))


def geomprop(g, name, typ, nid):
    return g.add(nid, typ, geomprop=(Sdf.ValueTypeNames.String, name))


def bind(prim, mat):
    UsdShade.MaterialBindingAPI.Apply(prim.GetPrim()).Bind(mat)


def primvar(mesh, name, typ, interp, value):
    UsdGeom.PrimvarsAPI(mesh).CreatePrimvar(name, typ, interp).Set(value)


# Shared material: noise3d x amp.
g = Graph('/World/mtl/amp', (0.8, 0.45, 0.2))
n = g.add('ND_noise3d_float', F, position=(V3, scaled_position(g, 3.0)))
amp = geomprop(g, 'amp', F, 'ND_geompropvalue_float')
m = g.add('ND_multiply_float', F, in1=(F, n), in2=(F, amp))
mat_amp = g.displace(m, 1.0)
for name, x, a in (('amp_low', -2.6, 0.15), ('amp_high', -1.3, 0.45), ('amp_none', 0.0, None)):
    s = uv_sphere('/World/' + name, (x, 1.6, 0), 0.5)
    if a is not None:
        primvar(s, 'amp', F, UsdGeom.Tokens.constant, a)
    bind(s, mat_amp)

# Vertex mask x fractal3d.
g = Graph('/World/mtl/mask', (0.35, 0.65, 0.35))
fr = g.add('ND_fractal3d_float', F, position=(V3, scaled_position(g, 3.0)),
           octaves=(Sdf.ValueTypeNames.Int, 3))
mask = geomprop(g, 'mask', F, 'ND_geompropvalue_float')
m = g.add('ND_multiply_float', F, in1=(F, fr), in2=(F, mask))
s = uv_sphere('/World/masked', (1.3, 1.6, 0), 0.5)
primvar(s, 'mask', Sdf.ValueTypeNames.FloatArray, UsdGeom.Tokens.vertex,
        Vt.FloatArray([1.0 if p[1] > 0.01 else 0.0 for p in sphere_points(0.5, 24, 12)]))
bind(s, g.displace(m, 0.35))

# Vertex vector3 primvar as the vector displacement.
g = Graph('/World/mtl/bulge', (0.4, 0.5, 0.85))
b = geomprop(g, 'bulge', V3, 'ND_geompropvalue_vector3')
s = uv_sphere('/World/bulged', (2.6, 1.6, 0), 0.5)
primvar(s, 'bulge', Sdf.ValueTypeNames.Vector3fArray, UsdGeom.Tokens.vertex,
        Vt.Vec3fArray([Gf.Vec3f(0, 0, 0.6 * max(0.0, p[0] / 0.5) ** 2)
                       for p in sphere_points(0.5, 24, 12)]))
bind(s, g.displace(b, 1.0, vector=True))

# Displaced deforming sphere (motion blur).
g = Graph('/World/mtl/moving', (0.75, 0.3, 0.6))
n = g.add('ND_noise3d_float', F, position=(V3, scaled_position(g, 4.0)))
s = uv_sphere('/World/deforming', (-1.0, 0.55, 1.2), 0.45)
pts = sphere_points(0.45, 24, 12)
attr = s.GetPointsAttr()
attr.Set(Vt.Vec3fArray(pts), 1)
attr.Set(Vt.Vec3fArray([Gf.Vec3f(p[0] * 1.5 + 0.9, p[1], p[2]) for p in pts]), 2)
s.GetPrim().CreateAttribute('primvars:karma:object:geosamples', Sdf.ValueTypeNames.Int).Set(2)
bind(s, g.displace(n, 0.18))
# A static displaced reference next to it (sharp).
s = uv_sphere('/World/static', (1.4, 0.55, 1.2), 0.45)
bind(s, g.mat)

rs = UsdRender.Settings.Define(stage, '/Render/settings')
rs.CreateCameraRel().SetTargets([cam.GetPath()])
rs.CreateResolutionAttr(Gf.Vec2i(640, 427))
stage.GetRootLayer().Save()
print('wrote', stage.GetRootLayer().realPath)
