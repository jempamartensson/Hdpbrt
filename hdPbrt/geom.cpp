// SPDX-License-Identifier: Apache-2.0
// Shared by the geometry rprims (mesh, curves, points).
#include "hdPbrt.h"
#include "pxr/base/gf/transform.h"
#include "pxr/base/gf/rotation.h"
#include "pxr/base/gf/quatd.h"

#include "pxr/imaging/hd/sceneDelegate.h"
#include "pxr/imaging/hd/renderIndex.h"
#include "pxr/imaging/hd/tokens.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>

PXR_NAMESPACE_OPEN_SCOPE

TF_DEFINE_PRIVATE_TOKENS(_tokens, (displayColor)(displayOpacity));

namespace {

template <typename T>
bool _Flatten(VtValue *v, VtIntArray const &indices)
{
    if (!v->IsHolding<VtArray<T>>())
        return false;
    const VtArray<T> &src = v->UncheckedGet<VtArray<T>>();
    VtArray<T> out(indices.size());
    for (size_t i = 0; i < indices.size(); ++i) {
        int k = indices[i];
        out[i] = (k >= 0 && size_t(k) < src.size()) ? src[k] : T();
    }
    *v = VtValue(out);
    return true;
}

// Get a primvar, expanding indexed primvars to one value per element.
VtValue _GetFlat(HdSceneDelegate *sd, SdfPath const &id, TfToken const &name)
{
    VtIntArray indices;
    VtValue v = sd->GetIndexedPrimvar(id, name, &indices);
    if (v.IsEmpty())
        return sd->Get(id, name);
    if (!indices.empty()) {
        _Flatten<GfVec3f>(&v, indices) || _Flatten<GfVec2f>(&v, indices) ||
            _Flatten<float>(&v, indices);
    }
    return v;
}

}  // namespace

HdPbrtPrimvar HdPbrt_FindPrimvar(HdSceneDelegate *sd, SdfPath const &id,
                                 const std::vector<TfToken> &names, bool anyVec2)
{
    static const HdInterpolation interps[] = {
        HdInterpolationFaceVarying, HdInterpolationVertex, HdInterpolationVarying,
        HdInterpolationUniform, HdInterpolationConstant};
    for (const TfToken &name : names)
        for (HdInterpolation interp : interps)
            for (const HdPrimvarDescriptor &pv : sd->GetPrimvarDescriptors(id, interp))
                if (pv.name == name)
                    return {_GetFlat(sd, id, pv.name), interp, true};
    if (anyVec2) {
        for (HdInterpolation interp : {HdInterpolationFaceVarying, HdInterpolationVertex,
                                       HdInterpolationVarying})
            for (const HdPrimvarDescriptor &pv : sd->GetPrimvarDescriptors(id, interp)) {
                VtValue v = _GetFlat(sd, id, pv.name);
                if (v.IsHolding<VtVec2fArray>())
                    return {v, interp, true};
            }
    }
    return {};
}

GfVec3f HdPbrt_AverageColor(VtValue const &v, bool *ok)
{
    *ok = false;
    if (v.IsHolding<VtVec3fArray>()) {
        const VtVec3fArray &a = v.UncheckedGet<VtVec3fArray>();
        if (a.empty())
            return GfVec3f(0.18f);
        GfVec3d sum(0);
        for (const GfVec3f &c : a)
            sum += GfVec3d(c);
        *ok = true;
        return GfVec3f(sum / double(a.size()));
    }
    if (v.IsHolding<GfVec3f>()) {
        *ok = true;
        return v.UncheckedGet<GfVec3f>();
    }
    return GfVec3f(0.18f);
}


HdPbrtGeom::HdPbrtGeom(HdPbrtRenderDelegate *delegate) : _delegate(delegate)
{
    _delegate->Register(this);
}

HdPbrtGeom::~HdPbrtGeom() { _delegate->Unregister(this); }

void HdPbrtGeom::_SyncTransform(HdSceneDelegate *sd)
{
    SdfPath const &id = _Prim().GetId();
    std::lock_guard<std::mutex> lock(_mutex);
    // Motion blur: sample over a window around the current time and
    // resample at the shutter when the scene is written.
    HdPbrt_SampleXform(
        [&](float a, float b, HdTimeSampleArray<GfMatrix4d, 4> *sa) {
            sd->SampleTransform(id, a, b, sa);
        },
        &_xform);
    if (_xform.count == 0) {
        _xform.Resize(1);
        _xform.times[0] = 0.f;
        _xform.values[0] = sd->GetTransform(id);
    }
    static const bool debug = getenv("HDPBRT_DEBUG") != nullptr;
    if (debug && _xform.count > 1)
        fprintf(stderr, "hdPbrt %s: %zu transform samples (%g..%g)\n", id.GetText(),
                _xform.count, _xform.times[0], _xform.times[_xform.count - 1]);
}

void HdPbrtGeom::_SyncInstancing(HdSceneDelegate *sd)
{
    HdRprim const &prim = _Prim();
    bool instanced = !prim.GetInstancerId().IsEmpty();
    if (instanced) {
        if (HdInstancer *instancer = sd->GetRenderIndex().GetInstancer(prim.GetInstancerId()))
            static_cast<HdPbrtInstancer *>(instancer)->UpdateInstanceIndices(prim.GetId());
    }
    std::lock_guard<std::mutex> lock(_mutex);
    _instanced = instanced;
}

void HdPbrtGeom::_SyncCategories(HdSceneDelegate *sd)
{
    VtArray<TfToken> c = sd->GetCategories(_Prim().GetId());
    if (getenv("HDPBRT_DEBUG")) {
        fprintf(stderr, "hdPbrt %s categories:", _Prim().GetId().GetText());
        for (auto const &t : c)
            fprintf(stderr, " %s", t.GetText());
        fprintf(stderr, "\n");
    }
    std::lock_guard<std::mutex> lock(_mutex);
    _categories = c;
}

void HdPbrtGeom::_SyncDisplayColor(HdSceneDelegate *sd)
{
    SdfPath const &id = _Prim().GetId();
    HdPbrtPrimvar c = HdPbrt_FindPrimvar(sd, id, {_tokens->displayColor});
    bool ok = false;
    GfVec3f col = c.valid ? HdPbrt_AverageColor(c.value, &ok) : GfVec3f(0.18f);
    HdPbrtPrimvar o = HdPbrt_FindPrimvar(sd, id, {_tokens->displayOpacity});
    float op = 1.f;
    if (o.valid) {
        if (o.value.IsHolding<VtFloatArray>() && !o.value.UncheckedGet<VtFloatArray>().empty())
            op = o.value.UncheckedGet<VtFloatArray>()[0];
        else if (o.value.IsHolding<float>())
            op = o.value.UncheckedGet<float>();
    }
    std::lock_guard<std::mutex> lock(_mutex);
    _displayColor = col;
    _hasDisplayColor = ok;
    _displayOpacity = op;
}

GfMatrix4d HdPbrt_InterpolateXform(const GfMatrix4d &a, const GfMatrix4d &b, double u)
{
    if (a == b || u <= 0)
        return a;
    if (u >= 1)
        return b;
    GfTransform ta(a), tb(b);
    GfTransform r;
    r.SetScale(ta.GetScale() + u * (tb.GetScale() - ta.GetScale()));
    r.SetTranslation(ta.GetTranslation() + u * (tb.GetTranslation() - ta.GetTranslation()));
    r.SetPivotPosition(ta.GetPivotPosition() +
                       u * (tb.GetPivotPosition() - ta.GetPivotPosition()));
    r.SetPivotOrientation(ta.GetPivotOrientation());
    r.SetScaleOrientation(ta.GetScaleOrientation());
    const GfQuatd qa = ta.GetRotation().GetQuat(), qb = tb.GetRotation().GetQuat();
    r.SetRotation(GfRotation(GfSlerp(u, qa, qb)));
    return r.GetMatrix();
}

GfMatrix4d HdPbrt_XformAt(const HdTimeSampleArray<GfMatrix4d, 4> &sa, float t)
{
    if (sa.count == 0)
        return GfMatrix4d(1.0);
    if (sa.count == 1 || t <= sa.times[0])
        return sa.values[0];
    if (t >= sa.times[sa.count - 1])
        return sa.values[sa.count - 1];
    size_t i = 0;
    while (i + 1 < sa.count && sa.times[i + 1] < t)
        ++i;
    const float t0 = sa.times[i], t1 = sa.times[i + 1];
    const double u = t1 > t0 ? double(t - t0) / double(t1 - t0) : 0.0;
    return HdPbrt_InterpolateXform(sa.values[i], sa.values[i + 1], u);
}

// pbrt transform text for keys at the shutter times: a plain Transform when
// they're all equal, pbrt's own two-key ActiveTransform pair for two keys.
// For more keys it returns "" and *motionAttr gets the Attribute line for
// pbrt patch 6 (N-key instance motion; it must precede an ObjectInstance).
static std::string _MotionTransform(const std::vector<GfMatrix4d> &keys,
                                    const std::vector<float> &times,
                                    std::string *motionAttr)
{
    bool moving = false;
    for (size_t k = 1; k < keys.size(); ++k)
        moving |= keys[k] != keys[0];
    if (!moving)
        return "Transform " + HdPbrt_Matrix(keys[0]);
    if (keys.size() == 2)
        return "ActiveTransform StartTime Transform " + HdPbrt_Matrix(keys[0]) +
               " ActiveTransform EndTime Transform " + HdPbrt_Matrix(keys[1]) +
               " ActiveTransform All";
    std::string m = "Attribute \"shape\" \"float hdpbrtmotionxforms\" [";
    for (const GfMatrix4d &k : keys) {
        std::string one = HdPbrt_Matrix(k);  // "[a b ...]"
        m += " " + one.substr(1, one.size() - 2);
    }
    m += " ] \"float hdpbrtmotiontimes\" [";
    char buf[32];
    for (float t : times) {
        snprintf(buf, sizeof(buf), " %.9g", t);
        m += buf;
    }
    m += " ]";
    *motionAttr = m;
    return "";
}

// N evenly spaced times over the shutter (N >= 2), or {open} without blur.
std::vector<float> HdPbrt_ShutterTimes(GfVec2f shutter, int n)
{
    if (shutter[0] == shutter[1] || n < 2)
        return {shutter[0]};
    std::vector<float> t(n);
    for (int k = 0; k < n; ++k)
        t[k] = shutter[0] + (shutter[1] - shutter[0]) * float(k) / float(n - 1);
    return t;
}

void HdPbrtGeom::Write(std::ostream &os, HdRenderIndex &index,
                       TfTokenVector const &renderTags, HdPbrtWriteContext &ctx) const
{
    std::lock_guard<std::mutex> lock(_mutex);
    HdRprim const &prim = _Prim();
    if (!_visible)
        return;
    if (!renderTags.empty() &&
        std::find(renderTags.begin(), renderTags.end(), prim.GetRenderTag()) == renderTags.end())
        return;
    // Transform keys over the shutter (one without motion blur).
    const GfVec2f shutter = _delegate->GetShutter();
    const bool blur = shutter[0] != shutter[1];
    const std::vector<float> xTimes = HdPbrt_ShutterTimes(
        shutter, std::max(2, _delegate->GetSetting<int>(HdPbrtSettingsTokens->xformSamples, 2)));
    const size_t nx = xTimes.size();
    std::vector<GfMatrix4d> xf(nx);
    for (size_t k = 0; k < nx; ++k)
        xf[k] = HdPbrt_XformAt(_xform, xTimes[k]);
    std::vector<VtMatrix4dArray> inst;
    if (_instanced) {
        if (auto *pi = static_cast<HdPbrtInstancer *>(index.GetInstancer(prim.GetInstancerId())))
            for (size_t k = 0; k < nx; ++k)
                inst.push_back(pi->ComputeInstanceTransforms(prim.GetId(), xTimes[k]));
        if (inst.empty() || inst[0].empty())
            return;
        for (size_t k = 1; k < nx; ++k)
            if (inst[k].size() != inst[0].size())  // the instance count changes
                inst[k] = inst[0];
    }

    HdPbrtMaterialDesc mat = HdPbrt_DefaultMaterial();
    const SdfPath &matId = prim.GetMaterialId();
    if (!matId.IsEmpty()) {
        if (auto *m = static_cast<HdPbrtMaterial *>(
                index.GetSprim(HdPrimTypeTokens->material, matId))) {
            mat = m->GetDesc();
            if (!mat.textures.empty() && ctx.texturesWritten.insert(matId).second)
                os << mat.textures;
        }
    }
    std::string matLine =
        HdPbrt_MaterialLine(mat, _hasDisplayColor ? &_displayColor : nullptr);
    // Transmissive materials with an interior medium (pbrt's volpath).
    if (!mat.mediumName.empty())
        matLine += "  MediumInterface " + HdPbrt_Quote(mat.mediumName) + " \"\"\n";
    const bool emissive =
        _CanEmit() && (mat.emission[0] > 0 || mat.emission[1] > 0 || mat.emission[2] > 0);

    // hdpbrtprimid: Hydra's prim id, streamed back by pbrt for the primId AOV
    // (selection). Alpha cut-outs go on the shapes too.
    std::ostringstream extra;
    extra << " \"integer hdpbrtprimid\" " << prim.GetPrimId();
    // Light linking: the collections this prim is in (pbrt patch 9).
    extra << _delegate->LinkParam(_categories);
    float alpha = mat.opacity * _displayOpacity;
    if (!mat.opacityTexture.empty())
        extra << " \"texture alpha\" " << HdPbrt_Quote(mat.opacityTexture);
    else if (alpha < 0.999f)
        extra << " \"float alpha\" " << std::max(alpha, 0.f);
    const std::string shapes = _Shapes(extra.str());
    if (shapes.empty())
        return;
    os << "# " << prim.GetId().GetString() << "\n";
    if (!_instanced) {
        std::string motionAttr;
        // Area lights can't be instanced in pbrt: emissive meshes get two keys.
        const std::vector<GfMatrix4d> keys =
            emissive && nx > 2 ? std::vector<GfMatrix4d>{xf.front(), xf.back()} : xf;
        const std::vector<float> keyTimes =
            emissive && nx > 2 ? std::vector<float>{xTimes.front(), xTimes.back()} : xTimes;
        const std::string xform = _MotionTransform(keys, keyTimes, &motionAttr);
        if (motionAttr.empty()) {
            os << "AttributeBegin\n  " << xform << "\n  " << matLine;
            if (emissive)
                os << "  AreaLightSource \"diffuse\" \"rgb L\" " << HdPbrt_Rgb(mat.emission)
                   << "\n";
            os << "  " << shapes << "AttributeEnd\n";
        } else {
            // N-key transform motion only exists for object instances.
            std::string name = "hdpbrt_obj_" + std::to_string(ctx.objectCounter++);
            os << "ObjectBegin " << HdPbrt_Quote(name) << "\n  Identity\n  " << matLine << "  "
               << shapes << "ObjectEnd\nAttributeBegin " << motionAttr
               << " ObjectInstance " << HdPbrt_Quote(name) << " AttributeEnd\n";
        }
        ++ctx.numMeshes;
    } else {
        std::string name = "hdpbrt_obj_" + std::to_string(ctx.objectCounter++);
        os << "ObjectBegin " << HdPbrt_Quote(name) << "\n  Identity\n  " << matLine << "  "
           << shapes << "ObjectEnd\n";
        // hdpbrtinstanceid: the instance index Hydra expects in the instanceId
        // AOV (flattened over nested instancers, see HdPbrtInstancer).
        std::vector<GfMatrix4d> keys(nx);
        for (size_t i = 0; i < inst[0].size(); ++i) {
            for (size_t k = 0; k < nx; ++k)
                keys[k] = xf[k] * inst[k][i];
            std::string motionAttr;
            const std::string xform = _MotionTransform(keys, xTimes, &motionAttr);
            os << "AttributeBegin " << xform << (xform.empty() ? "" : " ") << motionAttr
               << (motionAttr.empty() ? "" : " ")
               << "Attribute \"shape\" \"integer hdpbrtinstanceid\" " << i
               << " ObjectInstance " << HdPbrt_Quote(name) << " AttributeEnd\n";
        }
        ctx.numInstances += inst[0].size();
    }
}


PXR_NAMESPACE_CLOSE_SCOPE
