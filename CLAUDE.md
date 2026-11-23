# hdPbrt: working notes for Claude Code

hdPbrt is a Hydra render delegate for pbrt-v4, used from Houdini 22 Solaris (Apprentice) and
Blender 5.3. It is CPU only, on a Strix Halo machine (gfx1151, 128 GB). This repo is
`git@github.com:jempamartensson/Hdpbrt.git` (branch `main`). It pushes over SSH with
`~/.ssh/id_ed25519`, which is on Jens's GitHub. There are no HTTPS credentials and no `gh`.

Full design and gotchas: `docs/BUILD_NOTES.md` (the same file as `~/dev/pbrt_hydra/BUILD_NOTES.md`).

## Layout on this machine

- `~/dev/pbrt_hydra/`: the working tree.
  - `hdPbrt/`: this repo (delegate source, `blender/hydra_pbrt` add-on, patches, docs, tests).
  - `src/pbrt-v4`: pbrt-v4 at `20e41fd` plus branch `hdpbrt` (17 commits, the same as `patches/`;
    regenerate from this repo with `git -C ../src/pbrt-v4 format-patch -q 20e41fd -o "$PWD/patches"`;
    a relative `-o` lands inside the pbrt checkout).
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
7. `Transform::Decompose` handles reflections (pbrt had `XXX TODO FIXME deal with flip`):
   an improper rotation is negated so the reflection lands in the scale. Without it an
   animated camera (our camera matrix flips z) looks the wrong way.
8. USD light shaping on `DiffuseAreaLight` and `PointLight` (`HdPbrtShaping` in lights.h):
   `shapingfocus`, `shapingfocustint`, `shapingconeangle` (degrees), `shapingconesoftness`,
   `shapingies` (1-channel PFM, u = phi/2pi, v = theta/pi). Axis -z in light space.
9. Light/shadow linking in volpath: shapes `"integer hdpbrtlinks"` (collection bits;
   [-1] = none; absent = all), lights `hdpbrtlightlink` / `hdpbrtshadowlink`. Unlit
   surfaces skip the light in SampleLd and for BSDF-sampled emission (previous vertex's
   mask); shadow rays pass through non-member surfaces. Light link fields live on
   `LightBase`; dispatch over the 9 light types needs `DispatchCPU` on a const Light
   with a *named* lambda (pbrt's >8-type fallback forwards it as an lvalue).
10. USD light behavior: `DistantLight` `"float hdpbrtangle"` (angular diameter; a cone of
   radiance, an `Infinite`-type light that escaping rays hit, not seen by camera rays);
   area lights allowed on `interface` shapes (emit, never block or shadow);
   `DiffuseAreaLight` `"bool hdpbrtcamera"` / `"rgb hdpbrttint"` (image tint); shaping
   `"float shapingframe"` [9] (rotation, for the rotated cylinder).
11. Display channel buffers include the channel name in their size. Patch 3's "instanceId"
   (10 characters) overflowed pbrt's `title.size() + 32` by 2 bytes: random malloc aborts
   (status 134) depending on the temp path length. Fixed 2026-11-22.
12. Material `"hdpbrtlayered"` (`HdPbrtLayeredMaterial` / `HdPbrtLayeredBxDF`): MaterialX-style
   layering of a GGX dielectric specular lobe over Lambert, as Typhoon and MaterialX do it:
   f = w * tint * f_ggx(FrDielectric) + (1 - w * E_spec(cos theta_o)) * R / pi. E_spec is the
   untinted single-scatter albedo, tabulated at material creation (`HdPbrtInitDielectricAlbedo`,
   33 cos x 33 sqrt(alpha) x 25 eta, numerically integrated: no Typhoon/USD LUTs). Not
   reciprocal (view-direction throughput, like MaterialX). Params: `reflectance`, `roughness`
   (`remaproughness`), `float eta`, `specularweight`, `speculartint`. Two-sided. White furnace:
   exactly 1 at every roughness (pbrt's coateddiffuse: 0.44-0.87, why it looked dark).
13. `DielectricMaterial` `"spectrum transmissiontint"` (texture ok): multiplies every
   transmission event (in and out), like MaterialX's dielectric tint.
14. volpath: a scattered ray (depth > 0), or a camera ray at a camera-visible emitter, that
   reaches an emitting `interface` shape (a USD light) ends the path there after adding its
   MIS-weighted emission, from either side, as Typhoon does. Before, rays passed through the
   light: in the shader-ball box they reached the grey backplanes behind the rect lights,
   +15-19% light per bounce from bounce 2 on, ~1.07x overall. Shadow rays still pass, and
   camera rays still pass camera-hidden lights.
15. `hdpbrtlayered` layers: `sheenweight`/`sheencolor`/`sheenroughness` (Charlie D x Ashikhmin
   V, alpha = roughness, as Typhoon for both pbrlib sheen modes; albedo tabulated like
   E_spec) under the specular (standard_surface sheen) or on top (`bool sheenontop`: OpenPBR
   fuzz, pbrlib sheen_bsdf); `transmittance`: straight eta-1 transmission base (thin walls).
   Each layer scales those below by 1 - w * E(wo). Furnace: 1.000 for all combinations.
16. `hdpbrtlayered` coat: `coatweight`, `coatroughness` (alpha unless remapped), `coateta`,
   `coattint` (multiplies every layer under the coat). Above the specular, under
   `sheenontop` sheen. `HdPbrtDielectricLobe` is shared by coat and specular.
17. `DielectricMaterial` `"float hdpbrttopweight"`: transmission also scaled by
   1 - w E_R(|cos theta_o|) (albedo table). For pbrlib layer(dielectric R over dielectric T):
   MaterialX/Typhoon apply the top's layer throughput on top of the T lobe's own (1 - F), so
   transmission pays Fresnel twice per crossing. Real single-interface glass (OpenPBR,
   standard_surface) doesn't use it.

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

## Typhoon (USD's reference renderer) for comparisons

Typhoon is hdEmbree in a fork of OpenUSD 26.08 (`github.com/DGG3D/openusd-typhoon`, branch
`typhoon/main`), a CPU path tracer that implements UsdLux and MaterialX by the spec. It lives in
`~/dev/pbrt_hydra/typhoon/`: `bin/pixi` (the pixi binary, no shell changes), `openusd-typhoon/`
(shallow clone, built and installed into its `.pixi/envs/default`), `typhoon-test-suite/`
(scenes; its prebuilt-package environment 404s, so don't `pixi install` there), `refs/`
(the usdlux reference archive from the suite's GitHub release, hash-checked, extracted).
Always `export PIXI_HOME=~/dev/pbrt_hydra/typhoon/pixi-home PIXI_CACHE_DIR=~/dev/pbrt_hydra/typhoon/pixi-cache`.
Build: `pixi run configure`, then (never pixi's own `build`, it uses all cores)
`systemd-run --user --scope -p MemoryMax=64G -p MemorySwapMax=0 pixi run cmake --build build -j8 --target install`.
Render: `pixi run usdrender scene.usda -r Embree -f 25 --outputRoot <dir> -s '{settings}.ty:convergedSamplesPerPixel = 1024'`
(adaptive sampling). The env has no OpenImageIO Python/numpy; convert EXRs with pbrt's
`imgtool convert --channels R,G,B` and compare PFMs with system python3 (numpy 1.26).

`tests/typhoon/run_usdlux.sh` (SUITE, REFS, OUT, IMGTOOL, HYTHON) renders the suite's usdlux
scenes with hdPbrt (`render_hdpbrt.py`: camera/resolution from the RenderSettings prim,
`pbrt:maxDepth=1` = `ty:maxBounces 0`) and `compare.py` checks the frame means within 5%.
2026-11-22: 326/328 match; the 2 misses (frame 25 of disk/cylinder, a 10-degree cone) are
noise in the 16-spp references: at ~1000 spp Typhoon and hdPbrt agree within 2.5%. Lessons: light shapes must not block/shadow/show
(Typhoon: invisible unless `visibleInPrimaryRay`); color temperature is Gf Planckian locus
in linear Rec.709 normalized to luminance 1; distant lights are radiance cones (normalize
divides by pi sin^2); rect light textures flip u; specular-free UsdPreviewSurface (specular
workflow with specularColor 0, or ior 1) must be pbrt `diffuse`, not coateddiffuse.

## Open items and ideas

- Camera motion blur (done 2026-11-22): `HdPbrtCamera` (`camera.cpp`) samples its transform
  like the prims; the render pass writes views at shutter open/close as
  C(t)^-1 * C(0) * view, and `StartRender` writes pbrt's two-key camera
  (`ActiveTransform StartTime/EndTime`, `TransformTimes` before `Camera`). pbrt cameras
  take two keys only, so camera motion is linear. Test: `tests/camera_motion.usda` (the
  camera tracks the red sphere: sphere sharp, the rest streaks).
- Light shaping (done 2026-11-22): `light.cpp` writes shaping params only for authored
  values (ShapingAPI's default cone 90 = hemisphere, per the spec); cylinders pass a
  `shapingframe` because their pbrt shape is rotated. IES: Cycles' parser (`third_party/cycles_ies`, Apache-2.0;
  not USD's pxrIES, which is under USD's license), our own eval/power in `ies.cpp`
  following hdEmbree's conventions (theta from the light axis; phi of the vector toward the
  light; normalize divides by the average intensity), tabulated 180x180 into a PFM. The
  phi orientation follows hdEmbree and wasn't checked against Karma. Tests:
  `tests/shaping_scene.usda` with synthetic `ring.ies` / `lobes.ies`. A profile whose
  intensity varies with phi at theta 0 shows a seam through the nadir; that's the profile.
- Light linking (done 2026-11-22, pbrt patch 9): needs `HdsiLightLinkingSceneIndex`
  (registered in `sceneIndexPlugin.cpp` and `plugInfo.json`, phase 4 like hdPrman), else
  Hydra reports no collections. Lights get `lightLink`/`shadowLink` category tokens, rprims
  `GetCategories`; the delegate maps tokens to bits (`LinkBit`, max 64) and `HdPbrtGeom`
  writes `hdpbrtlinks` at scene-write time (nothing when no light links). Works in
  Houdini's viewport too. Test: `tests/linking_scene.usda`. Volumes and point-instance
  categories aren't linked. Area-light shapes cast shadows and show their black back
  (pre-existing).
- Not translated yet: volume
  materials (Karma pyro shaders), in-memory Houdini volumes (`op:` paths).
- Volumes (done 2026-11-22): `volume.cpp` (`HdPbrtField` bprims, `HdPbrtVolume`), converter
  `tools/vdb2nvdb.cpp` -> `hdpbrt_vdb2nvdb` next to pbrt (Houdini flavor, needs
  `-DPBRT_SOURCE`; Blender's install links to it). The interface box must face outward.
  Test: `tests/volume_scene.usda` (uses Houdini's sample cloud.vdb from /opt/hfs22.0).
- Curves, points and subdivision (done 2026-11-22): `curves.cpp`, `points.cpp`,
  `subdiv.cpp`. All geometry rprims share `HdPbrtGeom` (`geom.cpp`: transform samples,
  visibility, display color, instancing, and `Write()` with material, motion keys and
  instances); a prim type only supplies object-space `Shape` lines (`_Shapes(extra)`).
  Subdivision needs `topology.SetSubdivTags(sd->GetSubdivTags(id))` or creases are lost.
  Tests: `tests/curves_points.usda`, `tests/subdiv_scene.usda` (`pbrt:subdivLevel=0` vs 3).
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
- Camera exposure (done 2026-11-22): `GetLinearExposureScale()` (when the pass enables
  exposure compensation and the `enableExposureCompensation` setting is on) goes into the
  film `iso` with the shutter compensation. `tests/typhoon/exposure_scene.usda` matches
  Typhoon to 4 decimals. render_hdpbrt.py reuses the previous image when a frame changes
  nothing (hdPbrt doesn't re-render then).
- Materials vs Typhoon (2026-11-22): `tests/typhoon/run_materials.sh` renders clean Typhoon
  references (clamps off, caustics on) and compares; `MATERIALS_RESULTS.txt` 30 match /
  11 close / 1 differ (honey), median ratio 1.03; sheen/thin walls (patch 15) took it from
  27 / 11 / 4 (4 / 15 / 23 before patch 12; 12 / 9 / 21 after it;
  18 / 20 / 4 after the BSDF graphs and media; patch 14 took the median from 1.08). Still differ:
  open_pbr honey, pbr sheen_bsdf(_zeltner), standard_surface input_thin_walled. Fixed so far: subsurface weight blends
  toward subsurface_color, MaterialX tiledimage uvtiling (pbrt uscale/vscale), MaterialX images
  default to linear (only an authored sRGB colorspace decodes), (1) the layered BSDF
  (`hdpbrtlayered`, patch 12; 2026-11-23). Layered fixtures now read ~1.08x Typhoon (were
  ~0.91x): Typhoon's default throughput is BSDL's ReflFront LUT, maybe including multiple
  scattering, and its diffuse may differ. open_pbr feature_coat dropped to close (coat isn't
  translated; coateddiffuse's darkness had hidden that). Typhoon refs and renders are kept in
  `~/dev/pbrt_hydra/typhoon/materials_work` (cached; delete `ref/` to re-render).
  (2) BSDF-node graphs (done 2026-11-23, `_MtlxMapper` in material.cpp): ND_surface /
  ND_surface_unlit reduced to one pbrt material. layer(dielectric R over diffuse) ->
  hdpbrtlayered, over dielectric T -> dielectric, over conductor -> coatedconductor;
  mix -> the heavier side, add -> in1; Oren-Nayar/Burley/subsurface/sheen -> diffuse
  (no roughness, SSS or sheen yet); translucent -> diffusetransmission; conductor -> eta/k;
  generalized_schlick -> conductor with reflectance color0; EDFs -> emission
  (generalized_schlick_edf by its hemispherical average). Unauthored inputs use pbrlib
  nodedef defaults (Hydra passes only authored values); BSDF-node roughness is GGX alpha.
  (3) Transmission media (done 2026-11-23): standard_surface/OpenPBR transmission_depth > 0
  -> homogeneous interior medium (sigma_t = -ln(color)/depth, sigma_s = scatter/depth,
  sigma_a = difference shifted to >= 0, g = scatter_anisotropy), and layer_vdf volumes
  (absorption_vdf / anisotropic_vdf) in BSDF graphs. The material emits `MakeNamedMedium`
  with its textures; shapes get `MediumInterface "<mat>_interior" ""` (only right when the
  camera is outside). Tint (patch 13): standard_surface always, OpenPBR only without a
  medium (Typhoon's rules); transmission_extra_roughness adds to the roughness.
  Brightness (2026-11-23): fixed by patch 14. The rest (~1.7% on the shader ball) is a
  Typhoon quirk: it ends paths at geometry with no material bound (the backplanes), where
  we use displayColor diffuse; not copied. Bisect such gaps per bounce
  (`ty:maxBounces` b vs `pbrt:maxDepth` b+1).
  Thin walls: transmission passes straight through under the specular layer (Typhoon:
  ior 1 transmission), base takes 1 - transmission; standard_surface `thin_walled`, OpenPBR
  `geometry_thin_walled`. pbrt shadow rays still stop at thin walls (Typhoon lets them
  through), so light behind a thin shell arrives only by path continuation.
  Media as band spectra (2026-11-23, not pushed: 2 fixtures match -> close): pbrt's RGB
  upsampling leaks absorption into zero channels (honey red 0.75x); `_MediumSpectrum` writes
  sigma_a/sigma_s as 3-band steps (490/590 nm, fitted to RGB-exact transmittance). honey
  differ -> close (0.86 -> 0.90), scatter_anisotropy close -> match, dielectric_layer_absorption
  and _anisotropic match -> close (RMSE 0.09 -> 0.105, 1.07 -> 1.08 bright).
  Coat (2026-11-23, patch 16): OpenPBR coat roughens the specular below
  (r' = (r^4 + 2 c^4)^(1/4)), makes its IOR relative to the coat (both blended by weight),
  and darkens the base, (1 - K) / (1 - E K) with K = 1 - (1 - F0) / n^2 (spec; Typhoon does
  the same); coattint = darkening * lerp(1, coat_color, w). standard_surface: coat_color,
  coat_affect_color (base^(1 + w a)), coat_affect_roughness. feature_coat 1.14 -> 1.02
  (match). Not done: coat over metal (conductor path), textured base color in darkening.
  Absorption fixtures (patch 17): dielectric_layer_absorption/_anisotropic back to match
  (1.08 -> 1.04), dielectric_layer 1.15 -> 1.07. 32 match / 10 close / 0 differ.
  Metal (patch 18): standard_surface/OpenPBR metal is a lobe inside hdpbrtlayered, mixed
  by metalness under coat and sheen/fuzz (so coat over metal works now). OpenPBR: F82-tint
  (F0 = base_color, F82 = specular_color); standard_surface: artistic IOR (Gulbrandsen
  2014) to eta/k, or F82-tint when base_color is textured. Other shaders keep pbrt's
  conductor. standard_surface feature_metalness RMSE 0.065 -> 0.044; still 32/10/0.
  OpenPBR specular_weight on the metal lobe (patch 19, pbrt `metalscale`): weights the metal
  BSDF and its edge color (specular_color * specular_weight), as MaterialX. No suite fixture
  covers it: `tests/typhoon/materials_extra/open_pbr_metal` (4 fixtures, all match).
  Diffuse roughness (patch 20): energy-preserving Oren-Nayar (EON, Portsmouth et al. 2024,
  Typhoon's formula) for pbrt `diffuse` ("roughness") and the hdpbrtlayered base
  ("diffuseroughness"); cosine sampling. From standard_surface diffuse_roughness, OpenPBR
  base_diffuse_roughness, pbrlib oren_nayar_diffuse_bsdf roughness (EON even when
  energy_compensation is false; Typhoon uses the classic qualitative ON there). Rough-1 fixtures
  RMSE 0.059 -> 0.031; pbr oren_nayar 0.046 -> 0.033. Burley diffuse stays Lambert.
  Open: thin-film;
  dispersion; Oren-Nayar roughness; real subsurface.
- Next after materials: Typhoon scenes for the distant-light cone and light linking.
- Last: velocity motion blur (meshes whose point count changes, e.g. simulations; Jens
  wants it after everything else).
