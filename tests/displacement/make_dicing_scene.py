# hython tests/displacement/make_dicing_scene.py <outdir>
# Per-object dicing primvars: four spheres with the same procedural
# displacement (noise3d), left to right
# - no primvar (the render setting pbrt:dicingRate, default 2 px);
# - constant primvars:pbrt:dicingRate 16 (coarse: visibly faceted);
# - constant primvars:karma:object:dicingquality 0.25 (rate 2 / 0.25 = 8 px);
# - constant primvars:pbrt_dicingRate 0.5 (SOP attribute spelling; finer).
import math, os, sys
from pxr import Gf, Sdf, Usd, UsdGeom, UsdLux, UsdRender, UsdShade

out = os.path.abspath(sys.argv[1])
os.makedirs(out, exist_ok=True)
stage = Usd.Stage.CreateNew(os.path.join(out, 'dicing.usda'))
UsdGeom.SetStageUpAxis(stage, UsdGeom.Tokens.y)
UsdGeom.SetStageMetersPerUnit(stage, 1)
UsdGeom.Xform.Define(stage, '/World')
cam = UsdGeom.Camera.Define(stage, '/World/cam')
cam.CreateFocalLengthAttr(35); cam.CreateHorizontalApertureAttr(36); cam.CreateVerticalApertureAttr(24)
UsdGeom.XformCommonAPI(cam).SetTranslate(Gf.Vec3d(0, 0, 6.0))
UsdLux.DomeLight.Define(stage, '/World/dome').CreateIntensityAttr(0.6)
sun = UsdLux.DistantLight.Define(stage, '/World/sun')
sun.CreateIntensityAttr(2.5); sun.CreateAngleAttr(1)
UsdGeom.XformCommonAPI(sun).SetRotate(Gf.Vec3f(-40, 30, 0))
F, V3 = Sdf.ValueTypeNames.Float, Sdf.ValueTypeNames.Vector3f

mat = UsdShade.Material.Define(stage, '/World/mtl/noise')


def node(name, nid, typ):
    s = UsdShade.Shader.Define(stage, '/World/mtl/noise/' + name)
    s.CreateIdAttr(nid)
    return s, s.CreateOutput('out', typ)


surf, so = node('surf', 'ND_standard_surface_surfaceshader', Sdf.ValueTypeNames.Token)
surf.CreateInput('base_color', Sdf.ValueTypeNames.Color3f).Set(Gf.Vec3f(0.8, 0.5, 0.3))
mat.CreateSurfaceOutput('mtlx').ConnectToSource(so)
pos, po = node('pos', 'ND_position_vector3', V3)
sc, sco = node('scale', 'ND_multiply_vector3FA', V3)
sc.CreateInput('in1', V3).ConnectToSource(po); sc.CreateInput('in2', F).Set(4.0)
nz, no = node('noise', 'ND_noise3d_float', F)
nz.CreateInput('position', V3).ConnectToSource(sco)
d, do = node('disp', 'ND_displacement_float', Sdf.ValueTypeNames.Token)
d.CreateInput('displacement', F).ConnectToSource(no); d.CreateInput('scale', F).Set(0.15)
mat.CreateDisplacementOutput('mtlx').ConnectToSource(do)


def uv_sphere(path, x, r=0.6, nu=24, nv=12):
    pts, counts, idx, st = [], [], [], []
    for j in range(nv + 1):
        th = math.pi * j / nv
        for i in range(nu):
            ph = 2 * math.pi * i / nu
            pts.append(Gf.Vec3f(r * math.sin(th) * math.cos(ph), r * math.cos(th),
                                -r * math.sin(th) * math.sin(ph)))
    for j in range(nv):
        for i in range(nu):
            a, b = j * nu + i, j * nu + (i + 1) % nu
            c, e = (j + 1) * nu + (i + 1) % nu, (j + 1) * nu + i
            counts.append(4); idx += [a, e, c, b]
            u0, u1, v0, v1 = i / nu, (i + 1) / nu, 1 - j / nv, 1 - (j + 1) / nv
            st += [Gf.Vec2f(u0, v0), Gf.Vec2f(u0, v1), Gf.Vec2f(u1, v1), Gf.Vec2f(u1, v0)]
    m = UsdGeom.Mesh.Define(stage, path)
    m.CreatePointsAttr(pts); m.CreateFaceVertexCountsAttr(counts); m.CreateFaceVertexIndicesAttr(idx)
    m.CreateSubdivisionSchemeAttr('catmullClark')
    UsdGeom.PrimvarsAPI(m).CreatePrimvar('st', Sdf.ValueTypeNames.TexCoord2fArray,
                                         UsdGeom.Tokens.faceVarying).Set(st)
    UsdGeom.XformCommonAPI(m).SetTranslate(Gf.Vec3d(x, 0, 0))
    UsdShade.MaterialBindingAPI.Apply(m.GetPrim()).Bind(mat)
    return UsdGeom.PrimvarsAPI(m)


uv_sphere('/World/default', -2.1)
uv_sphere('/World/coarse', -0.7).CreatePrimvar('pbrt:dicingRate', F).Set(16.0)
uv_sphere('/World/quality', 0.7).CreatePrimvar('karma:object:dicingquality', F).Set(0.25)
uv_sphere('/World/fine', 2.1).CreatePrimvar('pbrt_dicingRate', F).Set(0.5)

rs = UsdRender.Settings.Define(stage, '/Render/settings')
rs.CreateCameraRel().SetTargets([cam.GetPath()])
rs.CreateResolutionAttr(Gf.Vec2i(640, 427))
stage.GetRootLayer().Save()
print('wrote', stage.GetRootLayer().realPath)
