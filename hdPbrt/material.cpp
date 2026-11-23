// SPDX-License-Identifier: Apache-2.0
//
// Maps UsdPreviewSurface, MaterialX standard_surface and OpenPBR networks to
// pbrt-v4 materials. pbrt has no "uber" material, so the mapping picks one:
//   metallic >= 0.5          -> conductor (reflectance = base color)
//   transmission >= 0.5      -> dielectric (eta = ior)
//   no specular lobe         -> diffuse (specular weight/color 0, or ior 1)
//   otherwise                -> hdpbrtlayered (pbrt patch 12: GGX dielectric
//                               specular over diffuse, MaterialX-style layering)
// Base color and roughness can come from image textures (UsdUVTexture,
// MaterialX image/tiledimage). Opacity becomes the shape's alpha (cut-out).
#include "hdPbrt.h"

#include "pxr/imaging/hd/sceneDelegate.h"
#include "pxr/usd/sdf/assetPath.h"
#include "pxr/base/gf/vec4f.h"
#include "pxr/base/tf/stringUtils.h"
#include "pxr/base/tf/pathUtils.h"

#include <algorithm>
#include <cmath>

PXR_NAMESPACE_OPEN_SCOPE

HdPbrtMaterialDesc HdPbrt_DefaultMaterial()
{
    HdPbrtMaterialDesc d;
    d.type = "diffuse";
    d.baseColorFromPrimvar = true;
    return d;
}

std::string HdPbrt_MaterialLine(const HdPbrtMaterialDesc &m, const GfVec3f *displayColor)
{
    std::ostringstream os;
    os << "Material " << HdPbrt_Quote(m.type);
    if (m.type != "dielectric" && m.writeReflectance) {
        if (!m.baseColorTexture.empty())
            os << " \"texture reflectance\" " << HdPbrt_Quote(m.baseColorTexture);
        else {
            GfVec3f c = (m.baseColorFromPrimvar && displayColor) ? *displayColor : m.baseColor;
            for (int i = 0; i < 3; ++i)
                c[i] = std::min(std::max(c[i], 0.f), 1.f);
            os << " \"rgb reflectance\" " << HdPbrt_Rgb(c);
        }
    }
    os << m.otherParams << "\n";
    return os.str();
}

namespace {

struct _Net {
    const HdMaterialNetwork2 &net;

    const HdMaterialNode2 *Node(const SdfPath &p) const {
        auto it = net.nodes.find(p);
        return it == net.nodes.end() ? nullptr : &it->second;
    }
    // The node connected to input `name` of `node`, if any.
    const HdMaterialNode2 *Upstream(const HdMaterialNode2 &node, const TfToken &name,
                                    TfToken *output = nullptr, SdfPath *path = nullptr) const {
        auto it = node.inputConnections.find(name);
        if (it == node.inputConnections.end() || it->second.empty())
            return nullptr;
        if (output)
            *output = it->second[0].upstreamOutputName;
        if (path)
            *path = it->second[0].upstreamNode;
        return Node(it->second[0].upstreamNode);
    }
};

VtValue _Param(const HdMaterialNode2 &n, const char *name)
{
    auto it = n.parameters.find(TfToken(name));
    return it == n.parameters.end() ? VtValue() : it->second;
}

float _Float(const HdMaterialNode2 &n, const char *name, float def)
{
    VtValue v = _Param(n, name);
    if (v.IsHolding<float>()) return v.UncheckedGet<float>();
    if (v.IsHolding<double>()) return (float)v.UncheckedGet<double>();
    if (v.IsHolding<int>()) return (float)v.UncheckedGet<int>();
    if (v.IsHolding<bool>()) return v.UncheckedGet<bool>() ? 1.f : 0.f;
    if (v.IsHolding<GfVec3f>()) {
        GfVec3f c = v.UncheckedGet<GfVec3f>();
        return (c[0] + c[1] + c[2]) / 3.f;
    }
    return def;
}

GfVec3f _Color(const HdMaterialNode2 &n, const char *name, GfVec3f def)
{
    VtValue v = _Param(n, name);
    if (v.IsHolding<GfVec3f>()) return v.UncheckedGet<GfVec3f>();
    if (v.IsHolding<GfVec3d>()) return GfVec3f(v.UncheckedGet<GfVec3d>());
    if (v.IsHolding<GfVec4f>()) {
        GfVec4f c = v.UncheckedGet<GfVec4f>();
        return GfVec3f(c[0], c[1], c[2]);
    }
    if (v.IsHolding<float>()) return GfVec3f(v.UncheckedGet<float>());
    return def;
}

std::string _AssetPath(const VtValue &v)
{
    if (v.IsHolding<SdfAssetPath>()) {
        const SdfAssetPath &a = v.UncheckedGet<SdfAssetPath>();
        return a.GetResolvedPath().empty() ? a.GetAssetPath() : a.GetResolvedPath();
    }
    if (v.IsHolding<std::string>()) return v.UncheckedGet<std::string>();
    return {};
}

bool _IsTextureNode(const HdMaterialNode2 &n)
{
    const std::string &t = n.nodeTypeId.GetString();
    return t == "UsdUVTexture" || TfStringStartsWith(t, "ND_image_") ||
           TfStringStartsWith(t, "ND_tiledimage_") || TfStringStartsWith(t, "ND_gltf_image");
}

bool _IsPrimvarNode(const HdMaterialNode2 &n)
{
    const std::string &t = n.nodeTypeId.GetString();
    return TfStringStartsWith(t, "UsdPrimvarReader") ||
           TfStringStartsWith(t, "ND_geompropvalue") || TfStringStartsWith(t, "ND_geomcolor");
}

bool _SupportedImage(const std::string &path)
{
    std::string ext = TfStringToLower(TfGetExtension(path));
    static const char *ok[] = {"exr", "png", "pfm", "hdr", "qoi", "jpg", "jpeg", "tga", "bmp"};
    if (path.find('<') != std::string::npos)  // UDIM or other tokens
        return false;
    for (const char *e : ok)
        if (ext == e)
            return true;
    return false;
}

struct _Builder {
    _Net net;
    std::string prefix;  // unique texture name prefix
    HdPbrtMaterialDesc desc;
    int texCount = 0;

    // Follow `input` on `node`: returns a named pbrt texture if it's driven by an
    // image, sets *fromPrimvar if it reads displayColor-like primvars.
    std::string Texture(const HdMaterialNode2 &node, const char *input, bool isColor,
                        bool *fromPrimvar = nullptr) {
        TfToken output;
        const HdMaterialNode2 *up = net.Upstream(node, TfToken(input), &output);
        // Look through a single convert/dot pass-through node.
        if (up && !_IsTextureNode(*up) && !_IsPrimvarNode(*up) &&
            (TfStringStartsWith(up->nodeTypeId.GetString(), "ND_convert") ||
             TfStringStartsWith(up->nodeTypeId.GetString(), "ND_dot"))) {
            up = net.Upstream(*up, TfToken("in"), &output);
        }
        if (!up)
            return {};
        if (_IsPrimvarNode(*up)) {
            if (fromPrimvar)
                *fromPrimvar = true;
            return {};
        }
        if (!_IsTextureNode(*up))
            return {};
        std::string file = _AssetPath(_Param(*up, "file"));
        if (file.empty() || !_SupportedImage(file))
            return {};
        // UsdUVTexture: sourceColorSpace "auto" decodes 8-bit images as sRGB.
        // MaterialX image nodes: a file is in the document's (linear) color
        // space unless its colorspace says otherwise (Hydra passes it as
        // "colorSpace:file"), as USD's reference renderer reads them.
        std::string encoding = "sRGB";
        if (TfStringStartsWith(up->nodeTypeId.GetString(), "ND_")) {
            VtValue mcs = _Param(*up, "colorSpace:file");
            std::string name = mcs.IsHolding<TfToken>() ? mcs.UncheckedGet<TfToken>().GetString()
                               : mcs.IsHolding<std::string>() ? mcs.UncheckedGet<std::string>()
                                                               : std::string();
            name = TfStringToLower(name);
            encoding = (name.find("srgb") != std::string::npos && name.find("lin") != 0)
                           ? "sRGB" : "linear";
        } else {
            VtValue cs = _Param(*up, "sourceColorSpace");
            if (cs.IsHolding<TfToken>() && cs.UncheckedGet<TfToken>() == "raw")
                encoding = "linear";
        }
        if (!isColor)
            encoding = "linear";
        std::string name = prefix + "_" + input + std::to_string(texCount++);
        std::ostringstream os;
        os << "Texture " << HdPbrt_Quote(name) << (isColor ? " \"spectrum\"" : " \"float\"")
           << " \"imagemap\" \"string filename\" " << HdPbrt_Quote(file)
           << " \"string encoding\" " << HdPbrt_Quote(encoding);
        // MaterialX tiledimage: uvtiling (often fed by a convert node from a
        // float) scales the texture coordinates, as pbrt's uscale/vscale.
        if (TfStringStartsWith(up->nodeTypeId.GetString(), "ND_tiledimage_")) {
            GfVec2f tiling(1.f);
            VtValue t = _Param(*up, "uvtiling");
            if (t.IsHolding<GfVec2f>()) {
                tiling = t.UncheckedGet<GfVec2f>();
            } else {
                TfToken o;
                if (const HdMaterialNode2 *c = net.Upstream(*up, TfToken("uvtiling"), &o)) {
                    VtValue in = _Param(*c, "in");
                    VtValue val = _Param(*c, "value");
                    if (in.IsHolding<float>())
                        tiling = GfVec2f(in.UncheckedGet<float>());
                    else if (in.IsHolding<GfVec2f>())
                        tiling = in.UncheckedGet<GfVec2f>();
                    else if (val.IsHolding<GfVec2f>())
                        tiling = val.UncheckedGet<GfVec2f>();
                }
            }
            if (tiling != GfVec2f(1.f))
                os << " \"float uscale\" " << tiling[0] << " \"float vscale\" " << tiling[1];
        }
        VtValue scale = _Param(*up, "scale");
        if (scale.IsHolding<GfVec4f>()) {
            GfVec4f s = scale.UncheckedGet<GfVec4f>();
            float f = isColor ? (s[0] + s[1] + s[2]) / 3.f : s[0];
            if (std::fabs(f - 1.f) > 1e-6f)
                os << " \"float scale\" " << f;
        }
        os << "\n";
        desc.textures += os.str();
        return name;
    }
};

// ---------------------------------------------------------------------------
// MaterialX pbrlib BSDF graphs (ND_surface / ND_surface_unlit). The graph is
// reduced to one pbrt material: layer/mix/add nodes are resolved, each BSDF
// node becomes the closest pbrt material. Unauthored inputs take the pbrlib
// nodedef defaults (Hydra passes authored values only). Roughness on BSDF
// nodes is already GGX alpha.

GfVec2f _Vec2(const HdMaterialNode2 &n, const char *name, GfVec2f def)
{
    VtValue v = _Param(n, name);
    if (v.IsHolding<GfVec2f>()) return v.UncheckedGet<GfVec2f>();
    if (v.IsHolding<GfVec2d>()) return GfVec2f(v.UncheckedGet<GfVec2d>());
    if (v.IsHolding<float>()) return GfVec2f(v.UncheckedGet<float>());
    return def;
}

std::string _String(const HdMaterialNode2 &n, const char *name, const char *def)
{
    VtValue v = _Param(n, name);
    if (v.IsHolding<std::string>()) return v.UncheckedGet<std::string>();
    if (v.IsHolding<TfToken>()) return v.UncheckedGet<TfToken>().GetString();
    return def;
}

bool _Is(const HdMaterialNode2 *n, const char *nodePrefix)
{
    return n && TfStringStartsWith(n->nodeTypeId.GetString(), nodePrefix);
}

// A medium coefficient given in RGB, as a three-band step spectrum
// (blue < 490 nm <= green < 590 nm <= red). pbrt renders spectrally and its
// smooth RGB upsampling leaks absorption into channels whose coefficient is
// 0 (honey's red: 0.39 instead of 1.0 transmittance through 10 units). The
// band edges were fitted to RGB-exact transmittance exp(-sigma d) over a set
// of colors and depths: mean error 0.019 vs 0.041, worst 0.06 vs 0.38.
std::string _MediumSpectrum(const char *name, const GfVec3f &c)
{
    std::ostringstream os;
    os << " \"spectrum " << name << "\" [360 " << c[2] << " 489.9 " << c[2] << " 490 " << c[1]
       << " 589.9 " << c[1] << " 590 " << c[0] << " 830 " << c[0] << "]";
    return os.str();
}

GfVec3f _Clamp01(GfVec3f c)
{
    for (int i = 0; i < 3; ++i)
        c[i] = std::min(std::max(c[i], 0.f), 1.f);
    return c;
}

struct _MtlxMapper {
    _Builder &b;
    const HdMaterialNode2 *vdf = nullptr;  // volume under a layer_vdf

    // layer_vdf (a BSDF with a volume below it): the BSDF part; the volume
    // becomes the interior medium of a transmissive result.
    const HdMaterialNode2 *ThroughVdf(const HdMaterialNode2 *n) {
        while (_Is(n, "ND_layer_vdf")) {
            if (!vdf)
                vdf = b.net.Upstream(*n, TfToken("base"));
            n = b.net.Upstream(*n, TfToken("top"));
        }
        return n;
    }

    static bool IsDiffuse(const HdMaterialNode2 *n) {
        return _Is(n, "ND_oren_nayar_diffuse_bsdf") || _Is(n, "ND_burley_diffuse_bsdf") ||
               _Is(n, "ND_subsurface_bsdf");
    }

    static bool IsDielectric(const HdMaterialNode2 *n, bool *reflects, bool *transmits) {
        if (!_Is(n, "ND_dielectric_bsdf"))
            return false;
        const std::string mode = _String(*n, "scatter_mode", "R");
        *reflects = mode.find('R') != std::string::npos;
        *transmits = mode.find('T') != std::string::npos;
        return true;
    }

    void Rough(std::ostringstream &os, const HdMaterialNode2 &n) {
        GfVec2f a = _Vec2(n, "roughness", GfVec2f(0.05f));
        os << " \"float uroughness\" " << std::max(a[0], 0.f) << " \"float vroughness\" "
           << std::max(a[1], 0.f) << " \"bool remaproughness\" false";
    }

    // Diffuse-like base color (color * weight); a texture on "color" wins.
    void DiffuseBase(const HdMaterialNode2 &n, HdPbrtMaterialDesc &d) {
        d.baseColor = _Color(n, "color", GfVec3f(0.18f)) * _Float(n, "weight", 1);
        bool fromPrimvar = false;
        d.baseColorTexture = b.Texture(n, "color", true, &fromPrimvar);
        d.baseColorFromPrimvar = fromPrimvar;
    }

    // Dielectric reflection over a diffuse base (or over black).
    void Layered(const HdMaterialNode2 &spec, const HdMaterialNode2 *base,
                 HdPbrtMaterialDesc &d) {
        std::ostringstream os;
        d.type = "hdpbrtlayered";
        if (base)
            DiffuseBase(*base, d);
        else
            d.baseColor = GfVec3f(0);
        Rough(os, spec);
        os << " \"float eta\" " << _Float(spec, "ior", 1.5f);
        const float w = std::min(std::max(_Float(spec, "weight", 1), 0.f), 1.f);
        if (w != 1.f)
            os << " \"float specularweight\" " << w;
        GfVec3f tint = _Clamp01(_Color(spec, "tint", GfVec3f(1)));
        if (tint != GfVec3f(1))
            os << " \"rgb speculartint\" " << HdPbrt_Rgb(tint);
        d.otherParams = os.str();
    }

    // Sheen over a diffuse base (or over nothing): hdpbrtlayered without
    // specular. pbrlib's sheen roughness is the Charlie alpha.
    void Sheen(const HdMaterialNode2 &sheen, const HdMaterialNode2 *base,
               HdPbrtMaterialDesc &d) {
        std::ostringstream os;
        d.type = "hdpbrtlayered";
        if (base)
            DiffuseBase(*base, d);
        else
            d.baseColor = GfVec3f(0);
        os << " \"float specularweight\" 0 \"float sheenweight\" "
           << std::min(std::max(_Float(sheen, "weight", 1), 0.f), 1.f)
           << " \"rgb sheencolor\" " << HdPbrt_Rgb(_Clamp01(_Color(sheen, "color", GfVec3f(1))))
           << " \"float sheenroughness\" "
           << std::min(std::max(_Float(sheen, "roughness", 0.3f), 0.f), 1.f)
           << " \"bool sheenontop\" true";
        d.otherParams = os.str();
    }

    void Map(const HdMaterialNode2 *n, HdPbrtMaterialDesc &d) {
        std::ostringstream os;
        n = ThroughVdf(n);
        bool refl = false, trans = false;
        if (!n) {
            d.type = "diffuse";
            d.baseColor = GfVec3f(0);
        } else if (_Is(n, "ND_layer_bsdf")) {
            const HdMaterialNode2 *top = ThroughVdf(b.net.Upstream(*n, TfToken("top")));
            const HdMaterialNode2 *base = ThroughVdf(b.net.Upstream(*n, TfToken("base")));
            bool br = false, bt = false;
            if (IsDielectric(top, &refl, &trans) && refl) {
                if (!base || IsDiffuse(base)) {
                    Layered(*top, base, d);
                } else if (IsDielectric(base, &br, &bt) && bt) {
                    // Reflection on top of transmission: one glass interface.
                    // MaterialX layering also scales the transmission by the
                    // top's 1 - w E_R(wo), on top of the T lobe's own
                    // Fresnel (as Typhoon does): pbrt patch 17.
                    d.type = "dielectric";
                    Rough(os, *top);
                    os << " \"float eta\" " << _Float(*top, "ior", 1.5f) << " \"float hdpbrttopweight\" "
                       << std::min(std::max(_Float(*top, "weight", 1), 0.f), 1.f);
                    const GfVec3f tint =
                        _Clamp01(_Color(*base, "tint", GfVec3f(1)) * _Float(*base, "weight", 1));
                    if (tint != GfVec3f(1))
                        os << " \"rgb transmissiontint\" " << HdPbrt_Rgb(tint);
                    d.otherParams = os.str();
                } else if (_Is(base, "ND_conductor_bsdf")) {
                    d.type = "coatedconductor";
                    d.writeReflectance = false;
                    GfVec2f ia = _Vec2(*top, "roughness", GfVec2f(0.05f));
                    GfVec2f ca = _Vec2(*base, "roughness", GfVec2f(0.05f));
                    os << " \"float interface.uroughness\" " << ia[0]
                       << " \"float interface.vroughness\" " << ia[1]
                       << " \"float interface.eta\" " << _Float(*top, "ior", 1.5f)
                       << " \"float conductor.uroughness\" " << ca[0]
                       << " \"float conductor.vroughness\" " << ca[1]
                       << " \"rgb conductor.eta\" "
                       << HdPbrt_Rgb(_Color(*base, "ior", GfVec3f(0.183f, 0.421f, 1.373f)))
                       << " \"rgb conductor.k\" "
                       << HdPbrt_Rgb(_Color(*base, "extinction", GfVec3f(3.424f, 2.346f, 1.770f)))
                       << " \"bool remaproughness\" false";
                    d.otherParams = os.str();
                } else {
                    Map(base, d);
                }
            } else if (_Is(top, "ND_sheen_bsdf") && (!base || IsDiffuse(base))) {
                Sheen(*top, base, d);
            } else if (_Is(top, "ND_sheen_bsdf")) {
                Map(base, d);  // sheen over anything else: not translated
            } else {
                Map(top ? top : base, d);
            }
        } else if (_Is(n, "ND_mix_bsdf")) {
            const float mix = _Float(*n, "mix", 0);
            const HdMaterialNode2 *fg = b.net.Upstream(*n, TfToken("fg"));
            const HdMaterialNode2 *bg = b.net.Upstream(*n, TfToken("bg"));
            Map(mix >= 0.5f ? (fg ? fg : bg) : (bg ? bg : fg), d);
        } else if (_Is(n, "ND_add_bsdf")) {
            const HdMaterialNode2 *a = b.net.Upstream(*n, TfToken("in1"));
            Map(a ? a : b.net.Upstream(*n, TfToken("in2")), d);
        } else if (_Is(n, "ND_sheen_bsdf")) {
            Sheen(*n, nullptr, d);
        } else if (IsDiffuse(n)) {
            d.type = "diffuse";
            DiffuseBase(*n, d);
        } else if (_Is(n, "ND_translucent_bsdf")) {
            d.type = "diffusetransmission";
            d.baseColor = GfVec3f(0);
            os << " \"rgb transmittance\" "
               << HdPbrt_Rgb(_Clamp01(_Color(*n, "color", GfVec3f(1)) * _Float(*n, "weight", 1)));
            d.otherParams = os.str();
        } else if (_Is(n, "ND_conductor_bsdf")) {
            d.type = "conductor";
            d.writeReflectance = false;
            Rough(os, *n);
            os << " \"rgb eta\" " << HdPbrt_Rgb(_Color(*n, "ior", GfVec3f(0.183f, 0.421f, 1.373f)))
               << " \"rgb k\" " << HdPbrt_Rgb(_Color(*n, "extinction", GfVec3f(3.424f, 2.346f, 1.770f)));
            d.otherParams = os.str();
        } else if (_Is(n, "ND_generalized_schlick_bsdf")) {
            // A conductor whose normal-incidence reflectance is color0.
            d.type = "conductor";
            d.baseColor = _Color(*n, "color0", GfVec3f(1)) * _Float(*n, "weight", 1);
            Rough(os, *n);
            d.otherParams = os.str();
        } else if (IsDielectric(n, &refl, &trans)) {
            if (trans) {
                d.type = "dielectric";
                Rough(os, *n);
                os << " \"float eta\" " << _Float(*n, "ior", 1.5f);
                d.otherParams = os.str();
            } else {
                Layered(*n, nullptr, d);
            }
        } else {
            TF_WARN("hdPbrt: unsupported MaterialX BSDF node %s, using diffuse",
                    n->nodeTypeId.GetText());
            d.type = "diffuse";
            d.baseColor = GfVec3f(0.18f);
        }
    }

    // Emission of an EDF graph: uniform_edf color; generalized_schlick_edf
    // scales its base by its cosine-weighted hemispherical average,
    // color0 + (color90 - color0) * 2 / ((e + 1)(e + 2)).
    GfVec3f Edf(const HdMaterialNode2 *n) {
        if (_Is(n, "ND_uniform_edf"))
            return _Color(*n, "color", GfVec3f(1));
        if (_Is(n, "ND_generalized_schlick_edf")) {
            const GfVec3f c0 = _Color(*n, "color0", GfVec3f(1));
            const GfVec3f c90 = _Color(*n, "color90", GfVec3f(1));
            const float e = std::max(_Float(*n, "exponent", 5), 0.f);
            const GfVec3f avg = c0 + (c90 - c0) * (2.f / ((e + 1) * (e + 2)));
            const GfVec3f base = Edf(b.net.Upstream(*n, TfToken("base")));
            return GfVec3f(avg[0] * base[0], avg[1] * base[1], avg[2] * base[2]);
        }
        if (_Is(n, "ND_add_edf"))
            return Edf(b.net.Upstream(*n, TfToken("in1"))) + Edf(b.net.Upstream(*n, TfToken("in2")));
        return GfVec3f(0);
    }

    // Returns false when `surf` isn't an ND_surface / ND_surface_unlit.
    bool MapSurface(const HdMaterialNode2 &surf, HdPbrtMaterialDesc &d) {
        const std::string &type = surf.nodeTypeId.GetString();
        if (TfStringStartsWith(type, "ND_surface_unlit")) {
            d.type = "diffuse";
            d.baseColor = GfVec3f(0);
            d.emission = _Color(surf, "emission_color", GfVec3f(1)) * _Float(surf, "emission", 1);
            d.opacity = _Float(surf, "opacity", 1);
            return true;
        }
        if (type != "ND_surface")
            return false;
        Map(b.net.Upstream(surf, TfToken("bsdf")), d);
        // absorption_vdf / anisotropic_vdf: coefficients per unit length.
        if (d.type == "dielectric" &&
            (_Is(vdf, "ND_absorption_vdf") || _Is(vdf, "ND_anisotropic_vdf"))) {
            GfVec3f sigA = _Color(*vdf, "absorption", GfVec3f(0));
            GfVec3f sigS = _Is(vdf, "ND_anisotropic_vdf") ? _Color(*vdf, "scattering", GfVec3f(0))
                                                          : GfVec3f(0);
            const float g = _Is(vdf, "ND_anisotropic_vdf") ? _Float(*vdf, "anisotropy", 0) : 0;
            d.mediumName = b.prefix + "_interior";
            std::ostringstream m;
            m << "MakeNamedMedium " << HdPbrt_Quote(d.mediumName)
              << " \"string type\" \"homogeneous\"" << _MediumSpectrum("sigma_a", sigA)
              << _MediumSpectrum("sigma_s", sigS) << " \"float scale\" 1 \"float g\" "
              << std::min(std::max(g, -0.99f), 0.99f) << "\n";
            d.textures += m.str();
        }
        d.emission = Edf(b.net.Upstream(surf, TfToken("edf")));
        d.opacity = _Float(surf, "opacity", 1);
        d.opacityTexture = b.Texture(surf, "opacity", false);
        return true;
    }
};

}  // namespace

HdPbrtMaterial::HdPbrtMaterial(SdfPath const &id) : HdMaterial(id) {}

void HdPbrtMaterial::Sync(HdSceneDelegate *sceneDelegate, HdRenderParam *renderParam,
                          HdDirtyBits *dirtyBits)
{
    static_cast<HdPbrtRenderParam *>(renderParam)->Edit();
    HdPbrtMaterialDesc result = HdPbrt_DefaultMaterial();

    VtValue res = sceneDelegate->GetMaterialResource(GetId());
    if (res.IsHolding<HdMaterialNetworkMap>()) {
        bool isVolume = false;
        HdMaterialNetwork2 net2 =
            HdConvertToHdMaterialNetwork2(res.UncheckedGet<HdMaterialNetworkMap>(), &isVolume);
        auto term = net2.terminals.find(HdMaterialTerminalTokens->surface);
        const HdMaterialNode2 *surf = nullptr;
        if (term != net2.terminals.end()) {
            auto it = net2.nodes.find(term->second.upstreamNode);
            if (it != net2.nodes.end())
                surf = &it->second;
        }
        // Houdini/MaterialX networks may put a ND_surfacematerial in front.
        if (surf && TfStringStartsWith(surf->nodeTypeId.GetString(), "ND_surfacematerial")) {
            _Net n{net2};
            surf = n.Upstream(*surf, TfToken("surfaceshader"));
        }
        if (surf) {
            std::string pfx = TfMakeValidIdentifier(GetId().GetString());
            _Builder b{_Net{net2}, pfx, HdPbrtMaterialDesc{}, 0};
            const std::string type = surf->nodeTypeId.GetString();
            // MaterialX BSDF-node graphs (ND_surface) go through their own mapper.
            const bool mtlxGraph = _MtlxMapper{b}.MapSurface(*surf, b.desc);
            if (mtlxGraph)
                result = b.desc;
            else {

            // Gather the generic inputs for the supported surface types.
            GfVec3f base(0.18f), emission(0.f);
            float metallic = 0, rough = 0.5f, transmission = 0, ior = 1.5f, opacity = 1;
            float specWeight = 1;
            GfVec3f specTint(1.f);
            // Transmission: tint, interior medium (depth > 0), extra roughness.
            GfVec3f transColor(1.f), transScatter(0.f);
            float transDepth = 0, transAniso = 0, transExtraRough = 0;
            bool tintWithMedium = true;  // standard_surface tints the BTDF too
            // Sheen (standard_surface sheen: under the specular layer; OpenPBR
            // fuzz: on top) and thin walls (straight transmission).
            float sheenW = 0, sheenRough = 0.3f;
            GfVec3f sheenColor(1.f);
            bool sheenOnTop = false, thinWalled = false;
            // Coat: a GGX dielectric layer on top; coatTint (coat color and
            // OpenPBR darkening) multiplies everything below it.
            float coatW = 0, coatRough = 0, coatIor = 1.5f;
            GfVec3f coatTint(1.f);
            // Metal inside hdpbrtlayered (patch 18), mixed by metalness:
            // OpenPBR F82-tint, standard_surface artistic IOR. Other shaders
            // keep pbrt's conductor.
            bool layeredMetal = false, metalF82 = false;
            GfVec3f metalBase(0.8f), metalEdge(1.f);
            // No specular lobe at all (specular weight or color 0, or ior 1,
            // i.e. no Fresnel reflection): a plain Lambertian "diffuse".
            bool noSpecular = false;
            const char *baseIn = nullptr, *roughIn = nullptr, *opacityIn = nullptr;
            if (type == "UsdPreviewSurface") {
                base = _Color(*surf, "diffuseColor", GfVec3f(0.18f));
                metallic = _Float(*surf, "metallic", 0);
                rough = _Float(*surf, "roughness", 0.5f);
                ior = _Float(*surf, "ior", 1.5f);
                opacity = _Float(*surf, "opacity", 1);
                emission = _Color(*surf, "emissiveColor", GfVec3f(0));
                baseIn = "diffuseColor"; roughIn = "roughness"; opacityIn = "opacity";
                const bool specWorkflow = _Float(*surf, "useSpecularWorkflow", 0) > 0.5f;
                noSpecular = (specWorkflow &&
                              _Color(*surf, "specularColor", GfVec3f(0)) == GfVec3f(0)) ||
                             (!specWorkflow && std::fabs(ior - 1.f) < 1e-4f);
                if (specWorkflow && !noSpecular) {
                    // Specular workflow: specularColor is F0. Use the eta with
                    // that mean F0 and keep the hue as a tint.
                    GfVec3f f0 = _Color(*surf, "specularColor", GfVec3f(0));
                    float m = std::min(std::max((f0[0] + f0[1] + f0[2]) / 3.f, 1e-4f), 0.99f);
                    float s = std::sqrt(m);
                    ior = (1 + s) / (1 - s);
                    specTint = f0 / m;
                }
            } else if (TfStringStartsWith(type, "ND_standard_surface")) {
                base = _Color(*surf, "base_color", GfVec3f(0.8f)) * _Float(*surf, "base", 1);
                metallic = _Float(*surf, "metalness", 0);
                rough = _Float(*surf, "specular_roughness", 0.2f);
                transmission = _Float(*surf, "transmission", 0);
                ior = _Float(*surf, "specular_IOR", 1.5f);
                opacity = _Float(*surf, "opacity", 1);
                emission = _Color(*surf, "emission_color", GfVec3f(1)) * _Float(*surf, "emission", 0);
                baseIn = "base_color"; roughIn = "specular_roughness"; opacityIn = "opacity";
                // No subsurface scattering yet: its weight blends the diffuse
                // albedo toward subsurface_color, which is what it converges to.
                {
                    const float w = std::min(std::max(_Float(*surf, "subsurface", 0), 0.f), 1.f);
                    if (w > 0) {
                        base = base * (1 - w) + _Color(*surf, "subsurface_color", GfVec3f(1)) * w;
                        if (w >= 0.5f)
                            baseIn = "subsurface_color";
                    }
                }
                specWeight = _Float(*surf, "specular", 1);
                specTint = _Color(*surf, "specular_color", GfVec3f(1));
                transColor = _Color(*surf, "transmission_color", GfVec3f(1));
                transDepth = _Float(*surf, "transmission_depth", 0);
                transScatter = _Color(*surf, "transmission_scatter", GfVec3f(0));
                transAniso = _Float(*surf, "transmission_scatter_anisotropy", 0);
                transExtraRough = _Float(*surf, "transmission_extra_roughness", 0);
                sheenW = _Float(*surf, "sheen", 0);
                sheenColor = _Color(*surf, "sheen_color", GfVec3f(1));
                sheenRough = _Float(*surf, "sheen_roughness", 0.3f);
                thinWalled = _Float(*surf, "thin_walled", 0) > 0.5f;
                layeredMetal = true;
                metalBase = _Color(*surf, "base_color", GfVec3f(0.8f)) * _Float(*surf, "base", 1);
                metalEdge = _Color(*surf, "specular_color", GfVec3f(1)) * _Float(*surf, "specular", 1);
                coatW = std::min(std::max(_Float(*surf, "coat", 0), 0.f), 1.f);
                if (coatW > 0) {
                    coatRough = _Float(*surf, "coat_roughness", 0.1f);
                    coatIor = _Float(*surf, "coat_IOR", 1.5f);
                    const GfVec3f cc = _Clamp01(_Color(*surf, "coat_color", GfVec3f(1)));
                    coatTint = GfVec3f(1.f) * (1 - coatW) + cc * coatW;
                    // coat_affect_color: base^(1 + coat * affect).
                    const float gamma =
                        1 + coatW * std::min(std::max(_Float(*surf, "coat_affect_color", 0), 0.f), 1.f);
                    for (int i = 0; i < 3; ++i)
                        base[i] = std::pow(std::max(base[i], 0.f), gamma);
                    // coat_affect_roughness pushes the specular toward rough.
                    const float mix = std::min(std::max(_Float(*surf, "coat_affect_roughness", 0), 0.f), 1.f) *
                                      coatW * std::min(std::max(coatRough, 0.f), 1.f);
                    rough = rough * (1 - mix) + mix;
                }
                noSpecular = specWeight <= 0.f && _Float(*surf, "coat", 0) <= 0.f;
            } else if (TfStringStartsWith(type, "ND_open_pbr_surface")) {
                base = _Color(*surf, "base_color", GfVec3f(0.8f)) * _Float(*surf, "base_weight", 1);
                metallic = _Float(*surf, "base_metalness", 0);
                rough = _Float(*surf, "specular_roughness", 0.3f);
                transmission = _Float(*surf, "transmission_weight", 0);
                ior = _Float(*surf, "specular_ior", 1.5f);
                opacity = _Float(*surf, "geometry_opacity", 1);
                emission = _Color(*surf, "emission_color", GfVec3f(1)) *
                           _Float(*surf, "emission_luminance", 0);
                baseIn = "base_color"; roughIn = "specular_roughness"; opacityIn = "geometry_opacity";
                {
                    const float w =
                        std::min(std::max(_Float(*surf, "subsurface_weight", 0), 0.f), 1.f);
                    if (w > 0) {
                        base = base * (1 - w) +
                               _Color(*surf, "subsurface_color", GfVec3f(0.8f)) * w;
                        if (w >= 0.5f)
                            baseIn = "subsurface_color";
                    }
                }
                specWeight = _Float(*surf, "specular_weight", 1);
                specTint = _Color(*surf, "specular_color", GfVec3f(1));
                transColor = _Color(*surf, "transmission_color", GfVec3f(1));
                transDepth = _Float(*surf, "transmission_depth", 0);
                transScatter = _Color(*surf, "transmission_scatter", GfVec3f(0));
                transAniso = _Float(*surf, "transmission_scatter_anisotropy", 0);
                tintWithMedium = false;  // OpenPBR: the medium carries the color
                sheenW = _Float(*surf, "fuzz_weight", 0);
                sheenColor = _Color(*surf, "fuzz_color", GfVec3f(1));
                sheenRough = _Float(*surf, "fuzz_roughness", 0.5f);
                sheenOnTop = true;
                thinWalled = _Float(*surf, "geometry_thin_walled", 0) > 0.5f;
                layeredMetal = metalF82 = true;
                metalBase = _Color(*surf, "base_color", GfVec3f(0.8f)) * _Float(*surf, "base_weight", 1);
                metalEdge = _Color(*surf, "specular_color", GfVec3f(1));
                coatW = std::min(std::max(_Float(*surf, "coat_weight", 0), 0.f), 1.f);
                if (coatW > 0) {
                    // OpenPBR spec: the coat roughens the specular below,
                    // r' = (r^4 + 2 c^4)^(1/4), and the specular's IOR becomes
                    // relative to the coat's, both blended by the coat weight.
                    coatRough = _Float(*surf, "coat_roughness", 0);
                    coatIor = std::max(_Float(*surf, "coat_ior", 1.6f), 1.f);
                    const float r4 = std::pow(std::max(rough, 0.f), 4.f);
                    const float c4 = std::pow(std::max(coatRough, 0.f), 4.f);
                    rough = rough * (1 - coatW) + std::pow(std::min(1.f, 2 * c4 + r4), 0.25f) * coatW;
                    const float iorS = std::max(ior, 1.f);
                    ior = iorS * (1 - coatW) + std::max(iorS / coatIor, coatIor / iorS) * coatW;
                    // Darkening from light trapped under the coat (internal
                    // reflection K of the coat, base albedo E):
                    // (1 - K) / (1 - E K), blended by weight * coat_darkening.
                    const float f0 = std::pow((coatIor - 1) / (coatIor + 1), 2.f);
                    const float K = 1 - (1 - f0) / (coatIor * coatIor);
                    const float dk = coatW * std::min(std::max(_Float(*surf, "coat_darkening", 1), 0.f), 1.f);
                    const GfVec3f cc = _Clamp01(_Color(*surf, "coat_color", GfVec3f(1)));
                    for (int i = 0; i < 3; ++i) {
                        const float e = std::min(std::max(base[i], 0.f), 1.f);
                        const float dark = std::min((1 - K) / std::max(1 - e * K, 1e-5f), 1.f);
                        coatTint[i] = (1 - dk + dark * dk) * (1 - coatW + cc[i] * coatW);
                    }
                }
                noSpecular = specWeight <= 0.f && _Float(*surf, "coat_weight", 0) <= 0.f;
            } else {
                // Unknown shader: look for common names, else grey diffuse.
                base = _Color(*surf, "base_color", _Color(*surf, "diffuseColor", GfVec3f(0.5f)));
                TF_WARN("hdPbrt: unsupported surface shader %s on %s, using diffuse",
                        type.c_str(), GetId().GetText());
            }

            bool fromPrimvar = false;
            std::string baseTex = baseIn ? b.Texture(*surf, baseIn, true, &fromPrimvar) : "";
            std::string roughTex = roughIn ? b.Texture(*surf, roughIn, false) : "";
            std::string opacityTex = opacityIn ? b.Texture(*surf, opacityIn, false) : "";

            HdPbrtMaterialDesc &d = b.desc;
            d.baseColor = base;
            d.baseColorTexture = baseTex;
            d.baseColorFromPrimvar = fromPrimvar;
            d.emission = emission;
            d.opacity = std::min(std::max(opacity, 0.f), 1.f);
            d.opacityTexture = opacityTex;
            std::ostringstream other;
            auto roughParam = [&](const char *name) {
                // USD/MaterialX roughness r means GGX alpha = r^2. pbrt's
                // "remaproughness" uses alpha = sqrt(r), so pass alpha
                // directly for constants. (Textured roughness keeps pbrt's
                // remap, which is too rough for glossy values.)
                if (!roughTex.empty())
                    other << " \"texture " << name << "\" " << HdPbrt_Quote(roughTex);
                else {
                    float r = std::min(std::max(rough, 0.f), 1.f);
                    other << " \"float " << name << "\" " << r * r
                          << " \"bool remaproughness\" false";
                }
            };
            if (layeredMetal && metallic > 0 && (metallic >= 0.5f || transmission < 0.5f)) {
                d.type = "hdpbrtlayered";
                roughParam("roughness");
                other << " \"float eta\" " << ior;
                if (noSpecular || metallic >= 1.f)
                    specWeight = 0;
                if (std::fabs(specWeight - 1.f) > 1e-6f)
                    other << " \"float specularweight\" " << std::min(std::max(specWeight, 0.f), 1.f);
                if (specTint != GfVec3f(1.f))
                    other << " \"rgb speculartint\" " << HdPbrt_Rgb(_Clamp01(specTint));
                other << " \"float metalweight\" " << std::min(metallic, 1.f);
                if (metalF82 || !baseTex.empty()) {
                    // F0 = base color (its texture if any), F82 tint = edge.
                    if (!baseTex.empty())
                        other << " \"texture metalf0\" " << HdPbrt_Quote(baseTex);
                    else
                        other << " \"rgb metalf0\" " << HdPbrt_Rgb(_Clamp01(metalBase));
                    other << " \"rgb metalf82\" " << HdPbrt_Rgb(_Clamp01(metalEdge));
                } else {
                    // Artistic IOR (Gulbrandsen 2014): n between
                    // (1+sqrt r)/(1-sqrt r) and (1-r)/(1+r) by edge tint g,
                    // k from r and n.
                    GfVec3f n, k;
                    for (int i = 0; i < 3; ++i) {
                        const float r = std::min(std::max(metalBase[i], 0.f), 0.99f);
                        const float g = std::min(std::max(metalEdge[i], 0.f), 1.f);
                        const float sr = std::sqrt(r);
                        const float nMin = (1 - r) / (1 + r), nMax = (1 + sr) / (1 - sr);
                        n[i] = nMax * (1 - g) + nMin * g;
                        const float k2 = ((n[i] + 1) * (n[i] + 1) * r - (n[i] - 1) * (n[i] - 1)) /
                                         std::max(1 - r, 1e-6f);
                        k[i] = std::sqrt(std::max(k2, 0.f));
                    }
                    other << " \"rgb metaleta\" " << HdPbrt_Rgb(n) << " \"rgb metalk\" " << HdPbrt_Rgb(k);
                }
                if (sheenW > 0) {
                    other << " \"float sheenweight\" " << std::min(sheenW, 1.f)
                          << " \"rgb sheencolor\" " << HdPbrt_Rgb(_Clamp01(sheenColor))
                          << " \"float sheenroughness\" " << std::min(std::max(sheenRough, 0.f), 1.f);
                    if (sheenOnTop)
                        other << " \"bool sheenontop\" true";
                }
                if (coatW > 0)
                    other << " \"float coatweight\" " << coatW << " \"float coatroughness\" "
                          << std::pow(std::min(std::max(coatRough, 0.f), 1.f), 2.f)
                          << " \"float coateta\" " << coatIor
                          << " \"rgb coattint\" " << HdPbrt_Rgb(_Clamp01(coatTint));
            } else if (metallic >= 0.5f) {
                d.type = "conductor";
                roughParam("roughness");
            } else if (transmission >= 0.5f && !thinWalled) {
                d.type = "dielectric";
                other << " \"float eta\" " << ior;
                rough = std::min(std::max(rough + transExtraRough, 0.f), 1.f);
                roughParam("roughness");
                // Interior medium, MaterialX's conversion: extinction
                // -ln(color) / depth, scattering scatter / depth, absorption
                // the difference shifted so that no channel is negative.
                const bool medium = transDepth > 1e-6f;
                if (medium) {
                    GfVec3f sigS, sigA;
                    float minA = 1e30f;
                    for (int i = 0; i < 3; ++i) {
                        const float c = std::min(std::max(transColor[i], 1e-6f), 1.f);
                        sigS[i] = std::max(transScatter[i], 0.f) / transDepth;
                        sigA[i] = -std::log(c) / transDepth - sigS[i];
                        minA = std::min(minA, sigA[i]);
                    }
                    if (minA < 0)
                        sigA -= GfVec3f(minA);
                    d.mediumName = pfx + "_interior";
                    std::ostringstream m;
                    m << "MakeNamedMedium " << HdPbrt_Quote(d.mediumName)
                      << " \"string type\" \"homogeneous\"" << _MediumSpectrum("sigma_a", sigA)
                      << _MediumSpectrum("sigma_s", sigS) << " \"float scale\" 1"
                      << " \"float g\" " << std::min(std::max(transAniso, -0.99f), 0.99f) << "\n";
                    d.textures += m.str();
                }
                if ((!medium || tintWithMedium) && transColor != GfVec3f(1.f))
                    other << " \"rgb transmissiontint\" " << HdPbrt_Rgb(_Clamp01(transColor));
            } else if (noSpecular && sheenW <= 0 && coatW <= 0 && !(thinWalled && transmission > 0)) {
                d.type = "diffuse";
            } else {
                d.type = "hdpbrtlayered";
                roughParam("roughness");
                other << " \"float eta\" " << ior;
                if (noSpecular)
                    specWeight = 0;
                if (coatW > 0)
                    other << " \"float coatweight\" " << coatW << " \"float coatroughness\" "
                          << std::pow(std::min(std::max(coatRough, 0.f), 1.f), 2.f)
                          << " \"float coateta\" " << coatIor
                          << " \"rgb coattint\" " << HdPbrt_Rgb(_Clamp01(coatTint));
                if (sheenW > 0) {
                    other << " \"float sheenweight\" " << std::min(sheenW, 1.f)
                          << " \"rgb sheencolor\" " << HdPbrt_Rgb(_Clamp01(sheenColor))
                          << " \"float sheenroughness\" " << std::min(std::max(sheenRough, 0.f), 1.f);
                    if (sheenOnTop)
                        other << " \"bool sheenontop\" true";
                }
                // Thin wall: transmission passes straight through (eta 1),
                // under the specular layer; the diffuse base takes the rest.
                if (thinWalled && transmission > 0) {
                    const float t = std::min(transmission, 1.f);
                    d.baseColor *= 1 - t;
                    other << " \"rgb transmittance\" " << HdPbrt_Rgb(_Clamp01(transColor * t));
                }
                if (std::fabs(specWeight - 1.f) > 1e-6f)
                    other << " \"float specularweight\" " << std::min(std::max(specWeight, 0.f), 1.f);
                if (specTint != GfVec3f(1.f)) {
                    GfVec3f t = specTint;
                    for (int i = 0; i < 3; ++i)
                        t[i] = std::min(std::max(t[i], 0.f), 1.f);
                    other << " \"rgb speculartint\" " << HdPbrt_Rgb(t);
                }
            }
            d.otherParams = other.str();
            result = d;
            }  // !mtlxGraph
        }
    }

    {
        std::lock_guard<std::mutex> l(_mutex);
        _desc = result;
    }
    *dirtyBits = HdMaterial::Clean;
}

PXR_NAMESPACE_CLOSE_SCOPE
