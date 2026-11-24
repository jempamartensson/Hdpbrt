Extra material fixtures for cases the Typhoon test suite doesn't cover, in the suite's
layout. They sublayer `../_assets/shaderball/scene.usda`, so link the suite's assets next to
them and point SUITE at a directory whose `materials/` holds both:

    mkdir -p $X/materials && ln -s $SUITE/materials/_assets $X/materials/_assets
    ln -s $PWD/tests/typhoon/materials_extra/* $X/materials/
    SUITE=$X OUT=... tests/typhoon/run_materials.sh open_pbr_metal diffuse_rough rough_glass furnace thin_film dispersion metal_film film_check glass_film

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
- `dispersion`: smooth OpenPBR glass, dispersion scale 1, Abbe 10 (patch 24): ratio 1.025,
  RMSE 0.086 (0.077 without dispersion: single-wavelength color noise).
- `metal_film`: thin film on metal (pbrt patch 28): OpenPBR metalness 1 with a full film and
  a half-weight film on a tinted metal, standard_surface metalness 1 and 0.5 (2026-11-24, ratio /
  RMSE): op_metal_film 1.177 / 0.315 differs, op_metal_film_w05 1.100 / 0.131 close,
  ss_metal_film 1.280 / 0.455 differs, ss_metal_film_half 1.140 / 0.206 close. Typhoon's side:
  see `film_check`. (The suite's conductor_bsdf_thinfilm_aluminum_500, a complex-index
  conductor, went from RMSE 0.097 to 0.048.)
- `film_check`: op_metal_film with no film and with a 1 nm film, which physically is no film.
  hdPbrt renders both the same; Typhoon's 1 nm film darkens the metal by 0.85 / 0.74 / 0.92
  (RGB) over the ball. Likely cause: its Schlick-substrate Airy term (`thinFilm.cpp`) sets
  each polarization's substrate reflectance to half the Fresnel value (R23p = R23s = F / 2). Without a film the
  same metal matches (1.032).
- `glass_film`: thin film on solid glass (pbrt patch 29): OpenPBR smooth (film ior 1.8, 400 nm)
  and rough (0.3, film weight 0.7), pbrlib dielectric_bsdf RT with thinfilm, standard_surface
  transmission 1 with a 350 nm film, and OpenPBR with a 1 nm film / no film (2026-11-24).
  OpenPBR and pbrlib all match (1.02; RMSE vs Typhoon's film refs 0.086 -> 0.073 and
  0.075 -> 0.060 against dropping the film). hdPbrt's 1 nm film equals no film; Typhoon's
  shifts the glass by +1% (it leaves its coupled-interface path when any film is present), while
  the 400 nm film's own effect is about the same in both. ss_glass_film and ss_glass_nofilm are
  both close at 1.16: a standard_surface glass gap that has nothing to do with the film (open).
