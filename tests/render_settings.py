# hython render_settings.py out.png scene.usda frame [key=value ...]
# Renders /World/cam with a Hydra delegate (HDT_RENDERER, default pbrt) until it
# converges, with renderer settings given as key=value (ints, floats, true/false
# or strings), then writes out.png.
import sys, os, time
from PySide6.QtGui import QGuiApplication, QOpenGLContext, QOffscreenSurface, QSurfaceFormat
app = QGuiApplication(sys.argv)
fmt = QSurfaceFormat(); fmt.setVersion(4, 6); fmt.setProfile(QSurfaceFormat.CompatibilityProfile)
QSurfaceFormat.setDefaultFormat(fmt)
ctx = QOpenGLContext(); ctx.setFormat(fmt); assert ctx.create()
surf = QOffscreenSurface(); surf.setFormat(ctx.format()); surf.create(); assert ctx.makeCurrent(surf)
from pxr import Usd, UsdImagingGL, Sdf, Gf, Tf, CameraUtil
out, scene, frame = sys.argv[1], sys.argv[2], float(sys.argv[3])
stage = Usd.Stage.Open(scene)
eng = UsdImagingGL.Engine()
eng.SetRendererPlugin(os.environ.get('HDT_RENDERER', 'HdPbrtRendererPlugin'))
def parse(v):
    if v in ('true', 'false'): return v == 'true'
    for f in (int, float):
        try: return f(v)
        except ValueError: pass
    return v
for kv in sys.argv[4:]:
    k, v = kv.split('=', 1)
    eng.SetRendererSetting(k, parse(v))
w, h = 640, 400
eng.SetRenderBufferSize(Gf.Vec2i(w, h))
eng.SetFraming(CameraUtil.Framing(Gf.Range2f(Gf.Vec2f(0), Gf.Vec2f(w, h)), Gf.Rect2i(Gf.Vec2i(0), w, h)))
eng.SetCameraPath(Sdf.Path('/World/cam'))
p = UsdImagingGL.RenderParams(); p.frame = Usd.TimeCode(frame); p.colorCorrectionMode = 'sRGB'
t = time.time()
while True:
    try:
        eng.Render(stage.GetPseudoRoot(), p)
    except Tf.ErrorException:
        pass  # GL interop errors of the offscreen context; pbrt's PFM is what we read
    if eng.IsConverged() or time.time() - t > 300: break
    time.sleep(0.05)
print('converged', eng.IsConverged(), '%.1fs' % (time.time() - t))

# The final image is pbrt's own PFM: with HDPBRT_KEEP_TMP=1 it stays in the
# delegate's temp dir (HDPBRT_TMPDIR). Convert the newest one to an sRGB PNG.
import glob, struct, zlib
pfms = sorted(glob.glob(os.path.join(os.environ.get('HDPBRT_TMPDIR', '/tmp'), 'hdpbrt-%d-*' % os.getpid(), 'render_*.pfm')),
              key=os.path.getmtime)
if not pfms:
    sys.exit('no PFM found: run with HDPBRT_KEEP_TMP=1 HDPBRT_TMPDIR=<dir>')
with open(pfms[-1], 'rb') as f:
    hdr = f.readline(); dims = f.readline().split(); scale = float(f.readline())
    pw, ph = int(dims[0]), int(dims[1])
    data = struct.unpack(('<' if scale < 0 else '>') + 'f' * (pw * ph * 3), f.read(pw * ph * 12))
def srgb(v):
    v = min(max(v, 0.0), 1.0)
    return int(round(255 * (12.92 * v if v <= 0.0031308 else 1.055 * v ** (1 / 2.4) - 0.055)))
rows = []
for y in range(ph - 1, -1, -1):  # PFM rows go bottom to top
    o = y * pw * 3
    rows.append(b'\0' + bytes(srgb(c) for c in data[o:o + pw * 3]))
def chunk(t, d):
    return struct.pack('>I', len(d)) + t + d + struct.pack('>I', zlib.crc32(t + d) & 0xffffffff)
png = b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', pw, ph, 8, 2, 0, 0, 0)) + \
      chunk(b'IDAT', zlib.compress(b''.join(rows), 6)) + chunk(b'IEND', b'')
open(out, 'wb').write(png)
print('wrote', out, 'from', pfms[-1])
os._exit(0)
