Extra material fixtures for cases the Typhoon test suite doesn't cover, in the suite's
layout. They sublayer `../_assets/shaderball/scene.usda`, so link the suite's assets next to
them and point SUITE at a directory whose `materials/` holds both:

    mkdir -p $X/materials && ln -s $SUITE/materials/_assets $X/materials/_assets
    ln -s $PWD/tests/typhoon/materials_extra/* $X/materials/
    SUITE=$X OUT=... tests/typhoon/run_materials.sh open_pbr_metal diffuse_rough rough_glass furnace thin_film

- `open_pbr_metal`: OpenPBR metal with specular_weight 0.5 / tinted specular_color /
  half metalness (pbrt patch 19). All match Typhoon (2026-11-23).
- `diffuse_rough`: standard_surface diffuse_roughness / OpenPBR base_diffuse_roughness, with
  and without specular (EON, pbrt patch 20). RMSE 0.059 -> 0.031 vs Lambert (2026-11-23).
- `rough_glass`: pbrlib dielectric_bsdf RT at roughness 0 / 0.05 / 0.2 and R-over-T layers at
  0.05 / 0.2 (the suite's dielectric_layer is 0.05, MaterialX's default). rt_r005 "differs"
  (1.22) and rt_r02 is close (1.10): Typhoon's side, see `furnace`.
- `furnace`: the same materials on a unit sphere in a uniform white dome (own camera and
  settings, no shaderball). Lossless glass should vanish (radiance 1);
  `python3 tests/typhoon/furnace_stats.py $OUT` prints the ball mean and a radial profile.
  2026-11-23, ball mean Typhoon / hdPbrt: f_r0 1.000 / 1.000, f_r005 0.832 / 0.975,
  f_r02 0.806 / 0.856, f_layer_r005 0.903 / 0.885, f_layer_r02 0.864 / 0.788. Typhoon's plain
  rough dielectric loses ~15% even at normal incidence at alpha 0.05 (a Typhoon bug); the R-over-T
  layer loses energy in both by MaterialX's definition (T keeps its own 1 - F and also pays the top's
  1 - E_R). At 0.2 Typhoon compensates microfacet multiple scattering and pbrt doesn't.
  f_op_r03 / f_op_r05: OpenPBR glass (transmission_weight 1) at specular_roughness 0.3 / 0.5,
  with pbrt patch 21's multiple-scattering compensation: hdPbrt 0.951 -> 0.976 and 0.808 -> 0.996
  (Typhoon 1.001 / 0.998).
- `thin_film`: standard_surface thin_film_thickness 400 nm / IOR 1.33 and OpenPBR
  thin_film_weight 0.5 / 0.3 um (pbrt patch 22). RMSE 0.061 -> 0.039 and 0.042 -> 0.036.
  f_sss_white / f_sss_08: OpenPBR subsurface (weight 1, radius 0.3, no specular) with walk
  albedo 1 / 0.8 (pbrt patch 23): hdPbrt 1.000 / 0.971, Typhoon 0.999 / 0.965 (above 0.8:
  light crosses a sphere only ~3 mean free paths thick).
