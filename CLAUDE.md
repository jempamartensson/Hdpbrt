# hdPbrt: working notes for Claude Code

hdPbrt is a Hydra render delegate for pbrt-v4, used from Houdini 22 Solaris (Apprentice) and
Blender 5.3. It is CPU only, on a Strix Halo machine (gfx1151, 128 GB). This repo is
`git@github.com:jempamartensson/Hdpbrt.git` (branch `main`). It pushes over SSH with
`~/.ssh/id_ed25519`, which is on Jens's GitHub. There are no HTTPS credentials and no `gh`.

Full design and gotchas: `docs/BUILD_NOTES.md` (the same file as `~/dev/pbrt_hydra/BUILD_NOTES.md`).

## Layout on this machine

- `~/dev/pbrt_hydra/`: the working tree.
  - `hdPbrt/`: this repo (delegate source, `blender/hydra_pbrt` add-on, patches, docs, tests).
  - `src/pbrt-v4`: pbrt-v4 at `20e41fd` plus branch `hdpbrt` (48 commits, the same as `patches/`;
    regenerate from this repo with `git -C ../src/pbrt-v4 format-patch -q 20e41fd -o "$PWD/patches"`;
    a relative `-o` lands inside the pbrt checkout).
  - `src/OpenUSD-26.08`, `src/oneTBB-2022.3.0`, `src/usd-blender-generated`: headers for
    the Blender flavor.
  - `build/pbrt`, `build/hdpbrt-houdini`, `build/hdpbrt-blender`: build dirs. Since
    2026-11-25 they use `/opt/hfs22.0.508` (HFS, and the Blender flavor's PYTHON_INCLUDE):
    the `/opt/hfs22.0` symlink still points at the removed 22.0.368. `src/osd-include/opensubdiv`
    (the Blender flavor's OpenSubdiv headers) pointed through that symlink: the Blender build
    kept OpenSubdiv 3.5 objects and failed to load in Blender (3.7) until it was repointed at
    22.0.508 on 2026-11-26.
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
   transmission pays Fresnel twice per crossing. standard_surface glass uses it too (its
   MaterialX graph is that layer; weight = specular); OpenPBR glass doesn't.
30. `DielectricMaterial` `"float hdpbrtreflectionroughness"` (or `hdpbrtreflection{u,v}roughness`,
   same remap rule as `roughness`): the reflection lobe gets its own GGX distribution, the
   transmission keeps `roughness`. Split lobes are chosen by the macro Fresnel (clamped
   0.05-0.95) and sample their own microfacets; patch 17's top weight then uses the
   reflection roughness and, from inside, a reflection albedo with TIR (without it a wide
   transmission lobe adds to a TIR'd sharp reflection: furnace rim 1.34).
31. `ObjectInstance` light linking: Attribute "shape" "integer hdpbrtlinks" [...] before an
   ObjectInstance is the instance's own membership, OR'ed into its prototype shapes' masks
   on each hit (native instances share one prototype; categories come per instance from
   `HdPbrtInstancer::ComputeInstanceCategories`, flattened over nested instancers; a
   point instancer's own categories go on all its instances).
32. volpath: escaped rays weight each infinite light by its own pdf (pbrt multiplied `r_l`
   in place, so a second infinite light lit in the same direction, e.g. a dome and a
   distant light's cone, got both pdfs: fireflies, depending on the lights' order).
33. Burley diffuse (`HdPbrtBurley`, as MaterialX burley_diffuse_bsdf and Typhoon): `diffuse`
   `"bool hdpbrtburley"` makes "roughness" Burley's instead of EON's; hdpbrtlayered
   `"bool diffuseburley"` does the same for "diffuseroughness". f = rho/pi F(mu_i) F(mu_o),
   F(c) = 1 + (F90 - 1)(1 - c)^5, F90 = 0.5 + 2 r cos^2(theta_d); cosine sampling.
34. hdpbrtlayered OpenPBR coat darkening per shading point: `"float coatdarkening"` (coat
   weight x coat_darkening) multiplies the coat tint by lerp(1, (1 - K) / (1 - E K), w), K =
   1 - (1 - F0) / n^2 from `coateta`, E = lerp(diffuse reflectance + `"rgb coatdarkeningadd"`,
   metal F0 x min(metalscale, 1), metal weight) (Typhoon's base albedo).
35. `nanovdb` medium Karma pyro mode (`"bool hdpbrtpyro"`, `HdPbrtPyro`): `hdpbrtdensityrange`
   remap; with `"bool hdpbrtfire"` the temperature grid emits color(T) x intensity(T) per unit
   length (`MediumProperties::LeIsCoefficient`: volpath adds Le, not sigma_a Le), intensity =
   `hdpbrtfireintscale` x clamp over `hdpbrtfireintrange`, color a two-key ramp
   (`hdpbrtfirecolor0/1` over `hdpbrtfirecolorrange`) or blackbody(T x `hdpbrtfiretempscale`)
   from `hdpbrtfirebbtable` [K r g b ...] (else MaterialX's formula); RGB through
   RGBIlluminantSpectrum / photometric(illuminant), like pbrt's lights. Majorant cells the
   temperature can reach get >= 1 and SampleRay a sigma floor of 4 / cell, so fire without smoke
   is sampled.
36. ACEScg: `RGBColorSpace::ACEScg` ("acescg", AP1, ACES white, its own rgb2spec table; EXR
   chromaticities find it too). `"string colorspace"` on imagemap textures, diffuse area lights
   and infinite lights overrides an image's primaries (pbrt otherwise takes the file's metadata,
   else sRGB). RGBFilm `"bool hdpbrtkeepcolorspace"` writes the PFM in the film's color space
   (pbrt converts PFM/PNG output to sRGB; the display stream was never converted, so without it
   the final image and the stream disagree).
37. Spectral lights and camera: `Lambda_max` 830 -> 1100 nm (visible rendering unchanged: camera
   wavelengths still come from SampleVisible, the CIE curves are 0 beyond 830). Lights take
   `"bool hdpbrtpowernormalize"`: the spectrum carries the radiant power (360-830) of the color
   space's white at 1 nit instead of luminance 1 (an infrared spectrum has none). RGBFilm sensor
   `"hdpbrtbands"` + `"float hdpbrtsensorbands"` [center FWHM] x 3 (nm): Gaussian responses
   written raw (XYZFromSensorRGB = the output space's XYZFromRGB), each normalized so an
   equal-energy spectrum with white's visible power reads 1, and wavelengths sampled uniformly
   over the bands' +-3 sigma (PixelSensor::HdPbrtSamplesRange, FilmBase::SampleWavelengths).
38. Infrared reflectance: `diffuse` and `hdpbrtlayered` take `"float hdpbrtir"` (reflectance above
   the red edge), `"float hdpbrtiredge"` (nm, default 720) and `"float hdpbrtirwidth"` (60). The
   reflectance texture is wrapped in a SpectrumMixTexture that blends per wavelength (smoothstep
   across the edge; `SetHdPbrtInfrared`) instead of by `amount`.
39. Displacement on `plymesh` (pbrt's own `TriQuadMesh::Displace`, extended): screen-space dicing
   with `"float hdpbrtdicepixel"` (pixel size: radians, CameraWorld so the camera is at the
   render-space origin; world units with `"bool hdpbrtdiceortho"`) and `"float hdpbrtdicerate"`
   (edge length in pixels); `"float hdpbrtdiceview"` [16] (camera from render space, USD camera
   looking down -z) + `"float hdpbrtdicewindow"` [4]: edges outside the window (+10%) or behind
   the camera count `"hdpbrtdiceoffscreen"` (0.25) as long; `"float hdpbrtdicexform"` [16]
   (world from object for shapes inside ObjectBegin/Identity: the nearest instance);
   `"integer hdpbrtdicemaxtris"` (refinement stops past it and restarts with the edge length
   doubled, warning); with dicing on, vertices sharing a position (unwelded seams) displace along
   their averaged normal and get averaged normals afterwards (no cracks); displacement =
   `hdpbrtdisplacementscale` x (texture + `hdpbrtdisplacementoffset`).
40. Vector displacement on `plymesh`: `"bool hdpbrtvectordisplacement"` reads the displacement
   texture (must be a float `imagemap`; `FloatImageTexture::EvaluateRGB`) as RGB, a vector in
   (dPdu, dPdv, N) space (MaterialX `ND_displacement_vector3`): d = `"float hdpbrtvdispscale"`
   [3] x rgb + `"float hdpbrtvdispbias"` [3]. `TriQuadMesh::ComputeTangents`: unit tangents from
   the uvs summed over vertices sharing position and uv, orthogonal to N (dPdv also to dPdu);
   `Displace` passes them to a callback that takes them (`std::is_invocable`), only when asked.
41. Procedural displacement: `"string hdpbrtgraph"` on `plymesh` (in place of the displacement
   texture) is a register program (`util/hdpbrtgraph.h/.cpp`: "op dst operands;...", constants
   "c", "out"; registers of 4 floats, floats broadcast so componentwise ops cover the FA
   variants) evaluated per vertex: math, mix/clamp/smoothstep/remap/if*/switch, dot/length/
   cross, extract/combine/convert, rotate2d/3d, object/world position/normal/tangent/bitangent,
   uv, MaterialX reference noise (perlin, fractal, cell, worley: mx_noise.glsl, OSL's) with
   Karma's vector2/vector4 variants (mxstd.h), image lookups (`"string hdpbrtgraphimages"`
   file/encoding pairs). `"float hdpbrtgraphcamera"` [3] (world position of the render origin)
   gives world space; displacement scalar (`hdpbrtdisplacementscale`/offset) or vector
   (`hdpbrtvectordisplacement`).
42. Displacement with deformation motion and primvars: `TriQuadMesh::Displace` takes the other
   poses (`Pose` p/n; plymesh reads `motionfilenames` before displacing), logs Refine's splits
   (`splitParents`) and replays them on each pose (same topology), then displaces every pose
   (its own normals/tangents) for `TriangleMesh::SetMotion`; dicing follows the first pose.
   `TriQuadMesh::attr`/`attrWidth`: per-vertex attributes averaged by Refine;
   `"string hdpbrtprimvars"` [name width file ...] loads raw float32 files (width per PLY vertex),
   `"string hdpbrtgraphprimvars"` [name width ...] are the graph's slots ("pv slot default" op,
   `HdPbrtGraph::BindPrimvars`; a missing primvar gives the default).
43. Surface primvars: `plymesh` keeps `hdpbrtprimvars` (now read for every plymesh, not only
   displaced ones) on the final `TriangleMesh` (`SetPrimvars`; names interned by
   `HdPbrtPrimvarId`, util/hdpbrtprimvar.h). Triangle hits carry `HdPbrtPrimvarHit` (vertex
   indices + barycentrics) in SurfaceInteraction -> TextureEvalContext (copied through instance
   transforms). Textures `"hdpbrtprimvar"`: float (`"string primvar"`, `"integer component"`,
   `"float default"`, `"float scale"`, `"float power"`) and spectrum (`"float default"` [3],
   RGB in the scene's color space, albedo/unbounded/illuminant by use). Not on quads (bilinear
   patches), bump contexts or non-mesh shapes (default). Graph slots now bind by name and
   width (merged primvar lists may hold one name at two widths).
44. Per-shape primvars on any shape: `"string hdpbrtshapeprimvars"` [name width ...],
   `"float hdpbrtshapeprimvarvalues"` (per primvar count x width), `"integer
   hdpbrtshapeprimvarcount"` (1, or a curve's Bezier segment count + 1). Read once per Shape
   statement (scene.cpp `HdPbrtReadShapePrimvars`) and set on its GeometricPrimitives, which
   fill the hit's `HdPbrtPrimvarHit` (before the alpha test) when the shape has no per-vertex
   primvars of its own. Curves: `CurveCommon::hdpbrtSeg` (segment index) and the hit's u go in
   the hit (v[0], b[0]); values interpolate linearly between that segment's boundaries.
45. hdpbrtlayered: `"coatdarkening"` (patch 34) is a float texture (weight x coat_darkening can
   vary) and `"texture hdpbrteta"` optionally replaces the constant `"eta"` of the specular
   lobe per shading point (clamped >= 1.0001; the albedo tables are indexed by eta anyway).
46. Emission textures: AreaLightSource "diffuse" `"string hdpbrtltexture"` names a spectrum
   texture (its illuminant version) that replaces L (times `scale`; L still sets the color
   space normalization). `Light::L` / `DiffuseAreaLight::L` take an optional
   `HdPbrtPrimvarHit *`: `SurfaceInteraction::Le` passes the hit's, `SampleLi` the sampled
   point's (`ShapeSample::hdpbrtPrimvars`, set by both Triangle::Sample). Power for light
   sampling (`Phi`, `Bounds`): mean / max of the texture at 8 points of the shape x 3
   wavelengths (`SetHdPbrtTexture`, floor 1e-3 so it stays sampled). Triangles only carry
   primvars (meshes; hdPbrt only makes meshes emit).
47. Instanced and moving emitters. An AreaLightSource inside ObjectBegin is kept (pbrt warned
   and dropped it). `BasicScene::CreateLights` (per-shape part now the `makeAreaLights` lambda)
   gives each emissive shape of a definition triangle lights in prototype space (not scene
   lights; `hdpbrtSlot` / `hdpbrtTri` mark them) and a shared `HdPbrtMeshEmitter` (per triangle:
   light, area, unit normal, bounds power; alias table by power; bounds, normal cone), and each
   instance one `DiffuseAreaLight` per emissive shape in mesh mode (`SetHdPbrtMesh`, a copy of
   the first triangle light for the emission parameters) with `HdPbrtLightXform` (static,
   ActiveTransform or N keys). Mesh mode: `SampleLi` picks a triangle by power, a point by area
   in prototype space, transforms it at `ctx.time`; pdf = P(tri) / (A x areaScale) x d^2 / cos
   (areaScale = |det M| |M^-T n|); `Phi`/`Bounds` sum the triangles scaled to the instance
   (normal cone kept only for static similarity transforms). Hits: Transformed/Animated/
   MotionPrimitive swap a prototype light for the instance's (`HdPbrtInstanceEmitter`,
   `SetHdPbrtLights`, arrays in `BasicScene::hdpbrtInstanceLights` by instance index) and set
   `SurfaceInteraction::hdpbrtEmitterTri`; path and volpath use `HdPbrtAreaPDF(light, ctx, wi,
   hit)` for the MIS pdf (mesh mode can't answer PDF_Li(ctx, wi)). `LightSampleContext::time`
   (from the interaction): area lights sample deforming shapes at the right time too.
   `CreatePrimitivesForShapes` takes the area-light map, definitions use their own. Not for
   SampleLe (light-path integrators) or AnimatedShape area lights inside definitions.
48. Per-instance primvars: `ObjectInstance` reads Attribute "shape" `"string
   hdpbrtinstanceprimvars"` [name width ...] + `"float hdpbrtinstanceprimvarvalues"` (one value
   each) into `InstanceSceneEntity::hdpbrtPv*` (the `hdpbrtTag` lambda tags all three instance
   kinds); `CreateLights` builds `BasicScene::hdpbrtInstancePrimvars`; the instance
   primitives set `HdPbrtPrimvarHit::inst/instCount` on hits, which `Lookup` checks before the
   shape's (USD: instance primvars override the prototype's); instanced emitters pass them to
   their samples (`DiffuseAreaLight::hdpbrtInstPv`). Alpha textures are evaluated inside the
   prototype (before the instance adds its primvars): per-instance opacity doesn't cut out.

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
  `ninja install` rewrites hdPbrt.so in place even right after such a `mv` (CMake compares
  timestamps): to install the Houdini UI files (soho/, UsdRenderers.json) while Houdini runs,
  copy them by hand instead.
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
  Houdini's viewport too. Test: `tests/linking_scene.usda`. Volumes aren't linked. Point
  instancers (2026-11-25): USD publishes their membership as the instancer prim's own
  `categories` (for all its instances; prototypes get none, and linking to a prototype
  under a point instancer isn't supported by USD); `HdPbrtInstancer` adds them to every
  instance. Before that, point instances were in no collection (unlit by any linked light). Area-light shapes don't block, shadow or show a
  back (pbrt `interface` material, patch 10; rechecked 2026-11-25 with a rect light seen
  from behind over a floor).
- Working color space ACEScg (2026-11-26, pbrt patch 36; Jens: always ACEScg, for Houdini with
  an ACES OCIO config and Blender files set to ACEScg). `ColorSpace "acescg"` heads the scene:
  material/light/displayColor numbers are ACEScg and so is the color AOV
  (`hdpbrtkeepcolorspace`). `pbrt:workingSpace` = `lin_rec709` switches back; not in the
  settings UI; `tests/typhoon/render_hdpbrt.py` sets it (Typhoon and Karma's default config are
  Rec.709), so the suites are unchanged. Conventions measured from Karma CPU (husk) under
  Houdini's `cg-config` (scene_linear ACEScg): constants are raw ACEScg; every texture (8-bit
  sRGB, unlabeled EXR, EXR labeled Rec.709 and even labeled AP1, .hdr) is read as Rec.709 and
  converted; color temperature is Rec.709 converted (Bradford D65 -> ACES white). hdPbrt: images
  keep pbrt's rule (file metadata, else sRGB primaries; an AP1-labeled EXR stays AP1, unlike
  Karma); `"string colorspace"` = working space for raw textures (UsdUVTexture raw, MaterialX
  Raw/data) and COP images; named MaterialX/OCIO spaces (ACEScg/AP1, 2020, sRGB/709) as such;
  `HdPbrtWorkingSpace::FromRec709` for color temperature and Karma's pyro blackbody table;
  working-space luminance for the dome's color tint. Media in ACEScg use pbrt's RGB
  upsampling (`rgb sigma_a`): the 3-band spectra were fitted to Rec.709 channels. vs Karma
  (`tests/aces/run_aces.sh`, `ACES_RESULTS.txt`; diffuse under a white dome): constant albedo
  within 1-3%, textures +2%
  red (pbrt lifts sRGB images with its D65 table under D60 white, Karma uses Bradford),
  lights 0.4-0.6% (white, 3000 K, 6500 K). Blender 5.3 (working space ACEScg via
  `wm.set_working_color_space`) vs Cycles: same hue, 1-3% darker (BSDF mapping). Limit:
  colors near the AP1 primaries can't be reached by pbrt's smooth spectra (ACEScg (1,0,0)
  light -> (0.90, 0.18, 0.01), reflectance -> (0.63, 0.14, 0.00)); everything inside Rec.709
  round-trips within 1%. The Blender add-on warns when a file's working space isn't ACEScg;
  Houdini needs an ACEScg OCIO config (`$OCIO`, e.g. `$HFS/packages/ocio/cg-config-*.ocio`),
  Houdini 22's default config is Rec.709.
- Houdini op: images (done 2026-11-26, Houdini flavor): texture files (UsdUVTexture, MaterialX
  image nodes) and rect/dome light textures that start with `op:` (COP nodes, with Houdini's
  `{output}`, `[frame]`, `(xres,yres)` modifiers) are cooked with the HDK's
  `TIL_CopResolver::getRaster` (what Karma uses; it handles Copernicus) in `houdiniImage.cpp`,
  the only file built with the HDK's defines (from `hcustom -c`) and no USD headers; it links
  `libHoudiniUT`. Written as float PFM (1 channel stays 1, else RGB; Houdini's bottom-first rows
  are PFM's), read as linear. USD's Ar can't do it: Houdini's `FS_ArResolver` takes `op:` but
  returns an empty asset for COPs. Live edits: Houdini's viewport resyncs nothing when a COP
  changes (its COP watcher restarts Karma only; marking sprims dirty from the delegate is never
  synced either, because Houdini stops syncing once the render converges), so each op: path
  keeps one file (plus one equal-area EXR for domes) and the render pass polls
  `getRasterVersion` (it moves on edit, before the cook; no cook needed) at most 4x/s, rewrites the
  file (temp + rename) and restarts pbrt. Don't use the resolver's thread cache: Houdini only
  clears it for Karma, so it returns the old image after an edit. Each `[frame]` path is its own
  file; LOPs author that suffix as a time sample for time-dependent COPs (a Dome Light LOP
  writes op:/obj/copnet1/sky[3] at frame 3), so scrubbing an animated COP leaves one file per
  frame visited in the temp dir (frames revisited reuse theirs). Tests: `tests/cop_textures/cop_textures.py` (op: vs the same
  COPs saved by Houdini's rop_image: pixel-identical; text upright), `tests/viewport/vp_cop.py`
  (live edit of a texture COP and a dome COP), `tests/viewport/vp_cop_frames.py` (animated COP
  through a Dome Light LOP at frames 3, 7, 3: exact in the viewport).
- Houdini .ds files (done 2026-11-26, Jens's request): `tools/make_houdini_ds.py` (hython)
  writes `houdini/soho/parameters/HdPbrtRendererPlugin_{Viewport,Global}.ds` from one settings
  list (keep it in step with `_Init`); CMake installs them with `houdini/UsdRenderers.json`
  into the install dir, which goes on HOUDINI_PATH (locally `~/dev/pbrt_hydra/houdini/soho` and
  `UsdRenderers.json` link to install/houdini). OpenMoonRay (hdMoonray `plugin/houdini/`) only
  ships a hand-written `_Viewport.ds` (parm names = the SetRenderSetting tokens) and a
  UsdRenderers.json entry; the Render Settings LOP needs Karma's pattern: `_Global.ds` with
  parm names `hou.text.encodeParm("pbrt:...")`, a `_control` parm each
  (`loputils.createEditPropertiesControlMenu`) and `usdvaluetype` tags. Checked: the LOP gets a
  **pbrt** tab, authors typed `pbrt:*` attributes on the RenderSettings prim, and the live
  viewport passes them to the delegate (an exported scene had pixelsamples 8 / path). The
  UsdRenderers.json entry is only `valid` + `menulabel` (flipbook pixel-identical without it;
  other fields such as depthstyle left at Houdini's defaults, which work).
- Spectral lights and camera (done 2026-11-26, Jens: "nanometers instead of RGB, think infrared
  photography"; pbrt patch 37). Lights: `pbrt:spectrum` = rgb (UsdLux color) | band (Gaussian at
  `pbrt:wavelength`, FWHM `pbrt:bandwidth`, nm; radiant power of a white light of the same
  intensity) | blackbody (pbrt's spectral black body at inputs:colorTemperature, luminance as
  white); spectral modes ignore color; textured lights stay RGB (`light.cpp` `emit`). Camera:
  `pbrt:sensor` = cie1931 | bands (`pbrt:sensorRed/Green/Blue` float2 center, FWHM; defaults
  850/100, 650/80, 550/80 = infrared false color), read in `HdPbrtCamera::Sync`, passed as
  `HdPbrtLens::sensor` onto the Film line. Hydra delivers plain `pbrt:*` attributes on light and
  camera prims (GetLightParamValue / GetCameraParamValue). `_Light.ds` / `_Camera.ds` give Light
  and Camera LOPs a pbrt tab (checked: authors those attributes; band fields hide unless their
  mode is on). Checks: an 850 nm light is black to the color camera, red (4.1) to the bands
  camera; white light through the bands reads (0.77, 1.03, 1.21) (ACES D60's real spectrum,
  with infrared). Test scene: `tests/spectral/ir_scene.usda` (+ `_infrared` override);
  `docs/images/infrared.png`. IR renders are noisier (uniform wavelength sampling).
- Infrared reflectance (2026-11-26, pbrt patch 38): RGB reflectances just extend their red end
  into the infrared (pbrt's sigmoid fit), so foliage stayed dark. A float shader input
  `pbrt:irReflectance` (any node of the material's network; `pbrt:irEdge` nm, default 720) or a
  constant geometry primvar `pbrt:irReflectance` / `pbrt_irReflectance` (SOP attribute name;
  overrides the material, per object) sets the diffuse reflectance above the red edge; only
  `diffuse` and `hdpbrtlayered` take it (`HdPbrt_MaterialLine`). The visible image is unchanged.
  The "leaf" sphere and the ground in `tests/spectral/ir_scene.usda` use both routes. Blender's
  Hydra export doesn't pass custom mesh attributes (checked), so Blender can't set it yet.
- Houdini material UI for it (2026-11-26). Houdini reads renderer .ds files only for Viewport,
  Global (Render Settings), Light, Camera and Geometry (Render Geometry Settings LOP; probed
  every LOP type): there is no material one. So: `houdini/scripts/vop/<type>_OnCreated.py` for
  mtlxstandard_surface, mtlxopen_pbr_surface, usdpreviewsurface and mtlxsurface call
  `scripts/python/hdpbrt_houdini.py`, which adds a collapsible "pbrt" folder of spare parms tagged
  `sidefx::shader_isparm` / `sidefx::shader_parmname` (Houdini's translator then writes
  `inputs:pbrt:irReflectance` on the USD shader; untagged spare parms are dropped). Parms at their
  default aren't written, and `__activate__` toggles / `sidefx::shader_forceparmval` are ignored
  for spare parms, so the "Infrared Reflectance" checkbox (tagged `shader_isparm 0`) has a
  callback that sets the value to 0.6 (on) or back to the -1 default (off). Existing nodes:
  `hdpbrt_houdini.add_material_parms(hou.selectedNodes())`. `_Geometry.ds` gives the Render
  Geometry Settings LOP `primvars:pbrt:irReflectance` (constant). Checked in hython: plain
  Material Library MaterialX shader -> `"float hdpbrtir" 0.6` in the pbrt scene.
- Material render contexts (2026-11-26, Jens: "try and switch, maybe add a toggle"): a Karma
  Material Builder writes its MaterialX graph on `outputs:kma:surface` plus a UsdPreviewSurface
  on `outputs:surface`. hdPbrt asked for mtlx, universal, kma, so such materials rendered as their
  preview. Now mtlx, kma, universal. `pbrt:materialPreview` (render setting, Materials) renders
  the universal context (UsdPreviewSurface) first, to compare. The delegate can't mark sprims
  dirty under scene indices ("requires emulation"), so `HdPbrtMaterial::Sync` fetches each
  material twice (a thread_local flag flips `GetMaterialRenderContexts`, which Hydra reads per
  fetch; `HdPbrt_PreviewContexts`) and keeps both descriptions; `GetDesc` picks by the setting
  and changing it only rewrites the scene. Checked in one session: builder material default ->
  its graph (with `hdpbrtir`), preview -> the UsdPreviewSurface, back -> the graph.
  The Karma pyro shader's kma network arrives flattened (100+ MaterialX nodes); `HdPbrt_PyroVolume`
  only recognizes the single pyro node of the universal network, so both descriptions take the
  pyro volume from whichever version has it (kma-first alone made every pyro frame plain smoke;
  run_pyro.sh caught it, now identical to PYRO_RESULTS.txt).
- True displacement (2026-11-26, pbrt patch 39; Jens chose screen-space dicing and scalar height
  maps first). `HdPbrtMaterial::_BuildDisplacement` reads the displacement terminal:
  `ND_displacement_float` (its `scale`) or UsdPreviewSurface's `displacement` input, an image
  through `_Builder::Texture` (float, linear), folding constant ND_multiply/add/subtract_float
  nodes into scale/offset (UsdUVTexture: scale on the pbrt texture, bias as offset). Other graphs
  are procedural (below). `HdPbrtGeom::Write` adds the shape params when the mesh has uvs
  (`_CanDisplace`; else one TF_WARN) and `pbrt:displacement` is on; the write context carries the
  pixel size, camera position and view (`StartRender`). Settings: `pbrt:displacement` (on),
  `pbrt:dicingRate` (pixels, default 2: same silhouettes as 1 within noise, 4x fewer triangles;
  1 px = 0.4 px^2 triangles, finer than Karma), `pbrt:dicingMaxTriangles` (20M per mesh). Checks
  (`tests/displacement/run_displacement.sh`, Karma via husk): silhouettes differ on 0.9% of object
  pixels (antialiasing/noise), displaced ground within 1.5%; triangle count exactly 4x lower at
  twice the rate; off-screen dicing 8.1M -> 5.0M triangles at rate 1 with an identical image;
  face-varying-uv cube closed; PointInstancer dices by the nearest instance; Karma Material
  Builder `mtlxdisplacement` + `mtlximage` reaches pbrt. `docs/images/displacement.png` (hdPbrt
  left, Karma right; Karma's EXR has a black background). Limits: needs uvs; camera moves re-dice (the scene is rewritten).
  Deforming displaced meshes are motion blurred since patch 42 (below).
- Vector displacement (2026-11-27, pbrt patch 40): `ND_displacement_vector3` with an image
  (`ND_image_vector3`/color3) and constant `ND_multiply/add/subtract_vector3[FA]` nodes folded
  into per-component scale/bias (`HdPbrtMaterialDesc::displacementVector*`). The vector is in
  (dPdu, dPdv, N) with unit tangents, as Karma: `tests/displacement/make_vector_scene.py`
  (mushroom overhangs on a grid, twisted ribs on a sphere sheared along dPdu) matches Karma's
  shapes and twist direction; silhouettes differ on 1.1-1.5% of object pixels (noise)
  (`docs/images/vector_displacement.png`). Its saturated base colors differ from Karma by
  5-15% on flat, undisplaced ground too (material, not displacement).
- Procedural displacement (2026-11-27, pbrt patch 41): graphs the image folding doesn't cover
  (or a connected `scale`) go to `HdPbrt_CompileDisplacementGraph` (`mtlxGraph.cpp`), which
  compiles the displacement node's inputs into an hdpbrtgraph program: nodes are primitives or
  expanded through their stdlib nodegraph; unauthored inputs take nodedef defaults (values,
  defaultgeomprop Pobject/Nworld/UV0...), both from `mtlxStdlib.inc` (generated by
  `tools/make_mtlx_defaults.py` from $HFS's MaterialX stdlib; rerun when MaterialX changes).
  noise/fractal/smoothstep/convert/separate/distance are native although nodegraphs exist.
  Unsupported (time, hextiledimage, UDIMs, matrices, string/integer primvars, ...) ->
  no displacement, reason under `HDPBRT_DEBUG`. Checks (`tests/displacement/
  make_procedural_scene.py`, in run_displacement.sh): noise3d, fractal3d, worleynoise3d,
  cellnoise3d, vector noise3d, unifiednoise3d, place2d+trianglewave, range, rotate3d+separate3,
  tiledimage x noise and a world-space fractal ground match Karma: silhouettes differ on 0.7%
  (first row) to 1.5% (second; Karma has a spike at one sphere's pole)
  (`docs/images/procedural_displacement.png`). Typhoon's sin(position) `open_pbr--displacement`
  now displaces like Typhoon's reference (rel RMSE 0.086 -> 0.042, materials 34/8/0 unchanged).
  Karma Material Builder mtlxnoise3d graphs compile.
- Displacement motion blur and primvars in graphs (2026-11-27, pbrt patch 42). Deforming
  displaced meshes: hdPbrt already writes the poses (motion PLYs); pbrt now refines them alike
  and displaces each (noise follows the deformed object-space position). Primvars:
  `geompropvalue` (other than st/uv) and `geomcolor` (displayColor) compile to "pv" slots
  (`HdPbrtMaterialDesc::displacementGraphPrimvars`); `HdPbrtGeom::_PrimvarFile` (mesh) writes
  them per PLY vertex when the scene is written, fetched then from the scene delegate kept from
  Sync (`_PvSource`: PLY vertex/corner maps, triangles, coarse faces, subdiv), so a material
  edit that adds a primvar needs no mesh resync. Constant, vertex/varying (refined through
  the subdivision as points), uniform and face-varying (not on subdivided meshes: warning,
  default); on welded meshes per-face/corner values are averaged per vertex, on unwelded ones
  discontinuous values tear the seams apart (as the data says). Houdini's Cd falls back to
  displayColor. Checks (`tests/displacement/make_primvar_motion_scene.py`, in
  run_displacement.sh): one material with per-mesh constant `amp` (0.15, 0.45, missing ->
  default 0, undisplaced), a vertex mask (subdivided), a vertex vector3 primvar as vector
  displacement all match Karma (2.6% silhouette difference, a uniform 1 px rim from the
  metric's edge threshold against hdPbrt's sky, also on the undisplaced sphere); the deforming
  displaced sphere's blur coverage correlates 0.991 with Karma's (mean 0.43 vs 0.50, the same
  without displacement); with near-zero deformation the blurred sphere equals the static one
  (every pose displaced). `tests/motion_scene.usda` still blurs.
- Varying primvars in surface shading (2026-11-28, pbrt patch 43). `_Builder::Texture`: a
  primvar reader (UsdPrimvarReader_* varname/fallback, ND_geompropvalue_* geomprop/default,
  ND_geomcolor_*) on base color, roughness or opacity becomes an `hdpbrtprimvar` texture
  (`PrimvarTexture`, listed in `HdPbrtMaterialDesc::surfacePrimvars`; width from the node type);
  roughness ones square in pbrt (`"float power" 2`, remaproughness off: MaterialX alpha = r^2;
  image roughness still goes through pbrt's remap). displayColor / Cd on the base color keeps
  the constant path, except that a mesh with non-constant displayColor (also with no material)
  uses the shared `hdpbrt_displayColor` texture (`HdPbrt_MaterialLine` displayColorTexture).
  `HdPbrtGeom::Write` writes every needed primvar per PLY vertex with `_PrimvarFile` (merged with
  the displacement graph's) as one `hdpbrtprimvars` list. The displacement builder keeps primvar
  readers for its graph (`primvarTextures` false). Curves, points and implicit prims
  (2026-11-28, pbrt patch 44): `HdPbrtGeom::_ShapePrimvars` (when the scene is written, from
  `_sd` kept by `_SyncDisplayColor`; meshes keep their files) gives one parameter string per
  pbrt Shape line (`_ShapePv(i)` in `_Shapes`): points one value per sphere (vertex/varying by
  point index via `_pointIndex`, else constant); curves (`_curveShapes`: point indices, segment
  count) values at Bezier segment boundaries: vertex primvars through `_Convert` like the
  points (linear; bspline boundary = (c0 + 4 c1 + c2) / 6), varying with one value per
  boundary as they are (else resampled), uniform per curve, constant; implicit prims constant
  only (`_ConstantShapePrimvars`). Varying displayColor uses `hdpbrt_displayColor` there too.
  `HdPbrt_FetchPrimvar` (geom.cpp, Cd -> displayColor) is shared with the mesh path. Not yet:
  other inputs (metalness, coat, ... aren't textured at all), per-instance primvars, varying
  displayOpacity without a material; cubic curves interpolate linearly within a segment.
  Check (`tests/primvars/run_primvars.sh`, vs Karma): vertex Cd on a subdivided sphere,
  uniform checker faces, a displaced sphere's colors, UsdPreviewSurface reader color + opacity,
  missing primvar -> default: patches within 1% of Karma; vertex roughness matches at r 0.05,
  and primvar roughness equals the same constant exactly (rough metal at r 0.8 is 0.63x Karma
  with or without primvars: a metal difference, not looked into). Karma shades unbound
  displayColor ~2x darker than its value (its default material), so the unbound grid and
  implicit sphere only check the pattern. Second scene (`make_curve_point_scene.py`, same
  script): points (vertex Cd) and implicit Sphere/Cylinder (constant tint) within 1-3% of
  Karma per patch; linear, bspline (boundary value = 2/3 of its vertex) and uniform-per-curve
  curves have the right colors, levels 0.8-1.08x Karma, as with a constant color (pbrt's
  "cylinder" curves are ribbons with bent normals, Karma's round tubes); run43 34/8/0 and usdlux
  326/328 unchanged. Materials run42 34/8/0 identical to run41;
  run_displacement.sh unchanged (0.83 / 1.54 / 1.40 / 2.57%, blur correlation 0.991).
- Primvars (and images) on every input pbrt can vary (2026-11-28, pbrt patch 45).
  `_Builder::Val` (texture or constant) with `In` (Texture() of an input), `Mul` (pbrt "scale"
  texture: value x float) and `Mix` (pbrt "mix"); `Param` writes either form. Varying:
  base weight (reflectance = base color x weight x the subsurface walk's share; textured base
  colors are now scaled by a constant weight too), metalness (forces the metal lobe), specular
  weight / color, metal F0 (base x weight) and F82 edge (specular color x weight; any varying
  edge or base uses F82-tint on standard_surface), coat weight / color (tint = mix(1, color,
  weight)) / roughness (squared), sheen / fuzz weight, color, roughness, diffuse roughness,
  transmission color (dielectric tint, thin-wall transmittance), and on UsdPreviewSurface
  metallic, clearcoat, clearcoatRoughness. Couplings with a varying coat weight: OpenPBR
  darkening = weight x coat_darkening (texture), specular IOR mix(uncoated, relative to the
  coat) (`hdpbrteta`), specular GGX alpha mix(uncoated, coated) (both standard_surface
  coat_affect_roughness and OpenPBR's r'). UsdPreviewSurface changes (constant ones too):
  clearcoat is translated now (white coat, IOR 1.5; it was ignored), metallic uses
  hdpbrtlayered's metal lobe (F0 = diffuseColor, white F82 edge, i.e. Schlick as the spec)
  instead of pbrt's conductor fit. Still constant in pbrt (a varying input warns, also on
  stderr under HDPBRT_DEBUG since hython drops Tf warnings): IOR (except the coat coupling),
  coat IOR, thin film, subsurface, transmission weight / depth / scatter, specular
  anisotropy, coat_affect_color, metalscale (OpenPBR specular weight on metal), and pbrlib
  BSDF-node graphs other than their diffuse color. Check (`tests/primvars/make_inputs_scene.py`,
  in run_primvars.sh, hdPbrt only): 19 spheres, one input each driven by a primvar; with the
  primvar holding A (B) everywhere the image equals the constant-A (-B) render within 0.2%
  (before the coat couplings: OpenPBR coat weight 1.4-1.9x); split spheres agree up to
  light from neighbours (transmission color: light crosses both tints). Materials run44 34/8/0
  identical to run43; materials_extra (8 groups) unchanged.
- Varying emission (2026-11-28, pbrt patch 46): standard_surface emission_color x emission,
  OpenPBR emission_color x emission_luminance, UsdPreviewSurface emissiveColor (images through
  the uvs, or primvars) -> `HdPbrtMaterialDesc::emissionTexture`; a mesh emits when the
  constant or the texture says so and writes `"rgb L" [1 1 1] "string hdpbrtltexture"`.
  Checks: the inputs scene's four emission spheres equal their constants exactly at A and B
  (camera hits and the light they cast); pbrt-only: a quad lit by a white-primvar texture equals
  "rgb L" [1 1 1] pixel for pixel. Emissive meshes only (instanced ones since patch 47).
  Materials run45 34/8/0 identical, usdlux 326/328.
- Instanced and moving emitters (2026-11-28, pbrt patch 47; plan: thousands of instances and
  motion blur). `HdPbrtGeom::Write` writes the AreaLightSource inside ObjectBegin for
  instanced prototypes, and a moving emitter (two keys: ActiveTransform, or N keys) as a
  one-instance object; pbrt stopped with "Animated area lights are not supported" before
  (blurred renders with a moving emissive mesh failed). Checks (`tests/instanced_lights/
  run_instanced_lights.sh`): pbrt-only pairs agree within 0.1% (instance vs plain,
  rotated / non-uniformly scaled instances vs pre-transformed meshes, ActiveTransform and N-key
  motion vs the average of 16 static renders, primvar emission instanced); hdPbrt
  PointInstancer of 100 / 10k emissive quads vs separate meshes: means within 0.02%, noise
  vs a 1024 spp reference the same (0.141 vs 0.140); 100k instances render in ~4 s, pbrt peak
  RSS 219 MB (10k: 44 MB instanced vs 76 MB as meshes).
- Per-instance primvars (2026-11-28, pbrt patch 48). `HdPbrtInstancer::ComputeInstancePrimvar`
  (instance-rate primvars it already sampled at sync, Cd -> displayColor, flattened like the
  transforms; a nested instancer's own value wins) feeds `HdPbrtGeom::Write`: for every primvar
  the material reads (and displayColor when the base color reads it, also with no material)
  each ObjectInstance gets its values; per-instance displayColor uses `hdpbrt_displayColor`
  (instances without one get the prototype's constant, unless the prototype has a varying
  one). `HdPbrt_FlattenPrimvar` is shared now. Works for every instanced shape type and for
  instanced emitters (emission color per instance). Check (`tests/primvars/
  make_instance_primvars_scene.py`, in run_primvars.sh): PointInstancers with per-instance
  displayColor (no material), tint + rough (MaterialX), glow (emission) equal separate meshes
  with constant primvars pixel for pixel (bright lamp pools within noise); vs Karma the tinted
  spheres 1.01-1.04x, lamp pools 0.96-0.99x (unbound displayColor: Karma's darker default as
  before). Not per instance: displacement (one diced prototype), alpha cutouts. Materials run47
  34/8/0 identical, linking identical, run_instanced_lights.sh unchanged.
- Per-object dicing (2026-11-27, hdPbrt only): constant primvars `pbrt:dicingRate` /
  `pbrt_dicingRate` (pixels, over the render setting), `karma:object:dicingquality` (divides
  the rate, min 0.01; Karma's meaning: 2 = twice as fine) and `pbrt:dicingMaxTriangles` /
  `pbrt_dicingMaxTriangles`, read in `_SyncDisplayColor` with irReflectance, applied in
  `HdPbrtGeom::Write`; rate clamped to [0.05, 1e4]. Render Geometry Settings LOP tab
  (`GEOMETRY_SETTINGS`). Check (`tests/displacement/make_dicing_scene.py`, HDPBRT_KEEP_TMP):
  the four spheres get rates 2, 16, 8, 0.5; 66k triangles per sphere at 2, 1.13M at 0.5, none
  added at 8/16 (the subdivided base mesh is already finer: dicing never coarsens).
- Not translated yet: in-memory Houdini volumes (`op:` paths). Volume materials other than
  the Karma pyro shader (e.g. MaterialX volumes on Volume prims) use the renderer settings.
- Karma pyro shader (2026-11-25, pbrt patch 35): Houdini exports kma_pyroshader as a MaterialX
  graph behind `outputs:kma:volume`; hdPbrt now asks for the "kma" render context (after
  mtlx; before universal since 2026-11-26). Hydra delivers the pyro nodegraph as one node with the shader's own
  parameters (`HDPBRT_DUMP_MATERIALS=1` prints networks), recognized by `enablesmoke` /
  `densityscale` / `firetempscale` (`HdPbrt_PyroVolume`, material.cpp): extinction = density x
  densityscale x complement(absorpcolor) (Karma's hmtlxcomplementcolor: same luminance,
  chromaticity mirrored through D65), scattering = that x smokecolor x smokeint, fire from the
  field `fireint` reads (`temperature` by default). The internal ramps aren't visible through
  Hydra (defaults assumed: identity intensity, black -> (1, 0.327, 0.1)); color remaps, scatter
  emission, masks, Cd tint, shadow tweaks and constant emission warn once. Karma's ND_blackbody
  isn't MaterialX's luminance-1 one: measured table `_karmaBlackbody`
  (`tests/pyro/measure_karma_blackbody.py`). `HdPbrtVolume` takes the material's density and
  fire fields at sync and its medium parameters when the scene is written (material edits
  don't resync the volume); vdb2nvdb's box covers the temperature grid too. Test:
  `tests/pyro/run_pyro.sh` (builds the scene in hython, Karma via husk works under Apprentice):
  `PYRO_RESULTS.txt`, smoke / fire / blackbody within 1%, tinted smoke with multiple
  scattering 0.95 in red and blue (spectral).
- Volumes (done 2026-11-22): `volume.cpp` (`HdPbrtField` bprims, `HdPbrtVolume`), converter
  `tools/vdb2nvdb.cpp` -> `hdpbrt_vdb2nvdb` next to pbrt (Houdini flavor, needs
  `-DPBRT_SOURCE`; Blender's install links to it). The interface box must face outward.
  Test: `tests/volume_scene.usda` (uses Houdini's sample cloud.vdb from /opt/hfs22.0.508).
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
  one-instance object (emissive ones too since patch 47, as are their two-key motions).
  `tests/curved_scene.usda` + `tests/render_settings.py` (renders with renderer settings;
  reads pbrt's PFM from HDPBRT_TMPDIR). `docs/images/curved_motion.png`: 2 vs 9 samples.
- Deformation blur (done 2026-11-22, pbrt patch 5): a mesh samples `points` over +-1 frame at
  sync; only when they move (same count) it keeps `_Motion` (samples, PLY vertex -> point
  map, indices, uvs, authored normals or a topology copy for smooth normals). `Write()`
  writes the shutter-open and -close PLYs (`_UpdateMotionPlys`, cached per shutter) as
  `filename` + `motionfilename`. Authored normals are treated as static. Changing point counts: no motion, unless velocities (below). Verified with `tests/motion_scene.usda` (green "flag") and in the live
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
- `scripts/build.sh` from a clean clone (done 2026-11-26): all 35 patches apply at 20e41fd, the
  patched tree equals `../src/pbrt-v4` (same tree hash), and `tests/volume_scene.usda` renders
  pixel-identical to the local install. Tested without the big download: `GIT_CONFIG_COUNT`
  `url.<local>.insteadOf` entries map pbrt and each submodule URL to the local checkouts (plus
  `protocol.file.allow=always`). Fixed then: HFS default -> /opt/hfs22.0.508, and
  `checkout -B hdpbrt` (a clone of the local repo already has that branch).
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
  Volume-only materials (2026-11-25): a material with no surface terminal but an ND_volume
  (absorption_vdf / anisotropic_vdf) becomes pbrt `interface` (invisible boundary) around the
  same homogeneous interior medium. pbr anisotropic_vdf_volume_only used to fall back to grey
  diffuse: close -> match (0.944 / 0.237 -> 1.029 / 0.043); suite 34/8/0.
  Brightness (2026-11-23): fixed by patch 14. The rest (~1.7% on the shader ball) is a
  Typhoon quirk: it ends paths at geometry with no material bound (the backplanes), where
  we use displayColor diffuse; not copied. Bisect such gaps per bounce
  (`ty:maxBounces` b vs `pbrt:maxDepth` b+1).
  Thin walls: transmission passes straight through under the specular layer (Typhoon:
  ior 1 transmission), base takes 1 - transmission; standard_surface `thin_walled`, OpenPBR
  `geometry_thin_walled`. Shadow rays pass thin walls (patch 27, below).
  Media as band spectra (2026-11-23, not pushed: 2 fixtures match -> close): pbrt's RGB
  upsampling leaks absorption into zero channels (honey red 0.75x); `_MediumSpectrum` writes
  sigma_a/sigma_s as 3-band steps (490/590 nm, fitted to RGB-exact transmittance). honey
  differ -> close (0.86 -> 0.90), scatter_anisotropy close -> match, dielectric_layer_absorption
  and _anisotropic match -> close (RMSE 0.09 -> 0.105, 1.07 -> 1.08 bright).
  (That band version did ship, 5c82058.) Honey's remaining gap (0.898, red 0.83, inside the
  ball) taken apart 2026-11-25 by editing the medium in hdPbrt's .pbrt and re-running pbrt:
  (1) Typhoon's medium NEE passes straight through the medium's own smooth dielectric
  boundary (visibility.cpp: exitsCurrentMedium -> visibility = transmission weight, no
  refraction or Fresnel, even with caustics on), so every interior scatter gets the dome
  directly. An env-var experiment doing the same in pbrt: 0.900 -> 0.952 (red 0.83 -> 0.92).
  Not physical (a smooth refracting boundary can't be crossed by a shadow ray); not copied.
  (2) RGB vs spectral: Typhoon's red channel has exactly 0 absorption (honey's red is pure
  scattering after the absorption shift), pbrt's red pixel also sees < 590 nm light from the
  absorbing 490-590 band over hundreds of scatters. Moving that edge to 560 nm takes red to
  1.005 but green to 1.075 (other media fixtures would move). Path length: pbrt maxdepth 16
  -> 64 adds red 0.83 -> 0.90 here, but Typhoon also stops at 16 bounces and counts medium
  scatters, so that isn't a mismatch. Left as is.
  Coat (2026-11-23, patch 16): OpenPBR coat roughens the specular below
  (r' = (r^4 + 2 c^4)^(1/4)), makes its IOR relative to the coat (both blended by weight),
  and darkens the base, (1 - K) / (1 - E K) with K = 1 - (1 - F0) / n^2 (spec; Typhoon does
  the same); coattint = darkening * lerp(1, coat_color, w). standard_surface: coat_color,
  coat_affect_color (base^(1 + w a)), coat_affect_roughness. feature_coat 1.14 -> 1.02
  (match). Coat over metal works since patch 18. Darkening per shading point (patch 34,
  2026-11-25): it used to be baked into coattint from the constant base color, so a textured
  base used the untextured fallback, and metal (E = F0 x specular_weight) and subsurface
  (E = lerp(base, subsurface_color, w)) were ignored. `materials_extra/coat_dark` (textured
  base, textured metal, metal at specular_weight 0.5, subsurface): all close -> match
  (tex_coat RMSE 0.107 -> 0.036, tex_coat_metal 0.109 -> 0.041, metal_coat 1.076 -> 1.027,
  sss_coat 1.059 -> 1.009). Suite 33/9/0; feature_coat 1.017 -> 1.010.
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
  RMSE 0.059 -> 0.031; pbr oren_nayar 0.046 -> 0.033.
  Burley diffuse (patch 33, 2026-11-25): pbrlib burley_diffuse_bsdf (also as a layer's base)
  writes the flag even at roughness 0 (F90 = 0.5 there, not Lambert). Suite fixture 1.016 ->
  1.024 (RMSE 0.035 both), now in line with the other diffuse-only fixtures (oren_nayar 1.025,
  feature_base_color 1.029; the shader-ball scene reads ~1.02-1.03 bright). 33/9/0.
  `tests/typhoon/burley.usda` (sphere, no floor; dome r 0/0.55/1, sun r 0/1): hdPbrt equals
  the numerically integrated dome radiance in every view-angle bin (e.g. r 1, mu 0.3: 0.924
  vs exact 0.921; r 0, mu 0.3: 0.712 vs 0.715); Typhoon agrees to 1% for mu > 0.4 and departs
  toward grazing (r 0: +13% at mu < 0.2), Typhoon-side (its smaller mesh sphere and BSDL's
  visible-normal adjustment).
  dielectric_layer (1.07, RMSE 0.101, close) left as a Typhoon-side difference (2026-11-23):
  it's the default roughness 0.05 (the absorption variants set 0 and match). White furnace
  (`tests/typhoon/materials_extra/furnace`, `furnace_stats.py`): Typhoon's plain rough
  dielectric loses ~15% at alpha 0.05 even at normal incidence (0.832 vs hdPbrt 0.975), its
  coupled R-over-T layer agrees with ours within ~2% (0.903 vs 0.885), yet on the shaderball
  we're ~6% brighter: cause not found inside Typhoon's coupled rough path (its transparent-shadow
  shortcut is off with caustics on). At 0.2 Typhoon compensates multiple scattering (0.864 vs 0.788).
  Rough-glass multiple scattering (patch 21, pbrt dielectric "hdpbrtmultiscatter"): R and T
  scale by 1/E(wo), E the single-scattering R+T albedo for wo's side (VNDF-integrated table
  next to patch 12's, outside and inside incl. TIR), like Typhoon's coupled interface. Only
  for OpenPBR transmission, as Typhoon (not standard_surface, not pbrlib). Furnace OpenPBR
  glass r0.5 0.808 -> 0.996 (Typhoon 0.998); feature_transmission RMSE 0.082 -> 0.048.
  Thin film (patch 22): hdpbrtlayered "filmweight"/"filmthickness" (nm)/"filmeta" on the
  specular layer: exact spectral Airy reflectance (s/p averaged, film over the specular eta),
  lerped with Fresnel by weight; the layers below get 1 - w R_film(wo) (smooth value for rough
  lobes too). The specular layer reflects at eta 1 when filmed (soap bubble). From OpenPBR
  thin_film_* (um) and standard_surface thin_film_thickness (nm)/thin_film_IOR. Not yet on
  metal or on transmissive (dielectric) glass. feature_thin_film RMSE 0.048 -> 0.041.
  soapbubble stays close (0.891, RMSE 0.212): the film matches (bubble centre 0.784 vs
  0.779), but what's seen through it was darker (0.55 vs 0.70): pbrt shadow rays stopped at
  thin-walled straight transmission; Typhoon's pass. Fixed by patch 27 (below).
  Subsurface (patch 23): a random walk, as Typhoon/Cycles. hdpbrtlayered "subsurface" w: from
  outside the base's share w enters by cosine-weighted transmission (no Fresnel); from inside
  (wo.z < 0) the surface is a Lambertian exit only. The interior is a homogeneous medium
  ("bool hdpbrtsss" true) from Chiang 2016's remap of walk albedo + mean free path (+ g), the
  radius used as the physical mfp (no Cycles 1/(4 pi)). volpath: scattering in such media
  counts against its own 256-step budget (reset at surfaces), not maxdepth, and does no NEE.
  From standard_surface subsurface*, OpenPBR subsurface_* (radius * radius_scale), pbrlib
  subsurface_bsdf; thin-walled (or zero radius) keeps the albedo blend. Base keeps 1 - w; a
  base texture stays unscaled unless w ~ 1. Other objects inside the mesh aren't handled
  (Typhoon walks only against the owner mesh). long_radius_anisotropic close -> match
  (0.159 -> 0.082); 33 match / 9 close. White furnace walk: 1.000.
  Patch 25: inside/outside and the entry/exit directions use the geometric normal (passed
  to the BxDF in the shading frame), as the medium switches by it; with the smooth shading
  normal, coarse meshes showed bright blotches along the terminator at short radii.
  Dispersion (patch 24): OpenPBR transmission_dispersion_scale/abbe_number -> "spectrum eta"
  (360-830 nm every 10) from a two-term Cauchy fit through n_d (589.3) with V = (n_d - 1) /
  (n_F - n_C), Abbe / scale, as Typhoon; pbrt traces it at the hero wavelength. pbrt fix in
  the same patch: volpath's spectral MIS averaged r_u/r_l over all four wavelengths even after
  the secondaries were terminated, biasing chromatic media (dispersion sweep went 2.4x blue,
  "differs"); now hero-only when terminated. Sweep 0.159 -> 0.153; dispersive glasses keep
  their means but get single-wavelength color noise (open_pbr--glass RMSE 0.069 -> 0.084).
  Thin-wall pass-through (patch 27, 2026-11-24): volpath treats hdpbrtlayered's straight
  (eta 1) transmission like a medium boundary. Shadow rays continue through it times the
  lobe's weight (`HdPbrtLayeredBxDF::PassThrough`, material evaluated at the shadow hit);
  a path that samples it doesn't count a bounce, keeps the previous vertex for light MIS
  and divides r_l by the lobe probability (NEE multiplies r_u by it), so light seen
  through a wall isn't counted twice. soapbubble 0.891 -> 0.922 (RMSE 0.212 -> 0.187),
  input_thin_walled 0.971 -> 0.999 (0.168 -> 0.156); both still close. 33/9/0, usdlux
  326/328 unchanged.
  Thin film on metal (patch 28, 2026-11-24): the metal lobe's Fresnel is lerped by film
  weight with an exact spectral Airy reflectance over the metal (`HdPbrtThinFilmMetalR`):
  complex-index conductors directly (eta + ik relative to the film); F82-tint metals by the
  film-side amplitude that gives back the F82 value under a zero-thickness film, with the
  sign of a dielectric of F0's index (Typhoon also takes a dielectric's phase). OpenPBR and
  standard_surface metals get it through hdpbrtlayered; pbrlib conductor_bsdf /
  generalized_schlick_bsdf with `thinfilm_thickness` > 0 become a metal-only hdpbrtlayered
  (the film used to be dropped). conductor_bsdf_thinfilm_aluminum_500 RMSE 0.097 -> 0.048;
  33/9/0. F82 metals with a film read 10-28% brighter than Typhoon, which is on Typhoon's
  side (`materials_extra/film_check`: its 1 nm film darkens the metal; ours equals no film).
  Thin film on glass (patch 29, 2026-11-24): pbrt `dielectric` takes "filmweight" /
  "filmthickness" / "filmeta"; `DielectricBxDF::Fr` lerps Fresnel with the spectral Airy
  reflectance of a film on the outside (from inside: the matching outside angle, since a
  lossless stack reflects the same from both sides; TIR -> 1), transmission 1 - Fr, as Typhoon
  (R + T = 1). Sampling uses the spectra's averages. From OpenPBR / standard_surface glass and
  pbrlib dielectric_bsdf thinfilm_* (also on the R-over-T layer). Suite unchanged (no fixture
  has a film on solid glass); `materials_extra/glass_film` match.
  standard_surface glass (2026-11-25): its MaterialX graph is layer(specular R over
  transmission T), so T pays Fresnel twice per crossing; Typhoon follows the graph (furnace,
  smooth: 0.909, centre 0.927 = two crossings at (1-F)^2 + F). hdPbrt now writes patch 17's
  `hdpbrttopweight` (= specular) on standard_surface glass: furnace 1.000 -> 0.904 (r 0.2:
  0.982 -> 0.890 vs 0.905), shaderball ss_glass_nofilm 1.16 -> 1.08, ss_glass_film 1.16 -> 1.07.
  Suite 33/9/0; input_transmission_extra_roughness 0.926 -> 0.901 (RMSE 0.130 -> 0.160, still
  close): MaterialX keeps R at specular_roughness and only T gets the extra roughness.
  Separate reflection roughness (patch 30, 2026-11-25): standard_surface glass with
  transmission_extra_roughness and pbrlib layer(R over T) with different top/base roughness
  write `hdpbrtreflectionroughness`. input_transmission_extra_roughness 0.901 -> 0.931 (RMSE
  0.160 -> 0.136, close); 33/9/0, nothing else moved. Furnace f_ss_r02_x045 (0.2 + 0.45):
  hdPbrt 0.572 vs Typhoon 0.817. Typhoon's gap is its own: when a rough transmission-only
  sample fails (microfacet TIR, wrong side) it falls back to a delta transmission
  (`closureTraversal.cpp`, SampleDeltaDielectricTransmission) instead of losing the path; not
  copied. Tried and dropped: the inside (TIR) albedo for unsplit lobes too: dielectric_layer
  close -> match (1.068 -> 0.998) and ss_glass 1.077 -> 1.035, but furnace f_layer_r02 0.788
  -> 0.683 (Typhoon 0.863) and extra rough_glass layer_r02 match -> close.
- DistantLight cone vs Typhoon (done 2026-11-25): `tests/typhoon/distant_cone.usda` (12 frames:
  normalize on at 0.53/2/5/10/20/45/90/180 deg, off at 0.53/5/20/0), `run_distant_cone.sh`,
  `cone_stats.py`, results in `DISTANT_CONE_RESULTS.txt`. A plate 1 unit up (penumbra), a mirror
  sphere (the sun disk) and a rough metal sphere (highlight). Everything outside the mirror
  agrees (mean within 0.2%, rel RMSE <= 0.083) up to 90 deg; the unshadowed floor matches the
  analytic irradiance in both to 0.3%. Not ours: (1) at 180 deg (and +17% at 90) Typhoon
  barely shadows: under the plate hdPbrt 0.370 vs analytic 0.385, Typhoon 0.598 (cause not
  found in its NEE/visibility code). (2) angle 0: Typhoon shows a ~0.26 deg disk in a mirror
  (its delta direction test uses cos >= 1 - 1e-5); hdPbrt, physically, nothing. (3) A 0.53 deg
  sun in a mirror is ~1/30 px: both are very noisy even at 4096 spp. Residual: the sun in the
  mirror read 4-10% low in hdPbrt at 2-90 deg with subdivided sphere meshes; with exact
  spheres (implicit prims, below) it reads ~10% high, and the comparison drops to 4/4/4 (was
  3/6/3) from outline rings: Typhoon's spheres are ~10% small. Outside the mirror rel RMSE
  0.083 -> 0.063. Typhoon draws `Cube` prims as
  ellipsoids? (its shadow was an oval): the plate is a box mesh.
- Light linking vs Typhoon (done 2026-11-25): `tests/typhoon/linking.usda` (rect light linked to
  one sphere + a native instance, sun excluding a sphere, sphere light with a shadow link
  excluding a wall, dome excluding the floor; frame 1 all, 2-5 one light each),
  `run_linking.sh` (generic harness `run_frames.sh`), `LINKING_RESULTS.txt`. Typhoon doesn't
  register `HdsiLightLinkingSceneIndex`: through scene-index emulation it gets no collections
  and links nothing; `run_linking.sh` renders it with `USDIMAGINGGL_ENGINE_ENABLE_SCENE_INDEX=0`
  (USD's legacy delegate resolves categories). Found and fixed: native instances got no
  categories (unlit by every linked light: pbrt patch 31) and fireflies with a dome plus a
  distant cone (stock volpath MIS bug, patch 32). Now 3 match / 1 close / 1 "differs"; means
  0.98-1.01; frame 2 (rect light alone, mostly black) is noise, frame 5 see below. usdlux
  326/328, materials 33/9/0 unchanged. Point instancers added the same day (balls linked to
  the rect light and out of C's shadows, cubes out of the sun): still 3/1/1. Typhoon draws
  `Cube` prims as spheres, so the cubes are meshes. Frame 5 (dome alone) had brighter
  contact shadows in Typhoon: both renderers' spheres hovered (subdivided implicit meshes,
  see "Implicit prims" below).
- Implicit prims (2026-11-25, `implicit.cpp`, `HdPbrtImplicit`): Sphere, Cylinder(_1), Cone and
  Capsule(_1) are no longer converted to meshes. USD's meshes for them are ~10-segment
  Catmull-Clark cages, and hdPbrt's default subdivision (level 2) shrank them: a radius-1
  sphere had vertices at 0.925-0.988 (hovering 0.075 over a floor it should touch; dark
  contact shadows came out 0.74x analytic) and cylinder/cone rims were rounded. Now: pbrt
  `sphere`; `cylinder` + `disk` caps (bottom flipped to face -z); capsule = cylinder + half
  spheres; cone and Cylinder_1 frustum = a 128-segment band with exact normals + disks;
  tapered Capsule_1 = GeomUtil's capsule mesh (96 x 24). Axis X/Y rotates pbrt's +z.
  Params via the emulated `Get(id, radius/height/axis/radiusTop/radiusBottom)`, re-read on
  DirtyPrimvar. Cube and plane stay bilinear meshes. Tests: `tests/implicit_scene.usda`
  (orthographic silhouettes match the authored sizes), `tests/typhoon/dome_contact.usda` +
  `dome_contact_stats.py`: sphere on a floor under a uniform dome vs the analytic
  0.8 (1 + n.y) / 2: hdPbrt 0.74-1.05x before, 1.001-1.003x after. Typhoon reads 1.1-4.2x
  there (its spheres are shrunk too; cause of the rest not looked into).
  Typhoon comparisons with Sphere prims lose agreement at outlines: linking 3/1/1 -> 2/2/1,
  distant cone 3/6/3 -> 4/4/4; materials 33/9/0 and usdlux 326/328 unchanged.
- Shadow/reflection terminator (done 2026-11-23, pbrt patch 26): on coarse smooth-shaded
  meshes, rays leaving near a facet's horizon were blocked by neighboring facets (whole dark
  polygons along the terminator; dark slivers in mirror reflections at silhouettes). Triangle
  hits with vertex normals store Hanika 2021's lift (hit point projected onto the vertex
  normals' tangent planes, barycentric mean; carried through instance transforms); volpath
  starts shadow rays and BSDF-sampled reflection rays from p + w * lift, w = Typhoon's
  facet-horizon weight (cutoff 0.1). Test: `tests/terminator_scene.usda` (12x6 sphere);
  `docs/images/terminator.png` rows off / on / always-on. Materials unchanged (33/9/0).
  Note: a DistantLight's unauthored angle is 0.53 deg and, without normalize, intensity is
  the radiance in that disk (UsdLux default intensity 50000; Typhoon the same): an intensity
  of a few is near black. Use normalize for sun-like values.
- Velocity motion blur (done 2026-11-23): `sceneIndexPlugin.cpp` registers USD's
  `HdsiVelocityMotionResolvingSceneIndex` (phase 2, as hdPrman; plugInfo entry), which turns
  `velocities`/`accelerations` (and point-instancer velocities) into point samples using the
  stage's timeCodesPerSecond. The deformation blur above then blurs meshes whose point count
  changes every frame (simulations). Test: `tests/velocity_scene.usda` at frame 1 (red "sim"
  changes count and has velocities: blurred 1 unit/frame; blue "still" without velocities:
  sharp); `docs/images/velocity_blur.png`.
