Extra material fixtures for cases the Typhoon test suite doesn't cover, in the suite's
layout. They sublayer `../_assets/shaderball/scene.usda`, so link the suite's assets next to
them and point SUITE at a directory whose `materials/` holds both:

    mkdir -p $X/materials && ln -s $SUITE/materials/_assets $X/materials/_assets
    ln -s $PWD/tests/typhoon/materials_extra/* $X/materials/
    SUITE=$X OUT=... tests/typhoon/run_materials.sh open_pbr_metal diffuse_rough rough_glass furnace

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
