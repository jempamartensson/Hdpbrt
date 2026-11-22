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
    if (*dirtyBits & HdChangeTracker::DirtyVisibility)
        _visible = delegate->GetVisible(GetId());
    if (HdChangeTracker::IsAnyPrimvarDirty(*dirtyBits, GetId())) {
        for (const HdPrimvarDescriptor &pv :
             delegate->GetPrimvarDescriptors(GetId(), HdInterpolationInstance)) {
            if (HdChangeTracker::IsPrimvarDirty(*dirtyBits, GetId(), pv.name)) {
                VtValue v = delegate->Get(GetId(), pv.name);
                if (!v.IsEmpty())
                    _primvars[pv.name] = v;
            }
        }
    }
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

VtMatrix4dArray HdPbrtInstancer::ComputeInstanceTransforms(SdfPath const &prototypeId)
{
    std::lock_guard<std::mutex> lock(_mutex);
    if (!_visible)
        return {};
    GfMatrix4d instancerXf = GetDelegate()->GetInstancerTransform(GetId());
    VtIntArray indices = GetDelegate()->GetInstanceIndices(GetId(), prototypeId);
    VtMatrix4dArray xfs(indices.size());
    for (size_t i = 0; i < indices.size(); ++i)
        xfs[i] = instancerXf;

    auto find = [&](TfToken const &t) -> const VtValue * {
        auto it = _primvars.find(t);
        return it == _primvars.end() ? nullptr : &it->second;
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
        static_cast<HdPbrtInstancer *>(parent)->ComputeInstanceTransforms(GetId());
    VtMatrix4dArray result(parentXfs.size() * xfs.size());
    for (size_t i = 0; i < parentXfs.size(); ++i)
        for (size_t j = 0; j < xfs.size(); ++j)
            result[i * xfs.size() + j] = xfs[j] * parentXfs[i];
    return result;
}

PXR_NAMESPACE_CLOSE_SCOPE
