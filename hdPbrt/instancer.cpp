// SPDX-License-Identifier: Apache-2.0
// Instance transforms, following USD's hdEmbree instancer.
#include "hdPbrt.h"

#include "pxr/imaging/hd/sceneDelegate.h"
#include "pxr/imaging/hd/renderIndex.h"
#include "pxr/imaging/hd/tokens.h"
#include "pxr/base/gf/quatd.h"
#include "pxr/base/gf/quatf.h"
#include "pxr/base/gf/quath.h"
#include "pxr/base/gf/vec4f.h"

PXR_NAMESPACE_OPEN_SCOPE

HdPbrtInstancer::HdPbrtInstancer(HdSceneDelegate *delegate, SdfPath const &id)
    : HdInstancer(delegate, id) {}

HdPbrtInstancer::~HdPbrtInstancer() = default;

void HdPbrtInstancer::Sync(HdSceneDelegate *delegate, HdRenderParam *renderParam,
                           HdDirtyBits *dirtyBits)
{
    _UpdateInstancer(delegate, dirtyBits);
    std::lock_guard<std::mutex> lock(_mutex);
    const float w = HdPbrt_SampleWindow;
    if (*dirtyBits & HdChangeTracker::DirtyVisibility)
        _visible = delegate->GetVisible(GetId());
    if (*dirtyBits & HdChangeTracker::DirtyTransform)
        HdPbrt_SampleXform(
            [&](float a, float b, HdTimeSampleArray<GfMatrix4d, 4> *sa) {
                delegate->SampleInstancerTransform(GetId(), a, b, sa);
            },
            &_xform);
    if (HdChangeTracker::IsAnyPrimvarDirty(*dirtyBits, GetId())) {
        for (const HdPrimvarDescriptor &pv :
             delegate->GetPrimvarDescriptors(GetId(), HdInterpolationInstance)) {
            if (HdChangeTracker::IsPrimvarDirty(*dirtyBits, GetId(), pv.name)) {
                HdTimeSampleArray<VtValue, 4> sa;
                delegate->SamplePrimvar(GetId(), pv.name, -w, w, &sa);
                if (sa.count > 0 && !sa.values[0].IsEmpty())
                    _primvars[pv.name] = sa;
            }
        }
    }
}

// A time-sampled value at time t: interpolated between the neighboring
// samples, or the nearest sample when they can't be blended (different
// types or array sizes, e.g. a changing instance count).
static VtValue _ValueAt(const HdTimeSampleArray<VtValue, 4> &sa, float t)
{
    if (sa.count == 0)
        return VtValue();
    if (sa.count == 1 || t <= sa.times[0])
        return sa.values[0];
    if (t >= sa.times[sa.count - 1])
        return sa.values[sa.count - 1];
    size_t i = 0;
    while (i + 1 < sa.count && sa.times[i + 1] < t)
        ++i;
    const VtValue &v0 = sa.values[i], &v1 = sa.values[i + 1];
    const float t0 = sa.times[i], t1 = sa.times[i + 1];
    const float a = t1 > t0 ? (t - t0) / (t1 - t0) : 0.f;
    // Orientations: slerp per instance (linear blending would shrink and
    // skew the rotations between samples).
    if (v0.IsHolding<VtQuathArray>() && v1.IsHolding<VtQuathArray>() &&
        v0.GetArraySize() == v1.GetArraySize()) {
        const VtQuathArray &q0 = v0.UncheckedGet<VtQuathArray>();
        const VtQuathArray &q1 = v1.UncheckedGet<VtQuathArray>();
        VtQuathArray r(q0.size());
        for (size_t k = 0; k < q0.size(); ++k)
            r[k] = GfQuath(GfSlerp(double(a), GfQuatd(q0[k]), GfQuatd(q1[k])));
        return VtValue(r);
    }
    if (v0.IsHolding<VtQuatfArray>() && v1.IsHolding<VtQuatfArray>() &&
        v0.GetArraySize() == v1.GetArraySize()) {
        const VtQuatfArray &q0 = v0.UncheckedGet<VtQuatfArray>();
        const VtQuatfArray &q1 = v1.UncheckedGet<VtQuatfArray>();
        VtQuatfArray r(q0.size());
        for (size_t k = 0; k < q0.size(); ++k)
            r[k] = GfQuatf(GfSlerp(double(a), GfQuatd(q0[k]), GfQuatd(q1[k])));
        return VtValue(r);
    }
    if (v0.GetType() == v1.GetType() && v0.GetArraySize() == v1.GetArraySize()) {
        VtValue r = HdResampleNeighbors(a, v0, v1);
        if (!r.IsEmpty())
            return r;
    }
    return a < 0.5f ? v0 : v1;
}

template <typename T>
static bool _Sample(const VtValue &v, int index, T *out)
{
    if (!v.IsHolding<VtArray<T>>())
        return false;
    const VtArray<T> &a = v.UncheckedGet<VtArray<T>>();
    if (index < 0 || size_t(index) >= a.size())
        return false;
    *out = a[index];
    return true;
}

void HdPbrtInstancer::UpdateInstanceIndices(SdfPath const &prototypeId)
{
    VtIntArray indices = GetDelegate()->GetInstanceIndices(GetId(), prototypeId);
    // Instancers have no categories dirty bit; refresh them with the indices.
    std::vector<VtArray<TfToken>> categories = GetDelegate()->GetInstanceCategories(GetId());
    {
        std::lock_guard<std::mutex> lock(_mutex);
        _indices[prototypeId] = indices;
        _categories = std::move(categories);
    }
    if (GetParentId().IsEmpty())
        return;
    if (HdInstancer *parent = GetDelegate()->GetRenderIndex().GetInstancer(GetParentId()))
        static_cast<HdPbrtInstancer *>(parent)->UpdateInstanceIndices(GetId());
}

VtMatrix4dArray HdPbrtInstancer::ComputeInstanceTransforms(SdfPath const &prototypeId,
                                                           float time)
{
    std::lock_guard<std::mutex> lock(_mutex);
    if (!_visible)
        return {};
    auto it = _indices.find(prototypeId);
    if (it == _indices.end())
        return {};
    const VtIntArray &indices = it->second;
    GfMatrix4d instancerXf = HdPbrt_XformAt(_xform, time);
    VtMatrix4dArray xfs(indices.size());
    for (size_t i = 0; i < indices.size(); ++i)
        xfs[i] = instancerXf;

    std::map<TfToken, VtValue> values;
    for (auto const &kv : _primvars)
        values[kv.first] = _ValueAt(kv.second, time);
    auto find = [&](TfToken const &t) -> const VtValue * {
        auto it = values.find(t);
        return it == values.end() ? nullptr : &it->second;
    };
    if (const VtValue *v = find(HdInstancerTokens->instanceTranslations)) {
        for (size_t i = 0; i < indices.size(); ++i) {
            GfVec3f t; GfVec3d td;
            GfMatrix4d m(1);
            if (_Sample(*v, indices[i], &t)) m.SetTranslate(GfVec3d(t));
            else if (_Sample(*v, indices[i], &td)) m.SetTranslate(td);
            else continue;
            xfs[i] = m * xfs[i];
        }
    }
    if (const VtValue *v = find(HdInstancerTokens->instanceRotations)) {
        for (size_t i = 0; i < indices.size(); ++i) {
            GfQuatd q; GfQuatf qf; GfQuath qh; GfVec4f q4;
            if (_Sample(*v, indices[i], &qh)) q = GfQuatd(qh);
            else if (_Sample(*v, indices[i], &qf)) q = GfQuatd(qf);
            else if (_Sample(*v, indices[i], &q4)) q = GfQuatd(q4[0], q4[1], q4[2], q4[3]);
            else if (!_Sample(*v, indices[i], &q)) continue;
            GfMatrix4d m(1);
            m.SetRotate(q);
            xfs[i] = m * xfs[i];
        }
    }
    if (const VtValue *v = find(HdInstancerTokens->instanceScales)) {
        for (size_t i = 0; i < indices.size(); ++i) {
            GfVec3f s; GfVec3d sd;
            GfMatrix4d m(1);
            if (_Sample(*v, indices[i], &s)) m.SetScale(GfVec3d(s));
            else if (_Sample(*v, indices[i], &sd)) m.SetScale(sd);
            else continue;
            xfs[i] = m * xfs[i];
        }
    }
    if (const VtValue *v = find(HdInstancerTokens->instanceTransforms)) {
        for (size_t i = 0; i < indices.size(); ++i) {
            GfMatrix4d m;
            if (_Sample(*v, indices[i], &m))
                xfs[i] = m * xfs[i];
        }
    }

    if (GetParentId().IsEmpty())
        return xfs;
    HdInstancer *parent = GetDelegate()->GetRenderIndex().GetInstancer(GetParentId());
    if (!parent)
        return xfs;
    VtMatrix4dArray parentXfs =
        static_cast<HdPbrtInstancer *>(parent)->ComputeInstanceTransforms(GetId(), time);
    VtMatrix4dArray result(parentXfs.size() * xfs.size());
    for (size_t i = 0; i < parentXfs.size(); ++i)
        for (size_t j = 0; j < xfs.size(); ++j)
            result[i * xfs.size() + j] = xfs[j] * parentXfs[i];
    return result;
}

std::vector<VtArray<TfToken>> HdPbrtInstancer::ComputeInstanceCategories(
    SdfPath const &prototypeId)
{
    std::vector<VtArray<TfToken>> cats;
    {
        std::lock_guard<std::mutex> lock(_mutex);
        if (!_visible)
            return {};
        auto it = _indices.find(prototypeId);
        if (it == _indices.end())
            return {};
        for (int idx : it->second)
            cats.push_back(idx >= 0 && size_t(idx) < _categories.size() ? _categories[idx]
                                                                       : VtArray<TfToken>());
    }
    if (GetParentId().IsEmpty())
        return cats;
    HdInstancer *parent = GetDelegate()->GetRenderIndex().GetInstancer(GetParentId());
    if (!parent)
        return cats;
    std::vector<VtArray<TfToken>> parentCats =
        static_cast<HdPbrtInstancer *>(parent)->ComputeInstanceCategories(GetId());
    std::vector<VtArray<TfToken>> result(parentCats.size() * cats.size());
    for (size_t i = 0; i < parentCats.size(); ++i)
        for (size_t j = 0; j < cats.size(); ++j) {
            VtArray<TfToken> &r = result[i * cats.size() + j];
            r = cats[j];
            for (TfToken const &t : parentCats[i])
                r.push_back(t);
        }
    return result;
}

PXR_NAMESPACE_CLOSE_SCOPE
