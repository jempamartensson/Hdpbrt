# Simulates viewport use: tumble the camera while rendering, then let it converge.
import sys, os, time, math
from PySide6.QtGui import QGuiApplication, QOpenGLContext, QOffscreenSurface, QSurfaceFormat
app = QGuiApplication(sys.argv)
fmt = QSurfaceFormat(); fmt.setVersion(4, 6); fmt.setProfile(QSurfaceFormat.CoreProfile)
ctx = QOpenGLContext(); ctx.setFormat(fmt); assert ctx.create()
surf = QOffscreenSurface(); surf.setFormat(ctx.format()); surf.create(); assert ctx.makeCurrent(surf)
from pxr import Usd, UsdImagingGL, Gf, Glf, CameraUtil
stage = Usd.Stage.Open(sys.argv[1])
prm = UsdImagingGL.Engine.Parameters(); prm.rendererPluginId = 'HdPbrtRendererPlugin'
eng = UsdImagingGL.Engine(prm)
print('renderer', eng.GetCurrentRendererId())
eng.SetRendererSetting('pbrt:pixelSamples', 64)
eng.SetRenderBufferSize(Gf.Vec2i(320, 240))
eng.SetFraming(CameraUtil.Framing(Gf.Range2f(Gf.Vec2f(0), Gf.Vec2f(320, 240)), Gf.Rect2i(Gf.Vec2i(0), 320, 240)))
params = UsdImagingGL.RenderParams()
params.clearColor = Gf.Vec4f(0, 0, 0, 1)
root = stage.GetPseudoRoot()
from pxr import Tf
def render():
    try:
        eng.Render(root, params)
    except Tf.ErrorException:
        pass
proj = Gf.Frustum(); proj.SetPerspective(40, 320 / 240, 0.1, 100)
t0 = time.time(); n = 0
for i in range(40):  # 40 frames of tumbling
    a = i * 2.0
    view = Gf.Matrix4d().SetLookAt(Gf.Vec3d(8 * math.sin(math.radians(a)), 2.5, 8 * math.cos(math.radians(a))), Gf.Vec3d(0, 0.7, 0), Gf.Vec3d(0, 1, 0))
    eng.SetCameraState(view, proj.ComputeProjectionMatrix())
    render(); n += 1
    time.sleep(0.03)
print('tumble frames', n, 'in %.2fs' % (time.time() - t0))
t1 = time.time()
while not eng.IsConverged():
    render(); time.sleep(0.05)
print('converged after %.2fs' % (time.time() - t1))
print('stats', eng.GetRenderStats())
