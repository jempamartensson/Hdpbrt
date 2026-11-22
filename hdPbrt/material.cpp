// SPDX-License-Identifier: Apache-2.0
//
// Maps UsdPreviewSurface, MaterialX standard_surface and OpenPBR networks to
// pbrt-v4 materials. pbrt has no "uber" material, so the mapping picks one:
//   metallic >= 0.5          -> conductor (reflectance = base color)
//   transmission >= 0.5      -> dielectric (eta = ior)
//   otherwise                -> coateddiffuse (reflectance = base color)
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
    if (m.type != "dielectric") {
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
        std::string encoding = "sRGB";
        VtValue cs = _Param(*up, "sourceColorSpace");
        if (!isColor || (cs.IsHolding<TfToken>() && cs.UncheckedGet<TfToken>() == "raw"))
            encoding = "linear";
        std::string name = prefix + "_" + input + std::to_string(texCount++);
        std::ostringstream os;
        os << "Texture " << HdPbrt_Quote(name) << (isColor ? " \"spectrum\"" : " \"float\"")
           << " \"imagemap\" \"string filename\" " << HdPbrt_Quote(file)
           << " \"string encoding\" " << HdPbrt_Quote(encoding);
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

            // Gather the generic inputs for the supported surface types.
            GfVec3f base(0.18f), emission(0.f);
            float metallic = 0, rough = 0.5f, transmission = 0, ior = 1.5f, opacity = 1;
            const char *baseIn = nullptr, *roughIn = nullptr, *opacityIn = nullptr;
            if (type == "UsdPreviewSurface") {
                base = _Color(*surf, "diffuseColor", GfVec3f(0.18f));
                metallic = _Float(*surf, "metallic", 0);
                rough = _Float(*surf, "roughness", 0.5f);
                ior = _Float(*surf, "ior", 1.5f);
                opacity = _Float(*surf, "opacity", 1);
                emission = _Color(*surf, "emissiveColor", GfVec3f(0));
                baseIn = "diffuseColor"; roughIn = "roughness"; opacityIn = "opacity";
            } else if (TfStringStartsWith(type, "ND_standard_surface")) {
                base = _Color(*surf, "base_color", GfVec3f(0.8f)) * _Float(*surf, "base", 1);
                metallic = _Float(*surf, "metalness", 0);
                rough = _Float(*surf, "specular_roughness", 0.2f);
                transmission = _Float(*surf, "transmission", 0);
                ior = _Float(*surf, "specular_IOR", 1.5f);
                opacity = _Float(*surf, "opacity", 1);
                emission = _Color(*surf, "emission_color", GfVec3f(1)) * _Float(*surf, "emission", 0);
                baseIn = "base_color"; roughIn = "specular_roughness"; opacityIn = "opacity";
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
            if (metallic >= 0.5f) {
                d.type = "conductor";
                roughParam("roughness");
            } else if (transmission >= 0.5f) {
                d.type = "dielectric";
                other << " \"float eta\" " << ior;
                roughParam("roughness");
            } else {
                d.type = "coateddiffuse";
                roughParam("roughness");
            }
            d.otherParams = other.str();
            result = d;
        }
    }

    {
        std::lock_guard<std::mutex> l(_mutex);
        _desc = result;
    }
    *dirtyBits = HdMaterial::Clean;
}

PXR_NAMESPACE_CLOSE_SCOPE
