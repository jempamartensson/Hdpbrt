# hython cop_textures.py op|file out.png [workdir]
# Houdini op: images (COP nodes) as textures. Builds a Copernicus network
# (a colored checkerboard, a one-channel font mask, a sky ramp), then renders the
# same scene with hdPbrt either reading the nodes through op: paths or reading
# the images Houdini's own rop_image COP saved to EXR. The two must agree.
#   quad left:  UsdPreviewSurface diffuseColor <- UsdUVTexture(checkerboard)
#   quad right: MaterialX standard_surface base_color <- ND_image_color3(font)
#   dome:       texture from the sky ramp
import sys, os, runpy
import hou
mode, out = sys.argv[1], os.path.abspath(sys.argv[2])
work = os.path.abspath(sys.argv[3] if len(sys.argv) > 3 else os.path.dirname(out))
os.makedirs(work, exist_ok=True)

net = hou.node('/obj').createNode('copnet', 'copnet1')
grid = net.createNode('checkerboard', 'grid')
# 3 x 5 cells: a vertical or horizontal flip changes the corner colors.
grid.parmTuple('even').set((0.9, 0.2, 0.05, 1))
grid.parmTuple('odd').set((0.05, 0.3, 0.8, 1))
grid.parm('rows').set(3)
grid.parm('cols').set(5)
font = net.createNode('font', 'font')
font.parm('text').set('pbrt F')
font.parm('fontsize').set(0.35)
font.parmTuple('t').set((0, 0.25))  # upper half: a flip shows
sky = net.createNode('ramp', 'sky')
images = {'grid': grid, 'font': font, 'sky': sky}

if mode == 'file':
    paths = {}
    for name, node in images.items():
        rop = net.createNode('rop_image', 'save_' + name)
        rop.setInput(0, node)
        f = os.path.join(work, 'cop_%s.exr' % name)
        rop.parm('copoutput').set(f)
        rop.parm('trange').set(0)
        rop.render()
        paths[name] = f
else:
    paths = {name: 'op:' + node.path() for name, node in images.items()}

scene = os.path.join(work, 'cop_%s.usda' % mode)
with open(scene, 'w') as fh:
    fh.write('''#usda 1.0
(
    upAxis = "Y"
)
def Xform "World"
{
    def Camera "cam"
    {
        float focalLength = 35
        float horizontalAperture = 36
        float verticalAperture = 22.5
        double3 xformOp:translate = (0, 0, 6)
        uniform token[] xformOpOrder = ["xformOp:translate"]
    }
    def DomeLight "dome"
    {
        asset inputs:texture:file = @%(sky)s@
        float inputs:intensity = 1
    }
    def Mesh "left" (prepend apiSchemas = ["MaterialBindingAPI"])
    {
        int[] faceVertexCounts = [4]
        int[] faceVertexIndices = [0, 1, 2, 3]
        point3f[] points = [(-3.2, -1.5, 0), (-0.2, -1.5, 0), (-0.2, 1.5, 0), (-3.2, 1.5, 0)]
        texCoord2f[] primvars:st = [(0, 0), (1, 0), (1, 1), (0, 1)] (interpolation = "faceVarying")
        uniform token subdivisionScheme = "none"
        rel material:binding = </World/mtl/grid>
    }
    def Mesh "right" (prepend apiSchemas = ["MaterialBindingAPI"])
    {
        int[] faceVertexCounts = [4]
        int[] faceVertexIndices = [0, 1, 2, 3]
        point3f[] points = [(0.2, -1.5, 0), (3.2, -1.5, 0), (3.2, 1.5, 0), (0.2, 1.5, 0)]
        texCoord2f[] primvars:st = [(0, 0), (1, 0), (1, 1), (0, 1)] (interpolation = "faceVarying")
        uniform token subdivisionScheme = "none"
        rel material:binding = </World/mtl/font>
    }
    def Scope "mtl"
    {
        def Material "grid"
        {
            token outputs:surface.connect = </World/mtl/grid/pbs.outputs:surface>
            def Shader "pbs"
            {
                uniform token info:id = "UsdPreviewSurface"
                color3f inputs:diffuseColor.connect = </World/mtl/grid/tex.outputs:rgb>
                float inputs:roughness = 1
                color3f inputs:specularColor = (0, 0, 0)
                int inputs:useSpecularWorkflow = 1
                token outputs:surface
            }
            def Shader "st"
            {
                uniform token info:id = "UsdPrimvarReader_float2"
                string inputs:varname = "st"
                float2 outputs:result
            }
            def Shader "tex"
            {
                uniform token info:id = "UsdUVTexture"
                asset inputs:file = @%(grid)s@
                token inputs:sourceColorSpace = "raw"
                float2 inputs:st.connect = </World/mtl/grid/st.outputs:result>
                color3f outputs:rgb
            }
        }
        def Material "font"
        {
            token outputs:mtlx:surface.connect = </World/mtl/font/ss.outputs:out>
            def Shader "ss"
            {
                uniform token info:id = "ND_standard_surface_surfaceshader"
                color3f inputs:base_color.connect = </World/mtl/font/img.outputs:out>
                float inputs:specular = 0
                token outputs:out
            }
            def Shader "img"
            {
                uniform token info:id = "ND_image_color3"
                asset inputs:file = @%(font)s@
                color3f outputs:out
            }
        }
    }
}
''' % paths)

sys.argv = ['render_settings.py', out, scene, '1']
runpy.run_path(os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', 'render_settings.py'),
               run_name='__main__')
