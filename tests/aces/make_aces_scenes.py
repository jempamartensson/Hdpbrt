# hython make_aces_scenes.py OUTDIR
# Scenes for the ACEScg working-space check (run_aces.sh): a camera filling the
# frame with one diffuse quad under a white dome (radiance = albedo), its albedo
# a constant or a red texture in different files; and a white quad under a
# distant light (white, 3000 K, 6500 K).
import os, sys
import OpenImageIO as oiio, numpy as np
out = sys.argv[1]
os.makedirs(out, exist_ok=True)

def write(name, rgb, fmt, chroma=None):
    spec = oiio.ImageSpec(4, 4, 3, fmt)
    if chroma:
        spec.attribute('chromaticities', 'float[8]', chroma)
    o = oiio.ImageOutput.create(name)
    o.open(os.path.join(out, name), spec)
    o.write_image(np.tile(np.array(rgb, np.float32), (4, 4, 1)))
    o.close()

write('red_srgb.png', [1, 0, 0], 'uint8')
write('red_lin.exr', [1, 0, 0], 'float')
write('red_lin709.exr', [1, 0, 0], 'float', (0.64, 0.33, 0.30, 0.60, 0.15, 0.06, 0.3127, 0.3290))
write('red_linap1.exr', [1, 0, 0], 'float', (0.713, 0.293, 0.165, 0.830, 0.128, 0.044, 0.32168, 0.33767))
write('red_lin.hdr', [1, 0, 0], 'float')

HEAD = '''#usda 1.0
(
    upAxis = "Y"
)
def Xform "World"
{
    def Camera "cam"
    {
        token projection = "orthographic"
        float horizontalAperture = 10
        float verticalAperture = 10
        double3 xformOp:translate = (0, 0, 10)
        uniform token[] xformOpOrder = ["xformOp:translate"]
    }
    def Mesh "q" (prepend apiSchemas = ["MaterialBindingAPI"])
    {
        int[] faceVertexCounts = [4]
        int[] faceVertexIndices = [0, 1, 2, 3]
        point3f[] points = [(-3, -3, 0), (3, -3, 0), (3, 3, 0), (-3, 3, 0)]
        texCoord2f[] primvars:st = [(0, 0), (1, 0), (1, 1), (0, 1)] (interpolation = "faceVarying")
        uniform token subdivisionScheme = "none"
        rel material:binding = </World/m>
    }
'''
def material(diffuse):
    return '''    def Material "m"
    {
        token outputs:surface.connect = </World/m/pbs.outputs:surface>
        def Shader "pbs"
        {
            uniform token info:id = "UsdPreviewSurface"
            %s
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
''' % diffuse

for name, f in [('const', None), ('png', 'red_srgb.png'), ('exr', 'red_lin.exr'),
                ('exr709', 'red_lin709.exr'), ('exrap1', 'red_linap1.exr'), ('hdr', 'red_lin.hdr')]:
    s = HEAD + '    def DomeLight "dome"\n    {\n        float inputs:intensity = 1\n    }\n'
    if f:
        s += material('color3f inputs:diffuseColor.connect = </World/m/tex.outputs:rgb>')
        s += '''        def Shader "tex"
        {
            uniform token info:id = "UsdUVTexture"
            asset inputs:file = @./%s@
            float2 inputs:st.connect = </World/m/st.outputs:result>
            color3f outputs:rgb
        }
''' % f
    else:  # Rec.709 red in ACEScg numbers (inside pbrt's reachable gamut)
        s += material('color3f inputs:diffuseColor = (0.6131, 0.0702, 0.0206)')
    s += '    }\n}\n'
    open(os.path.join(out, 'albedo_%s.usda' % name), 'w').write(s)

for name, light in [('white', 'color3f inputs:color = (1, 1, 1)'),
                    ('t3000', 'bool inputs:enableColorTemperature = 1\n        float inputs:colorTemperature = 3000'),
                    ('t6500', 'bool inputs:enableColorTemperature = 1\n        float inputs:colorTemperature = 6500')]:
    s = HEAD + '''    def DistantLight "sun"
    {
        float inputs:intensity = 1
        float inputs:angle = 0
        %s
    }
''' % light
    s += material('color3f inputs:diffuseColor = (1, 1, 1)') + '    }\n}\n'
    open(os.path.join(out, 'light_%s.usda' % name), 'w').write(s)
