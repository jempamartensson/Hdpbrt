# Comparisons with Typhoon

Typhoon is USD's reference renderer: hdEmbree in a fork of OpenUSD
(https://github.com/DGG3D/openusd-typhoon, branch `typhoon/main`), a CPU path tracer that
implements UsdLux, UsdGeomCamera exposure and MaterialX by the spec. These scripts render the
same USD files with hdPbrt and compare the images with Typhoon's.

- `render_hdpbrt.py`: renders frames with hdPbrt in hython, with the camera and resolution of
  the stage's RenderSettings prim, and keeps pbrt's float image per frame.
- `compare.py`: frame means within 5% (or both black).
- `run_usdlux.sh`: Typhoon's usdlux regression suite (from
  https://github.com/anderslanglands/typhoon-test-suite, references from its GitHub release)
  rendered with hdPbrt and compared. Direct lighting only (`pbrt:maxDepth=1`, as the suite's
  `ty:maxBounces = 0`). Result: `RESULTS.txt`.
- `exposure_scene.usda`: camera exposure ("Sunny 16": an 18% grey card under a 100,000 lux
  sun at ISO 100, 1/100 s, f/16, then one exposure attribute changed per frame). Render it
  with Typhoon (`usdrender exposure_scene.usda -r Embree -f 1:7`) and with
  `render_hdpbrt.py ... 1:7 out pbrt:maxDepth=1`. 2026-11-22: hdPbrt and Typhoon agree to
  4 decimals on all 7 frames, both within 0.25% of the analytic values in the file header
  (Sunny 16 gives 0.2233 for the expected 0.2238).

Typhoon's defaults include a firefly clamp of 20; reference scenes must set
`ty:fireflyClampThreshold = 0` (and `ty:causticsClampThreshold = 0`), or bright values are
clamped.
