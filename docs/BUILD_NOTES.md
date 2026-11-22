# hdPbrt: pbrt-v4 as a Hydra render delegate (Houdini 22 + Blender 5.3)

Built 2026-11-22 by Claude. CPU only (pbrt-v4 has no ROCm/HIP backend; its GPU path is CUDA/OptiX).

There was no existing open-source pbrt Hydra delegate (searched the web and GitHub for
hdPbrt / "pbrt hydra" / "pbrt usd delegate"), so `hdPbrt/` is new code. It borrows small
pieces (render buffer, instancer, implicit-surface scene index plugin) from USD's hdEmbree.

## Use it

- **Houdini Solaris:** pick **pbrt** in the LOP viewport's renderer menu.
  Package: `~/dev/houdini_packages/hdpbrt_pkg/hdpbrt.json` (listed in HOUDINI_PACKAGE_DIR in
  `~/.config/environment.d/houdini.conf`). Turn it on/off with `hdelegate pbrt` / the
  Render Delegates shelf tool, then restart Houdini.
- **rez:** `rez env houdini-22 hdpbrt -- houdini` (package in `~/dev/rez_packages/hdpbrt/0.1.0`).
- **Blender 5.3:** Preferences > Add-ons > enable **Hydra pbrt render engine**, then choose
  **Hydra pbrt** as the render engine. Settings are in Render Properties > pbrt.
  The add-on is a symlink in `~/.config/blender/5.3/scripts/addons/hydra_pbrt`.
  It only works in Blender 5.3 (the build is tied to Blender's USD 26.08).
- **Render settings** (Houdini: Render Settings LOP custom attributes or viewport delegate
  options; Blender: the add-on panel): `pbrt:pixelSamples` (256), `pbrt:maxDepth` (5),
  `pbrt:integrator` (volpath), `pbrt:sampler` (zsobol), `pbrt:threads` (0 = all but two),
  `pbrt:regularize`, `pbrt:depthOfField` (true), `pbrt:motionBlur` (true), `pbrt:xformSamples` (2), `pbrt:geoSamples` (2),
  `pbrt:exportPath` (also write the .pbrt scene there).
- **Husk:** not testable here: the Apprentice licence refuses third-party delegates in husk.

## How it works

The delegate does not link pbrt. Linking pbrt-v4 in-process was rejected because pbrt is
built around process-wide state (`Options`, a global thread pool, global buffer caches and
`Triangle::allMeshes` that only grow, image caches) and calls `exit()` on any scene error,
which would kill Houdini/Blender.

Instead:
1. Prims sync into a light copy of the scene. Each mesh is triangulated (`HdMeshUtil`) and
   written to a binary PLY in a temp dir as soon as it syncs (only changed meshes are
   rewritten). Face-varying normals/UVs are unwelded; indexed primvars are expanded.
2. When the scene, camera, settings or viewport size changes, the render pass writes a
   `.pbrt` scene that references the PLYs, kills the running pbrt, and starts
   `pbrt --display-server 127.0.0.1:<port> scene.pbrt` (restarts are debounced to 100 ms
   while tumbling).
3. pbrt streams its progressive image (waves of 1, 2, 4, ... 64 spp) over its tev
   display-server protocol to a socket the delegate listens on. The final image is read
   from the PFM pbrt writes when it exits.
4. pbrt is patched (branch `hdpbrt` in `src/pbrt-v4`; see `patches/`) to exit when its parent
   process dies (`PBRT_PARENT_PID`), so a Houdini crash doesn't leave renders running.
   The delegate strips `LD_LIBRARY_PATH` when it starts pbrt so Houdini's/Blender's
   bundled libraries don't leak into it.

Temp files go to `$HDPBRT_TMPDIR` or `/tmp/hdpbrt-<pid>-XXXXXX`, removed when the delegate
is destroyed; dirs of dead processes are cleaned at the next start.
Debug: `HDPBRT_KEEP_TMP=1` keeps them (scene_N.pbrt, mesh_N.ply, pbrt.log);
`HDPBRT_DEBUG=1` prints each mesh's primvars. `HDPBRT_PBRT_EXECUTABLE` overrides pbrt's path
(default: `pbrt` next to hdPbrt.so).

## What is translated

- **Geometry:** meshes (incl. holes, left-handed orientation), UsdGeom implicit shapes
  (converted to meshes by an `HdsiImplicitSurfaceSceneIndex` plugin), point instancers and
  nested instancers (pbrt ObjectBegin/ObjectInstance), basis curves, points and volumes. No
  camera motion blur yet.
  - **Volumes:** `Volume` rprims with `openvdbAsset` fields (and `houdiniFieldAsset` fields
    whose file is on disk; Houdini's in-memory `op:` volumes get a warning: cache them to
    disk). The field named `density` (else the first) and an optional `temperature` field
    in the same file are converted to NanoVDB by `hdpbrt_vdb2nvdb` (next to pbrt; cached per
    file, mtime and grid), which also writes the density's world-space active-voxel bounds.
    The scene gets `MakeNamedMedium "nanovdb"` under the volume's transform and an
    outward-facing `interface` box over those bounds (pbrt picks the entered medium from
    the box normals; an inward box renders nothing). No volume materials: sigma_s =
    `pbrt:volumeAlbedo` (0.9) x displayColor, sigma_a = 1 - sigma_s, `"float scale"` =
    `pbrt:volumeDensity` (1); temperature emits only with `pbrt:volumeTemperatureScale` > 0
    (blackbody of temperature x scale). Volumes don't write depth or ids, don't move and
    aren't instanced; the camera must be outside them.
  - **hdpbrt_vdb2nvdb** (`tools/vdb2nvdb.cpp`) is built with the Houdini flavor against
    `libopenvdb_sesi` (OpenVDB 13) and pbrt's own NanoVDB 32.3 headers
    (`-DPBRT_SOURCE`), so the file version matches pbrt's reader. pbrt's
    `OpenToNanoVDB.h` assigns OpenVDB 11+'s 64-bit `nodeCount()` to `uint32_t`, so CMake
    writes a fixed copy into the build dir. Houdini's toolkit also has a newer NanoVDB:
    pbrt's include dir must come first. The Blender install links to the Houdini one.
  - **Subdivision:** catmullClark and loop meshes are refined uniformly to
    `pbrt:subdivLevel` (default 2, max 6, 0 = off) with OpenSubdiv through USD's
    `PxOsdRefinerFactory` (creases, corners, holes and interpolation rules from
    `GetSubdivTags`, which Hydra delivers separately from the topology). Points and uvs
    (vertex, varying, face-varying; face-varying values are welded per vertex so shared
    corners refine smoothly) are refined; authored normals are ignored and smooth normals
    computed on the refined mesh. Deforming subdivision meshes refine every pose. The
    implicit spheres (catmullClark meshes from the implicit-surface scene index) come out
    round. Houdini links `libosdCPU`; Blender has `libosdCPU` 3.7 but no headers, so the
    build uses Houdini's (`OSD_INCLUDE`).
  - **Curves:** one pbrt `curve` per USD curve. bspline and cubic bezier map directly
    (periodic curves wrap, pinned bsplines get phantom end points); catmullRom (and
    centripetal, treated as uniform) and linear curves become bezier segments; hermite is
    treated as bezier. pbrt has a start and end width per curve, so vertex widths taper from
    the first to the last; no widths: 1 (Storm's fallback). With normals: `ribbon`
    (one normal per segment boundary picked from the vertices), else round `cylinder`.
    Curves don't deform (pbrt curves have no motion); transforms move.
  - **Points:** a sphere per point (diameter = width, fallback 1). Per-point colors are
    averaged like displayColor on meshes.
- **Materials:** UsdPreviewSurface, MaterialX `ND_standard_surface` and `ND_open_pbr_surface`
  (what Houdini's MtlX nodes and Blender's MaterialX export produce). pbrt has no uber
  material, so: metallic >= 0.5 -> `conductor` (reflectance = base color); transmission >= 0.5
  -> `dielectric`; else `coateddiffuse`. Roughness r is passed as GGX alpha r^2
  (`remaproughness false`). Base color, roughness and opacity can come from image textures
  (UsdUVTexture / MaterialX image), exr/png/hdr/pfm/jpg/tga only (no .rat/.tx/.tif, no UDIMs).
  Opacity becomes the shape's alpha (cut-out). Emission becomes a diffuse area light on the
  mesh (not for instances). No material -> diffuse with displayColor.
- **Lights:** distant, sphere (point if treatAsPoint/radius 0), rect, disk, cylinder (emissive
  shapes with a black material), dome (`infinite`; lat-long textures converted once to pbrt's
  equal-area layout with `imgtool makeequiarea`, cached). intensity, exposure, color,
  colorTemperature, normalize. No shaping/IES, no light-linking.
  The default lights HdxTaskController adds (GlfSimpleLight camera light at intensity 15000)
  are ignored; a scene without lights gets a uniform white environment.
- **Light shaping** (UsdLuxShapingAPI, pbrt patch 8): focus and focus tint, cone angle and
  softness, and IES profiles on sphere (area and point), rect and disk lights, applied as a
  multiplier on the emission in the light's frame (axis -z), following the spec and USD's
  reference implementation in hdEmbree. Written only for authored values; the API's
  default cone of 90 degrees limits a light to its hemisphere, as the spec says. IES files
  are parsed with Cycles' parser (`third_party/cycles_ies`), evaluated with the angle scale
  and optional normalization (divide by the average intensity over the covered
  (hemi)sphere), and tabulated into a 180x180 one-channel PFM that pbrt samples
  bilinearly. Raw candela values multiply the light's color, as in hdEmbree. Not on
  cylinder lights. pbrt's light sampling ignores shaping, so tight cones are noisier.
- **Light linking** (UsdLuxLightAPI `lightLink` / `shadowLink` collections, pbrt patch 9):
  Hydra resolves the collections only with `HdsiLightLinkingSceneIndex` in the scene index
  chain, which hdPbrt registers for itself (as hdPrman does). Lights then report a
  category token per non-trivial collection and geometry its categories; the delegate maps
  each token to a bit (up to 64) and writes `"integer hdpbrtlinks"` on shapes and
  `"integer hdpbrtlightlink"` / `"hdpbrtshadowlink"` on lights. In volpath (the default
  integrator; other integrators ignore links) a light skips surfaces outside its
  light-link collection, for light samples and for emission reached by BSDF sampling, and
  its shadow rays pass through surfaces outside its shadow-link collection (BSDF-sampled
  rays still stop there). Medium scattering points are lit by every light.
- **Camera:** the render pass state's view and projection (so viewport framing and window
  policy match), perspective and orthographic. Depth of field from the active HdCamera:
  fStop > 0 and focusDistance > 0 give pbrt's thin lens (`lensradius` = focalLength /
  (2 fStop), `focaldistance`; HdCamera values are in world units). Matches Karma's blur.
  Off with `pbrt:depthOfField` false or the render settings' `disableDepthOfField`.
- **Motion blur (transforms):** the camera's shutter:open/close (frames relative to the current
  time) become pbrt's `shutteropen`/`shutterclose` and `TransformTimes`. Meshes sample their
  transform, and instancers their transform and instance primvars, over a fixed window of
  +-1 frame when they sync (`HdPbrt_SampleWindow`); the scene writer resamples them at shutter
  open and close (`ActiveTransform StartTime/EndTime`). With `pbrt:xformSamples` N > 2, the
  keys are N evenly spaced shutter times, written as pbrt patch 6's
  `Attribute "shape" "float hdpbrtmotionxforms" [16*N] "float hdpbrtmotiontimes" [N]` before
  an `ObjectInstance` (a moving non-instanced mesh becomes a one-instance object; emissive
  ones keep two keys because pbrt can't instance area lights). pbrt's `MotionPrimitive`
  interpolates each segment like an AnimatedTransform, so curved motion follows its curve.
  For exact sub-frame transforms, time-varying transforms are evaluated on a 1/16-frame grid
  with `SampleTransform(id, t, t + 1e-3)` (USD evaluates an interval's edges exactly; a
  zero-width interval reports nothing time-varying); interpolating authored samples of the
  composed matrix would turn an orbit into a straight line. Between grid points, transforms
  are decomposed and slerped. A shutter longer than the +-1 frame window clamps. Lights
  don't move.
- **Camera motion blur:** the camera sprim is `HdPbrtCamera`, which samples its transform
  like the prims. The render pass turns it into views at shutter open and close
  (C(t)^-1 * C(0) * view, which keeps any extra viewport transform in `view`), and the
  scene gets pbrt's two-key camera: `TransformTimes` first (pbrt reads it when it creates
  the camera), then `ActiveTransform StartTime/EndTime` transforms. pbrt cameras have two
  keys, so camera motion is linear over the shutter. Our camera matrix flips z (USD
  cameras look down -z), and pbrt's `Transform::Decompose` didn't handle reflections
  (`XXX TODO FIXME deal with flip`): patch 7 moves the reflection into the scale, else a
  moving camera looks the wrong way. That also fixes animated mirrored objects.
- **Deformation blur:** stock pbrt-v4 has no moving triangles (motion comes only from
  animated transforms), so patch 5 adds end-pose vertices to `TriangleMesh`: positions and
  normals blend linearly by ray time between `"float motiontimes"` [t0 t1]; bounds cover both
  poses; area and solid angle (light sampling of emissive deforming meshes) use the start
  pose. Scene syntax: `trianglemesh` `"point3 P1"`/`"normal N1"`, `plymesh`
  `"string motionfilename"` (a PLY with the same vertices). The delegate samples `points`
  over +-1 frame at sync; when they move with a constant count it writes PLYs for shutter
  open and close at scene-write time (smooth normals recomputed per pose, authored normals
  static, uvs shared). `pbrt:geoSamples` N > 2 writes N poses (`"string motionfilenames"`,
  patch 6); points between authored samples are linear, as USD interpolates them. Off with `pbrt:motionBlur` false or the render
  settings' `disableMotionBlur`. Houdini: needs authored time samples (as Karma does; a LOP
  network evaluated per frame only has the current one, use a Cache LOP). Blender 5.3's
  Hydra export sends no shutter (0..0), so no motion blur there.
  Resampling at write time matters: re-dirtying the rprims from the render pass when the
  shutter changes worked in hython but never resynced them in Houdini's viewport.
- **AOVs:** color (pbrt image), depth, primId and instanceId (selection), elementId (-1).
  Ids: each mesh's Shape gets `"integer hdpbrtprimid" <HdRprim::GetPrimId()>`, each
  ObjectInstance is preceded by `Attribute "shape" "integer hdpbrtinstanceid" <i>` (i = index
  in the flattened instance list, nested instancers outer-major, as Hydra expects). The film
  (`"bool hdpbrtids" true`, patch 3) keeps the ids of the nearest first hit per pixel and
  streams them as display channels `primId`/`instanceId` (float values, exact up to 2^24).
  Alpha: with no dome light (and so no fallback white environment) the film also gets
  `"bool hdpbrtalpha" true` (patch 4): coverage of first hits, streamed as channel `A`, goes
  into the color AOV's alpha, so the host's viewport background shows through (pbrt's color
  is premultiplied, escaped rays are black). With an environment, alpha is 1.
  In Houdini the scene graph select tool then selects prims and point instances. Around a
  distant light's origin Houdini's own light guide wins the pick (Karma CPU does the same).
  Depth is real: pbrt's RGBFilm (patched, `"bool hdpbrtdepth" true`) keeps the nearest
  first-hit camera-space z per pixel and streams it as display channel `Z`; the render pass
  turns it into NDC depth in [0, 1] with the projection matrix. **Houdini's Vulkan viewport
  wants reversed depth (1 = near, 0 = far)**: with standard depth its grid/handles vanish.
  The Houdini build reverses, Blender doesn't; `HDPBRT_REVERSED_DEPTH=0/1` overrides.

## Layout (~/dev/pbrt_hydra)

- `src/pbrt-v4` pbrt-v4 at 20e41fd (2026-11-15) + branch `hdpbrt` (parent-PID watchdog, RGBFilm depth, id and alpha channels, deformation blur, N-key motion, Decompose reflections, light shaping, light linking).
- `src/OpenUSD-26.08`, `src/oneTBB-2022.3.0`: headers for the Blender build.
  `src/usd-blender-generated/pxr/pxr.h`: pxr.h filled in for Blender's USD
  (internal namespace `pxrBlender_v26_08`, version 2608).
- `hdPbrt/`: the delegate (git repo, branch main) and `blender/hydra_pbrt` add-on.
- `build/pbrt`, `build/hdpbrt-houdini`, `build/hdpbrt-blender`; `install/houdini`,
  `install/blender` (hdPbrt.so, plugInfo.json, pbrt + imgtool symlinks).
- `houdini/dso/usd_plugins/`: what Houdini reads via HOUDINI_PATH (package hpath and rez).
- `build.sh`: full rebuild. `test/`: scenes, scripts and renders. `logs/`.

## Build

`systemd-run --user --scope -p MemoryMax=64G -p MemorySwapMax=0 bash ~/dev/pbrt_hydra/build.sh`
(-j8). pbrt-v4 builds cleanly with GCC 14; it links the system OpenEXR 3.1 (Ubuntu), which
is why pbrt runs without Houdini's LD_LIBRARY_PATH.

Gotchas:
- Houdini ignores `PXR_PLUGINPATH_NAME` set from a package (hython never saw the plugin).
  Delegates must be registered through `HOUDINI_PATH` -> `dso/usd_plugins/plugInfo.json`,
  as hdMitsuba does.
- `HdRendererPlugin::IsSupported` changed between USD 26.05 (`HdRendererCreateArgs`) and
  26.08 (`HdRendererCreateArgsSchema`); `#if PXR_VERSION` handles both.
- Blender: no USD headers ship with Blender. The OpenUSD v26.08 source tree works as the
  include dir once `pxr/pxr.h` is generated with Blender's namespace. Python 3.13 headers
  come from Houdini (same version). The Blender flavor links `libusd_ms.so` and leaves
  Python symbols to be resolved by Blender at load time.
- Blender meshes arrive already triangulated: `ComputeTriangulatedFaceVaryingPrimvar` then
  returns `Unchanged` (not `Success`), which must be treated as "use the input".
- pbrt's `remaproughness` maps roughness r to alpha sqrt(r), much rougher than USD's r^2.

- Houdini's viewport (HdxTaskController, Houdini VK) draws NOTHING from a delegate that
  doesn't offer a `depth` AOV, even though the color buffer is filled (fixed 2026-11-22; hython
  FrameRecorder tests don't catch this). `HDPBRT_DEBUG=1` prints the AOV bindings per execute.
- Replace `install/houdini/hdPbrt.so` with cp to a temp name + mv while Houdini is running
  (`ninja install` would overwrite the mapped file in place).

## Tests (test/)

- `hython tests/hydra_render.py HdPbrtRendererPlugin out.png tests/motion_scene.usda 1`: the
  test scene with a moving sphere and point instances and a deforming quad, shutter 0..1,
  rendered at frame 1
  (`docs/images/motion_blur.png`).
- `hython tests/render_settings.py out.png tests/curved_scene.usda 1 pbrt:xformSamples=9
  pbrt:geoSamples=9` (with `HDPBRT_KEEP_TMP=1 HDPBRT_TMPDIR=<dir>`): an orbiting ball and a
  quad that rises and falls within the shutter. `docs/images/curved_motion.png` shows 2
  samples (chord, no deformation) next to 9 (arc, rise and fall).
- Live viewport test without touching the user's desktop: `test/viewport/` (vp_test.py,
  run_real.sh). Start a private headless compositor with an X server:
  `dbus-run-session` running `gnome-shell --headless --virtual-monitor 1920x1080 --wayland
  --no-x11 --wayland-display=wl-hdtest` and `WAYLAND_DISPLAY=wl-hdtest Xwayland :7 -geometry
  1920x1080 -fullscreen -ac`, then Houdini on DISPLAY=:7 with its own HOUDINI_USER_PREF_DIR and
  a 123.py that builds a scene, picks the renderer and flipbooks the viewport.
  (Rootless Xwayland of headless gnome-shell hangs; use the rootful one. Kill by PID, not pkill -f.)
- Selection: `vp_pick.py` (same setup) adds a point instancer, switches to the
  `scenegraphselect` state, clicks the viewport surface (the `RE_WindowWindow` QWindow) with
  QTest and logs `PICK ok/FAIL` per target to `$VPT_OUT.log`: instancer, ground, background,
  sphere, and the three point instances with `setSelectionPointInstances(True)`. All pass with
  pbrt; the build before patch 3 selects nothing.

- `hython test/hydra_render.py HdPbrtRendererPlugin out.png test/test_scene.usda`
  (UsdAppUtils.FrameRecorder; `make_scene.py` builds the scene: checker texture, glass via
  MaterialX, gold metal, emissive panel, point-instanced cubes, rect + sphere lights,
  lat-long dome with colored quadrants). 640x467 at 256 spp: about 12 s. `t_pkg.png` was
  rendered with only the Houdini package loaded.
- `hython test/interactive_test.py test/test_scene.usda`: 40 camera moves in 1.3 s with
  restarts, converges, no pbrt processes left behind.
- `blender -b --factory-startup --python test/blender_test2.py -- test/`: the same Blender
  scene in Hydra pbrt and Cycles (`bl_compare.png`, pbrt on top). Camera, HDRI orientation,
  texture orientation and the metal match. pbrt's coateddiffuse is darker, more saturated
  and noisier than Cycles' Principled diffuse.
