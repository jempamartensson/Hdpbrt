# hython tests/primvars/make_instance_primvars_scene.py <outdir> [flat]
# Per-instance primvars (pbrt patch 48). Three PointInstancers:
# - "plain": spheres with no material, per-instance displayColor (Houdini's
#   Cd on points copied to);
# - "tinted": spheres whose standard_surface reads geompropvalue "tint"
#   (color3) for base_color and "rough" (float) for specular_roughness, both
#   per instance;
# - "lamps": emissive quads (facing down) whose emission_color reads
#   geompropvalue "glow" per instance, over the ground.
# With "flat" the instances are separate meshes carrying the same values as
# constant primvars (the reference). Writes instance_primvars[_flat].usda and
# instance_primvars_patches.json (pixel positions of each sphere's centre).
import json, math, os, sys
from pxr import Gf, Sdf, Usd, UsdGeom, UsdLux, UsdRender, UsdShade, Vt

out = os.path.abspath(sys.argv[1])
flat = len(sys.argv) > 2 and sys.argv[2] == 'flat'
os.makedirs(out, exist_ok=True)
stage = Usd.Stage.CreateNew(os.path.join(out, 'instance_primvars%s.usda' % ('_flat' if flat else '')))
stage.SetStartTimeCode(1); stage.SetEndTimeCode(1)
UsdGeom.SetStageUpAxis(stage, UsdGeom.Tokens.y)
UsdGeom.SetStageMetersPerUnit(stage, 1)
UsdGeom.Xform.Define(stage, '/World')
RES = (640, 400)
cam = UsdGeom.Camera.Define(stage, '/World/cam')
cam.CreateFocalLengthAttr(35); cam.CreateHorizontalApertureAttr(36); cam.CreateVerticalApertureAttr(22.5)
cx = UsdGeom.XformCommonAPI(cam)
cx.SetTranslate(Gf.Vec3d(0, 3.2, 9.0)); cx.SetRotate(Gf.Vec3f(-14, 0, 0))
dome = UsdLux.DomeLight.Define(stage, '/World/dome')
dome.CreateIntensityAttr(0.5)
sun = UsdLux.DistantLight.Define(stage, '/World/sun')
sun.CreateIntensityAttr(2.0); sun.CreateAngleAttr(1)
UsdGeom.XformCommonAPI(sun).SetRotate(Gf.Vec3f(-50, 30, 0))
F, C3, S = Sdf.ValueTypeNames.Float, Sdf.ValueTypeNames.Color3f, Sdf.ValueTypeNames.String


def shader(path, nid):
    s = UsdShade.Shader.Define(stage, path)
    s.CreateIdAttr(nid)
    return s


def bind(prim, mat):
    UsdShade.MaterialBindingAPI.Apply(prim.GetPrim()).Bind(mat)


def sphere_mesh(path, r=0.35, nu=24, nv=12):
    pts, counts, idx = [], [], []
    for j in range(nv + 1):
        th = math.pi * j / nv
        for i in range(nu):
            ph = 2 * math.pi * i / nu
            pts.append(Gf.Vec3f(r * math.sin(th) * math.cos(ph), r * math.cos(th),
                                -r * math.sin(th) * math.sin(ph)))
    for j in range(nv):
        for i in range(nu):
            a, b = j * nu + i, j * nu + (i + 1) % nu
            c, d = (j + 1) * nu + (i + 1) % nu, (j + 1) * nu + i
            counts.append(4); idx += [a, d, c, b]
    m = UsdGeom.Mesh.Define(stage, path)
    m.CreatePointsAttr(pts); m.CreateFaceVertexCountsAttr(counts); m.CreateFaceVertexIndicesAttr(idx)
    m.CreateSubdivisionSchemeAttr('catmullClark')
    return m


def quad(path, half=0.3):
    m = UsdGeom.Mesh.Define(stage, path)
    m.CreatePointsAttr([(-half, 0, -half), (-half, 0, half), (half, 0, half), (half, 0, -half)])
    m.CreateFaceVertexCountsAttr([4]); m.CreateFaceVertexIndicesAttr([0, 3, 2, 1])
    m.CreateSubdivisionSchemeAttr('none')
    return m


ground = UsdGeom.Mesh.Define(stage, '/World/ground')
ground.CreatePointsAttr([(-8, 0, -8), (8, 0, -8), (8, 0, 8), (-8, 0, 8)])
ground.CreateFaceVertexCountsAttr([4]); ground.CreateFaceVertexIndicesAttr([0, 3, 2, 1])
ground.CreateSubdivisionSchemeAttr('none')
gm = UsdShade.Material.Define(stage, '/World/mtl/ground')
gs = shader('/World/mtl/ground/surf', 'ND_standard_surface_surfaceshader')
gs.CreateInput('base_color', C3).Set(Gf.Vec3f(0.5)); gs.CreateInput('specular_roughness', F).Set(0.8)
gm.CreateSurfaceOutput('mtlx').ConnectToSource(gs.CreateOutput('out', Sdf.ValueTypeNames.Token))
bind(ground, gm)

# The tinted material.
tm = UsdShade.Material.Define(stage, '/World/mtl/tinted')
ts = shader('/World/mtl/tinted/surf', 'ND_standard_surface_surfaceshader')
tint = shader('/World/mtl/tinted/tint', 'ND_geompropvalue_color3')
tint.CreateInput('geomprop', S).Set('tint')
tint.CreateInput('default', C3).Set(Gf.Vec3f(0.5))
ts.CreateInput('base_color', C3).ConnectToSource(tint.CreateOutput('out', C3))
rough = shader('/World/mtl/tinted/rough', 'ND_geompropvalue_float')
rough.CreateInput('geomprop', S).Set('rough')
rough.CreateInput('default', F).Set(0.4)
ts.CreateInput('specular_roughness', F).ConnectToSource(rough.CreateOutput('out', F))
tm.CreateSurfaceOutput('mtlx').ConnectToSource(ts.CreateOutput('out', Sdf.ValueTypeNames.Token))

# The lamp material.
lm = UsdShade.Material.Define(stage, '/World/mtl/lamp')
ls = shader('/World/mtl/lamp/surf', 'ND_standard_surface_surfaceshader')
glow = shader('/World/mtl/lamp/glow', 'ND_geompropvalue_color3')
glow.CreateInput('geomprop', S).Set('glow')
ls.CreateInput('emission', F).Set(1.0)
ls.CreateInput('emission_color', C3).ConnectToSource(glow.CreateOutput('out', C3))
ls.CreateInput('base', F).Set(0.0); ls.CreateInput('specular', F).Set(0.0)
lm.CreateSurfaceOutput('mtlx').ConnectToSource(ls.CreateOutput('out', Sdf.ValueTypeNames.Token))

hues = [(0.9, 0.2, 0.15), (0.2, 0.8, 0.25), (0.2, 0.35, 0.9), (0.9, 0.8, 0.2), (0.7, 0.2, 0.8)]
rows = {
    'plain': [((-2.4 + 1.2 * i, 1.6, 0), hues[i]) for i in range(5)],
    'tinted': [((-2.4 + 1.2 * i, 0.5, 0.5), hues[4 - i]) for i in range(5)],
}
roughs = [0.05, 0.2, 0.4, 0.6, 0.9]
lamps = [((-3 + 3 * i, 1.2, -4.0), tuple(40 * c for c in hues[i])) for i in range(3)]
patches = []

if flat:
    for i, (p, c) in enumerate(rows['plain']):
        m = sphere_mesh('/World/plain/s%d' % i)
        UsdGeom.XformCommonAPI(m).SetTranslate(Gf.Vec3d(*p))
        UsdGeom.PrimvarsAPI(m).CreatePrimvar('displayColor', Sdf.ValueTypeNames.Color3fArray,
                                             UsdGeom.Tokens.constant).Set([Gf.Vec3f(*c)])
    for i, (p, c) in enumerate(rows['tinted']):
        m = sphere_mesh('/World/tinted/s%d' % i)
        UsdGeom.XformCommonAPI(m).SetTranslate(Gf.Vec3d(*p))
        pv = UsdGeom.PrimvarsAPI(m)
        pv.CreatePrimvar('tint', C3, UsdGeom.Tokens.constant).Set(Gf.Vec3f(*c))
        pv.CreatePrimvar('rough', F, UsdGeom.Tokens.constant).Set(roughs[i])
        bind(m, tm)
    for i, (p, c) in enumerate(lamps):
        m = quad('/World/lamps/q%d' % i)
        UsdGeom.XformCommonAPI(m).SetTranslate(Gf.Vec3d(*p))
        UsdGeom.PrimvarsAPI(m).CreatePrimvar('glow', C3, UsdGeom.Tokens.constant).Set(Gf.Vec3f(*c))
        bind(m, lm)
else:
    def instancer(path, proto_fn, pts):
        pi = UsdGeom.PointInstancer.Define(stage, path)
        proto = proto_fn(path + '/protos/p')
        pi.CreatePrototypesRel().SetTargets([proto.GetPath()])
        pi.CreateProtoIndicesAttr([0] * len(pts))
        pi.CreatePositionsAttr([Gf.Vec3f(*p) for p in pts])
        return pi, proto
    pi, _ = instancer('/World/plain', sphere_mesh, [p for p, _ in rows['plain']])
    UsdGeom.PrimvarsAPI(pi).CreatePrimvar('displayColor', Sdf.ValueTypeNames.Color3fArray,
                                          UsdGeom.Tokens.vertex).Set([Gf.Vec3f(*c) for _, c in rows['plain']])
    pi, proto = instancer('/World/tinted', sphere_mesh, [p for p, _ in rows['tinted']])
    bind(proto, tm)
    pv = UsdGeom.PrimvarsAPI(pi)
    pv.CreatePrimvar('tint', Sdf.ValueTypeNames.Color3fArray, UsdGeom.Tokens.vertex).Set(
        [Gf.Vec3f(*c) for _, c in rows['tinted']])
    pv.CreatePrimvar('rough', Sdf.ValueTypeNames.FloatArray, UsdGeom.Tokens.vertex).Set(roughs)
    pi, proto = instancer('/World/lamps', quad, [p for p, _ in lamps])
    bind(proto, lm)
    UsdGeom.PrimvarsAPI(pi).CreatePrimvar('glow', Sdf.ValueTypeNames.Color3fArray,
                                          UsdGeom.Tokens.vertex).Set([Gf.Vec3f(*c) for _, c in lamps])

rs = UsdRender.Settings.Define(stage, '/Render/settings')
rs.CreateCameraRel().SetTargets([cam.GetPath()])
rs.CreateResolutionAttr(Gf.Vec2i(*RES))
stage.GetRootLayer().Save()

fr = cam.GetCamera(1).frustum
vp = fr.ComputeViewMatrix() * fr.ComputeProjectionMatrix()
def pix(w):
    q = vp.Transform(Gf.Vec3d(*w))
    return [int((q[0] * 0.5 + 0.5) * RES[0]), int((0.5 - q[1] * 0.5) * RES[1])]
for row in ('plain', 'tinted'):
    for i, (p, c) in enumerate(rows[row]):
        patches.append(['%s %d' % (row, i), pix((p[0], p[1], p[2] + 0.3)), c])
for i, (p, c) in enumerate(lamps):  # the ground under each lamp
    patches.append(['ground under lamp %d' % i, pix((p[0], 0, p[2])), c])
json.dump(patches, open(os.path.join(out, 'instance_primvars_patches.json'), 'w'), indent=1)
print('wrote', stage.GetRootLayer().realPath)
