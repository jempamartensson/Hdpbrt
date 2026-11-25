# Measures Karma CPU's ND_blackbody (the table in hdPbrt/material.cpp,
# _karmaBlackbody): renders unlit quads whose emission is blackbody(T) with husk
# and prints the C table. Run with plain python3 inside a Houdini environment
# (needs husk on PATH and pbrt's imgtool):
#
#   rez env houdini-22 -- python3 measure_karma_blackbody.py <work dir> <imgtool>
import os
import subprocess
import sys

import numpy as np

work, imgtool = os.path.abspath(sys.argv[1]), sys.argv[2]
os.makedirs(work, exist_ok=True)
temps = list(range(0, 3000, 100)) + list(range(3000, 10000, 250)) + list(range(10000, 20001, 1000))
cols = 12
rows = (len(temps) + cols - 1) // cols
quads = ''
for i, t in enumerate(temps):
    cx, cy = (i % cols) * 2 - cols + 1, -((i // cols) * 2 - rows + 1)
    quads += f'''
    def Mesh "q{i}" (prepend apiSchemas = ["MaterialBindingAPI"])
    {{
        int[] faceVertexCounts = [4]
        int[] faceVertexIndices = [0, 1, 2, 3]
        point3f[] points = [({cx - 0.9}, {cy - 0.9}, 0), ({cx + 0.9}, {cy - 0.9}, 0), ({cx + 0.9}, {cy + 0.9}, 0), ({cx - 0.9}, {cy + 0.9}, 0)]
        rel material:binding = </World/m{i}>
    }}
    def Material "m{i}"
    {{
        token outputs:mtlx:surface.connect = </World/m{i}/s.outputs:out>
        def Shader "bb"
        {{
            uniform token info:id = "ND_blackbody"
            float inputs:temperature = {t}
            color3f outputs:out
        }}
        def Shader "s"
        {{
            uniform token info:id = "ND_surface_unlit"
            color3f inputs:emission_color.connect = </World/m{i}/bb.outputs:out>
            float inputs:emission = 1
            token outputs:out
        }}
    }}'''
with open(os.path.join(work, 'bb.usda'), 'w') as f:
    f.write(f'''#usda 1.0
(
    defaultPrim = "World"
    upAxis = "Y"
)
def Xform "World"
{{
    def Camera "camera"
    {{
        token projection = "orthographic"
        float horizontalAperture = {cols * 20}
        float verticalAperture = {rows * 20}
        double3 xformOp:translate = (0, 0, 5)
        uniform token[] xformOpOrder = ["xformOp:translate"]
    }}
{quads}
}}
''')
subprocess.check_call(['husk', '-R', 'BRAY_HdKarma', '--res', str(cols * 20), str(rows * 20),
                       '-c', '/World/camera', '--pixel-samples', '4', '-o', 'bb.exr', 'bb.usda'],
                      cwd=work)
subprocess.check_call([imgtool, 'convert', '--channels', 'R,G,B', 'bb.exr', '--outfile', 'bb.pfm'],
                      cwd=work)
with open(os.path.join(work, 'bb.pfm'), 'rb') as f:
    f.readline()
    w, h = map(int, f.readline().split())
    s = float(f.readline())
    img = np.frombuffer(f.read(), dtype='<f4' if s < 0 else '>f4').reshape(h, w, 3)[::-1]
for i, t in enumerate(temps):
    x, y = i % cols, i // cols
    r, g, b = img[y * 20 + 7:y * 20 + 13, x * 20 + 7:x * 20 + 13].reshape(-1, 3).mean(0)
    print('{%g, %.6g, %.6g, %.6g},' % (t, r, g, b))
