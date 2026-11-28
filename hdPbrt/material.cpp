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
#include <functional>
#include <set>

PXR_NAMESPACE_OPEN_SCOPE

HdPbrtMaterialDesc HdPbrt_DefaultMaterial()
{
    HdPbrtMaterialDesc d;
    d.type = "diffuse";
    d.baseColorFromPrimvar = true;
    return d;
}

const char *const HdPbrt_DisplayColorTexture = "hdpbrt_displayColor";

std::string HdPbrt_MaterialLine(const HdPbrtMaterialDesc &m, const GfVec3f *displayColor,
                                float irReflectance, const std::string &displayColorTexture)
{
    std::ostringstream os;
    os << "Material " << HdPbrt_Quote(m.type);
    if (m.type != "dielectric" && m.writeReflectance) {
        if (!m.baseColorTexture.empty())
            os << " \"texture reflectance\" " << HdPbrt_Quote(m.baseColorTexture);
        else if (m.baseColorFromPrimvar && !displayColorTexture.empty())
            os << " \"texture reflectance\" " << HdPbrt_Quote(displayColorTexture);
        else {
            GfVec3f c = (m.baseColorFromPrimvar && displayColor) ? *displayColor : m.baseColor;
            for (int i = 0; i < 3; ++i)
                c[i] = std::min(std::max(c[i], 0.f), 1.f);
            os << " \"rgb reflectance\" " << HdPbrt_Rgb(c);
        }
    }
    os << m.otherParams;
    // Infrared reflectance (pbrt patch 38) on the diffuse base.
    const float ir = irReflectance >= 0 ? irReflectance : m.irReflectance;
    if (ir >= 0 && m.writeReflectance && (m.type == "diffuse" || m.type == "hdpbrtlayered"))
        os << " \"float hdpbrtir\" " << std::min(ir, 1.f) << " \"float hdpbrtiredge\" "
           << m.irEdge;
    os << "\n";
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

// A primvar reader's primvar name, width (from the node type) and default.
// UsdPrimvarReader_*: varname / fallback; ND_geompropvalue_*: geomprop /
// default; ND_geomcolor_*: displayColor.
void _PrimvarReader(const HdMaterialNode2 &n, std::string *name, int *width, GfVec4f *def)
{
    const std::string t = n.nodeTypeId.GetString();
    auto str = [&](const char *p) -> std::string {
        auto it = n.parameters.find(TfToken(p));
        if (it == n.parameters.end()) return {};
        if (it->second.IsHolding<TfToken>()) return it->second.UncheckedGet<TfToken>().GetString();
        if (it->second.IsHolding<std::string>()) return it->second.UncheckedGet<std::string>();
        return {};
    };
    const bool usd = TfStringStartsWith(t, "UsdPrimvarReader");
    *name = TfStringStartsWith(t, "ND_geomcolor") ? std::string("displayColor")
                                                  : str(usd ? "varname" : "geomprop");
    if (name->empty())
        *name = "displayColor";
    const std::string suffix = t.substr(t.rfind('_') + 1);
    *width = (suffix == "float2" || suffix == "vector2") ? 2
             : (suffix == "float4" || suffix == "color4" || suffix == "vector4") ? 4
             : (suffix == "float" || suffix == "int" || suffix == "integer" || suffix == "boolean")
                 ? 1
                 : 3;
    *def = GfVec4f(0.f);
    if (TfStringStartsWith(t, "ND_geomcolor"))
        *def = GfVec4f(0.18f, 0.18f, 0.18f, 1.f);
    auto it = n.parameters.find(TfToken(usd ? "fallback" : "default"));
    if (it != n.parameters.end()) {
        const VtValue &v = it->second;
        if (v.IsHolding<float>()) *def = GfVec4f(v.UncheckedGet<float>());
        else if (v.IsHolding<double>()) *def = GfVec4f(float(v.UncheckedGet<double>()));
        else if (v.IsHolding<int>()) *def = GfVec4f(float(v.UncheckedGet<int>()));
        else if (v.IsHolding<GfVec2f>()) { auto c = v.UncheckedGet<GfVec2f>(); *def = GfVec4f(c[0], c[1], 0, 0); }
        else if (v.IsHolding<GfVec3f>()) { auto c = v.UncheckedGet<GfVec3f>(); *def = GfVec4f(c[0], c[1], c[2], 0); }
        else if (v.IsHolding<GfVec4f>()) *def = v.UncheckedGet<GfVec4f>();
    }
    if (*width == 1)
        *def = GfVec4f((*def)[0]);
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
    HdPbrtRenderDelegate *delegate = nullptr;  // for Houdini op: images
    // Primvar readers become "hdpbrtprimvar" textures (surfaces); off for
    // displacement, whose graphs read primvars per vertex themselves.
    bool primvarTextures = true;
    // Float textures that already give GGX alpha (squared roughness primvars).
    std::set<std::string> alphaTextures;

    // Follow `input` on `node`: returns a named pbrt texture if it's driven by an
    // image, sets *fromPrimvar if it reads displayColor-like primvars.
    // An "hdpbrtprimvar" texture (pbrt): the primvar interpolated over the
    // mesh's triangles, `def` where a shape doesn't have it.
    std::string PrimvarTexture(const std::string &pv, int width, const GfVec4f &def,
                               const char *input, bool isColor, bool alpha = false) {
        std::string name = prefix + "_" + input + std::to_string(texCount++);
        std::ostringstream os;
        os << "Texture " << HdPbrt_Quote(name) << (isColor ? " \"spectrum\"" : " \"float\"")
           << " \"hdpbrtprimvar\" \"string primvar\" " << HdPbrt_Quote(pv);
        if (isColor)
            os << " \"float default\" [" << def[0] << " " << def[1] << " " << def[2] << "]";
        else
            os << " \"float default\" " << def[0];
        if (alpha) {  // roughness r -> GGX alpha r^2 (no pbrt remap)
            os << " \"float power\" 2";
            alphaTextures.insert(name);
        }
        os << "\n";
        desc.textures += os.str();
        auto entry = std::make_pair(pv, width);
        if (std::find(desc.surfacePrimvars.begin(), desc.surfacePrimvars.end(), entry) ==
            desc.surfacePrimvars.end())
            desc.surfacePrimvars.push_back(entry);
        return name;
    }

    // An input's value: a pbrt texture (image or primvar, or a product/mix
    // of those) or a constant (c; floats in c[0]).
    struct Val {
        std::string tex;
        GfVec3f c{0.f};
        bool Varies() const { return !tex.empty(); }
    };
    Val In(const HdMaterialNode2 &n, const char *input, bool isColor, GfVec3f c,
           bool roughness = false) {
        Val v;
        v.c = c;
        if (input)
            v.tex = Texture(n, input, isColor, nullptr, roughness);
        return v;
    }
    static Val Const(GfVec3f c) { Val v; v.c = c; return v; }
    static Val Const(float f) { return Const(GfVec3f(f)); }
    // A value as the operand of a pbrt texture or material parameter `name`.
    static std::string Param(const char *name, const Val &v, bool isColor) {
        std::ostringstream os;
        if (v.Varies())
            os << " \"texture " << name << "\" " << HdPbrt_Quote(v.tex);
        else if (isColor)
            os << " \"rgb " << name << "\" " << HdPbrt_Rgb(v.c);
        else
            os << " \"float " << name << "\" " << v.c[0];
        return os.str();
    }
    // a * s (s a float), as a pbrt "scale" texture when either varies.
    Val Mul(const Val &a, const Val &s, bool isColor, const char *what) {
        if (!a.Varies() && !s.Varies())
            return Const(a.c * s.c[0]);
        if (!s.Varies() && std::fabs(s.c[0] - 1.f) < 1e-6f)
            return a;
        Val r;
        r.tex = prefix + "_" + what + std::to_string(texCount++);
        r.c = a.c * s.c[0];
        desc.textures += "Texture " + HdPbrt_Quote(r.tex) +
                         (isColor ? " \"spectrum\"" : " \"float\"") + " \"scale\"" +
                         Param("tex", a, isColor) + Param("scale", s, false) + "\n";
        return r;
    }
    // lerp(a, b, t) (t a float), as a pbrt "mix" texture when any varies.
    Val Mix(const Val &a, const Val &b, const Val &t, bool isColor, const char *what) {
        if (!a.Varies() && !b.Varies() && !t.Varies())
            return Const(a.c * (1 - t.c[0]) + b.c * t.c[0]);
        Val r;
        r.tex = prefix + "_" + what + std::to_string(texCount++);
        r.c = a.c * (1 - t.c[0]) + b.c * t.c[0];
        desc.textures += "Texture " + HdPbrt_Quote(r.tex) +
                         (isColor ? " \"spectrum\"" : " \"float\"") + " \"mix\"" +
                         Param("tex1", a, isColor) + Param("tex2", b, isColor) +
                         Param("amount", t, false) + "\n";
        return r;
    }

    std::string Texture(const HdMaterialNode2 &node, const char *input, bool isColor,
                        bool *fromPrimvar = nullptr, bool roughness = false) {
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
            std::string pv;
            int width = 1;
            GfVec4f def(0.f);
            _PrimvarReader(*up, &pv, &width, &def);
            // displayColor on the base color: the shape's constant color, or
            // its per-vertex one (HdPbrt_MaterialLine's displayColorTexture).
            if (fromPrimvar && isColor && (pv == "displayColor" || pv == "Cd")) {
                *fromPrimvar = true;
                return {};
            }
            if (!primvarTextures)
                return {};
            return PrimvarTexture(pv, width, def, input, isColor, roughness);
        }
        if (!_IsTextureNode(*up))
            return {};
        std::string file = _AssetPath(_Param(*up, "file"));
        // Houdini op: images (COP nodes) are cooked into a float file; they
        // hold linear values, whatever the node's color space says.
        const bool houdiniImage = TfStringStartsWith(file, "op:");
        if (houdiniImage)
            file = delegate ? delegate->GetHoudiniImage(file) : std::string();
        else if (!_SupportedImage(file))
            file.clear();
        if (file.empty())
            return {};
        // UsdUVTexture: sourceColorSpace "auto" decodes 8-bit images as sRGB.
        // MaterialX image nodes: a file is in the document's (linear) color
        // space unless its colorspace says otherwise (Hydra passes it as
        // "colorSpace:file"), as USD's reference renderer reads them.
        //
        // Primaries ("colorspace"; color textures only): raw data and COP images
        // hold working-space numbers; a named color space (MaterialX or OCIO
        // names) is used as such; otherwise the file's metadata, else sRGB
        // primaries (pbrt's default; Karma and Blender also read unlabeled
        // textures as Rec.709 / sRGB and convert them to the working space).
        std::string encoding = "sRGB", primaries;
        const std::string working = delegate ? delegate->GetWorkingSpace().pbrtName : "srgb";
        if (TfStringStartsWith(up->nodeTypeId.GetString(), "ND_")) {
            VtValue mcs = _Param(*up, "colorSpace:file");
            std::string name = mcs.IsHolding<TfToken>() ? mcs.UncheckedGet<TfToken>().GetString()
                               : mcs.IsHolding<std::string>() ? mcs.UncheckedGet<std::string>()
                                                               : std::string();
            name = TfStringToLower(name);
            encoding = (name.find("srgb") != std::string::npos && name.find("lin") != 0)
                           ? "sRGB" : "linear";
            if (name.find("raw") != std::string::npos || name.find("data") != std::string::npos ||
                name.find("non-color") != std::string::npos)
                primaries = working;
            else if (name.find("acescg") != std::string::npos ||
                     name.find("ap1") != std::string::npos)
                primaries = "acescg";
            else if (name.find("2020") != std::string::npos)
                primaries = "rec2020";
            else if (name.find("srgb") != std::string::npos ||
                     name.find("709") != std::string::npos)
                primaries = "srgb";
        } else {
            VtValue cs = _Param(*up, "sourceColorSpace");
            const TfToken t = cs.IsHolding<TfToken>() ? cs.UncheckedGet<TfToken>() : TfToken();
            if (t == "raw") {
                encoding = "linear";
                primaries = working;
            } else if (t == "sRGB") {
                primaries = "srgb";
            }
        }
        if (!isColor || houdiniImage)
            encoding = "linear";
        if (houdiniImage)
            primaries = working;  // COPs work in the scene-linear space
        std::string name = prefix + "_" + input + std::to_string(texCount++);
        std::ostringstream os;
        os << "Texture " << HdPbrt_Quote(name) << (isColor ? " \"spectrum\"" : " \"float\"")
           << " \"imagemap\" \"string filename\" " << HdPbrt_Quote(file)
           << " \"string encoding\" " << HdPbrt_Quote(encoding);
        if (isColor && !primaries.empty())
            os << " \"string colorspace\" " << HdPbrt_Quote(primaries);
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
// The working space of the material being synced (HdPbrtMaterial::Sync sets
// it; materials may sync in parallel).
thread_local HdPbrtWorkingSpace t_workingSpace;

std::string _MediumSpectrum(const char *name, const GfVec3f &c)
{
    std::ostringstream os;
    // The bands below were fitted to Rec.709 channels; other working spaces
    // use pbrt's own RGB upsampling in that space.
    if (t_workingSpace.aces) {
        os << " \"rgb " << name << "\" " << HdPbrt_Rgb(c);
        return os.str();
    }
    os << " \"spectrum " << name << "\" [360 " << c[2] << " 489.9 " << c[2] << " 490 " << c[1]
       << " 589.9 " << c[1] << " 590 " << c[0] << " 830 " << c[0] << "]";
    return os.str();
}

// Subsurface random walk medium (pbrt patch 23): Chiang et al. 2016's remap
// from the multiple-scattering albedo (diffuse reflectance of the walk) and the
// mean free path to extinction and single-scattering albedo, with the
// anisotropy terms, as Cycles and Typhoon (radius used as the physical mean
// free path, no 1/(4 pi) shim).
static void _ChiangRemap(float albedo, float radius, float g, float *extinction, float *alpha)
{
    const float g2 = g * g, g3 = g2 * g, g4 = g3 * g, g5 = g4 * g, g6 = g5 * g, g7 = g6 * g;
    const float A = 1.8260523782f - 1.28451056436f * g - 1.79904629312f * g2 +
                    9.19393289202f * g3 - 22.8215585862f * g4 + 32.0234874259f * g5 -
                    23.6264803333f * g6 + 7.21067002658f * g7;
    const float B = 4.98511194385f +
                    0.127355959438f * std::exp(31.1491581433f * g - 201.847017512f * g2 +
                                               841.576016723f * g3 - 2018.09288505f * g4 +
                                               2731.71560286f * g5 - 1935.41424244f * g6 +
                                               559.009054474f * g7);
    const float C = 1.09686102424f - 0.394704063468f * g + 1.05258115941f * g2 -
                    8.83963712726f * g3 + 28.8643230661f * g4 - 46.8802913581f * g5 +
                    38.5402837518f * g6 - 12.7181042538f * g7;
    const float D = 0.496310210422f + 0.360146581622f * g - 2.15139309747f * g2 +
                    17.8896899217f * g3 - 55.2984010333f * g4 + 82.065982243f * g5 -
                    58.5106008578f * g6 + 15.8478295021f * g7;
    const float E = 4.23190299701f +
                    0.00310603949088f * std::exp(76.7316253952f * g - 594.356773233f * g2 +
                                                 2448.8834203f * g3 - 5576.68528998f * g4 +
                                                 7116.60171912f * g5 - 4763.54467887f * g6 +
                                                 1303.5318055f * g7);
    const float F = 2.40602999408f - 2.51814844609f * g + 9.18494908356f * g2 -
                    79.2191708682f * g3 + 259.082868209f * g4 - 403.613804597f * g5 +
                    302.85712436f * g6 - 87.4370473567f * g7;
    const float blend = std::pow(albedo, 0.25f);
    const float a = (1 - blend) * A * std::pow(std::atan(B * albedo), C) +
                    blend * D * std::pow(std::atan(E * albedo), F);
    *alpha = std::min(std::max(a, 0.f), 0.999999f);
    *extinction = (1 / std::max(radius, 1e-6f)) / std::max(1 - g, 1e-6f);
}

// "MakeNamedMedium" line for a subsurface interior (color = walk albedo,
// radius = mean free path per channel, in scene units).
std::string HdPbrt_SubsurfaceMedium(const std::string &name, const GfVec3f &color,
                                    const GfVec3f &radius, float anisotropy)
{
    const float g = std::min(std::max(anisotropy, -0.99f), 0.99f);
    GfVec3f sigA, sigS;
    for (int i = 0; i < 3; ++i) {
        float ext, alpha;
        _ChiangRemap(std::min(std::max(color[i], 0.f), 1.f), radius[i], g, &ext, &alpha);
        sigS[i] = ext * alpha;
        sigA[i] = ext - sigS[i];
    }
    std::ostringstream m;
    m << "MakeNamedMedium " << HdPbrt_Quote(name) << " \"string type\" \"homogeneous\""
      << _MediumSpectrum("sigma_a", sigA) << _MediumSpectrum("sigma_s", sigS)
      << " \"float scale\" 1 \"float g\" " << g << " \"bool hdpbrtsss\" true\n";
    return m.str();
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

    // MaterialX BSDF-node thin film (thinfilm_thickness in nm; 0 = none).
    float Film(const HdMaterialNode2 &n) {
        return std::max(_Float(n, "thinfilm_thickness", 0), 0.f);
    }

    // A pbrlib dielectric's thin film on a pbrt dielectric (patch 29).
    void DielectricFilm(std::ostringstream &os, const HdMaterialNode2 &n) {
        if (Film(n) > 0)
            os << " \"float filmweight\" 1 \"float filmthickness\" " << Film(n)
               << " \"float filmeta\" " << std::max(_Float(n, "thinfilm_ior", 1.5f), 1.f);
    }

    // A metal-only hdpbrtlayered: the node's weight and its thin film.
    void MetalWeightAndFilm(std::ostringstream &os, const HdMaterialNode2 &n) {
        const float w = std::max(_Float(n, "weight", 1), 0.f);
        if (std::fabs(w - 1.f) > 1e-6f)
            os << " \"float metalscale\" " << w;
        os << " \"float filmweight\" 1 \"float filmthickness\" " << Film(n)
           << " \"float filmeta\" " << std::max(_Float(n, "thinfilm_ior", 1.5f), 1.f);
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

    // oren_nayar_diffuse_bsdf roughness, as the pbrt parameter `name`
    // (energy-preserving Oren-Nayar, pbrt patch 20; Typhoon uses EON whatever
    // energy_compensation says for the surface shaders, and here too).
    // burley_diffuse_bsdf (pbrt patch 33): Burley's diffuse with that roughness,
    // flagged by "bool hdpbrtburley" (diffuse) or "bool diffuseburley"
    // (hdpbrtlayered). Written even at roughness 0, which isn't Lambert.
    static std::string DiffuseRough(const HdMaterialNode2 *n, const char *name) {
        const bool burley = _Is(n, "ND_burley_diffuse_bsdf");
        if (!burley && !_Is(n, "ND_oren_nayar_diffuse_bsdf"))
            return {};
        const float r = std::min(std::max(_Float(*n, "roughness", 0), 0.f), 1.f);
        if (r <= 0 && !burley)
            return {};
        std::ostringstream os;
        os << " \"float " << name << "\" " << r;
        if (burley)
            os << (std::string(name) == "roughness" ? " \"bool hdpbrtburley\" true"
                                                    : " \"bool diffuseburley\" true");
        return os.str();
    }

    // subsurface_bsdf as hdpbrtlayered's base: a random walk in an interior
    // medium (pbrt patch 23). Returns the parameters to add (or nothing).
    std::string SubsurfaceBase(const HdMaterialNode2 *n, HdPbrtMaterialDesc &d) {
        if (!_Is(n, "ND_subsurface_bsdf"))
            return {};
        const float w = std::min(std::max(_Float(*n, "weight", 1), 0.f), 1.f);
        d.baseColor = GfVec3f(0);
        d.baseColorTexture.clear();
        d.mediumName = b.prefix + "_sss";
        d.textures += HdPbrt_SubsurfaceMedium(d.mediumName, _Color(*n, "color", GfVec3f(0.18f)),
                                              _Color(*n, "radius", GfVec3f(1)),
                                              _Float(*n, "anisotropy", 0));
        std::ostringstream os;
        os << " \"float subsurface\" " << w;
        return os.str();
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
        os << DiffuseRough(base, "diffuseroughness") << SubsurfaceBase(base, d);
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
        os << DiffuseRough(base, "diffuseroughness") << SubsurfaceBase(base, d);
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
                    // Transmission at the base's roughness, reflection at the
                    // top's when they differ.
                    Rough(os, *base);
                    {
                        const GfVec2f ta = _Vec2(*top, "roughness", GfVec2f(0.05f));
                        const GfVec2f ba = _Vec2(*base, "roughness", GfVec2f(0.05f));
                        if (ta != ba)
                            os << " \"float hdpbrtreflectionuroughness\" " << std::max(ta[0], 0.f)
                               << " \"float hdpbrtreflectionvroughness\" " << std::max(ta[1], 0.f);
                    }
                    os << " \"float eta\" " << _Float(*top, "ior", 1.5f) << " \"float hdpbrttopweight\" "
                       << std::min(std::max(_Float(*top, "weight", 1), 0.f), 1.f);
                    DielectricFilm(os, Film(*top) > 0 ? *top : *base);
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
        } else if (_Is(n, "ND_subsurface_bsdf")) {
            d.type = "hdpbrtlayered";
            d.otherParams = " \"float specularweight\" 0" + SubsurfaceBase(n, d);
        } else if (IsDiffuse(n)) {
            d.type = "diffuse";
            DiffuseBase(*n, d);
            d.otherParams = DiffuseRough(n, "roughness");
        } else if (_Is(n, "ND_translucent_bsdf")) {
            d.type = "diffusetransmission";
            d.baseColor = GfVec3f(0);
            os << " \"rgb transmittance\" "
               << HdPbrt_Rgb(_Clamp01(_Color(*n, "color", GfVec3f(1)) * _Float(*n, "weight", 1)));
            d.otherParams = os.str();
        } else if (_Is(n, "ND_conductor_bsdf") && Film(*n) > 0) {
            // Thin film on a conductor: hdpbrtlayered's metal lobe alone
            // (pbrt patch 28: the film over a complex-index substrate).
            d.type = "hdpbrtlayered";
            d.writeReflectance = false;
            Rough(os, *n);
            os << " \"float specularweight\" 0 \"float metalweight\" 1"
               << " \"rgb metaleta\" " << HdPbrt_Rgb(_Color(*n, "ior", GfVec3f(0.183f, 0.421f, 1.373f)))
               << " \"rgb metalk\" " << HdPbrt_Rgb(_Color(*n, "extinction", GfVec3f(3.424f, 2.346f, 1.770f)));
            MetalWeightAndFilm(os, *n);
            d.otherParams = os.str();
        } else if (_Is(n, "ND_generalized_schlick_bsdf") && Film(*n) > 0) {
            // Thin film on a Schlick metal: the F82-tint metal lobe (color0,
            // color82; color90 and exponent are taken as 1 and 5).
            d.type = "hdpbrtlayered";
            d.writeReflectance = false;
            Rough(os, *n);
            os << " \"float specularweight\" 0 \"float metalweight\" 1"
               << " \"rgb metalf0\" " << HdPbrt_Rgb(_Clamp01(_Color(*n, "color0", GfVec3f(1))))
               << " \"rgb metalf82\" " << HdPbrt_Rgb(_Clamp01(_Color(*n, "color82", GfVec3f(1))));
            MetalWeightAndFilm(os, *n);
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
                DielectricFilm(os, *n);
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

// Karma's hmtlxcomplementcolor: the same luminance, chromaticity mirrored
// through the D65 white point (grey stays grey).
static GfVec3f _ComplementColor(const GfVec3f &c)
{
    const float X = 0.412391f * c[0] + 0.357584f * c[1] + 0.180481f * c[2];
    const float Y = 0.212639f * c[0] + 0.715169f * c[1] + 0.0721923f * c[2];
    const float Z = 0.0193308f * c[0] + 0.119195f * c[1] + 0.950532f * c[2];
    const float s = X + Y + Z;
    if (s == 0)
        return GfVec3f(0);
    const float x = 2 * 0.3127f - X / s, y = 2 * 0.329f - Y / s;
    if (y <= 0)
        return GfVec3f(0);
    const float X2 = x * Y / y, Z2 = (1 - x - y) * Y / y;
    return GfVec3f(3.24097f * X2 - 1.53738f * Y - 0.498611f * Z2,
                   -0.969244f * X2 + 1.87597f * Y + 0.0415551f * Z2,
                   0.0556301f * X2 - 0.203977f * Y + 1.05697f * Z2);
}

// Karma's ND_blackbody (linear Rec.709 by kelvin), measured from Karma CPU
// (Houdini 22.0.508) on unlit quads, 2026-11-25. It has MaterialX's chromaticity
// from ~2000 K up but not its luminance-1 normalization: the luminance grows
// about as T^4 until the largest channel reaches 20, which then caps it.
static const float _karmaBlackbody[][4] = {
    {0, 0, 0, 0}, {100, 8.88109e-06, -8.9407e-07, -5.96046e-08},
    {200, 0.000138283, -1.20401e-05, -1.01328e-06},
    {300, 0.000679016, -5.10812e-05, -5.126e-06},
    {400, 0.00208092, -0.000131011, -1.62721e-05},
    {500, 0.00492096, -0.000246048, -4.01139e-05},
    {600, 0.00988007, -0.000359058, -8.38041e-05},
    {700, 0.0177155, -0.000387907, -0.000156522}, {800, 0.0292206, -0.000193834, -0.000269175},
    {900, 0.0451965, 0.000432014, -0.000434875}, {1000, 0.0664673, 0.0017786, -0.000668049},
    {1100, 0.0938721, 0.00422668, -0.000985146}, {1200, 0.128052, 0.00826263, -0.00140667},
    {1300, 0.169678, 0.0144882, -0.00195122}, {1400, 0.21936, 0.0236206, -0.00264359},
    {1500, 0.277344, 0.0365295, -0.00350952}, {1600, 0.344238, 0.0541992, -0.00457382},
    {1700, 0.420898, 0.0772095, -0.00517654}, {1800, 0.508789, 0.105469, -0.00355148},
    {1900, 0.607422, 0.140747, -0.000140071}, {2000, 0.717773, 0.18396, 0.00570679},
    {2100, 0.84082, 0.235962, 0.0147324}, {2200, 0.977051, 0.297607, 0.0278015},
    {2300, 1.12695, 0.369873, 0.0457458}, {2400, 1.29297, 0.454102, 0.0696411},
    {2500, 1.47363, 0.550781, 0.100586}, {2600, 1.67188, 0.661621, 0.139893},
    {2700, 1.8877, 0.787598, 0.188721}, {2800, 2.12109, 0.929688, 0.248901},
    {2900, 2.375, 1.08984, 0.321533}, {3000, 2.64844, 1.26758, 0.408691},
    {3250, 3.42969, 1.80371, 0.70166}, {3500, 4.35547, 2.48633, 1.12793},
    {3750, 5.44531, 3.33789, 1.72363}, {4000, 6.71094, 4.38281, 2.5293},
    {4250, 8.17188, 5.64844, 3.58984}, {4500, 9.84375, 7.16016, 4.94531},
    {4750, 11.75, 8.94531, 6.64844}, {5000, 13.9141, 11.0312, 8.75781},
    {5250, 16.3594, 13.4453, 11.3203}, {5500, 19.0938, 16.2344, 14.4141},
    {5750, 20, 17.5156, 16.3125}, {6000, 20, 18, 17.5156}, {6250, 20, 18.4531, 18.7031},
    {6500, 20, 18.875, 19.8594}, {6750, 19.0781, 18.375, 20}, {7000, 18.125, 17.8125, 20},
    {7250, 17.2969, 17.2969, 20}, {7500, 16.5625, 16.8438, 20}, {7750, 15.9062, 16.4219, 20},
    {8000, 15.3203, 16.0312, 20}, {8250, 14.7891, 15.6875, 20}, {8500, 14.3125, 15.3672, 20},
    {8750, 13.875, 15.0781, 20}, {9000, 13.4766, 14.8047, 20}, {9250, 13.1172, 14.5547, 20},
    {9500, 12.7891, 14.3203, 20}, {9750, 12.4844, 14.1094, 20}, {10000, 12.2031, 13.9062, 20},
    {11000, 11.2656, 13.2266, 20}, {12000, 10.5547, 12.6953, 20},
    {13000, 10.0078, 12.2656, 20}, {14000, 9.5625, 11.9219, 20}, {15000, 9.20312, 11.6328, 20},
    {16000, 8.90625, 11.3984, 20}, {17000, 8.65625, 11.1875, 20},
    {18000, 8.44531, 11.0156, 20}, {19000, 8.26562, 10.8594, 20},
    {20000, 8.10156, 10.7266, 20}};

// Karma pyro shader (kma_pyroshader) -> pbrt nanovdb medium parameters (pbrt
// patch 35). Its MaterialX graph, with the defaults Hydra can't see (the
// internal ramps: identity intensity, black -> (1, 0.327, 0.1) fire color):
//   extinction = density x densityscale x complement(absorpcolor)
//   scattering = extinction x smokecolor x smokeint, absorption = the rest
//   fire emission (per unit length) = color(T) x fireintscale x
//     clamp((T - lo) / (hi - lo)) over fireintrange, color a ramp over
//     firecolorrange or blackbody(T x firetempscale)
// Unsupported options (color remaps, scatter emission, masks, Cd tint, shadow
// density tweaks, constant emission) warn once and are ignored.
bool HdPbrt_PyroVolume(const HdMaterialNetwork2 &net, const HdMaterialNode2 &n,
                       SdfPath const &id, HdPbrtMaterialDesc *d)
{
    auto has = [&](const char *p) { return n.parameters.count(TfToken(p)) > 0; };
    if (!has("enablesmoke") || !has("densityscale") || !has("firetempscale"))
        return false;
    auto num = [&](const char *p, float def) -> float {
        auto it = n.parameters.find(TfToken(p));
        if (it == n.parameters.end())
            return def;
        const VtValue &v = it->second;
        if (v.IsHolding<float>())
            return v.UncheckedGet<float>();
        if (v.IsHolding<int>())
            return float(v.UncheckedGet<int>());
        if (v.IsHolding<double>())
            return float(v.UncheckedGet<double>());
        return def;
    };
    auto col = [&](const char *p, GfVec3f def) -> GfVec3f {
        auto it = n.parameters.find(TfToken(p));
        return it != n.parameters.end() && it->second.IsHolding<GfVec3f>()
                   ? it->second.UncheckedGet<GfVec3f>()
                   : def;
    };
    auto range = [&](const char *p) -> GfVec2f {
        auto it = n.parameters.find(TfToken(p));
        return it != n.parameters.end() && it->second.IsHolding<GfVec2f>()
                   ? it->second.UncheckedGet<GfVec2f>()
                   : GfVec2f(0, 1);
    };
    // The field an input reads (its upstream geompropvalue's geomprop).
    auto field = [&](const char *input, const char *def) -> TfToken {
        auto it = n.inputConnections.find(TfToken(input));
        if (it != n.inputConnections.end() && !it->second.empty()) {
            auto up = net.nodes.find(it->second[0].upstreamNode);
            if (up != net.nodes.end()) {
                auto g = up->second.parameters.find(TfToken("geomprop"));
                if (g != up->second.parameters.end() && g->second.IsHolding<std::string>())
                    return TfToken(g->second.UncheckedGet<std::string>());
            }
        }
        return TfToken(def);
    };

    std::vector<std::string> ignored;
    for (const char *p : {"smokecolor_remapmode", "absorpcolor_remapmode", "shadowcolor_remapmode",
                          "density_enablecontrolscale", "enablescatter", "fireenablemask",
                          "tintwithcd", "mixusingalpha", "usesecondaryanisotropy",
                          "useanisotropyroughness"})
        if (num(p, 0) != 0)
            ignored.push_back(p);
    if (num("shadowint", 1) != 1 || col("shadowcolor", GfVec3f(1)) != GfVec3f(1))
        ignored.push_back("shadowint/shadowcolor");
    if (col("emission", GfVec3f(0)) != GfVec3f(0))
        ignored.push_back("emission");
    if (!ignored.empty())
        TF_WARN("hdPbrt: %s: Karma pyro options not translated: %s", id.GetText(),
                TfStringJoin(ignored, ", ").c_str());

    const bool smoke = num("enablesmoke", 1) != 0;
    const bool fire = num("enablefire", 0) != 0;
    GfVec3f ext = _ComplementColor(col("absorpcolor", GfVec3f(1)));
    for (int i = 0; i < 3; ++i)
        ext[i] = std::max(ext[i], 0.f);
    GfVec3f sigS(0), sigA(0);
    if (smoke) {
        const GfVec3f sc = col("smokecolor", GfVec3f(1)) * num("smokeint", 1);
        for (int i = 0; i < 3; ++i) {
            sigS[i] = ext[i] * std::max(sc[i], 0.f);
            sigA[i] = std::max(ext[i] - sigS[i], 0.f);
        }
    }
    std::ostringstream os;
    // RGB (pbrt's smooth upsampling): tinted smoke with multiple scattering
    // is within ~5% of Karma; the three-band spectra of the other media were
    // worse here (blue 0.96 -> 0.87).
    os << " \"rgb sigma_a\" " << HdPbrt_Rgb(sigA) << " \"rgb sigma_s\" " << HdPbrt_Rgb(sigS)
       << " \"float scale\" " << std::max(num("densityscale", 1), 0.f) << " \"float g\" "
       << std::min(std::max(num("anisotropy", 0), -0.99f), 0.99f) << " \"bool hdpbrtpyro\" true";
    if (num("density_remapmode", 0) != 0) {
        const GfVec2f r = range("density_sourcerange");
        os << " \"float hdpbrtdensityrange\" [" << r[0] << " " << r[1] << "]";
    }
    if (fire) {
        const GfVec2f ir = range("fireintrange"), cr = range("firecolorrange");
        os << " \"bool hdpbrtfire\" true \"float hdpbrtfireintscale\" " << num("fireintscale", 1)
           << " \"float hdpbrtfireintrange\" [" << ir[0] << " " << ir[1] << "]"
           << " \"integer hdpbrtfirecolormode\" " << int(num("firecolormode", 0))
           << " \"float hdpbrtfirecolorrange\" [" << cr[0] << " " << cr[1] << "]"
           << " \"float hdpbrtfiretempscale\" " << num("firetempscale", 2500);
        if (int(num("firecolormode", 0)) == 1) {
            os << " \"float hdpbrtfirebbtable\" [";
            // Measured in Rec.709; converted like a light's color temperature.
            for (const float(&e)[4] : _karmaBlackbody) {
                const GfVec3f c = t_workingSpace.FromRec709(GfVec3f(e[1], e[2], e[3]));
                os << " " << e[0] << " " << c[0] << " " << c[1] << " " << c[2];
            }
            os << " ]";
        }
    }
    d->volumeParams = os.str();
    d->densityField = field("density", "density");
    d->fireField = fire ? field("fireint", "temperature") : TfToken();
    return true;
}

HdPbrtMaterial::HdPbrtMaterial(SdfPath const &id, HdPbrtRenderDelegate *delegate)
    : HdMaterial(id), _delegate(delegate) {}

// GetMaterialRenderContexts() order for the material being fetched on this
// thread: the normal one, or the universal context (UsdPreviewSurface) first.
static thread_local bool t_previewContexts = false;

bool HdPbrt_PreviewContexts() { return t_previewContexts; }

void HdPbrtMaterial::Sync(HdSceneDelegate *sceneDelegate, HdRenderParam *renderParam,
                          HdDirtyBits *dirtyBits)
{
    static_cast<HdPbrtRenderParam *>(renderParam)->Edit();
    t_workingSpace = _delegate ? _delegate->GetWorkingSpace() : HdPbrtWorkingSpace();
    // Both versions of the material, so pbrt:materialPreview can switch
    // without fetching materials again (Hydra resolves the render context
    // when the material is fetched; GetMaterialRenderContexts reads the flag).
    VtValue res = sceneDelegate->GetMaterialResource(GetId());
    t_previewContexts = true;
    VtValue previewRes = sceneDelegate->GetMaterialResource(GetId());
    t_previewContexts = false;
    HdPbrtMaterialDesc result = _Build(res);
    HdPbrtMaterialDesc preview = previewRes == res ? result : _Build(previewRes);
    // The Karma pyro shader is recognized as the one node it is in the
    // universal network; its kma network arrives flattened into plain
    // MaterialX nodes. Both versions take the pyro volume from either.
    for (auto *d : {&result, &preview}) {
        const HdPbrtMaterialDesc &o = d == &result ? preview : result;
        if (d->volumeParams.empty() && !o.volumeParams.empty()) {
            d->volumeParams = o.volumeParams;
            d->densityField = o.densityField;
            d->fireField = o.fireField;
        }
    }
    {
        std::lock_guard<std::mutex> l(_mutex);
        _desc = result;
        _previewDesc = preview;
    }
    *dirtyBits = HdMaterial::Clean;
}

HdPbrtMaterialDesc HdPbrtMaterial::GetDesc() const
{
    const bool preview =
        _delegate && _delegate->GetSetting<bool>(HdPbrtSettingsTokens->materialPreview, false);
    std::lock_guard<std::mutex> l(_mutex);
    return preview ? _previewDesc : _desc;
}

void HdPbrtMaterial::_BuildDisplacement(HdMaterialNetworkMap const &map,
                                        HdPbrtMaterialDesc *result) const
{
    HdMaterialNetwork2 net2 = HdConvertToHdMaterialNetwork2(map);
    auto term = net2.terminals.find(HdMaterialTerminalTokens->displacement);
    if (term == net2.terminals.end())
        return;
    auto it = net2.nodes.find(term->second.upstreamNode);
    if (it == net2.nodes.end())
        return;
    const HdMaterialNode2 &disp = it->second;
    const std::string type = disp.nodeTypeId.GetString();
    const bool vector = TfStringStartsWith(type, "ND_displacement_vector3");
    if (!vector && !TfStringStartsWith(type, "ND_displacement_float") &&
        type != "UsdPreviewSurface")
        return;
    const std::string pfx = TfMakeValidIdentifier(GetId().GetString()) + "_disp";
    _Builder b{_Net{net2}, pfx, HdPbrtMaterialDesc{}, 0, _delegate, false};
    // Procedural graphs (anything the image folding below doesn't cover):
    // compiled for pbrt's graph evaluator (pbrt patch 41).
    auto procedural = [&]() {
        std::string program, error;
        std::vector<std::string> images;
        std::vector<std::pair<std::string, int>> primvars;
        if (type == "UsdPreviewSurface" ||
            !HdPbrt_CompileDisplacementGraph(net2, disp, _delegate, &program, &images, &primvars,
                                             &error)) {
            if (getenv("HDPBRT_DEBUG"))
                fprintf(stderr, "hdPbrt %s: displacement graph not supported: %s\n",
                        GetId().GetText(), error.c_str());
            return;
        }
        result->displacementGraph = program;
        result->displacementGraphImages = images;
        result->displacementGraphPrimvars = primvars;
        result->displacementVector = vector;
        if (getenv("HDPBRT_DEBUG"))
            fprintf(stderr, "hdPbrt %s: procedural displacement: %s\n", GetId().GetText(),
                    program.c_str());
    };
    if (vector) {
        // value = a * rgb + c per component, folding constant vector3 (or
        // vector3 by float) multiply / add / subtract nodes.
        std::function<bool(const HdMaterialNode2 &, const char *, GfVec3d *, GfVec3d *,
                           std::string *)>
            fold3 = [&](const HdMaterialNode2 &node, const char *input, GfVec3d *a,
                        GfVec3d *c, std::string *tex) -> bool {
            *tex = b.Texture(node, input, false);
            if (!tex->empty()) {
                *a = GfVec3d(1);
                *c = GfVec3d(0);
                return true;
            }
            const HdMaterialNode2 *up = b.net.Upstream(node, TfToken(input));
            if (!up)
                return false;
            const std::string t = up->nodeTypeId.GetString();
            const bool mul = TfStringStartsWith(t, "ND_multiply_vector3");
            const bool add = TfStringStartsWith(t, "ND_add_vector3");
            const bool sub = TfStringStartsWith(t, "ND_subtract_vector3");
            if (!mul && !add && !sub)
                return false;
            auto connected = [&](const char *in) {
                return up->inputConnections.count(TfToken(in)) > 0;
            };
            const bool first = connected("in1");
            if (first == connected("in2"))
                return false;
            const GfVec3f kf = _Color(*up, first ? "in2" : "in1", GfVec3f(mul ? 1.f : 0.f));
            const GfVec3d k(kf);
            GfVec3d ia, ic;
            if (!fold3(*up, first ? "in1" : "in2", &ia, &ic, tex))
                return false;
            for (int i = 0; i < 3; ++i) {
                if (mul) {
                    (*a)[i] = ia[i] * k[i];
                    (*c)[i] = ic[i] * k[i];
                } else if (add) {
                    (*a)[i] = ia[i];
                    (*c)[i] = ic[i] + k[i];
                } else if (first) {
                    (*a)[i] = ia[i];
                    (*c)[i] = ic[i] - k[i];
                } else {
                    (*a)[i] = -ia[i];
                    (*c)[i] = k[i] - ic[i];
                }
            }
            return true;
        };
        GfVec3d a(1), c(0);
        std::string tex;
        if (!fold3(disp, "displacement", &a, &c, &tex) ||
            disp.inputConnections.count(TfToken("scale"))) {
            procedural();
            return;
        }
        const double scale = _Float(disp, "scale", 1.f);
        a *= scale;
        c *= scale;
        result->displacementTexture = tex;
        result->displacementVector = true;
        result->displacementVectorScale = GfVec3f(a);
        result->displacementVectorBias = GfVec3f(c);
        result->textures += b.desc.textures;
        return;
    }
    // value = a * texture + c, folding constant multiply / add / subtract nodes.
    std::function<bool(const HdMaterialNode2 &, const char *, double *, double *, std::string *)>
        fold = [&](const HdMaterialNode2 &node, const char *input, double *a, double *c,
                   std::string *tex) -> bool {
        *tex = b.Texture(node, input, false);
        if (!tex->empty()) {
            *a = 1;
            *c = 0;
            // UsdUVTexture: scale is on the pbrt texture, bias added here.
            const HdMaterialNode2 *up = b.net.Upstream(node, TfToken(input));
            if (up && up->nodeTypeId == "UsdUVTexture") {
                VtValue bias = _Param(*up, "bias");
                if (bias.IsHolding<GfVec4f>())
                    *c = bias.UncheckedGet<GfVec4f>()[0];
            }
            return true;
        }
        const HdMaterialNode2 *up = b.net.Upstream(node, TfToken(input));
        if (!up)
            return false;
        const std::string t = up->nodeTypeId.GetString();
        auto connected = [&](const char *in) {
            return up->inputConnections.count(TfToken(in)) > 0;
        };
        const bool mul = TfStringStartsWith(t, "ND_multiply_float");
        const bool add = TfStringStartsWith(t, "ND_add_float");
        const bool sub = TfStringStartsWith(t, "ND_subtract_float");
        if (!mul && !add && !sub)
            return false;
        const bool first = connected("in1");
        if (first == connected("in2"))
            return false;  // both or neither connected
        const char *other = first ? "in2" : "in1";
        const double k = _Float(*up, other, mul ? 1.f : 0.f);
        double ia, ic;
        if (!fold(*up, first ? "in1" : "in2", &ia, &ic, tex))
            return false;
        if (mul) {
            *a = ia * k;
            *c = ic * k;
        } else if (add) {
            *a = ia;
            *c = ic + k;
        } else if (first) {  // in1 - k
            *a = ia;
            *c = ic - k;
        } else {  // k - in2
            *a = -ia;
            *c = k - ic;
        }
        return true;
    };
    double a = 1, c = 0;
    std::string tex;
    if (!fold(disp, "displacement", &a, &c, &tex) ||
        disp.inputConnections.count(TfToken("scale"))) {
        procedural();
        return;
    }
    if (type != "UsdPreviewSurface") {
        const double scale = _Float(disp, "scale", 1.f);
        a *= scale;
        c *= scale;
    }
    if (std::fabs(a) < 1e-12)
        return;
    result->displacementTexture = tex;
    result->displacementScale = float(a);
    result->displacementOffset = float(c / a);
    result->textures += b.desc.textures;
}

HdPbrtMaterialDesc HdPbrtMaterial::_Build(VtValue const &res) const
{
    HdPbrtMaterialDesc result = HdPbrt_DefaultMaterial();
    HdPbrtMaterialDesc volume;  // volume shading (pyro), merged into result
    if (res.IsHolding<HdMaterialNetworkMap>()) {
        bool isVolume = false;
        HdMaterialNetwork2 net2 =
            HdConvertToHdMaterialNetwork2(res.UncheckedGet<HdMaterialNetworkMap>(), &isVolume);
        if (getenv("HDPBRT_DUMP_MATERIALS")) {
            // The network as Hydra delivers it (after nodegraph flattening).
            fprintf(stderr, "hdPbrt material %s\n", GetId().GetText());
            for (auto const &t : net2.terminals)
                fprintf(stderr, "  terminal %s -> %s\n", t.first.GetText(),
                        t.second.upstreamNode.GetText());
            for (auto const &kv : net2.nodes) {
                fprintf(stderr, "  node %s %s\n", kv.first.GetText(),
                        kv.second.nodeTypeId.GetText());
                for (auto const &p : kv.second.parameters)
                    fprintf(stderr, "    %s = %s\n", p.first.GetText(),
                            TfStringify(p.second).c_str());
                for (auto const &c : kv.second.inputConnections)
                    for (auto const &cn : c.second)
                        fprintf(stderr, "    %s <- %s.%s\n", c.first.GetText(),
                                cn.upstreamNode.GetText(), cn.upstreamOutputName.GetText());
            }
        }
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
        // Karma pyro shader (kma:volume): Hydra delivers its nodegraph as one
        // node with the shader's own parameters; map those to pbrt's nanovdb
        // medium (see HdPbrt_PyroVolume).
        for (auto const &kv : net2.nodes)
            if (HdPbrt_PyroVolume(net2, kv.second, GetId(), &volume))
                break;
        // Volume-only material (no surface; MaterialX ND_volume with an
        // absorption_vdf / anisotropic_vdf): an invisible boundary ("interface")
        // around a homogeneous interior medium, coefficients per unit length.
        const HdMaterialNode2 *vol = nullptr;
        if (!surf) {
            auto vt = net2.terminals.find(HdMaterialTerminalTokens->volume);
            if (vt != net2.terminals.end()) {
                auto it = net2.nodes.find(vt->second.upstreamNode);
                if (it != net2.nodes.end() && _Is(&it->second, "ND_volume"))
                    vol = _Net{net2}.Upstream(it->second, TfToken("vdf"));
            }
        }
        if (vol && (_Is(vol, "ND_absorption_vdf") || _Is(vol, "ND_anisotropic_vdf"))) {
            const bool aniso = _Is(vol, "ND_anisotropic_vdf");
            const GfVec3f sigA = _Color(*vol, "absorption", GfVec3f(0));
            const GfVec3f sigS = aniso ? _Color(*vol, "scattering", GfVec3f(0)) : GfVec3f(0);
            const float g = aniso ? _Float(*vol, "anisotropy", 0) : 0;
            HdPbrtMaterialDesc d;
            d.type = "interface";
            d.writeReflectance = false;
            d.mediumName = TfMakeValidIdentifier(GetId().GetString()) + "_interior";
            std::ostringstream m;
            m << "MakeNamedMedium " << HdPbrt_Quote(d.mediumName)
              << " \"string type\" \"homogeneous\"" << _MediumSpectrum("sigma_a", sigA)
              << _MediumSpectrum("sigma_s", sigS) << " \"float scale\" 1 \"float g\" "
              << std::min(std::max(g, -0.99f), 0.99f) << "\n";
            d.textures = m.str();
            result = d;
        } else if (surf) {
            std::string pfx = TfMakeValidIdentifier(GetId().GetString());
            _Builder b{_Net{net2}, pfx, HdPbrtMaterialDesc{}, 0, _delegate};
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
            // Coat: a GGX dielectric layer on top; coatTint (coat color)
            // multiplies everything below it, and so does OpenPBR's darkening,
            // which pbrt computes per shading point (coatDark = its weight).
            float coatW = 0, coatRough = 0, coatIor = 1.5f, coatDark = 0;
            // With a varying coat weight: the coat-independent specular
            // roughness and the fully coated one (>= 0 when the coat changes
            // it), and coat_darkening (OpenPBR; times the weight).
            float roughUncoated = -1, roughCoated = -1, coatDarkK = 0;
            float iorUncoated = -1, iorCoated = -1;  // OpenPBR: specular IOR without / under the coat
            GfVec3f coatTint(1.f);
            // Metal inside hdpbrtlayered (patch 18), mixed by metalness:
            // OpenPBR F82-tint, standard_surface artistic IOR. Other shaders
            // keep pbrt's conductor.
            bool layeredMetal = false, metalF82 = false;
            GfVec3f metalBase(0.8f), metalEdge(1.f);
            // Diffuse roughness: energy-preserving Oren-Nayar (patch 20).
            float diffRough = 0;
            float metalScale = 1;
            // Rough glass with microfacet multiple-scattering compensation
            // (patch 21): OpenPBR, as Typhoon (not standard_surface).
            bool glassMultiScatter = false;
            // standard_surface glass is MaterialX's layer(specular R over
            // transmission T): T keeps its own 1 - F and also pays the top's
            // 1 - w E_R(wo), as Typhoon (pbrt patch 17's hdpbrttopweight).
            float glassTopWeight = 0;
            // Dispersion (OpenPBR): Abbe number of the glass, 0 for none.
            float dispAbbe = 0;
            // Thin film on the specular layer (patch 22; hdpbrtlayered only):
            // weight, thickness in nm, film IOR.
            float filmW = 0, filmNm = 0, filmIor = 1.4f;
            // Subsurface (patch 23): weight, walk albedo, mean free path,
            // anisotropy. A random walk in an interior medium for closed
            // (not thin-walled) surfaces; thin walls blend the albedo.
            float sssW = 0, sssAniso = 0;
            GfVec3f sssColor(0.8f), sssRadius(1.f);
            bool sssWalk = false;
            // No specular lobe at all (specular weight or color 0, or ior 1,
            // i.e. no Fresnel reflection): a plain Lambertian "diffuse".
            bool noSpecular = false;
            const char *baseIn = nullptr, *roughIn = nullptr, *opacityIn = nullptr;
            // Inputs that may vary (images, primvars): pbrt takes textures for
            // these; formulas that combine them with others (coat roughening
            // the specular, darkening, IOR) use their constant values.
            const char *baseWIn = nullptr, *metalIn = nullptr, *specWIn = nullptr,
                       *specColorIn = nullptr, *coatIn = nullptr, *coatColorIn = nullptr,
                       *coatRoughIn = nullptr, *sheenIn = nullptr, *sheenColorIn = nullptr,
                       *sheenRoughIn = nullptr, *diffRoughIn = nullptr, *transColorIn = nullptr;
            if (type == "UsdPreviewSurface") {
                base = _Color(*surf, "diffuseColor", GfVec3f(0.18f));
                metallic = _Float(*surf, "metallic", 0);
                rough = _Float(*surf, "roughness", 0.5f);
                ior = _Float(*surf, "ior", 1.5f);
                opacity = _Float(*surf, "opacity", 1);
                emission = _Color(*surf, "emissiveColor", GfVec3f(0));
                baseIn = "diffuseColor"; roughIn = "roughness"; opacityIn = "opacity";
                metalIn = "metallic"; coatIn = "clearcoat"; coatRoughIn = "clearcoatRoughness";
                // Clearcoat: a white GGX coat of IOR 1.5 (UsdPreviewSurface spec).
                coatW = std::min(std::max(_Float(*surf, "clearcoat", 0), 0.f), 1.f);
                coatRough = _Float(*surf, "clearcoatRoughness", 0.01f);
                coatIor = 1.5f;
                // Metal: the metal lobe of hdpbrtlayered with F0 = diffuseColor
                // and a white edge (the spec's Schlick Fresnel), constant or
                // varying metallic alike.
                layeredMetal = metalF82 = true;
                metalBase = base;
                metalEdge = GfVec3f(1.f);
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
                baseWIn = "base"; metalIn = "metalness"; specWIn = "specular";
                specColorIn = "specular_color"; coatIn = "coat"; coatColorIn = "coat_color";
                coatRoughIn = "coat_roughness"; sheenIn = "sheen"; sheenColorIn = "sheen_color";
                sheenRoughIn = "sheen_roughness"; diffRoughIn = "diffuse_roughness";
                transColorIn = "transmission_color";
                sssW = std::min(std::max(_Float(*surf, "subsurface", 0), 0.f), 1.f);
                sssColor = _Color(*surf, "subsurface_color", GfVec3f(1));
                sssRadius = _Color(*surf, "subsurface_radius", GfVec3f(1)) *
                            _Float(*surf, "subsurface_scale", 1);
                sssAniso = _Float(*surf, "subsurface_anisotropy", 0);
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
                diffRough = _Float(*surf, "diffuse_roughness", 0);
                filmNm = std::max(_Float(*surf, "thin_film_thickness", 0), 0.f);
                filmW = filmNm > 0 ? 1.f : 0.f;
                filmIor = std::max(_Float(*surf, "thin_film_IOR", 1.5f), 1.f);
                glassTopWeight = std::min(std::max(specWeight, 0.f), 1.f);
                layeredMetal = true;
                metalBase = _Color(*surf, "base_color", GfVec3f(0.8f)) * _Float(*surf, "base", 1);
                metalEdge = _Color(*surf, "specular_color", GfVec3f(1)) * _Float(*surf, "specular", 1);
                coatW = std::min(std::max(_Float(*surf, "coat", 0), 0.f), 1.f);
                if (coatW > 0 || surf->inputConnections.count(TfToken("coat"))) {
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
                    const float affect =
                        std::min(std::max(_Float(*surf, "coat_affect_roughness", 0), 0.f), 1.f) *
                        std::min(std::max(coatRough, 0.f), 1.f);
                    roughUncoated = rough;
                    roughCoated = rough * (1 - affect) + affect;
                    const float mix = affect * coatW;
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
                baseWIn = "base_weight"; metalIn = "base_metalness"; specWIn = "specular_weight";
                specColorIn = "specular_color"; coatIn = "coat_weight"; coatColorIn = "coat_color";
                coatRoughIn = "coat_roughness"; sheenIn = "fuzz_weight"; sheenColorIn = "fuzz_color";
                sheenRoughIn = "fuzz_roughness"; diffRoughIn = "base_diffuse_roughness";
                transColorIn = "transmission_color";
                sssW = std::min(std::max(_Float(*surf, "subsurface_weight", 0), 0.f), 1.f);
                sssColor = _Color(*surf, "subsurface_color", GfVec3f(0.8f));
                {
                    const GfVec3f sc = _Color(*surf, "subsurface_radius_scale", GfVec3f(1, 0.5f, 0.25f));
                    const float r = _Float(*surf, "subsurface_radius", 1);
                    sssRadius = GfVec3f(r * sc[0], r * sc[1], r * sc[2]);
                }
                sssAniso = _Float(*surf, "subsurface_scatter_anisotropy", 0);
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
                diffRough = _Float(*surf, "base_diffuse_roughness", 0);
                glassMultiScatter = true;
                {
                    const float scale = std::min(
                        std::max(_Float(*surf, "transmission_dispersion_scale", 0), 0.f), 1.f);
                    const float abbe =
                        std::max(_Float(*surf, "transmission_dispersion_abbe_number", 20), 0.f);
                    dispAbbe = scale > 0 ? abbe / scale : 0;
                }
                filmW = std::min(std::max(_Float(*surf, "thin_film_weight", 0), 0.f), 1.f);
                filmNm = std::max(_Float(*surf, "thin_film_thickness", 0.5f), 0.f) * 1000;
                filmIor = std::max(_Float(*surf, "thin_film_ior", 1.4f), 1.f);
                layeredMetal = metalF82 = true;
                metalBase = _Color(*surf, "base_color", GfVec3f(0.8f)) * _Float(*surf, "base_weight", 1);
                // MaterialX: the metal BSDF is weighted by specular_weight and
                // its edge color is specular_color * specular_weight.
                metalScale = std::max(specWeight, 0.f);
                metalEdge = _Color(*surf, "specular_color", GfVec3f(1)) * std::min(metalScale, 1.f);
                coatW = std::min(std::max(_Float(*surf, "coat_weight", 0), 0.f), 1.f);
                if (coatW > 0 || surf->inputConnections.count(TfToken("coat_weight"))) {
                    // OpenPBR spec: the coat roughens the specular below,
                    // r' = (r^4 + 2 c^4)^(1/4), and the specular's IOR becomes
                    // relative to the coat's, both blended by the coat weight.
                    coatRough = _Float(*surf, "coat_roughness", 0);
                    coatIor = std::max(_Float(*surf, "coat_ior", 1.6f), 1.f);
                    const float r4 = std::pow(std::max(rough, 0.f), 4.f);
                    const float c4 = std::pow(std::max(coatRough, 0.f), 4.f);
                    roughUncoated = rough;
                    roughCoated = std::pow(std::min(1.f, 2 * c4 + r4), 0.25f);
                    rough = rough * (1 - coatW) + roughCoated * coatW;
                    const float iorS = std::max(ior, 1.f);
                    iorUncoated = iorS;
                    iorCoated = std::max(iorS / coatIor, coatIor / iorS);
                    ior = iorS * (1 - coatW) + iorCoated * coatW;
                    // Darkening from light trapped under the coat,
                    // (1 - K) / (1 - E K) blended by weight * coat_darkening:
                    // pbrt evaluates it per shading point from the (textured)
                    // base, the metal F0 and the subsurface share.
                    coatDarkK = std::min(std::max(_Float(*surf, "coat_darkening", 1), 0.f), 1.f);
                    coatDark = coatW * coatDarkK;
                    const GfVec3f cc = _Clamp01(_Color(*surf, "coat_color", GfVec3f(1)));
                    for (int i = 0; i < 3; ++i)
                        coatTint[i] = 1 - coatW + cc[i] * coatW;
                }
                noSpecular = specWeight <= 0.f && _Float(*surf, "coat_weight", 0) <= 0.f;
            } else {
                // Unknown shader: look for common names, else grey diffuse.
                base = _Color(*surf, "base_color", _Color(*surf, "diffuseColor", GfVec3f(0.5f)));
                TF_WARN("hdPbrt: unsupported surface shader %s on %s, using diffuse",
                        type.c_str(), GetId().GetText());
            }

            if (sssW > 0) {
                if (thinWalled || std::max({sssRadius[0], sssRadius[1], sssRadius[2]}) <= 0) {
                    base = base * (1 - sssW) + sssColor * sssW;
                    if (sssW >= 0.5f)
                        baseIn = "subsurface_color";
                } else {
                    // The diffuse base keeps 1 - w (a base texture stays
                    // unscaled unless the walk takes it all).
                    base *= 1 - sssW;
                    sssWalk = true;
                    if (sssW > 0.999f)
                        baseIn = nullptr;
                }
            }
            // Coat darkening (OpenPBR): its weight, and the subsurface albedo
            // the walk took out of the diffuse reflectance.
            // (The darkening weight itself: darkening() below.)
            std::string coatDarkParams;
            if (coatDarkK > 0) {
                if (sssWalk)
                    coatDarkParams += " \"rgb coatdarkeningadd\" " +
                                      HdPbrt_Rgb(_Clamp01(sssColor * sssW));
            }
            bool fromPrimvar = false;
            std::string baseTex = baseIn ? b.Texture(*surf, baseIn, true, &fromPrimvar) : "";
            std::string roughTex = roughIn ? b.Texture(*surf, roughIn, false, nullptr, true) : "";
            std::string opacityTex = opacityIn ? b.Texture(*surf, opacityIn, false) : "";

            // Varying inputs (constants where they don't vary).
            using Val = _Builder::Val;
            const float baseWC = baseWIn ? _Float(*surf, baseWIn, 1) : 1.f;
            const float baseK = sssWalk ? 1 - sssW : 1.f;  // the walk's share
            const Val vBaseW = b.In(*surf, baseWIn, false, GfVec3f(baseWC));
            const Val vMetal =
                b.In(*surf, metalIn, false, GfVec3f(std::min(std::max(metallic, 0.f), 1.f)));
            const Val vSpecW = b.In(*surf, specWIn, false, GfVec3f(specWeight));
            const Val vSpecColor = b.In(*surf, specColorIn, true, _Clamp01(specTint));
            const Val vCoat = b.In(*surf, coatIn, false, GfVec3f(coatW));
            const Val vCoatColor = b.In(
                *surf, coatColorIn, true,
                _Clamp01(coatColorIn ? _Color(*surf, coatColorIn, GfVec3f(1)) : GfVec3f(1)));
            const Val vCoatRough = b.In(*surf, coatRoughIn, false,
                                        GfVec3f(std::pow(std::min(std::max(coatRough, 0.f), 1.f), 2.f)),
                                        true);
            const Val vSheen = b.In(*surf, sheenIn, false, GfVec3f(std::min(std::max(sheenW, 0.f), 1.f)));
            const Val vSheenColor = b.In(*surf, sheenColorIn, true, _Clamp01(sheenColor));
            const Val vSheenRough = b.In(*surf, sheenRoughIn, false,
                                         GfVec3f(std::min(std::max(sheenRough, 0.f), 1.f)));
            const Val vDiffRough = b.In(*surf, diffRoughIn, false,
                                        GfVec3f(std::min(std::max(diffRough, 0.f), 1.f)));
            const Val vTransColor = b.In(*surf, transColorIn, true, _Clamp01(transColor));
            // Base color x weight (no subsurface or thin-wall share).
            Val vBase;
            if (!baseTex.empty()) {
                vBase.tex = baseTex;
                vBase.c = base;
            } else {
                const float k = baseWC * baseK;
                vBase.c = k > 1e-6f ? base / k : base;
            }
            vBase = b.Mul(vBase, vBaseW, true, "base");
            const bool coatOn = coatW > 0 || vCoat.Varies();
            const bool sheenOn = sheenW > 0 || vSheen.Varies();

            HdPbrtMaterialDesc &d = b.desc;
            d.baseColor = base;
            d.baseColorTexture = baseTex;
            if (vBase.Varies())
                d.baseColorTexture = b.Mul(vBase, Val{"", GfVec3f(baseK)}, true, "refl").tex;
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
                if (!roughTex.empty()) {
                    other << " \"texture " << name << "\" " << HdPbrt_Quote(roughTex);
                    if (b.alphaTextures.count(roughTex))
                        other << " \"bool remaproughness\" false";
                }
                else if (vCoat.Varies() && roughCoated >= 0) {
                    // The coat roughens the specular by its (varying) weight:
                    // GGX alpha blended between uncoated and coated.
                    const float r0 = std::min(std::max(roughUncoated, 0.f), 1.f);
                    const float r1 = std::min(std::max(roughCoated, 0.f), 1.f);
                    other << _Builder::Param(name,
                                             b.Mix(Val{"", GfVec3f(r0 * r0)}, Val{"", GfVec3f(r1 * r1)},
                                                   vCoat, false, "coatrough"),
                                             false)
                          << " \"bool remaproughness\" false";
                }
                else {
                    float r = std::min(std::max(rough, 0.f), 1.f);
                    other << " \"float " << name << "\" " << r * r
                          << " \"bool remaproughness\" false";
                }
            };
            // Coat: weight, roughness (alpha), IOR, tint lerp(1, color, weight)
            // (times OpenPBR's darkening, per shading point in pbrt).
            // The specular IOR blended by a varying coat weight (OpenPBR).
            auto layeredEta = [&]() -> std::string {
                if (!vCoat.Varies() || iorCoated <= 0)
                    return {};
                return _Builder::Param("hdpbrteta",
                                       b.Mix(Val{"", GfVec3f(iorUncoated)}, Val{"", GfVec3f(iorCoated)},
                                             vCoat, false, "eta"),
                                       false);
            };
            // OpenPBR darkening weight = coat weight x coat_darkening.
            auto darkening = [&]() -> std::string {
                if (coatDarkK <= 0)
                    return {};
                if (vCoat.Varies())
                    return _Builder::Param(
                        "coatdarkening", b.Mul(vCoat, Val{"", GfVec3f(coatDarkK)}, false, "dark"),
                        false);
                return coatDark > 0 ? " \"float coatdarkening\" " + TfStringify(coatDark)
                                    : std::string();
            };
            auto coatParams = [&]() {
                const Val tint = (vCoat.Varies() || vCoatColor.Varies())
                                     ? b.Mix(Val{"", GfVec3f(1.f)}, vCoatColor, vCoat, true, "coattint")
                                     : Val{"", _Clamp01(coatTint)};
                return _Builder::Param("coatweight", vCoat, false) +
                       _Builder::Param("coatroughness", vCoatRough, false) +
                       " \"float coateta\" " + TfStringify(coatIor) +
                       _Builder::Param("coattint", tint, true) + darkening() + coatDarkParams;
            };
            if (layeredMetal && (metallic > 0 || vMetal.Varies()) &&
                (metallic >= 0.5f || transmission < 0.5f)) {
                d.type = "hdpbrtlayered";
                roughParam("roughness");
                other << " \"float eta\" " << ior << layeredEta();
                if (noSpecular || metallic >= 1.f)
                    specWeight = 0;
                if (vSpecW.Varies() && specWeight > 0)
                    other << _Builder::Param("specularweight", vSpecW, false);
                else if (std::fabs(specWeight - 1.f) > 1e-6f)
                    other << " \"float specularweight\" " << std::min(std::max(specWeight, 0.f), 1.f);
                if (vSpecColor.Varies())
                    other << _Builder::Param("speculartint", vSpecColor, true);
                else if (specTint != GfVec3f(1.f))
                    other << " \"rgb speculartint\" " << HdPbrt_Rgb(_Clamp01(specTint));
                other << _Builder::Param("metalweight", vMetal, false);
                if (std::fabs(metalScale - 1.f) > 1e-6f)
                    other << " \"float metalscale\" " << metalScale;
                // The edge color: specular color x weight (OpenPBR clamps the
                // weight to 1 there; pbrt's albedo spectra clamp to [0, 1]).
                const bool edgeVaries = specColorIn && (vSpecColor.Varies() || vSpecW.Varies());
                if (metalF82 || vBase.Varies() || edgeVaries) {
                    // F0 = base color (textured or primvar if so), F82 tint = edge.
                    const Val f0 = vBase.Varies() ? vBase : Val{"", _Clamp01(metalBase)};
                    const Val edge = edgeVaries ? b.Mul(vSpecColor, vSpecW, true, "edge")
                                                : Val{"", _Clamp01(metalEdge)};
                    other << _Builder::Param("metalf0", f0, true)
                          << _Builder::Param("metalf82", edge, true);
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
                if (sheenOn) {
                    other << _Builder::Param("sheenweight", vSheen, false)
                          << _Builder::Param("sheencolor", vSheenColor, true)
                          << _Builder::Param("sheenroughness", vSheenRough, false);
                    if (sheenOnTop)
                        other << " \"bool sheenontop\" true";
                }
                if (coatOn)
                    other << coatParams();
            } else if (metallic >= 0.5f) {
                d.type = "conductor";
                roughParam("roughness");
            } else if (transmission >= 0.5f && !thinWalled) {
                d.type = "dielectric";
                if (dispAbbe > 0) {
                    // Dispersion: two-term Cauchy n(l) = B + C / l^2 through
                    // n_d (589.3 nm) with V = (n_d - 1) / (n_F - n_C), as
                    // Typhoon. pbrt traces a non-constant eta at the hero
                    // wavelength only.
                    const double lD = 0.5893, lF = 0.4861, lC = 0.6563;  // um
                    const double C = (ior - 1.0) * (lC * lC * lF * lF / (lC * lC - lF * lF)) / dispAbbe;
                    const double B = ior - C / (lD * lD);
                    other << " \"spectrum eta\" [";
                    for (int nm = 360; nm <= 830; nm += 10) {
                        const double l = nm * 1e-3;
                        other << " " << nm << " " << std::max(B + C / (l * l), 1.0);
                    }
                    other << " ]";
                } else
                    other << " \"float eta\" " << ior;
                // MaterialX: the reflection stays at specular_roughness, only
                // the transmission gets the extra roughness (pbrt dielectric
                // "hdpbrtreflectionroughness", alpha like "roughness").
                const float reflRough = std::min(std::max(rough, 0.f), 1.f);
                rough = std::min(std::max(rough + transExtraRough, 0.f), 1.f);
                roughParam("roughness");
                if (roughTex.empty() && rough != reflRough)
                    other << " \"float hdpbrtreflectionroughness\" " << reflRough * reflRough;
                if (glassMultiScatter && rough > 0)
                    other << " \"bool hdpbrtmultiscatter\" true";
                if (glassTopWeight > 0)
                    other << " \"float hdpbrttopweight\" " << glassTopWeight;
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
                if ((!medium || tintWithMedium) && vTransColor.Varies())
                    other << _Builder::Param("transmissiontint", vTransColor, true);
                else if ((!medium || tintWithMedium) && transColor != GfVec3f(1.f))
                    other << " \"rgb transmissiontint\" " << HdPbrt_Rgb(_Clamp01(transColor));
            } else if (noSpecular && !sheenOn && !coatOn && !sssWalk &&
                       !(thinWalled && transmission > 0)) {
                d.type = "diffuse";
            } else {
                d.type = "hdpbrtlayered";
                roughParam("roughness");
                other << " \"float eta\" " << ior << layeredEta();
                if (noSpecular)
                    specWeight = 0;
                if (coatOn)
                    other << coatParams();
                if (sheenOn) {
                    other << _Builder::Param("sheenweight", vSheen, false)
                          << _Builder::Param("sheencolor", vSheenColor, true)
                          << _Builder::Param("sheenroughness", vSheenRough, false);
                    if (sheenOnTop)
                        other << " \"bool sheenontop\" true";
                }
                // Thin wall: transmission passes straight through (eta 1),
                // under the specular layer; the diffuse base takes the rest.
                if (thinWalled && transmission > 0) {
                    const float t = std::min(transmission, 1.f);
                    d.baseColor *= 1 - t;
                    if (!d.baseColorTexture.empty())
                        d.baseColorTexture =
                            b.Mul(Val{d.baseColorTexture, d.baseColor}, Val{"", GfVec3f(1 - t)},
                                  true, "thin").tex;
                    other << _Builder::Param("transmittance",
                                             b.Mul(vTransColor, Val{"", GfVec3f(t)}, true, "trans"),
                                             true);
                }
                if (vSpecW.Varies() && specWeight > 0)
                    other << _Builder::Param("specularweight", vSpecW, false);
                else if (std::fabs(specWeight - 1.f) > 1e-6f)
                    other << " \"float specularweight\" " << std::min(std::max(specWeight, 0.f), 1.f);
                if (vSpecColor.Varies()) {
                    other << _Builder::Param("speculartint", vSpecColor, true);
                } else if (specTint != GfVec3f(1.f)) {
                    GfVec3f t = specTint;
                    for (int i = 0; i < 3; ++i)
                        t[i] = std::min(std::max(t[i], 0.f), 1.f);
                    other << " \"rgb speculartint\" " << HdPbrt_Rgb(t);
                }
            }
            diffRough = std::min(std::max(diffRough, 0.f), 1.f);
            if ((diffRough > 0 || vDiffRough.Varies()) && d.type == "diffuse")
                other << _Builder::Param("roughness", vDiffRough, false);
            else if ((diffRough > 0 || vDiffRough.Varies()) && d.type == "hdpbrtlayered")
                other << _Builder::Param("diffuseroughness", vDiffRough, false);
            if (sssWalk && d.type == "hdpbrtlayered" && d.mediumName.empty()) {
                other << " \"float subsurface\" " << sssW;
                d.mediumName = pfx + "_sss";
                d.textures += HdPbrt_SubsurfaceMedium(d.mediumName, sssColor, sssRadius, sssAniso);
            }
            if (filmW > 0 && filmNm > 0 && (d.type == "hdpbrtlayered" || d.type == "dielectric"))
                other << " \"float filmweight\" " << filmW << " \"float filmthickness\" " << filmNm
                      << " \"float filmeta\" " << filmIor;
            d.otherParams = other.str();
            // Inputs driven by images or primvars that pbrt takes as constants
            // here (IOR, thin film, subsurface, emission, transmission weight,
            // ...): their constant value is used. Warn once per material.
            {
                const char *handled[] = {baseIn,      roughIn,      opacityIn,   baseWIn,
                                         metalIn,     specWIn,      specColorIn, coatIn,
                                         coatColorIn, coatRoughIn,  sheenIn,     sheenColorIn,
                                         sheenRoughIn, diffRoughIn, transColorIn};
                std::vector<std::string> fixed;
                for (const auto &c : surf->inputConnections) {
                    bool ok = false;
                    for (const char *h : handled)
                        ok = ok || (h && c.first == h);
                    if (ok || c.first == "normal" || c.second.empty())
                        continue;
                    const HdMaterialNode2 *up = b.net.Node(c.second[0].upstreamNode);
                    if (up && !_IsTextureNode(*up) && !_IsPrimvarNode(*up) &&
                        (TfStringStartsWith(up->nodeTypeId.GetString(), "ND_convert") ||
                         TfStringStartsWith(up->nodeTypeId.GetString(), "ND_dot")))
                        up = b.net.Upstream(*up, TfToken("in"));
                    if (up && (_IsTextureNode(*up) || _IsPrimvarNode(*up)))
                        fixed.push_back(c.first.GetString());
                }
                if (!fixed.empty()) {
                    TF_WARN("hdPbrt: %s: %s vary (images or primvars) but are constant in "
                            "pbrt's material; using their constant values.",
                            GetId().GetText(), TfStringJoin(fixed, ", ").c_str());
                    if (getenv("HDPBRT_DEBUG"))  // (hython drops Tf warnings)
                        fprintf(stderr, "hdPbrt %s: constant in pbrt, not varied: %s\n",
                                GetId().GetText(), TfStringJoin(fixed, ", ").c_str());
                }
            }
            result = d;
            }  // !mtlxGraph
        }
    }

    // True displacement (pbrt patch 39): the displacement terminal's height
    // texture with constant scale/offset (MaterialX ND_displacement_float, or
    // UsdPreviewSurface's displacement input).
    if (res.IsHolding<HdMaterialNetworkMap>())
        _BuildDisplacement(res.UncheckedGet<HdMaterialNetworkMap>(), &result);
    // Infrared reflectance: inputs pbrt:irReflectance / pbrt:irEdge on the
    // surface shader (or the Material's terminal node).
    if (res.IsHolding<HdMaterialNetworkMap>()) {
        for (auto const &kv : res.UncheckedGet<HdMaterialNetworkMap>().map)
            for (auto const &node : kv.second.nodes)
                for (auto const &p : node.parameters) {
                    float f;
                    if (p.second.IsHolding<float>()) f = p.second.UncheckedGet<float>();
                    else if (p.second.IsHolding<double>()) f = (float)p.second.UncheckedGet<double>();
                    else continue;
                    if (p.first == "pbrt:irReflectance")
                        result.irReflectance = f;
                    else if (p.first == "pbrt:irEdge")
                        result.irEdge = std::min(std::max(f, 400.f), 1100.f);
                }
    }
    result.volumeParams = volume.volumeParams;
    result.densityField = volume.densityField;
    result.fireField = volume.fireField;
    return result;
}

PXR_NAMESPACE_CLOSE_SCOPE
