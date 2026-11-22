# blender -b --factory-startup --python blender_test.py -- out.png
import sys, os, time
sys.path.insert(0, os.path.expanduser('~/dev/pbrt_hydra/hdPbrt/blender'))
import bpy
import hydra_pbrt
hydra_pbrt.register()
out = sys.argv[sys.argv.index('--') + 1]

sc = bpy.context.scene
# default scene: cube, light, camera. Add a red material, ground and a sphere.
cube = bpy.data.objects['Cube']
m = bpy.data.materials.new('red'); m.use_nodes = True
bsdf = m.node_tree.nodes['Principled BSDF']
bsdf.inputs['Base Color'].default_value = (0.8, 0.1, 0.05, 1)
bsdf.inputs['Roughness'].default_value = 0.3
cube.data.materials.append(m)
bpy.ops.mesh.primitive_plane_add(size=20, location=(0, 0, -1))
bpy.ops.mesh.primitive_uv_sphere_add(radius=0.8, location=(2.2, 0, -0.2), segments=48, ring_count=24)
bpy.ops.object.shade_smooth()
gm = bpy.data.materials.new('gold'); gm.use_nodes = True
b2 = gm.node_tree.nodes['Principled BSDF']
b2.inputs['Base Color'].default_value = (1.0, 0.75, 0.3, 1); b2.inputs['Metallic'].default_value = 1.0
b2.inputs['Roughness'].default_value = 0.15
bpy.context.object.data.materials.append(gm)
sc.world.color = (0.05, 0.05, 0.08)
sc.render.engine = 'HYDRA_PBRT'
sc.render.resolution_x, sc.render.resolution_y, sc.render.resolution_percentage = 480, 270, 100
sc.hydra_pbrt.samples = 64
sc.hydra_pbrt.export_path = os.path.splitext(out)[0] + '.pbrt'
sc.render.filepath = out
t = time.time()
bpy.ops.render.render(write_still=True)
print('RENDER DONE %.1fs' % (time.time() - t), os.path.exists(out))
