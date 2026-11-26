# hython tests/displacement/make_displacement_scene.py <outdir> [imgtool]
# True displacement test scene (pbrt patch 39): a height map (bumps.exr), a
# catmullClark sphere and a plane with MaterialX displacement (image ->
# subtract 0.5 -> ND_displacement_float), a cube with face-varying uvs (seams:
# must not crack) and a sphere with UsdPreviewSurface displacement (UsdUVTexture
# scale/bias), in front of a bright dome.
import math, os, struct, subprocess, sys
from pxr import Gf, Sdf, Usd, UsdGeom, UsdLux, UsdShade, Vt

out = os.path.abspath(sys.argv[1])
imgtool = sys.argv[2] if len(sys.argv) > 2 else 'imgtool'
os.makedirs(out, exist_ok=True)

# Height map: a grid of round bumps plus a ridge ring, values 0..1 (0.5 = flat).
N = 512
pix = []
for y in range(N):
    for x in range(N):
        u, v = (x + 0.5) / N, (y + 0.5) / N
        cu, cv = (u * 8) % 1 - 0.5, (v * 4) % 1 - 0.5
        r = math.sqrt(cu * cu + cv * cv)
        h = 0.5 + 0.5 * max(0.0, 1 - r / 0.35) ** 2
        ring = abs(math.sqrt((u - 0.5) ** 2 + (v - 0.5) ** 2) - 0.3)
        h -= 0.4 * max(0.0, 1 - ring / 0.02)
        pix.append(min(max(h, 0.0), 1.0))
pfm = os.path.join(out, 'bumps.pfm')
with open(pfm, 'wb') as f:
    f.write(b'Pf\n%d %d\n-1\n' % (N, N))
    for y in range(N):  # PFM rows bottom first; v up
        f.write(struct.pack('<%df' % N, *pix[y * N:(y + 1) * N]))
exr = os.path.join(out, 'bumps.exr')
subprocess.run([imgtool, 'convert', pfm, '--outfile', exr], check=True)

stage = Usd.Stage.CreateNew(os.path.join(out, 'displacement.usda'))
UsdGeom.SetStageUpAxis(stage, UsdGeom.Tokens.y)
UsdGeom.SetStageMetersPerUnit(stage, 1)
world = UsdGeom.Xform.Define(stage, '/World')
cam = UsdGeom.Camera.Define(stage, '/World/cam')
cam.CreateFocalLengthAttr(35); cam.CreateHorizontalApertureAttr(36); cam.CreateVerticalApertureAttr(24)
cx = UsdGeom.XformCommonAPI(cam)
cx.SetTranslate(Gf.Vec3d(0, 2.2, 7.5)); cx.SetRotate(Gf.Vec3f(-14, 0, 0))
dome = UsdLux.DomeLight.Define(stage, '/World/dome')
dome.CreateIntensityAttr(0.6); dome.CreateColorAttr(Gf.Vec3f(0.7, 0.8, 1.0))
sun = UsdLux.DistantLight.Define(stage, '/World/sun')
sun.CreateIntensityAttr(2.5); sun.CreateAngleAttr(1)
UsdGeom.XformCommonAPI(sun).SetRotate(Gf.Vec3f(-50, 35, 0))


def uv_sphere(path, center, radius, nu=24, nv=12, scheme='catmullClark'):
    """Quads with face-varying st (a seam at u=0/1)."""
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
    m.CreateSubdivisionSchemeAttr(scheme)
    pv = UsdGeom.PrimvarsAPI(m).CreatePrimvar('st', Sdf.ValueTypeNames.TexCoord2fArray,
                                             UsdGeom.Tokens.faceVarying)
    pv.Set(st)
    UsdGeom.XformCommonAPI(m).SetTranslate(Gf.Vec3d(*center))
    return m


def grid(path, size, n, y):
    pts, counts, idx, st = [], [], [], []
    for j in range(n + 1):
        for i in range(n + 1):
            pts.append(Gf.Vec3f(size * (i / n - 0.5), y, size * (j / n - 0.5)))
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
    return m


def cube(path, center, s):
    """Six faces, each with its own 0..1 uvs (face-varying: every edge is a seam)."""
    h = s / 2
    pts = [Gf.Vec3f(x, y, z) for x in (-h, h) for y in (-h, h) for z in (-h, h)]
    faces = [[0, 1, 3, 2], [4, 6, 7, 5], [0, 4, 5, 1], [2, 3, 7, 6], [0, 2, 6, 4], [1, 5, 7, 3]]
    m = UsdGeom.Mesh.Define(stage, path)
    m.CreatePointsAttr(pts); m.CreateFaceVertexCountsAttr([4] * 6)
    m.CreateFaceVertexIndicesAttr(sum(faces, []))
    m.CreateSubdivisionSchemeAttr('none')
    st = [Gf.Vec2f(0, 0), Gf.Vec2f(1, 0), Gf.Vec2f(1, 1), Gf.Vec2f(0, 1)] * 6
    UsdGeom.PrimvarsAPI(m).CreatePrimvar('st', Sdf.ValueTypeNames.TexCoord2fArray,
                                         UsdGeom.Tokens.faceVarying).Set(st)
    UsdGeom.XformCommonAPI(m).SetTranslate(Gf.Vec3d(*center))
    UsdGeom.XformCommonAPI(m).SetRotate(Gf.Vec3f(0, 30, 0))
    return m


def mtlx_material(path, color, scale):
    mat = UsdShade.Material.Define(stage, path)
    surf = UsdShade.Shader.Define(stage, path + '/surf')
    surf.CreateIdAttr('ND_standard_surface_surfaceshader')
    surf.CreateInput('base_color', Sdf.ValueTypeNames.Color3f).Set(Gf.Vec3f(*color))
    surf.CreateInput('specular_roughness', Sdf.ValueTypeNames.Float).Set(0.4)
    so = surf.CreateOutput('out', Sdf.ValueTypeNames.Token)
    img = UsdShade.Shader.Define(stage, path + '/height')
    img.CreateIdAttr('ND_image_float')
    img.CreateInput('file', Sdf.ValueTypeNames.Asset).Set('./bumps.exr')
    io = img.CreateOutput('out', Sdf.ValueTypeNames.Float)
    sub = UsdShade.Shader.Define(stage, path + '/center')
    sub.CreateIdAttr('ND_subtract_float')
    sub.CreateInput('in1', Sdf.ValueTypeNames.Float).ConnectToSource(io)
    sub.CreateInput('in2', Sdf.ValueTypeNames.Float).Set(0.5)
    sbo = sub.CreateOutput('out', Sdf.ValueTypeNames.Float)
    disp = UsdShade.Shader.Define(stage, path + '/disp')
    disp.CreateIdAttr('ND_displacement_float')
    disp.CreateInput('displacement', Sdf.ValueTypeNames.Float).ConnectToSource(sbo)
    disp.CreateInput('scale', Sdf.ValueTypeNames.Float).Set(scale)
    do = disp.CreateOutput('out', Sdf.ValueTypeNames.Token)
    mat.CreateSurfaceOutput('mtlx').ConnectToSource(so)
    mat.CreateDisplacementOutput('mtlx').ConnectToSource(do)
    return mat


def preview_material(path, color, scale):
    mat = UsdShade.Material.Define(stage, path)
    surf = UsdShade.Shader.Define(stage, path + '/surf')
    surf.CreateIdAttr('UsdPreviewSurface')
    surf.CreateInput('diffuseColor', Sdf.ValueTypeNames.Color3f).Set(Gf.Vec3f(*color))
    st = UsdShade.Shader.Define(stage, path + '/st')
    st.CreateIdAttr('UsdPrimvarReader_float2')
    st.CreateInput('varname', Sdf.ValueTypeNames.String).Set('st')
    tex = UsdShade.Shader.Define(stage, path + '/height')
    tex.CreateIdAttr('UsdUVTexture')
    tex.CreateInput('file', Sdf.ValueTypeNames.Asset).Set('./bumps.exr')
    tex.CreateInput('sourceColorSpace', Sdf.ValueTypeNames.Token).Set('raw')
    tex.CreateInput('st', Sdf.ValueTypeNames.Float2).ConnectToSource(
        st.CreateOutput('result', Sdf.ValueTypeNames.Float2))
    tex.CreateInput('scale', Sdf.ValueTypeNames.Float4).Set(Gf.Vec4f(scale))
    tex.CreateInput('bias', Sdf.ValueTypeNames.Float4).Set(Gf.Vec4f(-0.5 * scale))
    surf.CreateInput('displacement', Sdf.ValueTypeNames.Float).ConnectToSource(
        tex.CreateOutput('r', Sdf.ValueTypeNames.Float))
    so = surf.CreateOutput('surface', Sdf.ValueTypeNames.Token)
    mat.CreateSurfaceOutput().ConnectToSource(so)
    mat.CreateDisplacementOutput().ConnectToSource(surf.CreateOutput('displacement', Sdf.ValueTypeNames.Token))
    return mat


def bind(prim, mat):
    UsdShade.MaterialBindingAPI.Apply(prim.GetPrim()).Bind(mat)


m_sphere = mtlx_material('/World/mtl/orange', (0.8, 0.35, 0.1), 0.25)
m_ground = mtlx_material('/World/mtl/ground', (0.45, 0.45, 0.42), 0.3)
m_cube = mtlx_material('/World/mtl/cube', (0.2, 0.5, 0.8), 0.15)
m_prev = preview_material('/World/mtl/preview', (0.3, 0.7, 0.3), 0.25)
bind(uv_sphere('/World/sphere', (-1.6, 1.1, 0), 1.0), m_sphere)
bind(uv_sphere('/World/previewSphere', (1.6, 1.1, 0), 1.0), m_prev)
bind(cube('/World/cube', (0, 0.75, 2.0), 1.0), m_cube)
bind(grid('/World/ground', 12, 8, 0), m_ground)

rs = stage.DefinePrim('/Render/settings', 'RenderSettings')
rs.CreateAttribute('camera', Sdf.ValueTypeNames.Relationship) if False else None
from pxr import UsdRender
s = UsdRender.Settings(rs)
s.CreateCameraRel().SetTargets([cam.GetPath()])
s.CreateResolutionAttr(Gf.Vec2i(640, 427))
stage.GetRootLayer().Save()
print('wrote', stage.GetRootLayer().realPath)
