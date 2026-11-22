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
  `pbrt:regularize`, `pbrt:depthOfField` (true), `pbrt:motionBlur` (true), `pbrt:exportPath` (also write the .pbrt scene there).
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

- **Geometry:** meshes (incl. holes, left-handed orientation, smooth normals for subdiv
  scheme meshes without normals), UsdGeom implicit shapes (converted to meshes by an
  `HdsiImplicitSurfaceSceneIndex` plugin: coarse tessellation), point instancers and
  nested instancers (pbrt ObjectBegin/ObjectInstance). No curves, points, volumes, subdiv
  refinement, deformation blur or camera motion blur yet.
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
- **Camera:** the render pass state's view and projection (so viewport framing and window
  policy match), perspective and orthographic. Depth of field from the active HdCamera:
  fStop > 0 and focusDistance > 0 give pbrt's thin lens (`lensradius` = focalLength /
  (2 fStop), `focaldistance`; HdCamera values are in world units). Matches Karma's blur.
  Off with `pbrt:depthOfField` false or the render settings' `disableDepthOfField`.
- **Motion blur (transforms):** the camera's shutter:open/close (frames relative to the current
  time) become pbrt's `shutteropen`/`shutterclose` and `TransformTimes`. Meshes sample their
  transform, and instancers their transform and instance primvars, over a fixed window of
  +-1 frame when they sync (`HdPbrt_SampleWindow`); the scene writer resamples them at shutter
  open and close (`ActiveTransform StartTime/EndTime`). pbrt interpolates between those two,
  so a shutter longer than the window, or curved motion within it, is approximated. No
  deformation blur (pbrt has no moving triangles), no camera motion (the render pass gets a
  single view matrix), lights don't move. Off with `pbrt:motionBlur` false or the render
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

- `src/pbrt-v4` pbrt-v4 at 20e41fd (2026-11-15) + branch `hdpbrt` (parent-PID watchdog, RGBFilm depth, id and alpha channels).
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
  test scene with a moving sphere and point instances, shutter 0..1, rendered at frame 1
  (`docs/images/motion_blur.png`).
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
