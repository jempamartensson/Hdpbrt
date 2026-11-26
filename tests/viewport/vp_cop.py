# Live-viewport test for Houdini op: images (COP nodes), run from a startup
# 123.py like vp_test.py (see CLAUDE.md, "Live-viewport test"). Builds a quad
# textured from op:/obj/copnet1/checker and a dome lit by op:/obj/copnet1/sky,
# flipbooks to VPT_OUT with _a, edits both COPs, waits VPT_WAIT seconds and
# flipbooks _b. hdPbrt must show the edit (orange sky, green checker) in _b.
import hou, os
from PySide6 import QtCore
OUT = os.environ["VPT_OUT"]; RENDERER = os.environ.get("VPT_RENDERER", "pbrt"); WAIT = int(os.environ.get("VPT_WAIT", "25"))
import time
def log(*a):
    a = ("%.1f" % time.time(),) + a
    with open(OUT + ".log", "a") as f: f.write(" ".join(str(x) for x in a) + "\n")
    import sys; sys.stderr.write("VPT " + " ".join(str(x) for x in a) + "\n"); sys.stderr.flush()
SCENE = """
from pxr import UsdGeom, UsdLux, UsdShade, Sdf, Gf, Vt
stage = hou.pwd().editableStage()
m = UsdGeom.Mesh.Define(stage, "/geo/quad")
m.CreatePointsAttr([(-2, -1.5, 0), (2, -1.5, 0), (2, 1.5, 0), (-2, 1.5, 0)])
m.CreateFaceVertexCountsAttr([4]); m.CreateFaceVertexIndicesAttr([0, 1, 2, 3])
m.CreateSubdivisionSchemeAttr("none")
pv = UsdGeom.PrimvarsAPI(m).CreatePrimvar("st", Sdf.ValueTypeNames.TexCoord2fArray, UsdGeom.Tokens.faceVarying)
pv.Set([(0, 0), (1, 0), (1, 1), (0, 1)])
mat = UsdShade.Material.Define(stage, "/mtl/cop")
pbs = UsdShade.Shader.Define(stage, "/mtl/cop/pbs"); pbs.CreateIdAttr("UsdPreviewSurface")
st = UsdShade.Shader.Define(stage, "/mtl/cop/st"); st.CreateIdAttr("UsdPrimvarReader_float2")
st.CreateInput("varname", Sdf.ValueTypeNames.String).Set("st")
tex = UsdShade.Shader.Define(stage, "/mtl/cop/tex"); tex.CreateIdAttr("UsdUVTexture")
tex.CreateInput("file", Sdf.ValueTypeNames.Asset).Set("op:/obj/copnet1/checker")
tex.CreateInput("sourceColorSpace", Sdf.ValueTypeNames.Token).Set("raw")
tex.CreateInput("st", Sdf.ValueTypeNames.Float2).ConnectToSource(st.ConnectableAPI(), "result")
pbs.CreateInput("diffuseColor", Sdf.ValueTypeNames.Color3f).ConnectToSource(tex.ConnectableAPI(), "rgb")
mat.CreateSurfaceOutput().ConnectToSource(pbs.ConnectableAPI(), "surface")
UsdShade.MaterialBindingAPI.Apply(m.GetPrim()).Bind(mat)
d = UsdLux.DomeLight.Define(stage, "/lights/dome"); d.CreateIntensityAttr(1.0)
d.CreateTextureFileAttr().Set("op:/obj/copnet1/sky")
"""
def setup():
    try:
        net = hou.node('/obj').createNode('copnet', 'copnet1')
        c = net.createNode('checkerboard', 'checker')
        c.parmTuple('even').set((0.9, 0.2, 0.05, 1)); c.parmTuple('odd').set((0.05, 0.3, 0.8, 1))
        sky = net.createNode('constant', 'sky')
        sky.parm('signature').set('f3'); sky.parmTuple('f3').set((0.6, 0.6, 0.6))
        py = hou.node("/stage").createNode("pythonscript", "scene")
        py.parm("python").set(SCENE); py.setDisplayFlag(True)
        desk = hou.ui.desktop("Solaris")
        if desk: desk.setAsCurrent()
        sv = hou.ui.paneTabOfType(hou.paneTabType.SceneViewer)
        sv.setPwd(hou.node("/stage")); sv.setHydraRenderer(RENDERER)
        log("current", sv.currentHydraRenderer())
        sv.curViewport().frameAll()
        QtCore.QTimer.singleShot(WAIT * 1000, lambda: grab("a", edit))
    except Exception:
        import traceback; log(traceback.format_exc()); hou.exit(1, suppress_save_prompt=True)
def edit():
    hou.node('/obj/copnet1/checker').parmTuple('even').set((0.1, 0.8, 0.1, 1))
    sky = hou.node('/obj/copnet1/sky')
    sky.parmTuple('f3').set((1.0, 0.45, 0.15))
    log("edited checker")
    QtCore.QTimer.singleShot(WAIT * 1000, lambda: grab("b", lambda: hou.exit(0, suppress_save_prompt=True)))
def grab(tag, then):
    try:
        sv = hou.ui.paneTabOfType(hou.paneTabType.SceneViewer)
        s = sv.flipbookSettings().stash()
        s.output(OUT.replace(".png", "_%s.png" % tag)); s.frameRange((1, 1)); s.outputToMPlay(False)
        sv.flipbook(sv.curViewport(), s)
        log("flipbook", tag)
    except Exception:
        import traceback; log(traceback.format_exc())
    then()
QtCore.QTimer.singleShot(5000, setup)
