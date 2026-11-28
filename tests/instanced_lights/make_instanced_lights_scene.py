# hython tests/instanced_lights/make_instanced_lights_scene.py <outdir> <count> [flat]
# Instanced and moving emitters (pbrt patch 47). A PointInstancer of <count>
# small emissive quads (standard_surface emission, facing down, random
# rotation about the vertical and non-uniform scale) over a grey ground and a
# glossy sphere, plus one emissive quad that moves over the shutter (frame 1,
# shutter 0..1). With "flat", the instances are written as separate meshes
# with the same transforms (the reference). Writes instanced_lights_<count>
# [_flat].usda.
import math, os, random, sys
from pxr import Gf, Sdf, Usd, UsdGeom, UsdLux, UsdRender, UsdShade, Vt

out = os.path.abspath(sys.argv[1])
count = int(sys.argv[2])
flat = len(sys.argv) > 3 and sys.argv[3] == 'flat'
os.makedirs(out, exist_ok=True)
name = 'instanced_lights_%d%s.usda' % (count, '_flat' if flat else '')
stage = Usd.Stage.CreateNew(os.path.join(out, name))
stage.SetStartTimeCode(1)
stage.SetEndTimeCode(2)
UsdGeom.SetStageUpAxis(stage, UsdGeom.Tokens.y)
UsdGeom.SetStageMetersPerUnit(stage, 1)
UsdGeom.Xform.Define(stage, '/World')
cam = UsdGeom.Camera.Define(stage, '/World/cam')
cam.CreateFocalLengthAttr(30); cam.CreateHorizontalApertureAttr(36); cam.CreateVerticalApertureAttr(24)
cam.CreateShutterOpenAttr(0); cam.CreateShutterCloseAttr(1)
cx = UsdGeom.XformCommonAPI(cam)
cx.SetTranslate(Gf.Vec3d(0, 5.0, 9.0)); cx.SetRotate(Gf.Vec3f(-30, 0, 0))
# A dim dome so the ground isn't black where no emitter reaches.
dome = UsdLux.DomeLight.Define(stage, '/World/dome')
dome.CreateIntensityAttr(0.02)

F, C3 = Sdf.ValueTypeNames.Float, Sdf.ValueTypeNames.Color3f


def material(path, base, rough=0.5, emission=0.0, ecolor=(1, 1, 1)):
    mat = UsdShade.Material.Define(stage, path)
    s = UsdShade.Shader.Define(stage, path + '/surf')
    s.CreateIdAttr('ND_standard_surface_surfaceshader')
    s.CreateInput('base_color', C3).Set(Gf.Vec3f(*base))
    s.CreateInput('specular_roughness', F).Set(rough)
    if emission > 0:
        s.CreateInput('emission', F).Set(emission)
        s.CreateInput('emission_color', C3).Set(Gf.Vec3f(*ecolor))
        s.CreateInput('base', F).Set(0.0)
        s.CreateInput('specular', F).Set(0.0)
    mat.CreateSurfaceOutput('mtlx').ConnectToSource(s.CreateOutput('out', Sdf.ValueTypeNames.Token))
    return mat


def quad(path, half=0.12):
    m = UsdGeom.Mesh.Define(stage, path)
    # Facing down (-y): the winding makes the normal point down.
    m.CreatePointsAttr([(-half, 0, -half), (-half, 0, half), (half, 0, half), (half, 0, -half)])
    m.CreateFaceVertexCountsAttr([4]); m.CreateFaceVertexIndicesAttr([0, 3, 2, 1])
    m.CreateSubdivisionSchemeAttr('none')
    m.CreateDoubleSidedAttr(False)
    return m


def bind(prim, mat):
    UsdShade.MaterialBindingAPI.Apply(prim.GetPrim()).Bind(mat)


ground = UsdGeom.Mesh.Define(stage, '/World/ground')
ground.CreatePointsAttr([(-8, 0, -8), (8, 0, -8), (8, 0, 8), (-8, 0, 8)])
ground.CreateFaceVertexCountsAttr([4]); ground.CreateFaceVertexIndicesAttr([0, 3, 2, 1])
ground.CreateSubdivisionSchemeAttr('none')
bind(ground, material('/World/mtl/ground', (0.5, 0.5, 0.5), 0.8))
ball = UsdGeom.Sphere.Define(stage, '/World/ball')
ball.CreateRadiusAttr(0.8)
UsdGeom.XformCommonAPI(ball).SetTranslate(Gf.Vec3d(0, 0.8, 0))
bind(ball, material('/World/mtl/ball', (0.2, 0.3, 0.6), 0.25))

emit = material('/World/mtl/emit', (0, 0, 0), emission=1.0, ecolor=(6.0, 4.5, 3.0))

# Instance placement: a jittered grid on a ring of heights, random yaw and
# non-uniform scale (deterministic).
rnd = random.Random(7)
side = max(1, int(math.ceil(math.sqrt(count))))
positions, orients, scales = [], [], []
for i in range(count):
    gx, gz = i % side, i // side
    x = -5 + 10 * (gx + 0.5) / side + rnd.uniform(-0.1, 0.1) * 10 / side
    z = -5 + 10 * (gz + 0.5) / side + rnd.uniform(-0.1, 0.1) * 10 / side
    y = 2.2 + 0.6 * rnd.random()
    positions.append(Gf.Vec3f(x, y, z))
    yaw = rnd.uniform(0, 2 * math.pi)
    orients.append(Gf.Quath(math.cos(yaw / 2), 0, math.sin(yaw / 2), 0))
    scales.append(Gf.Vec3f(rnd.uniform(0.6, 1.8), 1, rnd.uniform(0.6, 1.4)) * (3.0 / side ** 0.5 if count > 9 else 1))

if flat:
    for i in range(count):
        m = quad('/World/flat/q%d' % i)
        xf = UsdGeom.Xformable(m)
        q = orients[i]
        rot = Gf.Rotation(Gf.Quatd(q.GetReal(), Gf.Vec3d(q.GetImaginary())))
        mtx = Gf.Matrix4d().SetScale(Gf.Vec3d(scales[i])) * Gf.Matrix4d().SetRotate(rot) * \
            Gf.Matrix4d().SetTranslate(Gf.Vec3d(positions[i]))
        xf.AddTransformOp().Set(mtx)
        bind(m, emit)
else:
    pi = UsdGeom.PointInstancer.Define(stage, '/World/lamps')
    proto = quad('/World/lamps/protos/lamp')
    bind(proto, emit)
    pi.CreatePrototypesRel().SetTargets([proto.GetPath()])
    pi.CreateProtoIndicesAttr([0] * count)
    pi.CreatePositionsAttr(positions)
    pi.CreateOrientationsAttr(orients)
    pi.CreateScalesAttr(scales)

# A moving emitter (not instanced): slides along x over the shutter.
mv = quad('/World/mover', 0.25)
bind(mv, emit)
t = UsdGeom.XformCommonAPI(mv)
t.SetTranslate(Gf.Vec3d(-2.0, 1.4, 2.5), 1)
t.SetTranslate(Gf.Vec3d(2.0, 1.4, 2.5), 2)

rs = UsdRender.Settings.Define(stage, '/Render/settings')
rs.CreateCameraRel().SetTargets([cam.GetPath()])
rs.CreateResolutionAttr(Gf.Vec2i(480, 320))
stage.GetRootLayer().Save()
print('wrote', stage.GetRootLayer().realPath)
