# hython tests/primvars/make_inputs_scene.py <outdir>
# Primvars on every texturable surface input. 19 spheres, each with one
# standard_surface / OpenPBR / UsdPreviewSurface input driven by a primvar
# reader of "pv" (geompropvalue, UsdPrimvarReader_float): value A on
# the upper half, B on the lower (a vertex primvar, blended over the equator
# ring). Writes inputs_pv.usda and the same spheres with the input set to the
# constant A (inputs_a.usda) and B (inputs_b.usda), plus inputs_patches.json
# (an upper and a lower patch per sphere): the primvar render's upper patch
# should equal the A render there and the lower one the B render (up to light
# from the neighbours, whose halves differ too). inputs_pva / inputs_pvb read
# the primvar but hold A (B) everywhere: they must equal inputs_a (inputs_b)
# up to sampling noise, which checks each varying input against its constant.
import json, math, os, sys
from pxr import Gf, Sdf, Usd, UsdGeom, UsdLux, UsdRender, UsdShade, Vt

out = os.path.abspath(sys.argv[1])
os.makedirs(out, exist_ok=True)
RES = (800, 600)
F, C3, S = Sdf.ValueTypeNames.Float, Sdf.ValueTypeNames.Color3f, Sdf.ValueTypeNames.String
SS, OP, PS = 'ND_standard_surface_surfaceshader', 'ND_open_pbr_surface_surfaceshader', 'UsdPreviewSurface'
# (shader, input, A, B, other inputs)
CASES = [
    (SS, 'metalness', 0.0, 1.0, {'base_color': (0.9, 0.6, 0.3)}),
    (SS, 'specular', 1.0, 0.0, {'base_color': (0.1, 0.1, 0.1), 'specular_roughness': 0.2}),
    (SS, 'specular_color', (1, 1, 1), (1, 0.2, 0.2), {'base_color': (0.05, 0.05, 0.05), 'specular_roughness': 0.2}),
    (SS, 'base', 1.0, 0.3, {'base_color': (0.2, 0.6, 0.9)}),
    (SS, 'coat', 0.0, 1.0, {'base_color': (0.6, 0.1, 0.1), 'coat_color': (0.5, 0.8, 1.0), 'specular_roughness': 0.6}),
    (SS, 'coat_color', (1, 1, 1), (0.2, 0.5, 1.0), {'coat': 1.0, 'base_color': (0.8, 0.8, 0.8)}),
    (SS, 'coat_roughness', 0.0, 0.6, {'coat': 1.0, 'base_color': (0.1, 0.1, 0.1)}),
    (SS, 'sheen', 0.0, 1.0, {'sheen_color': (1.0, 0.4, 0.8), 'base_color': (0.1, 0.1, 0.3)}),
    (SS, 'sheen_color', (1, 1, 1), (1.0, 0.5, 0.0), {'sheen': 1.0, 'base_color': (0.1, 0.1, 0.1)}),
    (SS, 'diffuse_roughness', 0.0, 1.0, {'base_color': (0.8, 0.8, 0.8), 'specular': 0.0}),
    (SS, 'transmission_color', (1, 1, 1), (0.3, 0.9, 0.3), {'transmission': 1.0, 'specular_roughness': 0.1}),
    (OP, 'base_metalness', 0.0, 1.0, {'base_color': (0.9, 0.6, 0.3)}),
    (OP, 'specular_weight', 1.0, 0.0, {'base_color': (0.1, 0.1, 0.1), 'specular_roughness': 0.2}),
    (OP, 'coat_weight', 0.0, 1.0, {'base_color': (0.6, 0.1, 0.1), 'coat_color': (0.5, 0.8, 1.0)}),
    (OP, 'fuzz_weight', 0.0, 1.0, {'fuzz_color': (1.0, 0.4, 0.8), 'base_color': (0.1, 0.1, 0.3)}),
    (OP, 'specular_color', (1, 1, 1), (0.2, 1.0, 0.2), {'base_metalness': 1.0, 'base_color': (0.8, 0.8, 0.8)}),
    (PS, 'metallic', 0.0, 1.0, {'diffuseColor': (0.9, 0.6, 0.3), 'roughness': 0.3}),
    (PS, 'clearcoat', 0.0, 1.0, {'diffuseColor': (0.6, 0.1, 0.1), 'roughness': 0.6}),
    (PS, 'clearcoatRoughness', 0.0, 0.6, {'diffuseColor': (0.1, 0.1, 0.1), 'clearcoat': 1.0}),
]


def sphere_points(radius, nu, nv):
    pts = []
    for j in range(nv + 1):
        th = math.pi * j / nv
        for i in range(nu):
            ph = 2 * math.pi * i / nu
            pts.append(Gf.Vec3f(radius * math.sin(th) * math.cos(ph),
                                radius * math.cos(th), -radius * math.sin(th) * math.sin(ph)))
    return pts


def uv_sphere(stage, path, center, radius, nu=32, nv=16):
    pts = sphere_points(radius, nu, nv)
    counts, idx = [], []
    for j in range(nv):
        for i in range(nu):
            a, b = j * nu + i, j * nu + (i + 1) % nu
            c, d = (j + 1) * nu + (i + 1) % nu, (j + 1) * nu + i
            counts.append(4); idx += [a, d, c, b]
    m = UsdGeom.Mesh.Define(stage, path)
    m.CreatePointsAttr(pts); m.CreateFaceVertexCountsAttr(counts); m.CreateFaceVertexIndicesAttr(idx)
    m.CreateSubdivisionSchemeAttr('catmullClark')
    UsdGeom.XformCommonAPI(m).SetTranslate(Gf.Vec3d(*center))
    return m, pts


def typed(v):
    return (C3, Gf.Vec3f(*v)) if isinstance(v, tuple) else (F, float(v))


def build(variant):
    stage = Usd.Stage.CreateNew(os.path.join(out, 'inputs_%s.usda' % variant))
    stage.SetStartTimeCode(1); stage.SetEndTimeCode(1)
    UsdGeom.SetStageUpAxis(stage, UsdGeom.Tokens.y)
    UsdGeom.SetStageMetersPerUnit(stage, 1)
    UsdGeom.Xform.Define(stage, '/World')
    cam = UsdGeom.Camera.Define(stage, '/World/cam')
    cam.CreateFocalLengthAttr(35); cam.CreateHorizontalApertureAttr(36); cam.CreateVerticalApertureAttr(27)
    UsdGeom.XformCommonAPI(cam).SetTranslate(Gf.Vec3d(0, 0, 16.0))
    dome = UsdLux.DomeLight.Define(stage, '/World/dome')
    dome.CreateIntensityAttr(0.6)
    sun = UsdLux.DistantLight.Define(stage, '/World/sun')
    sun.CreateIntensityAttr(2.5); sun.CreateAngleAttr(1)
    UsdGeom.XformCommonAPI(sun).SetRotate(Gf.Vec3f(-30, 25, 0))
    # No backdrop and wide spacing: little light between the spheres, whose
    # lower halves differ between the renders.
    patches = []
    for n, (sh, inp, a, b, others) in enumerate(CASES):
        x, y = -6.0 + 3.0 * (n % 5), 3.3 - 2.2 * (n // 5)
        path = '/World/mtl/m%d' % n
        mat = UsdShade.Material.Define(stage, path)
        surf = UsdShade.Shader.Define(stage, path + '/surf')
        surf.CreateIdAttr(sh)
        for k, v in others.items():
            t, val = typed(v)
            surf.CreateInput(k, t).Set(val)
        t, _ = typed(a)
        if variant.startswith('pv'):
            r = UsdShade.Shader.Define(stage, path + '/pv')
            if sh == PS:
                r.CreateIdAttr('UsdPrimvarReader_float')
                r.CreateInput('varname', Sdf.ValueTypeNames.Token).Set('pv')
                surf.CreateInput(inp, t).ConnectToSource(r.CreateOutput('result', t))
            else:
                r.CreateIdAttr('ND_geompropvalue_color3' if t == C3 else 'ND_geompropvalue_float')
                r.CreateInput('geomprop', S).Set('pv')
                surf.CreateInput(inp, t).ConnectToSource(r.CreateOutput('out', t))
        else:
            surf.CreateInput(inp, t).Set(typed(a if variant == 'a' else b)[1])
        if sh == PS:
            mat.CreateSurfaceOutput().ConnectToSource(surf.CreateOutput('surface', Sdf.ValueTypeNames.Token))
        else:
            mat.CreateSurfaceOutput('mtlx').ConnectToSource(surf.CreateOutput('out', Sdf.ValueTypeNames.Token))
        m, pts = uv_sphere(stage, '/World/s%d' % n, (x, y, 0), 0.7)
        if variant.startswith('pv'):
            vals = []
            for p in pts:
                f = min(max(0.5 - p[1] / 0.7 / 0.4, 0.0), 1.0)  # 0 above +0.2 r, 1 below -0.2 r
                f = {'pva': 0.0, 'pvb': 1.0}.get(variant, f)  # all A / all B
                if t == C3:
                    vals.append(Gf.Vec3f(*[a[i] * (1 - f) + b[i] * f for i in range(3)]))
                else:
                    vals.append(a * (1 - f) + b * f)
            pvar = UsdGeom.PrimvarsAPI(m).CreatePrimvar(
                'pv', Sdf.ValueTypeNames.Color3fArray if t == C3 else Sdf.ValueTypeNames.FloatArray,
                UsdGeom.Tokens.vertex)
            pvar.Set(Vt.Vec3fArray(vals) if t == C3 else Vt.FloatArray(vals))
        UsdShade.MaterialBindingAPI.Apply(m.GetPrim()).Bind(mat)
        label = '%s %s' % ({SS: 'ss', OP: 'openpbr', PS: 'preview'}[sh], inp)
        patches.append((label, (x - 0.15, y + 0.42, 0.5), (x - 0.15, y - 0.42, 0.5)))
    rs = UsdRender.Settings.Define(stage, '/Render/settings')
    rs.CreateCameraRel().SetTargets([cam.GetPath()])
    rs.CreateResolutionAttr(Gf.Vec2i(*RES))
    stage.GetRootLayer().Save()
    return cam.GetCamera(1).frustum, patches


for v in ('pv', 'a', 'b', 'pva', 'pvb'):
    fr, patches = build(v)
vp = fr.ComputeViewMatrix() * fr.ComputeProjectionMatrix()
def pix(w):
    q = vp.Transform(Gf.Vec3d(*w))
    return [int((q[0] * 0.5 + 0.5) * RES[0]), int((0.5 - q[1] * 0.5) * RES[1])]
json.dump([[l, pix(u), pix(d)] for l, u, d in patches],
          open(os.path.join(out, 'inputs_patches.json'), 'w'), indent=1)
print('wrote inputs_{pv,a,b}.usda')
