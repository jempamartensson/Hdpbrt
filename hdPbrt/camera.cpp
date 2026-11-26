// SPDX-License-Identifier: Apache-2.0
// Camera sprim that also keeps transform samples for camera motion blur.
#include "hdPbrt.h"

#include "pxr/imaging/hd/sceneDelegate.h"
#include "pxr/base/gf/vec2d.h"

#include <sstream>

PXR_NAMESPACE_OPEN_SCOPE

HdPbrtCamera::HdPbrtCamera(SdfPath const &id) : HdCamera(id) {}

void HdPbrtCamera::Sync(HdSceneDelegate *sceneDelegate, HdRenderParam *renderParam,
                        HdDirtyBits *dirtyBits)
{
    if ((*dirtyBits & DirtyTransform) && !GetId().IsEmpty()) {
        HdTimeSampleArray<GfMatrix4d, 4> sa;
        HdPbrt_SampleXform(
            [&](float a, float b, HdTimeSampleArray<GfMatrix4d, 4> *s) {
                sceneDelegate->SampleTransform(GetId(), a, b, s);
            },
            &sa);
        std::lock_guard<std::mutex> l(_mutex);
        _xform = sa;
    }
    if ((*dirtyBits & DirtyParams) && !GetId().IsEmpty()) {
        // pbrt:sensor "bands": the image's R, G and B are three Gaussian
        // spectral responses (pbrt:sensorRed/Green/Blue = center, FWHM in nm,
        // 360-1100), written raw: false-color infrared (the defaults: R 850,
        // G 650, B 550 nm), monochrome infrared (three equal bands), ...
        // "cie1931" (default) is the usual color camera.
        auto get = [&](const char *name) {
            return sceneDelegate->GetCameraParamValue(GetId(), TfToken(name));
        };
        VtValue m = get("pbrt:sensor");
        const std::string mode = m.IsHolding<TfToken>()       ? m.UncheckedGet<TfToken>().GetString()
                                 : m.IsHolding<std::string>() ? m.UncheckedGet<std::string>()
                                                              : std::string("cie1931");
        std::string sensor;
        if (mode == "bands") {
            auto band = [&](const char *name, GfVec2f def) {
                VtValue v = get(name);
                if (v.IsHolding<GfVec2f>()) return v.UncheckedGet<GfVec2f>();
                if (v.IsHolding<GfVec2d>()) return GfVec2f(v.UncheckedGet<GfVec2d>());
                return def;
            };
            const GfVec2f r = band("pbrt:sensorRed", GfVec2f(850, 100));
            const GfVec2f g = band("pbrt:sensorGreen", GfVec2f(650, 80));
            const GfVec2f b = band("pbrt:sensorBlue", GfVec2f(550, 80));
            std::ostringstream os;
            os << " \"string sensor\" \"hdpbrtbands\" \"float hdpbrtsensorbands\" [" << r[0]
               << " " << r[1] << " " << g[0] << " " << g[1] << " " << b[0] << " " << b[1] << "]";
            sensor = os.str();
        } else if (mode != "cie1931" && !mode.empty()) {
            TF_WARN("hdPbrt: %s: pbrt:sensor \"%s\" unknown (cie1931, bands)",
                    GetId().GetText(), mode.c_str());
        }
        std::lock_guard<std::mutex> l(_mutex);
        _sensor = sensor;
    }
    HdCamera::Sync(sceneDelegate, renderParam, dirtyBits);
}

std::string HdPbrtCamera::GetSensorParams() const
{
    std::lock_guard<std::mutex> l(_mutex);
    return _sensor;
}

HdTimeSampleArray<GfMatrix4d, 4> HdPbrtCamera::GetTransformSamples() const
{
    std::lock_guard<std::mutex> l(_mutex);
    return _xform;
}

PXR_NAMESPACE_CLOSE_SCOPE
