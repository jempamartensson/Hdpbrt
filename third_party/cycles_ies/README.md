# Cycles IES parser

`ies.h` and `ies.cpp` parse IES (IESNA LM-63) light profiles. They come from
[Cycles](https://projects.blender.org/blender/cycles) (Apache-2.0, Copyright
2011-2022 Blender Foundation), as vendored in OpenUSD 26.08
(`pxr/imaging/plugin/hdEmbree/pxrIES`, Cycles v4.1.1 with USD's patch that wraps
them in the USD namespace). hdPbrt renames the inner namespace `pxr_ccl` to
`hdpbrt_ccl` so the copy can't collide with hdEmbree's when both are loaded.

Evaluation, power integration and the table pbrt reads are hdPbrt's own
(`hdPbrt/ies.cpp`), written from the UsdLuxShapingAPI specification.
