"""hdPbrt material properties on Houdini shader nodes.

Houdini has no renderer property file for materials, so the pbrt properties
are spare parameters on the surface shader VOPs, tagged as shader parameters:
Houdini's USD translation writes them as inputs on the USD shader
(inputs:pbrt:irReflectance), where hdPbrt reads them. New nodes get them from
scripts/vop/<type>_OnCreated.py; for existing nodes run

    import hdpbrt_houdini; hdpbrt_houdini.add_material_parms(hou.selectedNodes())
"""
import hou

SHADER_TYPES = ('mtlxstandard_surface', 'mtlxopen_pbr_surface', 'usdpreviewsurface',
                'mtlxsurface')
FOLDER = 'hdpbrt_folder'
TOGGLE = 'hdpbrt_ir'
IR = 'hdpbrt_irReflectance'
EDGE = 'hdpbrt_irEdge'
IR_DEFAULT, IR_OFF, EDGE_DEFAULT = 0.6, -1.0, 720.0

# A parm at its default value isn't written to USD, and Houdini's activation
# toggles don't apply to spare parms. So the checkbox sets the values: off puts
# them back at their defaults (nothing written), on writes the reflectance.
_CALLBACK = ('import hdpbrt_houdini; hdpbrt_houdini.toggle_ir(kwargs["node"])')


def _shader_tags(usd_name):
    return {'sidefx::shader_isparm': '1', 'sidefx::shader_parmname': usd_name,
            'sidefx::shader_parmtype': 'float'}


def _folder():
    f = hou.FolderParmTemplate(FOLDER, 'pbrt', folder_type=hou.folderType.Collapsible)
    f.addParmTemplate(hou.ToggleParmTemplate(
        TOGGLE, 'Infrared Reflectance', default_value=False, script_callback=_CALLBACK,
        script_callback_language=hou.scriptLanguage.Python,
        tags={'sidefx::shader_isparm': '0'},  # UI only, not a shader input
        help='Give the diffuse reflectance its own value above the red edge, for infrared '
             'renders (pbrt:sensor bands on the camera). RGB colors otherwise just continue '
             'their red end into the infrared, so foliage stays dark; real leaves reflect about '
             '0.5-0.6 there (the Wood effect). The visible image is unchanged.'))
    hide = '{ %s == 0 }' % TOGGLE
    f.addParmTemplate(hou.FloatParmTemplate(
        IR, 'IR Reflectance', 1, default_value=(IR_OFF,), min=0, max=1,
        conditionals={hou.parmCondType.HideWhen: hide}, tags=_shader_tags('pbrt:irReflectance'),
        help='Diffuse reflectance above the red edge (pbrt:irReflectance).'))
    f.addParmTemplate(hou.FloatParmTemplate(
        EDGE, 'Red Edge (nm)', 1, default_value=(EDGE_DEFAULT,), min=650, max=1000,
        conditionals={hou.parmCondType.HideWhen: hide}, tags=_shader_tags('pbrt:irEdge'),
        help='Where the reflectance switches to the infrared value (60 nm wide transition; '
             'pbrt:irEdge). Vegetation: about 700-730.'))
    return f


def add_material_parms(nodes):
    """Adds the pbrt folder to surface shader VOPs (once; others are skipped)."""
    if isinstance(nodes, hou.Node):
        nodes = [nodes]
    for node in nodes:
        if node.type().category() != hou.vopNodeTypeCategory() or \
                node.type().nameComponents()[2] not in SHADER_TYPES or node.parm(TOGGLE):
            continue
        g = node.parmTemplateGroup()
        g.append(_folder())
        node.setParmTemplateGroup(g)


def toggle_ir(node):
    on = node.evalParm(TOGGLE)
    node.parm(IR).set(IR_DEFAULT if on else IR_OFF)
    if not on:
        node.parm(EDGE).set(EDGE_DEFAULT)
