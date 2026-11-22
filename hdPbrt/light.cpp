// SPDX-License-Identifier: Apache-2.0
//
// UsdLux lights -> pbrt-v4 lights. Area lights (sphere, rect, disk, cylinder)
// become emissive shapes with a black diffuse material; distant -> "distant";
// dome -> "infinite" (lat-long textures are converted to pbrt's equal-area
// square layout with imgtool).
#include "hdPbrt.h"

#include "pxr/imaging/hd/sceneDelegate.h"
#include "pxr/usd/sdf/assetPath.h"
#include "pxr/base/gf/math.h"
#include "pxr/base/gf/vec3d.h"
#include "pxr/base/tf/stringUtils.h"

#include <cmath>

PXR_NAMESPACE_OPEN_SCOPE

namespace {

template <typename T>
T _Get(HdSceneDelegate *sd, SdfPath const &id, TfToken const &name, T def)
{
    VtValue v = sd->GetLightParamValue(id, name);
    if (v.IsHolding<T>())
        return v.UncheckedGet<T>();
    if constexpr (std::is_same_v<T, float>) {
        if (v.IsHolding<double>()) return (float)v.UncheckedGet<double>();
        if (v.IsHolding<int>()) return (float)v.UncheckedGet<int>();
    }
    return def;
}

// Approximate linear-sRGB color of a black body, normalized to max 1.
GfVec3f _Blackbody(float kelvin)
{
    double t = std::min(std::max(kelvin, 1000.f), 40000.f) / 100.0;
    double r, g, b;
    if (t <= 66) {
        r = 255;
        g = 99.4708025861 * std::log(t) - 161.1195681661;
        b = t <= 19 ? 0 : 138.5177312231 * std::log(t - 10) - 305.0447927307;
    } else {
        r = 329.698727446 * std::pow(t - 60, -0.1332047592);
        g = 288.1221695283 * std::pow(t - 60, -0.0755148492);
        b = 255;
    }
    auto lin = [](double c) {
        c = std::min(std::max(c / 255.0, 0.0), 1.0);
        return float(c <= 0.04045 ? c / 12.92 : std::pow((c + 0.055) / 1.055, 2.4));
    };
    GfVec3f c(lin(r), lin(g), lin(b));
    float m = std::max(c[0], std::max(c[1], c[2]));
    return m > 0 ? c / m : GfVec3f(1);
}

// Area scale factor of the light's transform in its local XY plane / overall.
double _ScaleXY(const GfMatrix4d &m)
{
    GfVec3d x = m.TransformDir(GfVec3d(1, 0, 0)), y = m.TransformDir(GfVec3d(0, 1, 0));
    return GfCross(x, y).GetLength();
}

double _ScaleUniform2(const GfMatrix4d &m)
{
    GfVec3d x = m.TransformDir(GfVec3d(1, 0, 0)), y = m.TransformDir(GfVec3d(0, 1, 0)),
            z = m.TransformDir(GfVec3d(0, 0, 1));
    double s = (x.GetLength() + y.GetLength() + z.GetLength()) / 3.0;
    return s * s;
}

}  // namespace

HdPbrtLight::HdPbrtLight(SdfPath const &id, TfToken const &lightType,
                         HdPbrtRenderDelegate *delegate)
    : HdLight(id), _delegate(delegate), _type(lightType)
{
    _delegate->Register(this);
}

HdPbrtLight::~HdPbrtLight() { _delegate->Unregister(this); }

void HdPbrtLight::Finalize(HdRenderParam *renderParam)
{
    static_cast<HdPbrtRenderParam *>(renderParam)->Edit();
}

void HdPbrtLight::Write(std::ostream &os) const
{
    std::lock_guard<std::mutex> l(_mutex);
    os << _text;
}

void HdPbrtLight::Sync(HdSceneDelegate *sd, HdRenderParam *renderParam, HdDirtyBits *dirtyBits)
{
    static_cast<HdPbrtRenderParam *>(renderParam)->Edit();
    SdfPath const &id = GetId();
    *dirtyBits = Clean;

    std::ostringstream os;
    // Skip the built-in "simple lights" HdxTaskController adds (camera
    // headlight with RenderMan-scale intensity 15000).
    const bool builtin =
        TfStringContains(sd->GetLightParamValue(id, HdLightTokens->params).GetTypeName(),
                         "GlfSimpleLight");
    if (builtin || !sd->GetVisible(id)) {
        std::lock_guard<std::mutex> l(_mutex);
        _text.clear();
        return;
    }

    const GfMatrix4d xf = sd->GetTransform(id);
    float intensity = _Get<float>(sd, id, HdLightTokens->intensity, 1.f);
    float exposure = _Get<float>(sd, id, HdLightTokens->exposure, 0.f);
    GfVec3f color = _Get<GfVec3f>(sd, id, HdLightTokens->color, GfVec3f(1));
    if (_Get<bool>(sd, id, HdLightTokens->enableColorTemperature, false))
        color = GfCompMult(color, _Blackbody(_Get<float>(sd, id, HdLightTokens->colorTemperature,
                                                         6500.f)));
    const bool normalize = _Get<bool>(sd, id, HdLightTokens->normalize, false);
    const float scale = intensity * std::exp2(exposure);
    GfVec3f L = color * scale;

    // Light shaping (UsdLuxShapingAPI), written only for authored values: pbrt
    // patch 8 multiplies the emission of area and point lights by it.
    std::ostringstream sh;
    {
        auto has = [&](TfToken const &t) {
            return !sd->GetLightParamValue(id, t).IsEmpty();
        };
        if (has(HdLightTokens->shapingFocus)) {
            const float focus = _Get<float>(sd, id, HdLightTokens->shapingFocus, 0.f);
            if (focus > 0) {
                sh << " \"float shapingfocus\" " << focus;
                const GfVec3f tint =
                    _Get<GfVec3f>(sd, id, HdLightTokens->shapingFocusTint, GfVec3f(0));
                if (tint != GfVec3f(0))
                    sh << " \"rgb shapingfocustint\" " << HdPbrt_Rgb(tint);
            }
        }
        if (has(HdLightTokens->shapingConeAngle)) {
            const float angle = _Get<float>(sd, id, HdLightTokens->shapingConeAngle, 180.f);
            if (angle < 180.f) {
                sh << " \"float shapingconeangle\" " << angle;
                const float soft =
                    _Get<float>(sd, id, HdLightTokens->shapingConeSoftness, 0.f);
                if (soft > 0)
                    sh << " \"float shapingconesoftness\" " << soft;
            }
        }
        VtValue iv = sd->GetLightParamValue(id, HdLightTokens->shapingIesFile);
        if (iv.IsHolding<SdfAssetPath>()) {
            const SdfAssetPath &a = iv.UncheckedGet<SdfAssetPath>();
            const std::string ies =
                a.GetResolvedPath().empty() ? a.GetAssetPath() : a.GetResolvedPath();
            if (!ies.empty()) {
                const std::string table = _delegate->GetIESTable(
                    ies, _Get<float>(sd, id, HdLightTokens->shapingIesAngleScale, 0.f),
                    _Get<bool>(sd, id, HdLightTokens->shapingIesNormalize, false));
                if (!table.empty())
                    sh << " \"string shapingies\" " << HdPbrt_Quote(table);
            }
        }
    }
    std::ostringstream lk;
    // Light linking (pbrt patch 9): the light-/shadow-link collections, as
    // bits the geometry's "hdpbrtlinks" refer to. Empty: the default (all).
    {
        auto collection = [&](TfToken const &t) {
            VtValue v = sd->GetLightParamValue(id, t);
            return v.IsHolding<TfToken>() ? v.UncheckedGet<TfToken>() : TfToken();
        };
        if (getenv("HDPBRT_DEBUG"))
            fprintf(stderr, "hdPbrt light %s: lightLink %s (%s) shadowLink %s (%s)\n", id.GetText(),
                    collection(HdTokens->lightLink).GetText(),
                    sd->GetLightParamValue(id, HdTokens->lightLink).GetTypeName().c_str(),
                    collection(HdTokens->shadowLink).GetText(),
                    sd->GetLightParamValue(id, HdTokens->shadowLink).GetTypeName().c_str());
        const int lb = _delegate->LinkBit(collection(HdTokens->lightLink));
        const int sb = _delegate->LinkBit(collection(HdTokens->shadowLink));
        if (lb >= 0)
            lk << " \"integer hdpbrtlightlink\" " << lb;
        if (sb >= 0)
            lk << " \"integer hdpbrtshadowlink\" " << sb;
    }
    const std::string links = lk.str();
    const std::string shaping = sh.str() + links;

    os << "# light " << id.GetString() << "\n";
    auto areaHeader = [&](double area) {
        GfVec3f Le = L;
        if (normalize && area > 0)
            Le /= float(area);
        os << "AttributeBegin\n  Transform " << HdPbrt_Matrix(xf) << "\n"
           << "  Material \"diffuse\" \"rgb reflectance\" [0 0 0]\n"
           << "  AreaLightSource \"diffuse\" \"rgb L\" " << HdPbrt_Rgb(Le)
           << (_type == HdPrimTypeTokens->cylinderLight ? links : shaping) << "\n";
    };

    if (_type == HdPrimTypeTokens->distantLight) {
        os << "AttributeBegin\n  Transform " << HdPbrt_Matrix(xf) << "\n"
           << "  LightSource \"distant\" \"point3 from\" [0 0 0] \"point3 to\" [0 0 -1]"
           << " \"rgb L\" " << HdPbrt_Rgb(L) << links << "\nAttributeEnd\n";
    } else if (_type == HdPrimTypeTokens->sphereLight) {
        float r = _Get<float>(sd, id, HdLightTokens->radius, 0.5f);
        bool point = _Get<bool>(sd, id, TfToken("treatAsPoint"), false) || r <= 0;
        if (point) {
            // Point light intensity from a sphere of radius r (or a unit point).
            float rr = r > 0 ? r : 1.f;
            GfVec3f I = normalize ? L / float(4 * M_PI) : L * float(M_PI * rr * rr);
            os << "AttributeBegin\n  Transform " << HdPbrt_Matrix(xf) << "\n"
               << "  LightSource \"point\" \"rgb I\" " << HdPbrt_Rgb(I) << shaping
               << "\nAttributeEnd\n";
        } else {
            areaHeader(4 * M_PI * r * r * _ScaleUniform2(xf));
            os << "  Shape \"sphere\" \"float radius\" " << r << "\nAttributeEnd\n";
        }
    } else if (_type == HdPrimTypeTokens->rectLight) {
        float w = _Get<float>(sd, id, HdLightTokens->width, 1.f) * 0.5f;
        float h = _Get<float>(sd, id, HdLightTokens->height, 1.f) * 0.5f;
        areaHeader(4 * w * h * _ScaleXY(xf));
        // Emits toward -Z: the normals point that way.
        os << "  Shape \"trianglemesh\" \"point3 P\" [" << -w << " " << -h << " 0 " << w << " "
           << -h << " 0 " << w << " " << h << " 0 " << -w << " " << h << " 0]"
           << " \"integer indices\" [0 1 2 0 2 3] \"normal N\" [0 0 -1 0 0 -1 0 0 -1 0 0 -1]"
           << " \"point2 uv\" [0 0 1 0 1 1 0 1]\nAttributeEnd\n";
    } else if (_type == HdPrimTypeTokens->diskLight) {
        float r = _Get<float>(sd, id, HdLightTokens->radius, 0.5f);
        areaHeader(M_PI * r * r * _ScaleXY(xf));
        os << "  ReverseOrientation\n  Shape \"disk\" \"float radius\" " << r
           << "\nAttributeEnd\n";
    } else if (_type == HdPrimTypeTokens->cylinderLight) {
        float r = _Get<float>(sd, id, HdLightTokens->radius, 0.5f);
        float len = _Get<float>(sd, id, HdLightTokens->length, 1.f);
        areaHeader(2 * M_PI * r * len * _ScaleUniform2(xf));
        os << "  Rotate 90 0 1 0\n  Shape \"cylinder\" \"float radius\" " << r
           << " \"float zmin\" " << -0.5f * len << " \"float zmax\" " << 0.5f * len
           << "\nAttributeEnd\n";
    } else if (_type == HdPrimTypeTokens->domeLight) {
        std::string tex;
        VtValue tv = sd->GetLightParamValue(id, HdLightTokens->textureFile);
        if (tv.IsHolding<SdfAssetPath>()) {
            const SdfAssetPath &a = tv.UncheckedGet<SdfAssetPath>();
            tex = a.GetResolvedPath().empty() ? a.GetAssetPath() : a.GetResolvedPath();
        }
        std::string eq = tex.empty() ? std::string() : _delegate->GetEquiAreaTexture(tex);
        // pbrt's environment frame is +Z up with phi = atan2(y, x); USD's dome
        // is +Y up with the lat-long image's center toward +Z and u growing
        // toward -X (OpenEXR convention). d_usd = (p.y, p.z, -p.x).
        os << "AttributeBegin\n  Transform " << HdPbrt_Matrix(xf) << "\n"
           << "  ConcatTransform [0 0 -1 0 1 0 0 0 0 1 0 0 0 0 0 1]\n";
        if (!eq.empty()) {
            float lum = 0.2126f * color[0] + 0.7152f * color[1] + 0.0722f * color[2];
            os << "  LightSource \"infinite\" \"string filename\" " << HdPbrt_Quote(eq)
               << " \"float scale\" " << scale * lum << links << "\n";
        } else {
            if (!tex.empty())
                TF_WARN("hdPbrt: could not use dome texture %s", tex.c_str());
            os << "  LightSource \"infinite\" \"rgb L\" " << HdPbrt_Rgb(L) << links << "\n";
        }
        os << "AttributeEnd\n";
    } else {
        os << "# unsupported light type " << _type.GetString() << "\n";
    }

    std::lock_guard<std::mutex> l(_mutex);
    _text = os.str();
}

PXR_NAMESPACE_CLOSE_SCOPE
