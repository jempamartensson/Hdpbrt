# hython tests/primvars/make_primvar_scene.py <outdir>
# Varying primvars in surface shading (pbrt "hdpbrtprimvar" textures), for
# comparison with Karma (run_primvars.sh). Top row, left to right:
# - unbound_grid: a grid with no material and a vertex displayColor ramp;
# - mtlx_cd: a subdivided sphere, standard_surface base_color from
#   geompropvalue "displayColor" (vertex, refined through the subdivision);
# - uniform_tint: a polygonal sphere, base_color from a uniform color3
#   primvar "tint" (a checker of faces; face-varying st unwelds the mesh, so
#   each face keeps its own color);
# - rough_metal: a metal sphere, specular_roughness from a vertex float
#   primvar "rough" (0.05 at the top to 0.8 at the bottom).
# Bottom row:
# - displaced_cd: mtlx_cd's colors on a noise-displaced sphere (primvars
#   through pbrt's displacement refinement);
# - preview_fade: a UsdPreviewSurface card, diffuseColor from
#   UsdPrimvarReader_float3 "displayColor", opacity from
#   UsdPrimvarReader_float "fade" (0 on the left, 1 on the right);
# - implicit_const: a Sphere prim with constant displayColor, no material;
# - missing: base_color from geompropvalue "nothere": the node's default.
# Writes primvars.usda and primvars_boxes.json (each object's pixel box).
import json, math, os, sys
from pxr import Gf, Sdf, Usd, UsdGeom, UsdLux, UsdRender, UsdShade, Vt

out = os.path.abspath(sys.argv[1])
os.makedirs(out, exist_ok=True)
stage = Usd.Stage.CreateNew(os.path.join(out, 'primvars.usda'))
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

F, C3, V3, S = (Sdf.ValueTypeNames.Float, Sdf.ValueTypeNames.Color3f,
                Sdf.ValueTypeNames.Vector3f, Sdf.ValueTypeNames.String)
FV, UNI, VTX, CONST = (UsdGeom.Tokens.faceVarying, UsdGeom.Tokens.uniform,
                       UsdGeom.Tokens.vertex, UsdGeom.Tokens.constant)


def sphere_points(radius, nu, nv):
    pts = []
    for j in range(nv + 1):
        th = math.pi * j / nv
        for i in range(nu):
            ph = 2 * math.pi * i / nu
            pts.append(Gf.Vec3f(radius * math.sin(th) * math.cos(ph),
                                radius * math.cos(th), -radius * math.sin(th) * math.sin(ph)))
    return pts


def uv_sphere(path, center, radius, nu=24, nv=12, subdiv=True):
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
    m.CreateSubdivisionSchemeAttr('catmullClark' if subdiv else 'none')
    primvar(m, 'st', Sdf.ValueTypeNames.TexCoord2fArray, FV, st)
    UsdGeom.XformCommonAPI(m).SetTranslate(Gf.Vec3d(*center))
    return m, pts


def grid(path, center, w, h, n=10):
    pts, counts, idx = [], [], []
    for j in range(n + 1):
        for i in range(n + 1):
            pts.append(Gf.Vec3f(w * (i / n - 0.5), h * (j / n - 0.5), 0))
    for j in range(n):
        for i in range(n):
            a = j * (n + 1) + i
            counts.append(4); idx += [a, a + 1, a + n + 2, a + n + 1]
    m = UsdGeom.Mesh.Define(stage, path)
    m.CreatePointsAttr(pts); m.CreateFaceVertexCountsAttr(counts); m.CreateFaceVertexIndicesAttr(idx)
    m.CreateSubdivisionSchemeAttr('none')
    UsdGeom.XformCommonAPI(m).SetTranslate(Gf.Vec3d(*center))
    return m, pts


def primvar(mesh, name, typ, interp, value):
    UsdGeom.PrimvarsAPI(mesh).CreatePrimvar(name, typ, interp).Set(value)


def bind(prim, mat):
    UsdShade.MaterialBindingAPI.Apply(prim.GetPrim()).Bind(mat)


def shader(path, nid):
    s = UsdShade.Shader.Define(stage, path)
    s.CreateIdAttr(nid)
    return s


def mtlx(name, base=(0.8, 0.8, 0.8), rough=0.4, metal=0.0, base_from=None, rough_from=None):
    path = '/World/mtl/' + name
    mat = UsdShade.Material.Define(stage, path)
    surf = shader(path + '/surf', 'ND_standard_surface_surfaceshader')
    surf.CreateInput('base_color', C3).Set(Gf.Vec3f(*base))
    surf.CreateInput('specular_roughness', F).Set(rough)
    surf.CreateInput('metalness', F).Set(metal)
    if base_from:
        pv, default = base_from
        r = shader(path + '/base_pv', 'ND_geompropvalue_color3')
        r.CreateInput('geomprop', S).Set(pv)
        if default:
            r.CreateInput('default', C3).Set(Gf.Vec3f(*default))
        surf.GetInput('base_color').ConnectToSource(r.CreateOutput('out', C3))
    if rough_from:
        r = shader(path + '/rough_pv', 'ND_geompropvalue_float')
        r.CreateInput('geomprop', S).Set(rough_from)
        surf.GetInput('specular_roughness').ConnectToSource(r.CreateOutput('out', F))
    mat.CreateSurfaceOutput('mtlx').ConnectToSource(surf.CreateOutput('out', Sdf.ValueTypeNames.Token))
    return mat, path


def rainbow(p, r):
    return Gf.Vec3f(0.5 + 0.45 * p[0] / r, 0.5 + 0.45 * p[1] / r, 0.5 - 0.45 * p[2] / r)


boxes = {}  # name -> world-space center, radius (projected below)
X = (-2.7, -0.9, 0.9, 2.7)
TOP, BOT = 1.75, 0.35

# Backdrop: plain grey MaterialX (Karma shades unbound displayColor its own
# way, darker than its value: see unbound_grid / implicit_const).
back, _ = grid('/World/backdrop', (0, 1.0, -1.5), 16, 10, 1)
bind(back, mtlx('backdrop', base=(0.5, 0.5, 0.5), rough=0.6)[0])

m, pts = grid('/World/unbound_grid', (X[0], TOP, 0), 1.1, 1.1)
primvar(m, 'displayColor', Sdf.ValueTypeNames.Color3fArray, VTX,
        Vt.Vec3fArray([Gf.Vec3f(p[0] / 1.1 + 0.5, p[1] / 1.1 + 0.5, 0.5 - p[0] / 1.1) for p in pts]))
boxes['unbound_grid'] = ((X[0], TOP, 0), 0.5)

mat, _ = mtlx('mtlx_cd', base_from=('displayColor', None))
m, pts = uv_sphere('/World/mtlx_cd', (X[1], TOP, 0), 0.6)
primvar(m, 'displayColor', Sdf.ValueTypeNames.Color3fArray, VTX,
        Vt.Vec3fArray([rainbow(p, 0.6) for p in pts]))
bind(m, mat)
boxes['mtlx_cd'] = ((X[1], TOP, 0), 0.6)

mat, _ = mtlx('uniform_tint', base_from=('tint', None))
m, pts = uv_sphere('/World/uniform_tint', (X[2], TOP, 0), 0.6, 32, 16, subdiv=False)
tint = []
for j in range(16):
    for i in range(32):
        tint.append(Gf.Vec3f(0.85, 0.2, 0.15) if (i // 4 + j // 2) % 2 else Gf.Vec3f(0.15, 0.3, 0.85))
primvar(m, 'tint', Sdf.ValueTypeNames.Color3fArray, UNI, Vt.Vec3fArray(tint))
bind(m, mat)
boxes['uniform_tint'] = ((X[2], TOP, 0), 0.6)

mat, _ = mtlx('rough_metal', base=(0.9, 0.9, 0.9), metal=1.0, rough_from='rough')
m, pts = uv_sphere('/World/rough_metal', (X[3], TOP, 0), 0.6)
primvar(m, 'rough', Sdf.ValueTypeNames.FloatArray, VTX,
        Vt.FloatArray([0.05 + 0.75 * (1 - (p[1] / 0.6 + 1) / 2) for p in pts]))
bind(m, mat)
boxes['rough_metal'] = ((X[3], TOP, 0), 0.6)

# Displaced: noise3d along the normal, colors from displayColor.
mat, path = mtlx('displaced_cd', base_from=('displayColor', None))
pos = shader(path + '/pos', 'ND_position_vector3')
mul = shader(path + '/mul', 'ND_multiply_vector3FA')
mul.CreateInput('in1', V3).ConnectToSource(pos.CreateOutput('out', V3))
mul.CreateInput('in2', F).Set(4.0)
noise = shader(path + '/noise', 'ND_noise3d_float')
noise.CreateInput('position', V3).ConnectToSource(mul.CreateOutput('out', V3))
disp = shader(path + '/disp', 'ND_displacement_float')
disp.CreateInput('displacement', F).ConnectToSource(noise.CreateOutput('out', F))
disp.CreateInput('scale', F).Set(0.15)
mat.CreateDisplacementOutput('mtlx').ConnectToSource(disp.CreateOutput('out', Sdf.ValueTypeNames.Token))
m, pts = uv_sphere('/World/displaced_cd', (X[0], BOT, 0), 0.55)
primvar(m, 'displayColor', Sdf.ValueTypeNames.Color3fArray, VTX,
        Vt.Vec3fArray([rainbow(p, 0.55) for p in pts]))
bind(m, mat)
boxes['displaced_cd'] = ((X[0], BOT, 0), 0.62)

# UsdPreviewSurface card with primvar readers.
path = '/World/mtl/preview_fade'
mat = UsdShade.Material.Define(stage, path)
ps = shader(path + '/surf', 'UsdPreviewSurface')
ps.CreateInput('roughness', F).Set(0.6)
rc = shader(path + '/cd', 'UsdPrimvarReader_float3')
rc.CreateInput('varname', Sdf.ValueTypeNames.Token).Set('displayColor')
ps.CreateInput('diffuseColor', C3).ConnectToSource(rc.CreateOutput('result', Sdf.ValueTypeNames.Float3))
rf = shader(path + '/fade', 'UsdPrimvarReader_float')
rf.CreateInput('varname', Sdf.ValueTypeNames.Token).Set('fade')
rf.CreateInput('fallback', F).Set(1.0)
ps.CreateInput('opacity', F).ConnectToSource(rf.CreateOutput('result', F))
mat.CreateSurfaceOutput().ConnectToSource(ps.CreateOutput('surface', Sdf.ValueTypeNames.Token))
m, pts = grid('/World/preview_fade', (X[1], BOT, 0.3), 1.1, 1.1)
primvar(m, 'displayColor', Sdf.ValueTypeNames.Color3fArray, VTX,
        Vt.Vec3fArray([Gf.Vec3f(0.9, 0.35 + 0.5 * (p[1] / 1.1 + 0.5), 0.2) for p in pts]))
primvar(m, 'fade', Sdf.ValueTypeNames.FloatArray, VTX,
        Vt.FloatArray([p[0] / 1.1 + 0.5 for p in pts]))
bind(m, mat)
boxes['preview_fade'] = ((X[1], BOT, 0.3), 0.5)

s = UsdGeom.Sphere.Define(stage, '/World/implicit_const')
s.CreateRadiusAttr(0.55)
s.CreateDisplayColorAttr([Gf.Vec3f(0.2, 0.5, 0.8)])
UsdGeom.XformCommonAPI(s).SetTranslate(Gf.Vec3d(X[2], BOT, 0))
boxes['implicit_const'] = ((X[2], BOT, 0), 0.55)

mat, _ = mtlx('missing', base_from=('nothere', (0.9, 0.6, 0.1)))
m, pts = uv_sphere('/World/missing', (X[3], BOT, 0), 0.55)
bind(m, mat)
boxes['missing'] = ((X[3], BOT, 0), 0.55)

rs = UsdRender.Settings.Define(stage, '/Render/settings')
rs.CreateCameraRel().SetTargets([cam.GetPath()])
rs.CreateResolutionAttr(Gf.Vec2i(*RES))
stage.GetRootLayer().Save()

# Pixel boxes (y down) from the camera's frustum.
fr = cam.GetCamera(1).frustum
vp = fr.ComputeViewMatrix() * fr.ComputeProjectionMatrix()
pix = {}
for name, (c, r) in boxes.items():
    xs, ys = [], []
    for dx in (-r, r):
        for dy in (-r, r):
            q = vp.Transform(Gf.Vec3d(c[0] + dx, c[1] + dy, c[2]))
            xs.append((q[0] * 0.5 + 0.5) * RES[0]); ys.append((0.5 - q[1] * 0.5) * RES[1])
    pix[name] = [int(min(xs)), int(min(ys)), int(max(xs)) + 1, int(max(ys)) + 1]
json.dump(pix, open(os.path.join(out, 'primvars_boxes.json'), 'w'), indent=1)
print('wrote', stage.GetRootLayer().realPath)
