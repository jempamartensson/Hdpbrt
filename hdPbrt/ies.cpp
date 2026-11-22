// SPDX-License-Identifier: Apache-2.0
// IES profiles for USD light shaping (shaping:ies:*). The file is parsed with
// Cycles' parser (third_party/cycles_ies); evaluation follows the
// UsdLuxShapingAPI spec: theta is the angle from the light's axis (-z), the
// angle scale rescales theta about 0 (positive) or pi (negative), and
// normalize divides by the profile's average intensity over the (hemi)sphere
// it covers. The result is tabulated over theta x phi for pbrt (patch 8).
#include "hdPbrt.h"
#include "../third_party/cycles_ies/ies.h"

#include <cmath>
#include <cstdio>
#include <fstream>
#include <sstream>

PXR_NAMESPACE_OPEN_SCOPE

namespace {

class _IES : public hdpbrt_ccl::IESFile {
public:
    bool Load(const std::string &text)
    {
        return load(text) && h_angles.size() >= 2 && v_angles.size() >= 2 &&
               intensity.size() == h_angles.size();
    }

    // Luminous intensity toward (theta, phi) of the light's frame (radians).
    float Eval(float theta, float phi, float angleScale) const
    {
        const float pi = float(M_PI);
        phi = std::fmod(phi, 2 * pi);
        if (phi < 0)
            phi += 2 * pi;
        if (angleScale > 0)
            theta = theta / angleScale;
        else if (angleScale < 0)
            theta = (theta - pi) / -angleScale + pi;
        if (theta < 0)
            return 0.f;
        int hi = -1, vi = -1;
        float dh = 0, dv = 0;
        for (size_t i = 0; i + 1 < h_angles.size(); ++i)
            if (phi >= h_angles[i] && phi < h_angles[i + 1]) {
                hi = int(i);
                dh = (phi - h_angles[i]) / (h_angles[i + 1] - h_angles[i]);
                break;
            }
        if (theta >= pi) {
            vi = int(v_angles.size()) - 2;
            dv = 1;
        } else {
            for (size_t i = 0; i + 1 < v_angles.size(); ++i)
                if (theta >= v_angles[i] && theta < v_angles[i + 1]) {
                    vi = int(i);
                    dv = (theta - v_angles[i]) / (v_angles[i + 1] - v_angles[i]);
                    break;
                }
        }
        if (hi < 0 || vi < 0)
            return 0.f;  // outside the profile (e.g. above a hemisphere profile)
        const float a = intensity[hi][vi] * (1 - dv) + intensity[hi][vi + 1] * dv;
        const float b = intensity[hi + 1][vi] * (1 - dv) + intensity[hi + 1][vi + 1] * dv;
        return std::max(0.f, a * (1 - dh) + b * dh);
    }

    // Average intensity over the solid angle the profile covers.
    float Power() const
    {
        const float pi = float(M_PI);
        const auto [vmin, vmax] = std::minmax_element(v_angles.begin(), v_angles.end());
        const bool sphere = (*vmax - *vmin) > pi / 2 + 0.1f;
        double p = 0;
        for (size_t h = 0; h + 1 < h_angles.size(); ++h)
            for (size_t v = 0; v + 1 < v_angles.size(); ++v) {
                const float dh = h_angles[h + 1] - h_angles[h];
                const float dv = v_angles[v + 1] - v_angles[v];
                const float c = 0.25f * (intensity[h][v] + intensity[h][v + 1] +
                                         intensity[h + 1][v] + intensity[h + 1][v + 1]);
                p += dh * dv * std::sin(v_angles[v] + dv / 2) * c;
            }
        return float(p / (pi * (sphere ? 4 : 2)));
    }
};

}  // namespace

bool HdPbrt_WriteIESTable(const std::string &iesPath, float angleScale, bool normalize,
                          const std::string &outPfm)
{
    std::ifstream in(iesPath);
    if (!in)
        return false;
    std::stringstream ss;
    ss << in.rdbuf();
    _IES ies;
    if (!ies.Load(ss.str()))
        return false;
    float norm = 1.f;
    if (normalize) {
        norm = ies.Power();
        if (!(norm > 0))
            norm = 1.f;
    }
    // u = phi / 2pi (columns), v = theta / pi (rows, theta = 0 at the top),
    // sampled at pixel centers like pbrt's bilinear lookup.
    const int W = 180, H = 180;
    std::vector<float> rows(size_t(W) * H);
    for (int j = 0; j < H; ++j) {
        const float theta = float(M_PI) * (j + 0.5f) / H;
        for (int i = 0; i < W; ++i) {
            const float phi = 2 * float(M_PI) * (i + 0.5f) / W;
            rows[size_t(j) * W + i] = ies.Eval(theta, phi, angleScale) / norm;
        }
    }
    FILE *f = fopen(outPfm.c_str(), "wb");
    if (!f)
        return false;
    fprintf(f, "Pf\n%d %d\n-1\n", W, H);  // grayscale, little endian
    for (int j = H - 1; j >= 0; --j)       // PFM rows go bottom to top
        fwrite(&rows[size_t(j) * W], sizeof(float), W, f);
    return fclose(f) == 0;
}

PXR_NAMESPACE_CLOSE_SCOPE
