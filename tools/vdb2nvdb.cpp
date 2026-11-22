// SPDX-License-Identifier: Apache-2.0
// hdpbrt_vdb2nvdb: convert float grids of an OpenVDB file to a NanoVDB file
// for pbrt's "nanovdb" medium.
//
//   hdpbrt_vdb2nvdb in.vdb out.nvdb densityGrid [temperatureGrid]
//
// The density grid is written as "density", the temperature grid (if given
// and found) as "temperature" (pbrt's default grid names). The density
// grid's world-space bounding box goes to out.nvdb.bbox as six numbers
// (min x y z, max x y z). Exit status 0 on success.
//
// Built with pbrt's own NanoVDB headers so the file version matches what pbrt
// reads, against OpenVDB (Houdini's libopenvdb_sesi).
#include <openvdb/openvdb.h>
#include <nanovdb/util/IO.h>
#include <nanovdb/util/OpenToNanoVDB.h>

#include <cstdio>
#include <string>
#include <vector>

static openvdb::FloatGrid::Ptr readFloatGrid(openvdb::io::File &file, const std::string &name)
{
    if (name.empty() || !file.hasGrid(name))
        return nullptr;
    openvdb::GridBase::Ptr base = file.readGrid(name);
    openvdb::FloatGrid::Ptr grid = openvdb::gridPtrCast<openvdb::FloatGrid>(base);
    if (!grid)
        fprintf(stderr, "hdpbrt_vdb2nvdb: grid \"%s\" is not a float grid\n", name.c_str());
    return grid;
}

int main(int argc, char **argv)
{
    if (argc < 4) {
        fprintf(stderr, "usage: %s in.vdb out.nvdb densityGrid [temperatureGrid]\n", argv[0]);
        return 2;
    }
    const std::string in = argv[1], out = argv[2], dname = argv[3],
                      tname = argc > 4 ? argv[4] : "";
    try {
        openvdb::initialize();
        openvdb::io::File file(in);
        file.open(false);  // no delayed loading
        openvdb::FloatGrid::Ptr density = readFloatGrid(file, dname);
        if (!density) {
            fprintf(stderr, "hdpbrt_vdb2nvdb: %s: no float grid \"%s\"\n", in.c_str(),
                    dname.c_str());
            return 1;
        }
        openvdb::FloatGrid::Ptr temperature = readFloatGrid(file, tname);
        file.close();

        std::vector<nanovdb::GridHandle<nanovdb::HostBuffer>> handles;
        density->setName("density");
        handles.push_back(nanovdb::openToNanoVDB(*density));
        if (temperature) {
            temperature->setName("temperature");
            handles.push_back(nanovdb::openToNanoVDB(*temperature));
        }
        nanovdb::io::writeGrids(out, handles);

        // World-space bounds of the active voxels (voxel corners).
        const openvdb::CoordBBox ib = density->evalActiveVoxelBoundingBox();
        openvdb::BBoxd wb;
        if (!ib.empty()) {
            const openvdb::Vec3d lo = ib.min().asVec3d() - openvdb::Vec3d(0.5);
            const openvdb::Vec3d hi = ib.max().asVec3d() + openvdb::Vec3d(0.5);
            for (int c = 0; c < 8; ++c) {
                openvdb::Vec3d p((c & 1) ? hi.x() : lo.x(), (c & 2) ? hi.y() : lo.y(),
                                 (c & 4) ? hi.z() : lo.z());
                wb.expand(density->transform().indexToWorld(p));
            }
        }
        FILE *f = fopen((out + ".bbox").c_str(), "w");
        if (!f)
            return 1;
        fprintf(f, "%.9g %.9g %.9g %.9g %.9g %.9g\n", wb.min().x(), wb.min().y(),
                wb.min().z(), wb.max().x(), wb.max().y(), wb.max().z());
        fclose(f);
    } catch (const std::exception &e) {
        fprintf(stderr, "hdpbrt_vdb2nvdb: %s\n", e.what());
        return 1;
    }
    return 0;
}
