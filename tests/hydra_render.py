import sys, os
from PySide6.QtGui import QGuiApplication, QOpenGLContext, QOffscreenSurface, QSurfaceFormat
app = QGuiApplication(sys.argv)
fmt = QSurfaceFormat(); fmt.setVersion(4,6); fmt.setProfile(QSurfaceFormat.CompatibilityProfile)
QSurfaceFormat.setDefaultFormat(fmt)
ctx = QOpenGLContext(); ctx.setFormat(fmt); assert ctx.create()
surf = QOffscreenSurface(); surf.setFormat(ctx.format()); surf.create(); assert ctx.makeCurrent(surf)
print('GL', ctx.format().majorVersion(), ctx.format().minorVersion(), ctx.format().profile())
from pxr import Usd, UsdAppUtils, Sdf, Tf
stage = Usd.Stage.Open(sys.argv[3] if len(sys.argv)>3 else 'scene.usda')
rec = UsdAppUtils.FrameRecorder(sys.argv[1], True)
rec.SetImageWidth(640); rec.SetColorCorrectionMode('sRGB')
cam = UsdAppUtils.GetCameraAtPath(stage, Sdf.Path('/World/cam'))
try:
    print('record:', rec.Record(stage, cam, Usd.TimeCode.Default(), sys.argv[2]))
except Tf.ErrorException as e:
    print('errors:', str(e).count('Error in'), str(e)[-300:])
print('exists:', os.path.exists(sys.argv[2]))
