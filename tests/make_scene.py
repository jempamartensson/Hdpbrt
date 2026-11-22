# Builds test/test_scene.usda plus its textures. Run with hython.
import os, struct, zlib, math
from pxr import Usd, UsdGeom, UsdLux, UsdShade, Sdf, Gf, Vt

here = os.path.dirname(os.path.abspath(__file__))

def write_png(path, w, h, pix):  # pix(x, y) -> (r,g,b) 0..255, y=0 top
    raw = b''.join(b'\x00' + bytes(sum((pix(x, y) for x in range(w)), ())) for y in range(h))
    def chunk(t, d):
        return struct.pack('>I', len(d)) + t + d + struct.pack('>I', zlib.crc32(t + d) & 0xffffffff)
    open(path, 'wb').write(b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', w, h, 8, 2, 0, 0, 0))
                           + chunk(b'IDAT', zlib.compress(raw)) + chunk(b'IEND', b''))

def write_pfm(path, w, h, pix):  # pix(u, v) -> (r,g,b), v=0 top
    with open(path, 'wb') as f:
        f.write(b'PF\n%d %d\n-1.0\n' % (w, h))
        for y in reversed(range(h)):  # PFM is bottom to top
            for x in range(w):
                f.write(struct.pack('<3f', *pix((x + .5) / w, (y + .5) / h)))

# Checker with an "F" marker in the top-left texel block to show orientation.
def checker(x, y):
    if x < 32 and y < 32:
        return (255, 255, 0)
    return (230, 230, 230) if ((x // 32) + (y // 32)) % 2 == 0 else (40, 40, 160)
write_png(os.path.join(here, 'checker.png'), 256, 256, checker)

# Lat-long quadrants: u in [0,.25) red, [.25,.5) green, [.5,.75) blue, [.75,1) yellow; v<.15 white.
def env(u, v):
    if v < 0.15:
        return (2.0, 2.0, 2.0)
    return [(1, .1, .1), (.1, 1, .1), (.1, .1, 1), (1, 1, .1)][min(int(u * 4), 3)]
write_pfm(os.path.join(here, 'quadrants.pfm'), 256, 128, env)

s = Usd.Stage.CreateNew(os.path.join(here, 'test_scene.usda'))
UsdGeom.SetStageUpAxis(s, 'Y')
UsdGeom.Xform.Define(s, '/World')

def preview(path, **inputs):
    m = UsdShade.Material.Define(s, path)
    sh = UsdShade.Shader.Define(s, path + '/pbs')
    sh.CreateIdAttr('UsdPreviewSurface')
    for k, v in inputs.items():
        t = Sdf.ValueTypeNames.Color3f if isinstance(v, tuple) else Sdf.ValueTypeNames.Float
        sh.CreateInput(k, t).Set(v)
    m.CreateSurfaceOutput().ConnectToSource(sh.ConnectableAPI(), 'surface')
    return m, sh

def bind(prim, m):
    UsdShade.MaterialBindingAPI.Apply(prim.GetPrim()).Bind(m)

# Ground with checker texture.
g = UsdGeom.Mesh.Define(s, '/World/ground')
g.CreatePointsAttr([(-6, 0, -6), (6, 0, -6), (6, 0, 6), (-6, 0, 6)])
g.CreateFaceVertexCountsAttr([4]); g.CreateFaceVertexIndicesAttr([0, 3, 2, 1])
g.CreateSubdivisionSchemeAttr('none')
st = UsdGeom.PrimvarsAPI(g).CreatePrimvar('st', Sdf.ValueTypeNames.TexCoord2fArray, UsdGeom.Tokens.faceVarying)
st.Set([(0, 2), (0, 0), (2, 0), (2, 2)])  # 2x2 repeats; v up toward -Z
gm, gs = preview('/World/mtl/ground', roughness=0.8)
reader = UsdShade.Shader.Define(s, '/World/mtl/ground/st'); reader.CreateIdAttr('UsdPrimvarReader_float2')
reader.CreateInput('varname', Sdf.ValueTypeNames.Token).Set('st')
tex = UsdShade.Shader.Define(s, '/World/mtl/ground/tex'); tex.CreateIdAttr('UsdUVTexture')
tex.CreateInput('file', Sdf.ValueTypeNames.Asset).Set('./checker.png')
tex.CreateInput('st', Sdf.ValueTypeNames.Float2).ConnectToSource(reader.ConnectableAPI(), 'result')
tex.CreateInput('wrapS', Sdf.ValueTypeNames.Token).Set('repeat')
gs.CreateInput('diffuseColor', Sdf.ValueTypeNames.Color3f).ConnectToSource(tex.ConnectableAPI(), 'rgb')
bind(g, gm)

def sphere(path, pos, r, m):
    sp = UsdGeom.Sphere.Define(s, path); sp.CreateRadiusAttr(r)
    sp.AddTranslateOp().Set(pos)
    if m: bind(sp, m)
    return sp

sphere('/World/red', (-2.2, 0.7, 0), 0.7, preview('/World/mtl/red', diffuseColor=(0.8, 0.1, 0.05), roughness=0.4)[0])
sphere('/World/gold', (0, 0.9, -0.5), 0.9, preview('/World/mtl/gold', diffuseColor=(1.0, 0.75, 0.3), metallic=1.0, roughness=0.15)[0])

# Glass via MaterialX standard_surface.
mx = UsdShade.Material.Define(s, '/World/mtl/glass')
sh = UsdShade.Shader.Define(s, '/World/mtl/glass/ss'); sh.CreateIdAttr('ND_standard_surface_surfaceshader')
sh.CreateInput('transmission', Sdf.ValueTypeNames.Float).Set(1.0)
sh.CreateInput('specular_roughness', Sdf.ValueTypeNames.Float).Set(0.0)
sh.CreateInput('specular_IOR', Sdf.ValueTypeNames.Float).Set(1.5)
mx.CreateSurfaceOutput('mtlx').ConnectToSource(sh.ConnectableAPI(), 'out')
sphere('/World/glass', (2.2, 0.7, 0.3), 0.7, mx)

# Emissive panel at the back right.
e = UsdGeom.Mesh.Define(s, '/World/panel')
e.CreatePointsAttr([(2.5, 0.2, -3), (4.5, 0.2, -3), (4.5, 1.6, -3), (2.5, 1.6, -3)])
e.CreateFaceVertexCountsAttr([4]); e.CreateFaceVertexIndicesAttr([0, 1, 2, 3])
bind(e, preview('/World/mtl/emit', diffuseColor=(0, 0, 0), emissiveColor=(4, 1.5, 0.3))[0])

# Point instancer: a row of small blue cubes.
pi = UsdGeom.PointInstancer.Define(s, '/World/cubes')
proto = UsdGeom.Cube.Define(s, '/World/cubes/proto/cube'); proto.CreateSizeAttr(0.35)
proto.CreateDisplayColorAttr([(0.1, 0.3, 0.9)])
pi.CreatePrototypesRel().SetTargets([proto.GetPath()])
pi.CreateProtoIndicesAttr([0] * 7)
pi.CreatePositionsAttr([(-3 + i, 0.175, 2.0) for i in range(7)])
pi.CreateOrientationsAttr([Gf.Quath(Gf.Rotation(Gf.Vec3d(0, 1, 0), 15 * i).GetQuat()) for i in range(7)])

# Lights.
rl = UsdLux.RectLight.Define(s, '/World/rect')
rl.CreateWidthAttr(2); rl.CreateHeightAttr(2); rl.CreateIntensityAttr(4)
rx = UsdGeom.Xformable(rl); rx.AddTranslateOp().Set((0, 4, 1)); rx.AddRotateXOp().Set(-90)
sl = UsdLux.SphereLight.Define(s, '/World/orb')
sl.CreateRadiusAttr(0.15); sl.CreateIntensityAttr(40); sl.CreateColorAttr((1, 0.5, 0.2))
UsdGeom.Xformable(sl).AddTranslateOp().Set((-1.2, 0.5, 1.4))
d = UsdLux.DomeLight.Define(s, '/World/dome'); d.CreateIntensityAttr(0.6)
d.CreateTextureFileAttr('./quadrants.pfm')

c = UsdGeom.Camera.Define(s, '/World/cam')
c.AddTranslateOp().Set((0, 2.2, 8)); c.AddRotateXOp().Set(-12)
c.CreateFocalLengthAttr(35)
s.GetRootLayer().Save()
print('wrote', s.GetRootLayer().realPath)
