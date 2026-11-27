# hython tests/displacement/make_vector_scene.py <outdir> [imgtool]
# Vector displacement test scene (MaterialX ND_displacement_vector3, a vector
# in (dPdu, dPdv, N) space): a ground grid of mushrooms (overhangs that a
# height map can't make; raw signed vectors -> multiply_vector3FA) and a
# catmullClark sphere with twisted ribs (normal ribs sheared along dPdu by an
# amount that grows with v; midlevel 0.5 -> subtract_vector3), in front of a
# bright dome.
import math, os, struct, subprocess, sys
from pxr import Gf, Sdf, Usd, UsdGeom, UsdLux, UsdRender, UsdShade

out = os.path.abspath(sys.argv[1])
imgtool = sys.argv[2] if len(sys.argv) > 2 else 'imgtool'
os.makedirs(out, exist_ok=True)


def write_map(name, n, fn):
    pfm = os.path.join(out, name + '.pfm')
    with open(pfm, 'wb') as f:
        f.write(b'PF\n%d %d\n-1\n' % (n, n))
        for y in range(n):  # PFM rows bottom first; v up
            row = []
            for x in range(n):
                row += fn((x + 0.5) / n, (y + 0.5) / n)
            f.write(struct.pack('<%df' % (3 * n), *row))
    exr = os.path.join(out, name + '.exr')
    subprocess.run([imgtool, 'convert', pfm, '--outfile', exr], check=True)


# Mushrooms: GROUND m grid, CELLS x CELLS cells. The profile (radius, height)
# maps distance s = r / R0 from the cell centre to a point on the mushroom.
GROUND, CELLS = 4.0, 4
R0, H = 0.45, 0.45
PROFILE = [(0.0, (0.0, H + 0.15)), (0.45, (0.38, H)), (0.6, (0.1, H - 0.05)),
           (0.85, (0.1, 0.03)), (1.0, (R0, 0.0))]


def mushroom(u, v):
    cell = GROUND / CELLS
    cx = ((u * GROUND) % cell - cell / 2)
    cy = ((v * GROUND) % cell - cell / 2)
    r = math.hypot(cx, cy)
    s = r / R0
    if s >= 1:
        return [0.0, 0.0, 0.0]
    for (s0, (r0, h0)), (s1, (r1, h1)) in zip(PROFILE, PROFILE[1:]):
        if s <= s1:
            t = (s - s0) / (s1 - s0)
            pr, ph = r0 + t * (r1 - r0), h0 + t * (h1 - h0)
            break
    dr = pr - r
    du, dv = (dr * cx / r, dr * cy / r) if r > 1e-6 else (0.0, 0.0)
    return [2 * du, 2 * dv, 2 * ph]  # halved again by multiply_vector3FA


def ribs(u, v):
    rib = 0.5 + 0.5 * math.cos(2 * math.pi * 12 * u)
    taper = math.sin(math.pi * v)
    return [0.5 + 1.2 * (v - 0.5) * taper, 0.5, 0.5 + 0.5 * rib ** 3 * taper]


write_map('mushroom', 512, mushroom)
write_map('ribs', 512, ribs)

stage = Usd.Stage.CreateNew(os.path.join(out, 'vector.usda'))
UsdGeom.SetStageUpAxis(stage, UsdGeom.Tokens.y)
UsdGeom.SetStageMetersPerUnit(stage, 1)
UsdGeom.Xform.Define(stage, '/World')
cam = UsdGeom.Camera.Define(stage, '/World/cam')
cam.CreateFocalLengthAttr(35); cam.CreateHorizontalApertureAttr(36); cam.CreateVerticalApertureAttr(24)
cx = UsdGeom.XformCommonAPI(cam)
cx.SetTranslate(Gf.Vec3d(0, 1.6, 5.5)); cx.SetRotate(Gf.Vec3f(-12, 0, 0))
dome = UsdLux.DomeLight.Define(stage, '/World/dome')
dome.CreateIntensityAttr(0.6); dome.CreateColorAttr(Gf.Vec3f(0.7, 0.8, 1.0))
sun = UsdLux.DistantLight.Define(stage, '/World/sun')
sun.CreateIntensityAttr(2.5); sun.CreateAngleAttr(1)
UsdGeom.XformCommonAPI(sun).SetRotate(Gf.Vec3f(-50, 35, 0))


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


V3 = Sdf.ValueTypeNames.Vector3f


def vector_material(path, color, file, op, k, scale):
    """image (vector3) -> op (constant k) -> ND_displacement_vector3."""
    mat = UsdShade.Material.Define(stage, path)
    surf = UsdShade.Shader.Define(stage, path + '/surf')
    surf.CreateIdAttr('ND_standard_surface_surfaceshader')
    surf.CreateInput('base_color', Sdf.ValueTypeNames.Color3f).Set(Gf.Vec3f(*color))
    surf.CreateInput('specular_roughness', Sdf.ValueTypeNames.Float).Set(0.4)
    so = surf.CreateOutput('out', Sdf.ValueTypeNames.Token)
    img = UsdShade.Shader.Define(stage, path + '/vectors')
    img.CreateIdAttr('ND_image_vector3')
    img.CreateInput('file', Sdf.ValueTypeNames.Asset).Set('./' + file)
    io = img.CreateOutput('out', V3)
    o = UsdShade.Shader.Define(stage, path + '/op')
    o.CreateIdAttr(op)
    o.CreateInput('in1', V3).ConnectToSource(io)
    if isinstance(k, float):
        o.CreateInput('in2', Sdf.ValueTypeNames.Float).Set(k)
    else:
        o.CreateInput('in2', V3).Set(Gf.Vec3f(*k))
    oo = o.CreateOutput('out', V3)
    disp = UsdShade.Shader.Define(stage, path + '/disp')
    disp.CreateIdAttr('ND_displacement_vector3')
    disp.CreateInput('displacement', V3).ConnectToSource(oo)
    disp.CreateInput('scale', Sdf.ValueTypeNames.Float).Set(scale)
    do = disp.CreateOutput('out', Sdf.ValueTypeNames.Token)
    mat.CreateSurfaceOutput('mtlx').ConnectToSource(so)
    mat.CreateDisplacementOutput('mtlx').ConnectToSource(do)
    return mat


def bind(prim, mat):
    UsdShade.MaterialBindingAPI.Apply(prim.GetPrim()).Bind(mat)


m_ground = vector_material('/World/mtl/mushrooms', (0.75, 0.4, 0.3), 'mushroom.exr',
                           'ND_multiply_vector3FA', 0.5, 1.0)
m_sphere = vector_material('/World/mtl/ribs', (0.3, 0.55, 0.8), 'ribs.exr',
                           'ND_subtract_vector3', (0.5, 0.5, 0.5), 0.5)
bind(grid('/World/ground', GROUND, 8, 0), m_ground)
bind(uv_sphere('/World/sphere', (-1.3, 1.25, 1.2), 0.55), m_sphere)

s = UsdRender.Settings.Define(stage, '/Render/settings')
s.CreateCameraRel().SetTargets([cam.GetPath()])
s.CreateResolutionAttr(Gf.Vec2i(640, 427))
stage.GetRootLayer().Save()
print('wrote', stage.GetRootLayer().realPath)
