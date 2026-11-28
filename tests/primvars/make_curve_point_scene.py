# hython tests/primvars/make_curve_point_scene.py <outdir>
# Primvars on curves, points and implicit prims (pbrt shape primvars), for
# comparison with Karma (run_primvars.sh). All bound to a MaterialX
# standard_surface whose base_color reads a primvar:
# - lin_curves: thick linear curves, vertex displayColor red -> green -> blue
#   along each;
# - bspline_curves: cubic bspline curves, vertex displayColor alternating
#   yellow / magenta (blended by the basis);
# - uniform_curves: linear curves, uniform primvar "tint" (one color each);
# - points: a grid of points (spheres), vertex displayColor ramp;
# - implicit: a Sphere and a Cylinder prim, constant primvar "tint".
# Writes curves_points.usda and curves_points_patches.json (pixel positions
# with the expected primvar value, for the comparison).
import json, math, os, sys
from pxr import Gf, Sdf, Usd, UsdGeom, UsdLux, UsdRender, UsdShade, Vt

out = os.path.abspath(sys.argv[1])
os.makedirs(out, exist_ok=True)
stage = Usd.Stage.CreateNew(os.path.join(out, 'curves_points.usda'))
stage.SetStartTimeCode(1)
stage.SetEndTimeCode(1)
UsdGeom.SetStageUpAxis(stage, UsdGeom.Tokens.y)
UsdGeom.SetStageMetersPerUnit(stage, 1)
UsdGeom.Xform.Define(stage, '/World')
RES = (640, 427)
cam = UsdGeom.Camera.Define(stage, '/World/cam')
cam.CreateFocalLengthAttr(35); cam.CreateHorizontalApertureAttr(36); cam.CreateVerticalApertureAttr(24)
UsdGeom.XformCommonAPI(cam).SetTranslate(Gf.Vec3d(0, 1.0, 7.0))
dome = UsdLux.DomeLight.Define(stage, '/World/dome')
dome.CreateIntensityAttr(0.6)
sun = UsdLux.DistantLight.Define(stage, '/World/sun')
sun.CreateIntensityAttr(2.5); sun.CreateAngleAttr(1)
UsdGeom.XformCommonAPI(sun).SetRotate(Gf.Vec3f(-40, 30, 0))

F, C3, S = Sdf.ValueTypeNames.Float, Sdf.ValueTypeNames.Color3f, Sdf.ValueTypeNames.String
VTX, UNI, CONST = UsdGeom.Tokens.vertex, UsdGeom.Tokens.uniform, UsdGeom.Tokens.constant


def shader(path, nid):
    s = UsdShade.Shader.Define(stage, path)
    s.CreateIdAttr(nid)
    return s


def mtlx(name, base_from=None, base=(0.5, 0.5, 0.5)):
    path = '/World/mtl/' + name
    mat = UsdShade.Material.Define(stage, path)
    surf = shader(path + '/surf', 'ND_standard_surface_surfaceshader')
    surf.CreateInput('base_color', C3).Set(Gf.Vec3f(*base))
    surf.CreateInput('specular_roughness', F).Set(0.5)
    if base_from:
        r = shader(path + '/pv', 'ND_geompropvalue_color3')
        r.CreateInput('geomprop', S).Set(base_from)
        surf.GetInput('base_color').ConnectToSource(r.CreateOutput('out', C3))
    mat.CreateSurfaceOutput('mtlx').ConnectToSource(surf.CreateOutput('out', Sdf.ValueTypeNames.Token))
    return mat


def bind(prim, mat):
    UsdShade.MaterialBindingAPI.Apply(prim.GetPrim()).Bind(mat)


def primvar(prim, name, typ, interp, value):
    UsdGeom.PrimvarsAPI(prim).CreatePrimvar(name, typ, interp).Set(value)


mat_cd = mtlx('cd', 'displayColor')
mat_tint = mtlx('tint', 'tint')
back = UsdGeom.Mesh.Define(stage, '/World/backdrop')
back.CreatePointsAttr([(-8, -4, -1.5), (8, -4, -1.5), (8, 6, -1.5), (-8, 6, -1.5)])
back.CreateFaceVertexCountsAttr([4]); back.CreateFaceVertexIndicesAttr([0, 1, 2, 3])
back.CreateSubdivisionSchemeAttr('none')
bind(back, mtlx('backdrop'))

patches = []  # (label, world point, expected color)


def lerp3(a, b, t):
    return tuple(a[i] * (1 - t) + b[i] * t for i in range(3))


RED, GREEN, BLUE = (0.9, 0.1, 0.1), (0.1, 0.9, 0.1), (0.1, 0.1, 0.9)
YELLOW, MAGENTA = (0.9, 0.8, 0.1), (0.8, 0.1, 0.8)

# Linear curves: 3 horizontal curves of 5 vertices, red -> green -> blue.
c = UsdGeom.BasisCurves.Define(stage, '/World/lin_curves')
c.CreateTypeAttr('linear')
pts, cols = [], []
for j in range(3):
    y = 2.3 - 0.35 * j
    for i in range(5):
        t = i / 4
        pts.append(Gf.Vec3f(-3.3 + 2.0 * t, y, 0))
        cols.append(Gf.Vec3f(*(lerp3(RED, GREEN, 2 * t) if t <= 0.5 else lerp3(GREEN, BLUE, 2 * t - 1))))
c.CreatePointsAttr(pts); c.CreateCurveVertexCountsAttr([5, 5, 5])
c.CreateWidthsAttr([0.16]); c.SetWidthsInterpolation(CONST)
primvar(c, 'displayColor', Sdf.ValueTypeNames.Color3fArray, VTX, Vt.Vec3fArray(cols))
bind(c, mat_cd)
for t, col in ((0.0625, lerp3(RED, GREEN, 0.125)), (0.5, GREEN), (0.875, lerp3(GREEN, BLUE, 0.75))):
    patches.append(('lin_curves t=%.2f' % t, (-3.3 + 2.0 * t, 2.3 - 0.35, 0.08), col))

# Bspline curves: 2 curves of 8 vertices alternating yellow / magenta.
c = UsdGeom.BasisCurves.Define(stage, '/World/bspline_curves')
c.CreateTypeAttr('cubic'); c.CreateBasisAttr('bspline')
pts, cols = [], []
for j in range(2):
    y = 2.2 - 0.5 * j
    for i in range(8):
        pts.append(Gf.Vec3f(-0.6 + 0.35 * i, y + 0.15 * (-1) ** i, 0))
        cols.append(Gf.Vec3f(*(YELLOW if i % 2 == 0 else MAGENTA)))
c.CreatePointsAttr(pts); c.CreateCurveVertexCountsAttr([8, 8])
c.CreateWidthsAttr([0.16]); c.SetWidthsInterpolation(CONST)
primvar(c, 'displayColor', Sdf.ValueTypeNames.Color3fArray, VTX, Vt.Vec3fArray(cols))
bind(c, mat_cd)
# At a bspline segment boundary the value is (c0 + 4 c1 + c2) / 6 of the
# vertex values around it (as the position): 2/3 of the middle one.
for i, col in ((2, lerp3(MAGENTA, YELLOW, 4 / 6)), (3, lerp3(YELLOW, MAGENTA, 4 / 6))):
    x = -0.6 + 0.35 * i
    y = 2.2 + 0.15 * (-1) ** i * (4 / 6 - 2 / 6)
    patches.append(('bspline_curves v%d' % i, (x, y, 0.08), col))

# Uniform per-curve tint.
c = UsdGeom.BasisCurves.Define(stage, '/World/uniform_curves')
c.CreateTypeAttr('linear')
pts = []
tints = [RED, GREEN, BLUE]
for j in range(3):
    for i in range(2):
        pts.append(Gf.Vec3f(2.0 + 0.6 * j, 1.6 + 0.9 * i, 0))
c.CreatePointsAttr(pts); c.CreateCurveVertexCountsAttr([2, 2, 2])
c.CreateWidthsAttr([0.2]); c.SetWidthsInterpolation(CONST)
primvar(c, 'tint', Sdf.ValueTypeNames.Color3fArray, UNI, Vt.Vec3fArray([Gf.Vec3f(*t) for t in tints]))
bind(c, mat_tint)
for j in range(3):
    patches.append(('uniform_curves %d' % j, (2.0 + 0.6 * j, 2.05, 0.1), tints[j]))

# Points: a 5 x 4 grid of spheres, vertex displayColor ramp.
p = UsdGeom.Points.Define(stage, '/World/points')
pts, cols = [], []
for j in range(4):
    for i in range(5):
        pts.append(Gf.Vec3f(-3.3 + 0.45 * i, 0.9 - 0.42 * j, 0))
        cols.append(Gf.Vec3f(0.1 + 0.8 * i / 4, 0.1 + 0.8 * j / 3, 0.5))
p.CreatePointsAttr(pts)
p.CreateWidthsAttr([0.36]); p.SetWidthsInterpolation(CONST)
primvar(p, 'displayColor', Sdf.ValueTypeNames.Color3fArray, VTX, Vt.Vec3fArray(cols))
bind(p, mat_cd)
for i, j in ((0, 0), (4, 0), (0, 3), (4, 3), (2, 1)):
    patches.append(('points %d,%d' % (i, j), (-3.3 + 0.45 * i, 0.9 - 0.42 * j, 0.18),
                    (0.1 + 0.8 * i / 4, 0.1 + 0.8 * j / 3, 0.5)))

# Implicit prims with a constant primvar.
s = UsdGeom.Sphere.Define(stage, '/World/implicit_sphere')
s.CreateRadiusAttr(0.5)
UsdGeom.XformCommonAPI(s).SetTranslate(Gf.Vec3d(0.5, 0.4, 0))
primvar(s, 'tint', C3, CONST, Gf.Vec3f(0.9, 0.55, 0.1))
bind(s, mat_tint)
patches.append(('implicit_sphere', (0.5, 0.4, 0.5), (0.9, 0.55, 0.1)))
cy = UsdGeom.Cylinder.Define(stage, '/World/implicit_cylinder')
cy.CreateRadiusAttr(0.4); cy.CreateHeightAttr(1.0); cy.CreateAxisAttr('Y')
UsdGeom.XformCommonAPI(cy).SetTranslate(Gf.Vec3d(2.3, 0.4, 0))
primvar(cy, 'tint', C3, CONST, Gf.Vec3f(0.2, 0.6, 0.7))
bind(cy, mat_tint)
patches.append(('implicit_cylinder', (2.3, 0.4, 0.4), (0.2, 0.6, 0.7)))

rs = UsdRender.Settings.Define(stage, '/Render/settings')
rs.CreateCameraRel().SetTargets([cam.GetPath()])
rs.CreateResolutionAttr(Gf.Vec2i(*RES))
stage.GetRootLayer().Save()

fr = cam.GetCamera(1).frustum
vp = fr.ComputeViewMatrix() * fr.ComputeProjectionMatrix()
outp = []
for label, wp, col in patches:
    q = vp.Transform(Gf.Vec3d(*wp))
    outp.append([label, int((q[0] * 0.5 + 0.5) * RES[0]), int((0.5 - q[1] * 0.5) * RES[1]), col])
json.dump(outp, open(os.path.join(out, 'curves_points_patches.json'), 'w'), indent=1)
print('wrote', stage.GetRootLayer().realPath)
