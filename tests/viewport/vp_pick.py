# Live-viewport selection test (run from a 123.py startup script, see
# CLAUDE.md). Builds a sphere, a ground box and a point instancer of three
# cubes, switches the viewer to $VPT_RENDERER, waits $VPT_WAIT seconds, then
# clicks the sphere and the middle instance and logs the scene graph selection
# after each click to $VPT_OUT.log. The flipbook goes to $VPT_OUT.
import hou, os
from PySide6 import QtCore, QtGui, QtWidgets
from PySide6.QtTest import QTest

OUT = os.environ["VPT_OUT"]
RENDERER = os.environ.get("VPT_RENDERER", "pbrt")
WAIT = int(os.environ.get("VPT_WAIT", "40"))


def log(*a):
    with open(OUT + ".log", "a") as f:
        f.write(" ".join(str(x) for x in a) + "\n")


SCENE = """
from pxr import UsdGeom, UsdLux, Gf, Vt
stage = hou.pwd().editableStage()
s = UsdGeom.Sphere.Define(stage, "/geo/sphere"); s.CreateRadiusAttr(1.0)
s.CreateDisplayColorAttr(Vt.Vec3fArray([Gf.Vec3f(0.8, 0.1, 0.1)]))
c = UsdGeom.Cube.Define(stage, "/geo/ground"); c.CreateSizeAttr(1.0)
UsdGeom.Xformable(c).AddTranslateOp().Set(Gf.Vec3d(0, -1.05, 0))
UsdGeom.Xformable(c).AddScaleOp().Set(Gf.Vec3f(8, 0.1, 8))
c.CreateDisplayColorAttr(Vt.Vec3fArray([Gf.Vec3f(0.5, 0.5, 0.5)]))
pi = UsdGeom.PointInstancer.Define(stage, "/geo/inst")
proto = UsdGeom.Cube.Define(stage, "/geo/inst/protos/box"); proto.CreateSizeAttr(0.6)
proto.CreateDisplayColorAttr(Vt.Vec3fArray([Gf.Vec3f(0.1, 0.3, 0.8)]))
pi.CreatePrototypesRel().AddTarget(proto.GetPath())
pi.CreateProtoIndicesAttr(Vt.IntArray([0, 0, 0]))
pi.CreatePositionsAttr(Vt.Vec3fArray([Gf.Vec3f(x, -0.7, 2.2) for x in (-1.5, 0, 1.5)]))
l = UsdLux.DistantLight.Define(stage, "/lights/sun"); l.CreateIntensityAttr(3.0)
UsdGeom.Xformable(l).AddRotateXYZOp().Set(Gf.Vec3f(-50, 30, 0))
d = UsdLux.DomeLight.Define(stage, "/lights/dome"); d.CreateIntensityAttr(0.3)
"""

# (label, world point, select point instances?, expected selection)
TARGETS = [
    ("instancer", hou.Vector3(0, -0.7, 2.2), False, ["/geo/inst"]),
    ("ground", hou.Vector3(3, -1.0, -3), False, ["/geo/ground"]),
    ("background", None, False, []),  # top-left corner of the viewport
    # Off to the side: around the origin Houdini's own guide for the distant
    # light wins the pick with any render delegate (Karma CPU too).
    ("sphere edge", hou.Vector3(-0.95, 0.3, 0.0), False, ["/geo/sphere"]),
    # Houdini reports a point instance as "/geo/inst[1:/geo/inst/protos/box]".
    ("instance 0", hou.Vector3(-1.5, -0.7, 2.2), True, ["/geo/inst[0:"]),
    ("instance 1", hou.Vector3(0, -0.7, 2.2), True, ["/geo/inst[1:"]),
    ("instance 2", hou.Vector3(1.5, -0.7, 2.2), True, ["/geo/inst[2:"]),
]


def matches(got, want):
    """Each selected path starts with the expected one (instances carry a suffix)."""
    return len(got) == len(want) and all(g.startswith(w) for g, w in zip(got, want))


def viewer():
    return hou.ui.paneTabOfType(hou.paneTabType.SceneViewer)


def setup():
    try:
        stage = hou.node("/stage")
        py = stage.createNode("pythonscript", "scene")
        py.parm("python").set(SCENE)
        py.setDisplayFlag(True)
        desk = hou.ui.desktop("Solaris")
        if desk:
            desk.setAsCurrent()
        sv = viewer()
        sv.setPwd(stage)
        sv.setHydraRenderer(RENDERER)
        log("current", sv.currentHydraRenderer())
        sv.curViewport().frameAll()
        QtCore.QTimer.singleShot(WAIT * 1000, pick)
    except Exception:
        import traceback
        log(traceback.format_exc())
        hou.exit(1, suppress_save_prompt=True)


def find_surfaces(vp):
    """QWindows/QWidgets with the viewport's size (one of them shows it)."""
    w, h = vp.size()[-2:]  # (x, y, w, h)
    found = []
    for win in QtGui.QGuiApplication.allWindows():
        if win.isVisible() and (win.width(), win.height()) == (w, h):
            found.append(win)
    for wid in QtWidgets.QApplication.allWidgets():
        if wid.isVisible() and (wid.width(), wid.height()) == (w, h):
            found.append(wid)
    return found


def click(surface, x, y):
    pos = QtCore.QPoint(int(x), int(y))
    QTest.mouseMove(surface, pos)
    QTest.mouseClick(surface, QtCore.Qt.LeftButton, QtCore.Qt.NoModifier, pos)


def name_of(o):
    return type(o).__name__ + ":" + (o.objectName() or "")


def pick():
    """Queue the clicks; each runs after a delay so Houdini can process it."""
    try:
        sv = viewer()
        vp = sv.curViewport()
        s = sv.flipbookSettings().stash()
        s.output(OUT); s.frameRange((1, 1)); s.outputToMPlay(False)
        sv.flipbook(vp, s)
        log("state", sv.currentState(), "vp.size", vp.size())
        for st in os.environ.get("VPT_STATES", "select,lopselect,scenegraphselect").split(","):
            try:
                sv.setCurrentState(st)
                log("setCurrentState", st, "->", sv.currentState())
                break
            except Exception as e:
                log("setCurrentState", st, "failed:", e)
        surfaces = find_surfaces(vp)
        log("surfaces", [name_of(o) for o in surfaces])
        h = vp.size()[-1]
        # The QWindow is the Vulkan surface; all candidates behave the same.
        surf = next((o for o in surfaces if isinstance(o, QtGui.QWindow)), surfaces[0])
        steps = []
        for name, p, inst, want in TARGETS:
            if p is None:
                x, y = 8, 8
            else:
                sp = vp.mapToScreen(p)  # viewport pixels, origin bottom-left
                x, y = sp[0], h - sp[1]
            steps.append((name, surf, x, y, inst, want))
        run_steps(steps)
    except Exception:
        import traceback
        log(traceback.format_exc())
        hou.exit(1, suppress_save_prompt=True)


def run_steps(steps, prev=None):
    sv = viewer()
    if prev:
        name, surf, x, y, inst, want = prev
        got = list(sv.currentSceneGraphSelection())
        log("PICK", "ok  " if matches(got, want) else "FAIL", name, "at", (int(x), int(y)),
            "selection", got, "expected", want)
    if not steps:
        hou.exit(0, suppress_save_prompt=True)
        return
    step = steps[0]
    sv.setCurrentSceneGraphSelection([])
    sv.setSelectionPointInstances(step[4])
    try:
        click(step[1], step[2], step[3])
    except Exception as e:
        log("click failed", e)
    QtCore.QTimer.singleShot(2000, lambda: run_steps(steps[1:], step))


QtCore.QTimer.singleShot(5000, setup)
