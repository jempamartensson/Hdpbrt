# hdPbrt

A Hydra render delegate for [pbrt-v4](https://github.com/mmp/pbrt-v4), for Houdini 22 Solaris
and Blender 5.3. CPU only.

![pbrt in the Houdini Solaris viewport](docs/images/houdini_viewport_pbrt_depth.png)

## How it works

The delegate does not link pbrt. pbrt keeps process-wide state and calls `exit()` on scene
errors, so it runs as a separate process instead:

1. Hydra prims sync into a light copy of the scene. Meshes are triangulated and written to
   binary PLY files as they change.
2. When the scene, camera, settings or viewport size change, the delegate writes a `.pbrt`
   scene, restarts pbrt (debounced while tumbling) and listens on a local socket.
3. pbrt streams its progressive image, plus first-hit depth, over its tev display-server
   protocol. The final image is read from the PFM pbrt writes when it exits.

pbrt needs the four patches in `patches/` (against pbrt-v4 `20e41fd`): pbrt exits when its
parent process dies, and its RGB film can record and stream depth, per-pixel prim/instance
ids (selection) and coverage (alpha) for the viewport.

## Build

```bash
scripts/build.sh                 # pbrt + Houdini flavor, into ./install/houdini
BLENDER=1 USD_SOURCE=... TBB_INCLUDE=... scripts/build.sh   # also the Blender flavor
```

The Houdini flavor builds against `$HFS` (default `/opt/hfs22.0`). The Blender flavor builds
against Blender's `libusd_ms.so` with OpenUSD v26.08 source headers and the generated
`pxr.h` in `third_party/usd-blender-generated`. Details and gotchas are in
[docs/BUILD_NOTES.md](docs/BUILD_NOTES.md).

## Use

- **Houdini:** put a `dso/usd_plugins/plugInfo.json` that includes the install dir on
  `HOUDINI_PATH` (Houdini ignores `PXR_PLUGINPATH_NAME` set from a package), then pick
  **pbrt** in the LOP viewport's renderer menu.
- **Blender 5.3:** install `blender/hydra_pbrt` as an add-on (its `plugin` folder points at
  the Blender build) and choose **Hydra pbrt** as the render engine.

Render settings: `pbrt:pixelSamples`, `pbrt:maxDepth`, `pbrt:integrator`, `pbrt:sampler`,
`pbrt:threads`, `pbrt:regularize`, `pbrt:exportPath`.

Environment: `HDPBRT_PBRT_EXECUTABLE`, `HDPBRT_TMPDIR`, `HDPBRT_KEEP_TMP=1`,
`HDPBRT_DEBUG=1` (prints AOV bindings and mesh primvars), `HDPBRT_REVERSED_DEPTH=0/1`
(Houdini's Vulkan viewport expects reversed depth, the default in the Houdini build).

## Supported

Meshes, implicit shapes, point instancers; UsdPreviewSurface and MaterialX standard_surface /
open_pbr_surface (mapped to pbrt conductor, dielectric or coateddiffuse) with image textures;
distant, sphere, rect, disk, cylinder and dome lights; perspective and orthographic cameras;
color, depth, primId and instanceId AOVs (viewport selection, including point instances).
Without a dome light the background is transparent, so the host's viewport background shows.
Not yet: curves, points, volumes, subdivision, motion blur, depth of field, light shaping.

## Tests

`tests/`: hython and Blender render scripts and a test scene. `tests/viewport/vp_test.py`
drives a live Solaris viewport and flipbooks it (see the Tests section of the build notes for
running it in a headless compositor).

## License

Apache-2.0 (see `LICENSE`). Parts of the render buffer, instancer and implicit-surface plugin
are adapted from USD's hdEmbree (Apache-2.0, Pixar). `third_party/usd-blender-generated/pxr/pxr.h`
is generated from OpenUSD and is under its license.
