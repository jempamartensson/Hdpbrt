# Live-viewport test for an animated COP read through a LOP (run like vp_test.py,
# see CLAUDE.md "Live-viewport test"): a Copernicus constant with red = $F/10 is
# a Dome Light LOP's texture. The LOP authors op:/obj/copnet1/sky[<frame>], so
# each frame is its own op: path. Flipbooks frames 3, 7, 3 to VPT_OUT with
# _<i>_f<frame>; the background must read linear (F/10, 0.2, 0.2) (sRGB 149 /
# 218 / 149, 124, 124). Checked 2026-11-26: exact; hython's LOP stage can't
# test this (dome.stage() returns a new stage per cook with no change notices).
import hou, os, time, sys
from PySide6 import QtCore
OUT = os.environ["VPT_OUT"]; RENDERER = os.environ.get("VPT_RENDERER", "pbrt"); WAIT = int(os.environ.get("VPT_WAIT", "20"))
def log(*a):
    a = ("%.1f" % time.time(),) + a
    with open(OUT + ".log", "a") as f: f.write(" ".join(str(x) for x in a) + "\n")
    sys.stderr.write("VPT " + " ".join(str(x) for x in a) + "\n"); sys.stderr.flush()
FRAMES = [3, 7, 3]
def setup():
    try:
        net = hou.node('/obj').createNode('copnet', 'copnet1')
        sky = net.createNode('constant', 'sky'); sky.parm('signature').set('f3')
        for i, p in enumerate(sky.parmTuple('f3')):
            p.setExpression('$F/10' if i == 0 else '0.2')
        dome = hou.node('/stage').createNode('domelight')
        dome.parm('xn__inputstexturefile_r3ah').set('op:/obj/copnet1/sky')
        dome.setDisplayFlag(True)
        desk = hou.ui.desktop("Solaris")
        if desk: desk.setAsCurrent()
        sv = hou.ui.paneTabOfType(hou.paneTabType.SceneViewer)
        sv.setPwd(hou.node("/stage")); sv.setHydraRenderer(RENDERER)
        step(0)
    except Exception:
        import traceback; log(traceback.format_exc()); hou.exit(1, suppress_save_prompt=True)
def step(i):
    if i == len(FRAMES):
        hou.exit(0, suppress_save_prompt=True); return
    hou.setFrame(FRAMES[i]); log("frame", FRAMES[i])
    QtCore.QTimer.singleShot(WAIT * 1000, lambda: grab(i))
def grab(i):
    try:
        sv = hou.ui.paneTabOfType(hou.paneTabType.SceneViewer)
        s = sv.flipbookSettings().stash()
        s.output(OUT.replace(".png", "_%d_f%d.png" % (i, FRAMES[i]))); s.frameRange((FRAMES[i], FRAMES[i])); s.outputToMPlay(False)
        sv.flipbook(sv.curViewport(), s); log("flipbook", i, FRAMES[i])
    except Exception:
        import traceback; log(traceback.format_exc())
    step(i + 1)
QtCore.QTimer.singleShot(5000, setup)
