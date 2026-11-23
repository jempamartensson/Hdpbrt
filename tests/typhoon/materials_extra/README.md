Extra material fixtures for cases the Typhoon test suite doesn't cover, in the suite's
layout. They sublayer `../_assets/shaderball/scene.usda`, so link the suite's assets next to
them and point SUITE at a directory whose `materials/` holds both:

    mkdir -p $X/materials && ln -s $SUITE/materials/_assets $X/materials/_assets
    ln -s $PWD/tests/typhoon/materials_extra/* $X/materials/
    SUITE=$X OUT=... tests/typhoon/run_materials.sh open_pbr_metal diffuse_rough

- `open_pbr_metal`: OpenPBR metal with specular_weight 0.5 / tinted specular_color /
  half metalness (pbrt patch 19). All match Typhoon (2026-11-23).
- `diffuse_rough`: standard_surface diffuse_roughness / OpenPBR base_diffuse_roughness, with
  and without specular (EON, pbrt patch 20). RMSE 0.059 -> 0.031 vs Lambert (2026-11-23).
