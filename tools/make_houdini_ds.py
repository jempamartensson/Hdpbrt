# hython tools/make_houdini_ds.py
# Writes houdini/soho/parameters/HdPbrtRendererPlugin_{Viewport,Global}.ds from
# SETTINGS below (keep it in step with HdPbrtRenderDelegate::_Init). Houdini
# finds the files by the Hydra plugin's name on HOUDINI_PATH:
#   _Viewport.ds: the viewport's display options (renderer settings, sent to
#                 the delegate with SetRenderSetting under the decoded name);
#   _Global.ds:   the Render Settings LOP and Edit Properties: each setting is
#                 a "pbrt:..." attribute on the RenderSettings prim, which
#                 Houdini (viewport and husk) hands to the delegate;
#   _Light.ds, _Camera.ds: a pbrt tab on light and camera LOPs ("pbrt:..."
#                 attributes on those prims, read by HdPbrtLight / HdPbrtCamera);
#   _Geometry.ds: a pbrt tab on the Render Geometry Settings LOP (primvars).
# Houdini has no such file for materials: see scripts/python/hdpbrt_houdini.py.
# Parm names are the setting names encoded with hou.text.encodeParm.
import os
import hou

here = os.path.dirname(os.path.abspath(__file__))
outdir = os.path.join(here, '..', 'houdini', 'soho', 'parameters')

# name, label, type (int/float/bool/string/menu), default, range or menu, category, help
SETTINGS = [
    ('pbrt:pixelSamples', 'Pixel Samples', 'int', 256, (1, 4096), 'Sampling',
     'Samples per pixel; the render stops there (the viewport refines progressively).'),
    ('pbrt:sampler', 'Sampler', 'menu', 'zsobol',
     ['zsobol', 'sobol', 'paddedsobol', 'halton', 'pmj02bn', 'stratified', 'independent'],
     'Sampling', "pbrt's sampler."),
    ('pbrt:maxDepth', 'Max Depth', 'int', 5, (1, 64), 'Integrator',
     'Maximum path length (1 = direct lighting only).'),
    ('pbrt:integrator', 'Integrator', 'menu', 'volpath',
     ['volpath', 'path', 'bdpt', 'mlt', 'sppm', 'simplepath', 'simplevolpath', 'lightpath',
      'ambientocclusion', 'randomwalk'],
     'Integrator', "pbrt's integrator. volpath handles media, light linking and all hdPbrt features."),
    ('pbrt:regularize', 'Regularize BSDFs', 'bool', False, None, 'Integrator',
     'Roughen near-specular BSDFs after the first diffuse bounce (fewer caustic fireflies, biased).'),
    ('pbrt:depthOfField', 'Depth of Field', 'bool', True, None, 'Camera',
     "Thin-lens depth of field from the camera's fStop and focusDistance."),
    ('pbrt:motionBlur', 'Motion Blur', 'bool', True, None, 'Motion Blur',
     "Transform and deformation blur over the camera's shutter."),
    ('pbrt:xformSamples', 'Transform Motion Samples', 'int', 2, (2, 16), 'Motion Blur',
     'Transform keys over the shutter (more than 2 follow curved motion).'),
    ('pbrt:geoSamples', 'Deformation Motion Samples', 'int', 2, (2, 16), 'Motion Blur',
     'Point-position keys over the shutter for deforming meshes.'),
    ('pbrt:subdivLevel', 'Subdivision Level', 'int', 2, (0, 6), 'Geometry',
     'Refinement level for catmullClark/loop meshes (0 = the cage).'),
    ('pbrt:displacement', 'Displacement', 'bool', True, None, 'Geometry',
     "True displacement of meshes by their material's displacement (height maps with uvs)."),
    ('pbrt:dicingRate', 'Dicing Rate (pixels)', 'float', 2.0, (0.25, 16), 'Geometry',
     'Edge length of displaced triangles on screen, in pixels (smaller = finer, slower). Geometry '
     'outside the view is diced 4x more coarsely.'),
    ('pbrt:dicingMaxTriangles', 'Dicing Max Triangles', 'int', 20000000, (100000, 100000000),
     'Geometry', 'Triangle budget per displaced mesh: past it the mesh is diced more coarsely.'),
    ('pbrt:volumeDensity', 'Volume Density Scale', 'float', 1.0, (0, 10), 'Volumes',
     'Density scale for volumes without a pyro shader.'),
    ('pbrt:volumeAlbedo', 'Volume Albedo', 'float', 0.9, (0, 1), 'Volumes',
     'Single-scattering albedo for volumes without a pyro shader.'),
    ('pbrt:volumeTemperatureScale', 'Volume Temperature Scale', 'float', 0.0, (0, 5000), 'Volumes',
     'Kelvin per unit of a temperature field (0 = no emission) for volumes without a pyro shader.'),
    ('pbrt:materialPreview', 'Material Preview', 'bool', False, None, 'Materials',
     "Render materials' UsdPreviewSurface (e.g. the preview a Karma Material Builder writes) "
     'instead of their MaterialX / Karma graph, to compare.'),
    ('pbrt:threads', 'Threads', 'int', 0, (0, 256), 'System',
     'Render threads (0 = all but two cores).'),
    ('pbrt:exportPath', 'Export .pbrt Scene To', 'file', '', None, 'System',
     'Also write the pbrt scene (with its PLY meshes) here.'),
]

# Per-prim properties (same tuple layout; an optional 8th item: the setting this
# one shows with, as (name, value)).
LIGHT_SETTINGS = [
    ('pbrt:spectrum', 'Spectrum', 'menu', 'rgb', ['rgb', 'band', 'blackbody'], 'Spectrum',
     'rgb: the light\'s color (UsdLux). band: a Gaussian at Wavelength with FWHM Bandwidth (nm, '
     '360-1100), carrying the radiant power of a white light of the same intensity, so it can be '
     'invisible infrared. blackbody: a spectral black body at the color temperature (infrared '
     'included), as bright as a white light. The spectral modes ignore color; textured lights '
     'stay RGB.'),
    ('pbrt:wavelength', 'Wavelength (nm)', 'float', 850.0, (360, 1100), 'Spectrum',
     'Peak wavelength of a band light.', ('pbrt:spectrum', 'band')),
    ('pbrt:bandwidth', 'Bandwidth (nm)', 'float', 40.0, (1, 400), 'Spectrum',
     'Full width at half maximum of a band light.', ('pbrt:spectrum', 'band')),
]
CAMERA_SETTINGS = [
    ('pbrt:sensor', 'Sensor', 'menu', 'cie1931', ['cie1931', 'bands'], 'Sensor',
     'cie1931: a color camera (the image in the working color space). bands: the image\'s R, G '
     'and B are three Gaussian spectral responses in nm (360-1100), written raw: infrared false '
     'color (default bands), monochrome infrared (three equal bands), ...'),
    ('pbrt:sensorRed', 'Red Band (center, FWHM nm)', 'float2', (850.0, 100.0), (360, 1100), 'Sensor',
     'Spectral response written to the red channel.', ('pbrt:sensor', 'bands')),
    ('pbrt:sensorGreen', 'Green Band (center, FWHM nm)', 'float2', (650.0, 80.0), (360, 1100),
     'Sensor', 'Spectral response written to the green channel.', ('pbrt:sensor', 'bands')),
    ('pbrt:sensorBlue', 'Blue Band (center, FWHM nm)', 'float2', (550.0, 80.0), (360, 1100),
     'Sensor', 'Spectral response written to the blue channel.', ('pbrt:sensor', 'bands')),
]

# Render Geometry Settings LOP: primvars on geometry prims.
GEOMETRY_SETTINGS = [
    ('primvars:pbrt:irReflectance', 'Infrared Reflectance', 'float', 0.6, (0, 1), 'Infrared',
     'Diffuse reflectance above the red edge for these objects, for infrared renders '
     '(pbrt:sensor bands on the camera); overrides the material\'s. Real leaves reflect about '
     '0.5-0.6 there. The visible image is unchanged.'),
]

VALUE_TYPE = {'int': 'int', 'float': 'float', 'bool': 'bool', 'string': 'string', 'menu': 'token',
              'file': 'string', 'float2': 'float2'}
PARM_TYPE = {'int': 'integer', 'float': 'float', 'bool': 'toggle', 'string': 'string',
             'menu': 'string', 'file': 'file', 'float2': 'float'}


def q(s):
    return '"%s"' % str(s).replace('\\', '\\\\').replace('"', '\\"')


def value_parm(name, label, kind, default, extra, category, help, scope, usd, shows=None):
    enc = hou.text.encodeParm(name)
    lines = ['    parm {', '        name    %s' % q(enc), '        label   %s' % q(label),
             '        type    %s' % PARM_TYPE[kind],
             '        size    %d' % (2 if kind == 'float2' else 1)]
    if kind == 'float2':
        lines.append('        default { %s %s }' % default)
        lines.append('        range   { %s %s }' % extra)
    elif kind == 'bool':
        lines.append('        default { %d }' % int(default))
    elif kind in ('int', 'float'):
        lines.append('        default { %s }' % default)
        if extra:
            lines.append('        range   { %s %s }' % extra)
    else:
        lines.append('        default { %s }' % q(default))
    if kind == 'menu':
        lines.append('        menu {')
        for m in extra:
            lines.append('            %s %s' % (q(m), q(m)))
        lines.append('        }')
    lines.append('        help    %s' % q(help))
    lines.append('        parmtag { "spare_category" %s }' % q(category))
    lines.append('        parmtag { "uiscope" %s }' % q(scope))
    lines.append('        parmtag { "usdvaluetype" %s }' % q(VALUE_TYPE[kind]))
    if usd:
        c = hou.text.encodeParm(name + '_control')
        lines.append('        disablewhen "{ %s == block } { %s == none }"' % (c, c))
    if shows:
        lines.append('        hidewhen "{ %s != %s }"' % (hou.text.encodeParm(shows[0]), q(shows[1])))
    lines.append('    }')
    return lines


def control_parm(name, label, kind):
    vt = {'int': 'int', 'float': 'float', 'bool': 'bool', 'float2': 'float2'}.get(kind, 'string')
    return ['    parm {', '        name    %s' % q(hou.text.encodeParm(name + '_control')),
            '        label   %s' % q(label), '        type    string',
            '        default { "none" }', '        menujoin {',
            '            [ "import loputils" ]',
            '            [ "return loputils.createEditPropertiesControlMenu(kwargs, %s)" ]' % q(vt).replace('"', "'"),
            '            language python', '        }', '        parmtag { "sidefx::look" "icon" }',
            '    }']


def write(fname, header, usd, settings=SETTINGS):
    out = ['/// Generated by tools/make_houdini_ds.py; edit that, not this file.', header,
           '#include "$HFS/houdini/soho/parameters/CommonMacros.ds"', '{',
           '    name    "pbrt"', '    label   "pbrt"',
           '    parmtag { spare_opfilter "!!SHOP/PROPERTIES!!" }',
           '    parmtag { spare_classtags "render" }', '']
    for entry in settings:
        name, label, kind, default, extra, category, help = entry[:7]
        shows = entry[7] if len(entry) > 7 else None
        if usd:
            out += control_parm(name, label, kind)
        out += value_parm(name, label, kind, default, extra, category, help,
                          'viewport', usd, shows)
        out.append('')
    out.append('}')
    path = os.path.join(outdir, fname)
    with open(path, 'w') as f:
        f.write('\n'.join(out) + '\n')
    print('wrote', os.path.normpath(path))


os.makedirs(outdir, exist_ok=True)
write('HdPbrtRendererPlugin_Viewport.ds',
      '/// hdPbrt renderer settings in the viewport display options (SetRenderSetting).', False)
write('HdPbrtRendererPlugin_Global.ds',
      '/// hdPbrt render settings for the Render Settings LOP ("pbrt:*" attributes on the\n'
      '/// RenderSettings prim).', True)
write('HdPbrtRendererPlugin_Light.ds',
      '/// hdPbrt light properties ("pbrt:*" attributes on UsdLux light prims).', True,
      LIGHT_SETTINGS)
write('HdPbrtRendererPlugin_Camera.ds',
      '/// hdPbrt camera properties ("pbrt:*" attributes on camera prims).', True,
      CAMERA_SETTINGS)
write('HdPbrtRendererPlugin_Geometry.ds',
      '/// hdPbrt geometry properties (constant "primvars:pbrt:*" on geometry prims).', True,
      GEOMETRY_SETTINGS)
