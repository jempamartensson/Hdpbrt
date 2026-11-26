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
#include "pxr/base/gf/color.h"
#include "pxr/base/gf/colorSpace.h"
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
    if constexpr (std::is_same_v<T, TfToken>) {
        if (v.IsHolding<std::string>()) return TfToken(v.UncheckedGet<std::string>());
    }
    return def;
}

// pbrt's spectrum for a band light (pbrt:spectrum "band"): a Gaussian of the
// given center and FWHM in nanometers, on pbrt's 360-1100 nm range.
std::string _BandSpectrum(float center, float fwhm)
{
    const float sigma = std::max(fwhm, 1.f) / 2.35482f;
    const float lo = std::max(360.f, center - 4 * sigma), hi = std::min(1100.f, center + 4 * sigma);
    std::ostringstream os;
    os << "[";
    const float step = std::max(0.5f, std::min(5.f, sigma / 4));
    if (lo > 360.f)
        os << " " << lo - step << " 0";
    for (float l = lo; l <= hi + 1e-3f; l += step)
        os << " " << l << " " << std::exp(-(l - center) * (l - center) / (2 * sigma * sigma));
    if (hi < 1100.f)
        os << " " << hi + step << " 0";
    os << " ]";
    return os.str();
}

// Color of a black body in linear Rec.709 (converted to the working space by
// the caller), normalized to luminance 1, as USD's UsdLuxBlackbodyTemperatureAsRgb and its
// reference renderer (Typhoon) compute it.
GfVec3f _Blackbody(float kelvin)
{
    static const GfColorSpace rec709(GfColorSpaceNames->LinearRec709);
    GfColor c(rec709);
    c.SetFromPlanckianLocus(kelvin, 1.0f);
    const GfVec3f rgb = c.GetRGB();
    const float lum = GfDot(rgb, GfVec3f(0.2126f, 0.7152f, 0.0722f));
    return lum > 0.f ? rgb / lum : GfVec3f(1.f);
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
    const HdPbrtWorkingSpace ws = _delegate->GetWorkingSpace();
    float intensity = _Get<float>(sd, id, HdLightTokens->intensity, 1.f);
    float exposure = _Get<float>(sd, id, HdLightTokens->exposure, 0.f);
    GfVec3f color = _Get<GfVec3f>(sd, id, HdLightTokens->color, GfVec3f(1));
    // The color is in the working space; a color temperature is computed in
    // Rec.709 and converted (Karma does the same under an ACES config).
    if (_Get<bool>(sd, id, HdLightTokens->enableColorTemperature, false))
        color = GfCompMult(color, ws.FromRec709(_Blackbody(_Get<float>(
                                      sd, id, HdLightTokens->colorTemperature, 6500.f))));
    const bool normalize = _Get<bool>(sd, id, HdLightTokens->normalize, false);
    const float scale = intensity * std::exp2(exposure);
    // pbrt:spectrum: "rgb" (color, as UsdLux), "band" (a Gaussian at
    // pbrt:wavelength with FWHM pbrt:bandwidth, nm, carrying the radiant power
    // of a white light of the same intensity: it can be invisible infrared or
    // ultraviolet) or "blackbody" (pbrt's spectral black body at
    // inputs:colorTemperature, luminance as a white light, infrared included).
    // The spectral modes ignore color and color temperature; textured lights
    // stay RGB.
    const TfToken spectrumMode =
        _Get<TfToken>(sd, id, TfToken("pbrt:spectrum"), TfToken("rgb"));
    const bool bandLight = spectrumMode == "band", blackbodyLight = spectrumMode == "blackbody";
    if (bandLight || blackbodyLight)
        color = GfVec3f(1);
    else if (spectrumMode != "rgb")
        TF_WARN("hdPbrt: %s: pbrt:spectrum \"%s\" unknown (rgb, band, blackbody)", id.GetText(),
                spectrumMode.GetText());
    // A light's emission parameter (L or I) for the color times scale v.
    auto emit = [&](const char *name, GfVec3f const &v) {
        std::ostringstream e;
        if (bandLight)
            e << " \"spectrum " << name << "\" "
              << _BandSpectrum(_Get<float>(sd, id, TfToken("pbrt:wavelength"), 850.f),
                               _Get<float>(sd, id, TfToken("pbrt:bandwidth"), 40.f))
              << " \"bool hdpbrtpowernormalize\" true \"float scale\" " << v[0];
        else if (blackbodyLight)
            e << " \"blackbody " << name << "\" "
              << _Get<float>(sd, id, HdLightTokens->colorTemperature, 6500.f)
              << " \"float scale\" " << v[0];
        else
            e << " \"rgb " << name << "\" " << HdPbrt_Rgb(v);
        return e.str();
    };
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
    // Area lights: the emitting shape has pbrt's "interface" material, so it
    // emits but doesn't block or shadow anything, and camera rays only see it
    // with visibleInPrimaryRay (as in USD's reference renderer, Typhoon).
    const bool cameraVisible =
        _Get<bool>(sd, id, TfToken("visibleInPrimaryRay"), false);
    // A rect light's texture (inputs:texture:file) is the emission image,
    // tinted by color.
    std::string emissionImage, emissionColorSpace;
    if (_type == HdPrimTypeTokens->rectLight) {
        VtValue tv = sd->GetLightParamValue(id, HdLightTokens->textureFile);
        if (tv.IsHolding<SdfAssetPath>()) {
            const SdfAssetPath &a = tv.UncheckedGet<SdfAssetPath>();
            emissionImage = a.GetResolvedPath().empty() ? a.GetAssetPath() : a.GetResolvedPath();
            if (TfStringStartsWith(emissionImage, "op:")) {  // Houdini COP node
                emissionImage = _delegate->GetHoudiniImage(emissionImage);
                emissionColorSpace = ws.pbrtName;  // COPs hold working-space values
            }
        }
    }
    auto areaHeader = [&](double area) {
        float s = scale;
        if (normalize && area > 0)
            s /= float(area);
        os << "AttributeBegin\n  Transform " << HdPbrt_Matrix(xf) << "\n"
           << "  Material \"interface\"\n  AreaLightSource \"diffuse\"";
        if (!emissionImage.empty())
            os << " \"string filename\" " << HdPbrt_Quote(emissionImage)
               << (emissionColorSpace.empty()
                       ? std::string()
                       : " \"string colorspace\" " + HdPbrt_Quote(emissionColorSpace))
               << " \"float scale\" " << s << " \"rgb hdpbrttint\" " << HdPbrt_Rgb(color);
        else
            os << emit("L", color * s);
        if (!cameraVisible)
            os << " \"bool hdpbrtcamera\" false";
        // The cylinder is written rotated (pbrt's runs along z, USD's along x):
        // map its light frame back to USD's for the shaping.
        if (_type == HdPrimTypeTokens->cylinderLight && !sh.str().empty())
            os << " \"float shapingframe\" [0 0 1 0 1 0 -1 0 0]";
        os << shaping << "\n";
    };

    if (_type == HdPrimTypeTokens->distantLight) {
        os << "AttributeBegin\n  Transform " << HdPbrt_Matrix(xf) << "\n"
           << "  LightSource \"distant\" \"point3 from\" [0 0 0] \"point3 to\" [0 0 -1]";
        // USD: L is the radiance (nits) of a disc of angular diameter
        // `angle`; normalize divides it by pi sin^2(half angle) (or
        // (2 - sin^2) pi beyond a hemisphere), so intensity becomes the
        // illuminance in lux. pbrt patch 10 gives the light that cone.
        const float angle = std::min(std::max(_Get<float>(sd, id, HdLightTokens->angle, 0.53f),
                                              0.f), 360.f);
        const double thetaMax = std::min(0.5 * angle * M_PI / 180.0, M_PI);
        GfVec3f Ld = L;
        if (normalize && thetaMax > 0) {
            const double s2 = std::pow(std::sin(thetaMax), 2);
            Ld /= float(thetaMax <= M_PI / 2 ? s2 * M_PI : (2 - s2) * M_PI);
        }
        os << emit("L", Ld);
        if (angle > 0)
            os << " \"float hdpbrtangle\" " << angle;
        os << links << "\nAttributeEnd\n";
    } else if (_type == HdPrimTypeTokens->sphereLight) {
        float r = _Get<float>(sd, id, HdLightTokens->radius, 0.5f);
        bool point = _Get<bool>(sd, id, TfToken("treatAsPoint"), false) || r <= 0;
        if (point) {
            // Point light intensity from a sphere of radius r (or a unit point).
            float rr = r > 0 ? r : 1.f;
            // Same power as the sphere: intensity = luminance * pi r^2, and
            // with normalize the luminance is divided by the area 4 pi r^2.
            GfVec3f I = normalize ? L / 4.f : L * float(M_PI * rr * rr);
            os << "AttributeBegin\n  Transform " << HdPbrt_Matrix(xf) << "\n"
               << "  LightSource \"point\"" << emit("I", I) << shaping
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
           // u runs from +x to -x, v from -y to +y (pbrt flips v on lookup):
           // the texture orientation of USD's reference renderer.
           << " \"point2 uv\" [1 0 0 0 0 1 1 1]\nAttributeEnd\n";
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
        std::string eq = tex.empty()                     ? std::string()
                         : TfStringStartsWith(tex, "op:") ? _delegate->GetHoudiniEquiArea(tex)
                                                          : _delegate->GetEquiAreaTexture(tex);
        // pbrt's environment frame is +Z up with phi = atan2(y, x); USD's dome
        // is +Y up with the lat-long image's center toward +Z and u growing
        // toward -X (OpenEXR convention). d_usd = (p.y, p.z, -p.x).
        os << "AttributeBegin\n  Transform " << HdPbrt_Matrix(xf) << "\n"
           << "  ConcatTransform [0 0 -1 0 1 0 0 0 0 1 0 0 0 0 0 1]\n";
        if (!eq.empty()) {
            const float lum = ws.Luminance(color);
            os << "  LightSource \"infinite\" \"string filename\" " << HdPbrt_Quote(eq);
            if (TfStringStartsWith(tex, "op:"))  // COPs hold working-space values
                os << " \"string colorspace\" " << HdPbrt_Quote(ws.pbrtName);
            os << " \"float scale\" " << scale * lum << links << "\n";
        } else {
            if (!tex.empty())
                TF_WARN("hdPbrt: could not use dome texture %s", tex.c_str());
            os << "  LightSource \"infinite\"" << emit("L", L) << links << "\n";
        }
        os << "AttributeEnd\n";
    } else {
        os << "# unsupported light type " << _type.GetString() << "\n";
    }

    std::lock_guard<std::mutex> l(_mutex);
    _text = os.str();
}

PXR_NAMESPACE_CLOSE_SCOPE
