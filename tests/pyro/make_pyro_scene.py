# Builds the Karma pyro comparison scene (run with hython):
#
#   hython make_pyro_scene.py <out dir>
#
# Writes pyro_test.vdb (a noisy sphere of "density" and a hot core of
# "temperature"), pyro_mat.usda (a Karma Pyro Shader material exported by
# Houdini, so it isn't kept in the repo) and pyro_scene.usda: five frames lit by
# a sun, Karma volume limit 8 (hdPbrt: pbrt:maxDepth=9).
#   1 smoke   2 fire only, ramp color   3 fire only, blackbody
#   4 tinted denser smoke   5 grey smoke + blackbody fire (fireintscale 4)
import os
import sys

import hou

out = os.path.abspath(sys.argv[1])
os.makedirs(out, exist_ok=True)

# Fields.
geo = hou.node('/obj').createNode('geo', 'vol')
sphere = geo.createNode('sphere')
sphere.parm('type').set('poly')


def fog(name):
    n = geo.createNode('vdbfrompolygons')
    n.setInput(0, sphere)
    n.parm('builddistance').set(False)
    n.parm('buildfog').set(True)
    n.parm('fogname').set(name)
    n.parm('voxelsize').set(0.03)
    return n


dens = geo.createNode('volumewrangle')
dens.setInput(0, fog('density'))
dens.parm('snippet').set(
    'float r = length(@P); float n = noise(@P * 4.0) * 0.6 + 0.4;'
    ' @density = clamp((1.0 - r) * 3.0, 0, 1) * n;')
temp = geo.createNode('volumewrangle')
temp.setInput(0, fog('temperature'))
temp.parm('snippet').set(
    'float r = length(@P - set(0, -0.3, 0));'
    ' @temperature = clamp(1.0 - r * 1.6, 0, 1) * (noise(@P * 5.0) * 0.5 + 0.75);')
merge = geo.createNode('merge')
merge.setInput(0, dens)
merge.setInput(1, temp)
merge.geometry().saveToFile(os.path.join(out, 'pyro_test.vdb'))

# Material.
lib = hou.node('/stage').createNode('materiallibrary', 'mlib')
shader = lib.createNode('kma_pyroshader', 'pyro')
shader.setMaterialFlag(True)
lib.parm('materials').set(1)
lib.parm('matnode1').set('pyro')
lib.parm('matpath1').set('/materials/pyro')
lib.cook(force=True)
lib.stage().Export(os.path.join(out, 'pyro_mat.usda'))

with open(os.path.join(out, 'pyro_scene.usda'), 'w') as f:
    f.write('''#usda 1.0
(
    defaultPrim = "World"
    endTimeCode = 5
    metersPerUnit = 1
    startTimeCode = 1
    upAxis = "Y"
    subLayers = [@./pyro_mat.usda@]
)

over "materials"
{
    over "pyro"
    {
        over "pyro"
        {
            int inputs:enablesmoke.timeSamples = {1: 1, 2: 0, 3: 0, 4: 1, 5: 1}
            int inputs:enablefire.timeSamples = {1: 0, 2: 1, 3: 1, 4: 0, 5: 1}
            int inputs:firecolormode.timeSamples = {1: 0, 2: 0, 3: 1, 4: 0, 5: 1}
            float inputs:fireintscale.timeSamples = {1: 1, 4: 1, 5: 4}
            color3f inputs:smokecolor.timeSamples = {1: (1, 1, 1), 2: (1, 1, 1), 3: (1, 1, 1), 4: (0.9, 0.6, 0.3), 5: (0.5, 0.5, 0.5)}
            float inputs:densityscale.timeSamples = {1: 1, 2: 1, 3: 1, 4: 3, 5: 2}
        }
    }
}

def Xform "World"
{
    def Camera "camera"
    {
        float focalLength = 35
        float horizontalAperture = 36
        float verticalAperture = 36
        float2 clippingRange = (0.1, 100)
        double3 xformOp:translate = (0, 0, 4)
        uniform token[] xformOpOrder = ["xformOp:translate"]
    }
    def DistantLight "sun"
    {
        float inputs:angle = 1
        bool inputs:normalize = 1
        float inputs:intensity = 3
        float3 xformOp:rotateXYZ = (-40, 30, 0)
        uniform token[] xformOpOrder = ["xformOp:rotateXYZ"]
    }
    def Volume "smoke" (
        prepend apiSchemas = ["MaterialBindingAPI"]
    )
    {
        rel field:density = </World/smoke/density>
        rel field:temperature = </World/smoke/temperature>
        rel material:binding = </materials/pyro>
        int karma:object:volumelimit = 8
        int primvars:karma:object:volumelimit = 8
        def OpenVDBAsset "density"
        {
            asset filePath = @./pyro_test.vdb@
            token fieldName = "density"
        }
        def OpenVDBAsset "temperature"
        {
            asset filePath = @./pyro_test.vdb@
            token fieldName = "temperature"
        }
    }
}

def Scope "Render"
{
    def RenderSettings "Settings"
    {
        rel camera = </World/camera>
        uniform int2 resolution = (240, 240)
    }
}
''')
print('wrote', out)
