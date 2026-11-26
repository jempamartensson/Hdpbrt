# SPDX-License-Identifier: Apache-2.0
"""pbrt-v4 render engine for Blender, through Blender's Hydra render engine
support and the hdPbrt render delegate (built against Blender's USD)."""

bl_info = {
    "name": "Hydra pbrt render engine",
    "author": "hdPbrt",
    "version": (0, 1, 0),
    "blender": (5, 3, 0),
    "description": "pbrt-v4 (CPU) path tracer through Hydra",
    "category": "Render",
}

import os
import bpy

# Directory holding hdPbrt.so + plugInfo.json (+ the pbrt/imgtool executables).
# Defaults to the "plugin" folder (or symlink) next to this file.
PLUGIN_DIR = os.environ.get(
    "HDPBRT_BLENDER_PLUGIN_DIR",
    os.path.join(os.path.dirname(os.path.realpath(__file__)), "plugin"))


_warned_working_space = set()


def _check_working_space(engine):
    """hdPbrt renders in ACEScg: material and light colors are read as ACEScg
    and the image is ACEScg. Blender reads the render as the file's working
    space, so anything other than ACEScg shows shifted colors."""
    cs = getattr(bpy.data, "colorspace", None)
    ws = getattr(cs, "working_space", "ACEScg") if cs else "ACEScg"
    key = (bpy.data.filepath, ws)
    if ws != "ACEScg" and key not in _warned_working_space:
        _warned_working_space.add(key)
        msg = ("Hydra pbrt renders in ACEScg, but this file's working color space is "
               "%s: set Color Management > Working Space to ACEScg" % ws)
        print("hdPbrt:", msg)
        try:
            engine.report({'WARNING'}, msg)
        except Exception:
            pass


class PbrtHydraRenderEngine(bpy.types.HydraRenderEngine):
    bl_idname = 'HYDRA_PBRT'
    bl_label = "Hydra pbrt"
    bl_info = "pbrt-v4 physically based path tracer (CPU)"

    bl_use_preview = False
    bl_use_gpu_context = False
    bl_use_materialx = True

    bl_delegate_id = 'HdPbrtRendererPlugin'

    @classmethod
    def register(cls):
        import pxr.Plug
        pxr.Plug.Registry().RegisterPlugins([PLUGIN_DIR])

    def get_render_settings(self, engine_type):
        _check_working_space(self)
        s = bpy.context.scene.hydra_pbrt
        viewport = engine_type == 'VIEWPORT'
        result = {
            'pbrt:pixelSamples': s.viewport_samples if viewport else s.samples,
            'pbrt:maxDepth': s.max_depth,
            'pbrt:integrator': s.integrator,
            'pbrt:sampler': s.sampler,
            'pbrt:threads': s.threads,
            'pbrt:regularize': s.regularize,
        }
        if not viewport:
            result['aovToken:Combined'] = "color"
            if s.export_path:
                result['pbrt:exportPath'] = bpy.path.abspath(s.export_path)
        return result

    def update_render_passes(self, scene, render_layer):
        if render_layer.use_pass_combined:
            self.register_pass(scene, render_layer, 'Combined', 4, 'RGBA', 'COLOR')


class PbrtSceneProperties(bpy.types.PropertyGroup):
    samples: bpy.props.IntProperty(
        name="Render Samples", description="Samples per pixel for final renders",
        default=256, min=1, soft_max=8192)
    viewport_samples: bpy.props.IntProperty(
        name="Viewport Samples", description="Samples per pixel in the viewport",
        default=64, min=1, soft_max=4096)
    max_depth: bpy.props.IntProperty(
        name="Max Bounces", default=5, min=1, max=128)
    integrator: bpy.props.EnumProperty(
        name="Integrator",
        items=[('volpath', "Volumetric Path", ""),
               ('path', "Path", ""),
               ('bdpt', "Bidirectional Path", ""),
               ('simplepath', "Simple Path", ""),
               ('lightpath', "Light Path", ""),
               ('ambientocclusion', "Ambient Occlusion", "")],
        default='volpath')
    sampler: bpy.props.EnumProperty(
        name="Sampler",
        items=[('zsobol', "Z-Sobol", ""), ('sobol', "Sobol", ""), ('halton', "Halton", ""),
               ('pmj02bn', "PMJ02BN", ""), ('paddedsobol', "Padded Sobol", ""),
               ('independent', "Independent", "")],
        default='zsobol')
    threads: bpy.props.IntProperty(
        name="Threads", description="0 = all cores but two", default=0, min=0)
    regularize: bpy.props.BoolProperty(
        name="Regularize", description="Roughen near-specular paths to reduce fireflies",
        default=False)
    export_path: bpy.props.StringProperty(
        name="Export .pbrt", description="Also write the pbrt scene file here on final renders",
        subtype='FILE_PATH', default="")


class PBRT_HYDRA_RENDER_PT_settings(bpy.types.Panel):
    bl_space_type = 'PROPERTIES'
    bl_region_type = 'WINDOW'
    bl_context = 'render'
    bl_label = "pbrt"
    COMPAT_ENGINES = {PbrtHydraRenderEngine.bl_idname}

    @classmethod
    def poll(cls, context):
        return context.engine in cls.COMPAT_ENGINES

    def draw(self, context):
        layout = self.layout
        layout.use_property_split = True
        layout.use_property_decorate = False
        s = context.scene.hydra_pbrt
        col = layout.column(align=True)
        col.prop(s, "viewport_samples")
        col.prop(s, "samples")
        layout.prop(s, "max_depth")
        layout.prop(s, "integrator")
        layout.prop(s, "sampler")
        layout.prop(s, "threads")
        layout.prop(s, "regularize")
        layout.prop(s, "export_path")


def _compat_panels():
    exclude = {'RENDER_PT_stamp', 'DATA_PT_light', 'DATA_PT_spot', 'NODE_DATA_PT_light',
               'DATA_PT_falloff_curve', 'RENDER_PT_post_processing', 'RENDER_PT_simplify',
               'SCENE_PT_audio', 'RENDER_PT_freestyle'}
    eevee = {'MATERIAL_PT_preview', 'EEVEE_MATERIAL_PT_context_material',
             'EEVEE_MATERIAL_PT_surface', 'EEVEE_MATERIAL_PT_volume',
             'EEVEE_MATERIAL_PT_settings', 'EEVEE_WORLD_PT_surface'}
    for cls in bpy.types.Panel.__subclasses__():
        compat = getattr(cls, 'COMPAT_ENGINES', None)
        if compat is None:
            continue
        if ('BLENDER_RENDER' in compat and cls.__name__ not in exclude) or \
           ('BLENDER_EEVEE' in compat and cls.__name__ in eevee):
            yield cls


_classes = (PbrtHydraRenderEngine, PbrtSceneProperties, PBRT_HYDRA_RENDER_PT_settings)


def register():
    for c in _classes:
        bpy.utils.register_class(c)
    bpy.types.Scene.hydra_pbrt = bpy.props.PointerProperty(type=PbrtSceneProperties)
    for cls in _compat_panels():
        cls.COMPAT_ENGINES.add(PbrtHydraRenderEngine.bl_idname)


def unregister():
    for cls in _compat_panels():
        cls.COMPAT_ENGINES.discard(PbrtHydraRenderEngine.bl_idname)
    del bpy.types.Scene.hydra_pbrt
    for c in reversed(_classes):
        bpy.utils.unregister_class(c)
