# Render frames of a USD scene with hdPbrt the way Typhoon's test suite does:
# the camera and resolution come from the stage's RenderSettings prim.
#
#   HDPBRT_KEEP_TMP=1 HDPBRT_TMPDIR=<dir> hython render_hdpbrt.py scene.usda FIRST:LAST outdir \
#       [key=value ...]
#
# Writes outdir/<scene stem>-hdpbrt.NNNN.pfm (pbrt's own float RGB image) per
# frame. Renderer settings are given as key=value (ints, floats, true/false or
# strings), e.g. pbrt:maxDepth=1 pbrt:pixelSamples=64. The working space is
# lin_rec709 unless pbrt:workingSpace=acescg is given.
import sys, os, time, glob, shutil
from PySide6.QtGui import QGuiApplication, QOpenGLContext, QOffscreenSurface, QSurfaceFormat
app = QGuiApplication(sys.argv)
fmt = QSurfaceFormat(); fmt.setVersion(4, 6); fmt.setProfile(QSurfaceFormat.CompatibilityProfile)
QSurfaceFormat.setDefaultFormat(fmt)
ctx = QOpenGLContext(); ctx.setFormat(fmt); assert ctx.create()
surf = QOffscreenSurface(); surf.setFormat(ctx.format()); surf.create(); assert ctx.makeCurrent(surf)
from pxr import Usd, UsdRender, UsdImagingGL, Sdf, Gf, Tf, CameraUtil

scene, frames, outdir = sys.argv[1], sys.argv[2], sys.argv[3]
first, last = (int(x) for x in frames.split(':'))
os.makedirs(outdir, exist_ok=True)
stem = os.path.splitext(os.path.basename(scene))[0]
stage = Usd.Stage.Open(scene)

# Camera and resolution from the first RenderSettings prim.
settings = next((UsdRender.Settings(p) for p in stage.Traverse()
                 if p.IsA(UsdRender.Settings)), None)
cam = Sdf.Path('/cameras/camera1')
w, h = 128, 128
if settings:
    targets = settings.GetCameraRel().GetTargets()
    if targets:
        cam = targets[0]
    res = settings.GetResolutionAttr().Get()
    if res:
        w, h = int(res[0]), int(res[1])

eng = UsdImagingGL.Engine()
eng.SetRendererPlugin('HdPbrtRendererPlugin')

def parse(v):
    if v in ('true', 'false'):
        return v == 'true'
    for f in (int, float):
        try:
            return f(v)
        except ValueError:
            pass
    return v

# The references (Typhoon, Karma under Houdini's default OCIO config) are
# linear Rec.709; hdPbrt's default working space is ACEScg.
eng.SetRendererSetting('pbrt:workingSpace', 'lin_rec709')
for kv in sys.argv[4:]:
    k, v = kv.split('=', 1)
    eng.SetRendererSetting(k, parse(v))
eng.SetRenderBufferSize(Gf.Vec2i(w, h))
eng.SetFraming(CameraUtil.Framing(Gf.Range2f(Gf.Vec2f(0), Gf.Vec2f(w, h)),
                                  Gf.Rect2i(Gf.Vec2i(0), w, h)))
eng.SetCameraPath(cam)

tmp = os.path.join(os.environ.get('HDPBRT_TMPDIR', '/tmp'), 'hdpbrt-%d-*' % os.getpid())
last_pfm = None
for frame in range(first, last + 1):
    p = UsdImagingGL.RenderParams()
    p.frame = Usd.TimeCode(frame)
    t = time.time()
    seen = set(glob.glob(os.path.join(tmp, 'render_*.pfm')))
    # Render until converged and a new final image exists for this frame.
    while True:
        try:
            eng.Render(stage.GetPseudoRoot(), p)
        except Tf.ErrorException:
            pass  # GL interop errors of the offscreen context
        if eng.IsConverged():
            new = set(glob.glob(os.path.join(tmp, 'render_*.pfm'))) - seen
            if new:
                break
            # Nothing that affects the image changed since the last frame, so
            # hdPbrt doesn't render again: the previous image is this frame's.
            if last_pfm and time.time() - t > 2:
                new = {last_pfm}
                break
        if time.time() - t > 600:
            sys.exit('frame %d: no converged image after 600 s' % frame)
        time.sleep(0.02)
    pfm = max(new, key=os.path.getmtime)
    last_pfm = pfm
    out = os.path.join(outdir, '%s-hdpbrt.%04d.pfm' % (stem, frame))
    shutil.copy(pfm, out)
    print('frame %d -> %s (%.1fs)' % (frame, out, time.time() - t), flush=True)
os._exit(0)
