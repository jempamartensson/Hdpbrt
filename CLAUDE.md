# hdPbrt: working notes for Claude Code

hdPbrt is a Hydra render delegate for pbrt-v4, used from Houdini 22 Solaris (Apprentice) and
Blender 5.3. It is CPU only, on a Strix Halo machine (gfx1151, 128 GB). This repo is
`git@github.com:jempamartensson/Hdpbrt.git` (branch `main`). It pushes over SSH with
`~/.ssh/id_ed25519`, which is on Jens's GitHub. There are no HTTPS credentials and no `gh`.

Full design and gotchas: `docs/BUILD_NOTES.md` (the same file as `~/dev/pbrt_hydra/BUILD_NOTES.md`).

## Layout on this machine

- `~/dev/pbrt_hydra/`: the working tree.
  - `hdPbrt/`: this repo (delegate source, `blender/hydra_pbrt` add-on, patches, docs, tests).
  - `src/pbrt-v4`: pbrt-v4 at `20e41fd` plus branch `hdpbrt` (6 commits, the same as `patches/`;
    regenerate with `git -C ../src/pbrt-v4 format-patch -q 20e41fd -o patches`).
  - `src/OpenUSD-26.08`, `src/oneTBB-2022.3.0`, `src/usd-blender-generated`: headers for
    the Blender flavor.
  - `build/pbrt`, `build/hdpbrt-houdini`, `build/hdpbrt-blender`: build dirs.
  - `install/houdini`, `install/blender`: hdPbrt.so, plugInfo.json, and symlinks to pbrt and
    imgtool.
  - `houdini/dso/usd_plugins/`: what Houdini loads through `HOUDINI_PATH`.
  - `build.sh`: full local rebuild. The repo's `scripts/build.sh` is the portable version.
  - `test/`: scenes, renders, `viewport/` (live-viewport test). The repo copy is `tests/`.
- Houdini package: `~/dev/houdini_packages/hdpbrt_pkg`. rez package: `hdpbrt-0.1.0` in
  `~/dev/rez_packages`. Jens runs `rez env houdini-22 hdpbrt -- houdini`.
- Blender add-on symlink: `~/.config/blender/5.3/scripts/addons/hydra_pbrt`.

## How it works (short)

Hydra prims become PLY files plus a `.pbrt` scene. pbrt runs as a child process
(`pbrt --display-server 127.0.0.1:<port>`) and streams its progressive image over the tev
protocol. The final image comes from the PFM pbrt writes. The pbrt patches are:
1. Exit when `PBRT_PARENT_PID` dies.
2. RGBFilm `"bool hdpbrtdepth" true` records the nearest first-hit camera-space z and streams
   it as display channel `Z`.
3. RGBFilm `"bool hdpbrtids" true` records the `hdpbrtprimid` (shape parameter) and
   `hdpbrtinstanceid` (`Attribute "shape"` before `ObjectInstance`) of the nearest first hit,
   streamed as channels `primId`/`instanceId`. They fill the selection AOVs.
4. RGBFilm `"bool hdpbrtalpha" true` records coverage (the filter-weighted share of camera
   samples whose first hit is a surface) and streams it as channel `A`. The delegate asks for
   it only when the scene has no dome (and no fallback white environment); then the color AOV
   gets that alpha and pbrt's color is already premultiplied. Alpha comes only from the stream
   (the final PFM has RGB only; pbrt sends one last update on exit).
5. Deformation blur: `TriangleMesh` can hold end-pose vertices (`p1`, `n1`) and blends by ray
   time over `time0..time1`. `trianglemesh` takes `"point3 P1"`/`"normal N1"`, `plymesh` takes
   `"string motionfilename"` (PLY with the same vertices); both take `"float motiontimes"`.
   Bounds cover both poses; Area/SolidAngle use the start pose. Not for quads/displacement.
6. N-key motion. An `ObjectInstance` takes `Attribute "shape" "float hdpbrtmotionxforms"
   [16*N]` (world-from-instance, Transform layout) and `"float hdpbrtmotiontimes" [N]`;
   `MotionPrimitive` picks the segment by ray time (each segment an `AnimatedTransform`).
   Meshes hold N poses: `plymesh` `"string motionfilenames" [...]` + `"float motiontimes"
   [N]` (filename is pose 0; patch 5's `motionfilename` still works).

## Done in the 2026-11-22 session

1. **Blank Solaris viewport fixed.** Houdini's viewport (HdxTaskController, "Houdini VK")
   draws nothing from a delegate without a `depth` AOV, even when color is filled. The
   delegate now offers `depth` (Float32) and `primId`/`instanceId`/`elementId` (Int32, -1).
2. **Real depth.** It comes from patch 2 and is converted to NDC [0,1] with the projection
   matrix in `HdPbrtRenderPass::_ToNdcDepth`. **Houdini VK expects reversed depth (1 = near,
   0 = far)**: with standard depth, the grid and handles disappear. The Houdini build reverses
   (`HDPBRT_HOUDINI` define), Blender does not, and `HDPBRT_REVERSED_DEPTH=0/1` overrides.
   Verified: the grid is hidden by the top of the sphere and shows in front of its lower half.
3. `HDPBRT_DEBUG=1` prints the AOV bindings (name, format, size) on every execute, plus
   snapshot and depth ranges.
4. The repo was set up and pushed: README, Apache-2.0 LICENSE, `patches/`, `scripts/build.sh`
   (only syntax-checked, never run from a fresh clone), docs, tests.

## Rules and gotchas

- Build at no more than `-j8`, inside a memory cap:
  `systemd-run --user --scope -p MemoryMax=64G -p MemorySwapMax=0 ...`. No sudo.
- **Never `ninja install` over `install/houdini/hdPbrt.so` while Houdini runs**, because it
  overwrites the mapped file. Build, then `cp` to a temp name and `mv` it over the old one.
  Jens has to restart Houdini to load a new build.
- Never use `pkill -f` with a pattern that also appears in your own command line, because it
  kills your shell. Kill by PID.
- Houdini ignores `PXR_PLUGINPATH_NAME` set from a package. Register through `HOUDINI_PATH`
  and `dso/usd_plugins/plugInfo.json`.
- hython FrameRecorder tests (`test/hydra_render.py`) don't catch viewport problems. Use the
  live-viewport test.
- Husk can't be tested: the Apprentice licence refuses third-party delegates.

## Live-viewport test (without touching Jens's desktop)

1. Start a private headless compositor with a rootful X server. The rootless Xwayland of a
   headless gnome-shell hangs.
   ```bash
   # both.sh, run as: setsid dbus-run-session -- ./both.sh &
   gnome-shell --headless --virtual-monitor 1920x1080 --wayland --no-x11 --wayland-display=wl-hdtest &
   sleep 5; WAYLAND_DISPLAY=wl-hdtest Xwayland :7 -geometry 1920x1080 -fullscreen -ac -noreset &
   wait
   ```
2. Run Houdini on `DISPLAY=:7` with `QT_QPA_PLATFORM=xcb`, its own
   `HOUDINI_USER_PREF_DIR=<dir>/houdini__HVER__`, and
   `HOUDINI_SCRIPT_PATH=<dir with 123.py>:&`. The `123.py` runs
   `test/viewport/vp_test.py`. That script builds a scene (sphere, ground, sun, dome) with a
   Python Script LOP, switches the viewer to the renderer named in `VPT_RENDERER`, waits
   `VPT_WAIT` seconds, and flipbooks to `VPT_OUT`. See `test/viewport/run_real.sh`; its
   scratchpad paths need replacing.
3. To test a build without replacing the installed one, point `HOUDINI_PATH` at a temporary
   dir whose `dso/usd_plugins/plugInfo.json` includes a test install.
4. Afterwards, kill gnome-shell and Xwayland :7 by PID.
   If the shell runs from the VS Code snap, gnome-shell aborts with "Settings schema
   'org.gnome.desktop.privacy' does not contain a key named 'privacy-screen'". Run
   `unset GSETTINGS_SCHEMA_DIR; export XDG_DATA_DIRS=/usr/share/ubuntu:/usr/share/gnome:/usr/local/share/:/usr/share/`
   in both.sh and in the Houdini run script.
5. The Houdini segfault on `hou.exit()` from the startup script is Houdini's own and harmless.
6. Selection: `tests/viewport/vp_pick.py` clicks the viewport through QTest in the
   `scenegraphselect` state and logs `PICK ok/FAIL` lines. Houdini VK and Karma CPU work as
   controls. The default `lopview` state does not select on click.
7. `VPT_DOME=0` leaves the dome out of the vp_test scene (alpha test). Flipbooks keep the
   viewport's alpha: with no dome, pbrt's and Houdini VK's flipbooks are both transparent
   outside the geometry (checked 2026-11-22).

## Open items and ideas

- Not translated yet: curves, points, volumes, subdivision refinement, camera motion blur,
  light shaping/IES, light linking. Implicit spheres are coarse.
- Curved motion (done 2026-11-22, pbrt patch 6): `pbrt:xformSamples` / `pbrt:geoSamples`
  (default 2) keys evenly spaced over the shutter. Transforms are sampled at sync on a
  1/16-frame grid over +-1 frame, each with `SampleTransform(id, t, t + 1e-3)`: a zero-width
  interval returns nothing time-varying, a tiny one has its edges evaluated exactly by USD
  (a rotateY orbit follows its arc; Houdini's viewport does this too). Between keys,
  `HdPbrt_InterpolateXform` decomposes (slerp), instance quaternions slerp. More than two
  transform keys need an ObjectInstance, so a moving non-instanced mesh is written as a
  one-instance object (except emissive ones: pbrt can't instance area lights, two keys).
  `tests/curved_scene.usda` + `tests/render_settings.py` (renders with renderer settings;
  reads pbrt's PFM from HDPBRT_TMPDIR). `docs/images/curved_motion.png`: 2 vs 9 samples.
- Deformation blur (done 2026-11-22, pbrt patch 5): a mesh samples `points` over +-1 frame at
  sync; only when they move (same count) it keeps `_Motion` (samples, PLY vertex -> point
  map, indices, uvs, authored normals or a topology copy for smooth normals). `Write()`
  writes the shutter-open and -close PLYs (`_UpdateMotionPlys`, cached per shutter) as
  `filename` + `motionfilename`. Authored normals are treated as static. Changing point
  counts (no motion). Verified with `tests/motion_scene.usda` (green "flag") and in the live
  viewport.
- Motion blur (done 2026-11-22): transform blur over the camera shutter. Prims keep time
  samples over +-1 frame (`HdPbrt_SampleWindow`) and the scene writer resamples at shutter
  open/close; instancers cache instance indices in `UpdateInstanceIndices` during the
  prototype's sync. Don't go back to re-dirtying rprims from the render pass on a shutter
  change: Houdini's viewport never resynced them. Verified in the live viewport (Python LOP
  with time samples, camera shutter 0..1) and with `tests/motion_scene.usda` at frame 1.
  Blender's Hydra sends shutter 0..0. Karma, in a FrameRecorder without RenderSettings,
  blurs less and doesn't blur instances, so it isn't a reference here.
- Depth of field (done 2026-11-22): the render pass reads the HdCamera from the pass state.
  Perspective + fStop > 0 + focusDistance > 0 -> pbrt `lensradius` = focalLength / (2 fStop),
  `focaldistance` = focusDistance (HdCamera values are already in world units). Off with
  `pbrt:depthOfField` false or `disableDepthOfField`. Blur matches Karma on the test scene.
  Houdini's free viewport camera has fStop 0. Blender exports focalLength in tenths (50 mm ->
  5) and multiplies fStop by 100, so the lens radius comes out right in meters. `focusOn`
  and `dofAspect` are ignored.
- pbrt's coateddiffuse looks darker and more saturated than Karma's or Cycles' diffuse.
- Jens's own test scene (a 1×1 rect light about 8 units away, no dome) renders almost black.
  That is the scene, not a bug.
- `scripts/build.sh` has never run from a clean clone (pbrt clone plus submodules is a large
  download; cap downloads with `curl --limit-rate` style limits).
