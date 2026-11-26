// Houdini op: images (COP nodes) for textures, Houdini flavor only.
//
// Compiled with the HDK's defines (see CMakeLists.txt) and kept free of USD
// headers. TIL_CopResolver is what Karma's texture code uses: it parses
// op:/path/to/cop{output}[frame](xres,yres), cooks the node (Copernicus,
// image networks or old COP2s) and returns a raster.

#include <TIL/TIL_CopResolver.h>
#include <TIL/TIL_Raster.h>

#include <cstdio>
#include <string>
#include <vector>

// The node's image version: changes as soon as the node is edited (before it
// cooks again), so it's cheap to poll. -1 without a resolver.
long long HdPbrt_HoudiniImageVersion(int opid)
{
    if (!TIL_CopResolver::getResolver() || opid < 0)
        return -1;
    return (long long)TIL_CopResolver::getRasterVersion(opid);
}

// Cooks `path` (*opid: the node, for HdPbrt_HoudiniImageVersion). *version is
// a key that changes when the image does (node id, version, time, resolution).
// When writeTo(version) returns a path, the image is written there as a float
// PFM (1 channel for single-channel rasters, else RGB; Houdini rasters start
// at the bottom row, as PFM does). Returns false (with *error) when the path
// doesn't resolve or the file can't be written.
bool HdPbrt_HoudiniImage(const std::string &path, std::string *version,
                         std::string (*writeTo)(void *ctx, const std::string &version),
                         void *ctx, int *opid, std::string *error)
{
    if (!TIL_CopResolver::getResolver()) {
        *error = "no COP resolver (not running inside Houdini)";
        return false;
    }
    // Not through the thread's resolver cache: Houdini only clears that for
    // Karma, so after an edit it would keep handing out the old image.
    // Uncached, an unchanged node costs well under a millisecond.
    *opid = -1;
    TIL_Raster *ras = TIL_CopResolver::getRaster(path.c_str(), *opid);
    if (!ras) {
        *error = "the node doesn't exist or produced no image";
        return false;
    }
    const exint w = ras->getXres(), h = ras->getYres();
    const int nc = ras->getNumChannels();
    char buf[256];
    std::snprintf(buf, sizeof(buf), "%d/%zu/%.6g/%lldx%lldx%d", *opid,
                  TIL_CopResolver::getRasterVersion(*opid), TIL_CopResolver::getRasterTime(*opid),
                  (long long)w, (long long)h, nc);
    *version = buf;
    bool ok = true;
    const std::string out = writeTo(ctx, *version);
    if (!out.empty()) {
        ok = false;
        if (FILE *f = std::fopen(out.c_str(), "wb")) {
            const int oc = nc == 1 ? 1 : 3;
            std::fprintf(f, "%s\n%lld %lld\n-1\n", oc == 1 ? "Pf" : "PF", (long long)w,
                         (long long)h);
            std::vector<float> row(w * oc);
            float v[4];
            for (exint y = 0; y < h; ++y) {
                for (exint x = 0; x < w; ++x) {
                    v[0] = v[1] = v[2] = v[3] = 0.f;
                    ras->getPixelValue(x, y, v);
                    for (int k = 0; k < oc; ++k)
                        row[x * oc + k] = nc == 2 && k == 2 ? 0.f : v[k];
                }
                std::fwrite(row.data(), sizeof(float), row.size(), f);
            }
            ok = std::fclose(f) == 0;
        }
        if (!ok)
            *error = "could not write " + out;
    }
    TIL_CopResolver::doneWithRaster(ras);
    return ok;
}
