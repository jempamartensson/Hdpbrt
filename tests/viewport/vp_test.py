import hou, os, time
from PySide6 import QtCore
OUT = os.environ["VPT_OUT"]
RENDERER = os.environ.get("VPT_RENDERER", "pbrt")
WAIT = int(os.environ.get("VPT_WAIT", "40"))
def log(*a):
    with open(OUT + ".log", "a") as f: f.write(" ".join(str(x) for x in a) + "\n")
SCENE = """
from pxr import UsdGeom, UsdLux, Gf, Vt
stage = hou.pwd().editableStage()
s = UsdGeom.Sphere.Define(stage, "/geo/sphere"); s.CreateRadiusAttr(1.0)
s.CreateDisplayColorAttr(Vt.Vec3fArray([Gf.Vec3f(0.8, 0.1, 0.1)]))
c = UsdGeom.Cube.Define(stage, "/geo/ground"); c.CreateSizeAttr(1.0)
UsdGeom.Xformable(c).AddTranslateOp().Set(Gf.Vec3d(0, -1.05, 0)); UsdGeom.Xformable(c).AddScaleOp().Set(Gf.Vec3f(8, 0.1, 8))
c.CreateDisplayColorAttr(Vt.Vec3fArray([Gf.Vec3f(0.5, 0.5, 0.5)]))
l = UsdLux.DistantLight.Define(stage, "/lights/sun"); l.CreateIntensityAttr(3.0)
UsdGeom.Xformable(l).AddRotateXYZOp().Set(Gf.Vec3f(-50, 30, 0))
d = UsdLux.DomeLight.Define(stage, "/lights/dome"); d.CreateIntensityAttr(0.3)
"""
def setup():
    try:
        stage = hou.node("/stage")
        py = stage.createNode("pythonscript", "scene")
        py.parm("python").set(SCENE)
        py.setDisplayFlag(True)
        desk = hou.ui.desktop("Solaris")
        if desk: desk.setAsCurrent()
        sv = hou.ui.paneTabOfType(hou.paneTabType.SceneViewer)
        sv.setPwd(stage)
        log("renderers", sv.hydraRenderers() if hasattr(sv, "hydraRenderers") else "?")
        sv.setHydraRenderer(RENDERER)
        log("current", sv.currentHydraRenderer())
        sv.curViewport().frameAll()
        QtCore.QTimer.singleShot(WAIT * 1000, grab)
    except Exception as e:
        import traceback; log(traceback.format_exc()); hou.exit(1, suppress_save_prompt=True)
def grab():
    try:
        sv = hou.ui.paneTabOfType(hou.paneTabType.SceneViewer)
        vp = sv.curViewport()
        s = sv.flipbookSettings().stash()
        s.output(OUT); s.frameRange((1, 1)); s.outputToMPlay(False)
        sv.flipbook(vp, s)
        log("flipbook done")
    except Exception as e:
        import traceback; log(traceback.format_exc())
    hou.exit(0, suppress_save_prompt=True)
QtCore.QTimer.singleShot(5000, setup)
