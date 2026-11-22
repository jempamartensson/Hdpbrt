# Same scene rendered with pbrt (Hydra) and Cycles for comparison.
# blender -b --factory-startup --python blender_test2.py -- outdir
import sys, os, time
sys.path.insert(0, os.path.expanduser('~/dev/pbrt_hydra/hdPbrt/blender'))
import bpy, hydra_pbrt
hydra_pbrt.register()
outdir = sys.argv[sys.argv.index('--') + 1]
here = os.path.dirname(os.path.abspath(__file__))
sc = bpy.context.scene
cube = bpy.data.objects['Cube']
mat = cube.data.materials[0]
b = mat.node_tree.nodes['Principled BSDF']
b.inputs['Base Color'].default_value = (0.8, 0.1, 0.05, 1)
b.inputs['Roughness'].default_value = 0.35
bpy.ops.mesh.primitive_plane_add(size=12, location=(0, 0, -1))
pm = bpy.data.materials.new('checker')
pb = pm.node_tree.nodes['Principled BSDF']
img = pm.node_tree.nodes.new('ShaderNodeTexImage')
img.image = bpy.data.images.load(os.path.join(here, 'checker.png'))
pm.node_tree.links.new(img.outputs['Color'], pb.inputs['Base Color'])
bpy.context.object.data.materials.append(pm)
bpy.ops.mesh.primitive_uv_sphere_add(radius=0.8, location=(2.2, 0, -0.2), segments=48, ring_count=24)
bpy.ops.object.shade_smooth()
gm = bpy.data.materials.new('gold')
g = gm.node_tree.nodes['Principled BSDF']
g.inputs['Base Color'].default_value = (1.0, 0.75, 0.3, 1); g.inputs['Metallic'].default_value = 1.0
g.inputs['Roughness'].default_value = 0.1
bpy.context.object.data.materials.append(gm)
bpy.ops.mesh.primitive_uv_sphere_add(radius=0.7, location=(-2.2, 0.5, -0.3), segments=48, ring_count=24)
bpy.ops.object.shade_smooth()
glass = bpy.data.materials.new('glass')
gl = glass.node_tree.nodes['Principled BSDF']
gl.inputs['Transmission Weight'].default_value = 1.0; gl.inputs['Roughness'].default_value = 0.0
bpy.context.object.data.materials.append(glass)
# World: HDR environment
w = sc.world
env = w.node_tree.nodes.new('ShaderNodeTexEnvironment')
env.image = bpy.data.images.load('/opt/blender-5.3.0-linux-x64/5.3/datafiles/studiolights/world/courtyard.exr')
w.node_tree.links.new(env.outputs['Color'], w.node_tree.nodes['Background'].inputs['Color'])
bpy.data.objects['Light'].data.energy = 300
sc.render.resolution_x, sc.render.resolution_y, sc.render.resolution_percentage = 480, 270, 100
for engine in ('HYDRA_PBRT', 'CYCLES'):
    sc.render.engine = engine
    sc.hydra_pbrt.samples = 128
    sc.cycles.samples = 128
    sc.cycles.device = 'CPU'
    sc.render.filepath = os.path.join(outdir, 'bl_%s.png' % engine.lower())
    t = time.time()
    bpy.ops.render.render(write_still=True)
    print('RENDER', engine, '%.1fs' % (time.time() - t))
