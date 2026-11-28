// SPDX-License-Identifier: Apache-2.0
// Points -> pbrt spheres (diameter = width; fallback width 1, as in Storm).
#include "hdPbrt.h"

#include "pxr/imaging/hd/sceneDelegate.h"
#include "pxr/imaging/hd/renderIndex.h"
#include "pxr/imaging/hd/tokens.h"

#include <cstdio>

PXR_NAMESPACE_OPEN_SCOPE

HdPbrtPoints::HdPbrtPoints(SdfPath const &id, HdPbrtRenderDelegate *delegate)
    : HdPoints(id), HdPbrtGeom(delegate)
{
}

HdPbrtPoints::~HdPbrtPoints() = default;

void HdPbrtPoints::Finalize(HdRenderParam *renderParam)
{
    static_cast<HdPbrtRenderParam *>(renderParam)->Edit();
}

HdDirtyBits HdPbrtPoints::GetInitialDirtyBitsMask() const
{
    return HdChangeTracker::Clean | HdChangeTracker::InitRepr |
           HdChangeTracker::DirtyPoints | HdChangeTracker::DirtyTransform |
           HdChangeTracker::DirtyVisibility | HdChangeTracker::DirtyPrimvar |
           HdChangeTracker::DirtyWidths | HdChangeTracker::DirtyInstancer |
           HdChangeTracker::DirtyCategories | HdChangeTracker::DirtyMaterialId;
}

void HdPbrtPoints::Sync(HdSceneDelegate *sceneDelegate, HdRenderParam *renderParam,
                        HdDirtyBits *dirtyBits, TfToken const &reprToken)
{
    SdfPath const &id = GetId();
    static_cast<HdPbrtRenderParam *>(renderParam)->Edit();

    if (*dirtyBits & HdChangeTracker::DirtyMaterialId)
        SetMaterialId(sceneDelegate->GetMaterialId(id));
    if (HdChangeTracker::IsTransformDirty(*dirtyBits, id))
        _SyncTransform(sceneDelegate);
    if (*dirtyBits & HdChangeTracker::DirtyCategories)
        _SyncCategories(sceneDelegate);
    {
        std::lock_guard<std::mutex> lock(_mutex);
        if (HdChangeTracker::IsVisibilityDirty(*dirtyBits, id))
            _UpdateVisibility(sceneDelegate, dirtyBits);
        _visible = IsVisible();
    }
    if (HdChangeTracker::IsAnyPrimvarDirty(*dirtyBits, id) ||
        (*dirtyBits & HdChangeTracker::DirtyWidths)) {
        _SyncDisplayColor(sceneDelegate);
        VtVec3fArray points;
        VtValue pv = sceneDelegate->Get(id, HdTokens->points);
        if (pv.IsHolding<VtVec3fArray>())
            points = pv.UncheckedGet<VtVec3fArray>();
        HdPbrtPrimvar wPv = HdPbrt_FindPrimvar(sceneDelegate, id, {HdTokens->widths});
        VtFloatArray widths;
        if (wPv.valid && wPv.value.IsHolding<VtFloatArray>())
            widths = wPv.value.UncheckedGet<VtFloatArray>();
        else if (wPv.valid && wPv.value.IsHolding<float>())
            widths = VtFloatArray(1, wPv.value.UncheckedGet<float>());
        std::vector<std::string> shapes;
        std::vector<int> pointIndex;
        shapes.reserve(points.size());
        char buf[160];
        for (size_t i = 0; i < points.size(); ++i) {
            float w = widths.empty() ? 1.f
                      : widths.size() == points.size() ? widths[i] : widths[0];
            if (!(w > 0.f))
                continue;
            const GfVec3f &p = points[i];
            snprintf(buf, sizeof(buf),
                     "AttributeBegin Translate %.7g %.7g %.7g Shape \"sphere\" \"float radius\" %.7g",
                     p[0], p[1], p[2], 0.5f * w);
            shapes.push_back(buf);
            pointIndex.push_back(int(i));
        }
        std::lock_guard<std::mutex> lock(_mutex);
        _shapes = std::move(shapes);
        _pointIndex = std::move(pointIndex);
        _numPoints = points.size();
    }

    _UpdateInstancer(sceneDelegate, dirtyBits);
    HdInstancer::_SyncInstancerAndParents(sceneDelegate->GetRenderIndex(), GetInstancerId());
    _SyncInstancing(sceneDelegate);

    *dirtyBits &= ~HdChangeTracker::AllSceneDirtyBits;
}

std::string HdPbrtPoints::_Shapes(std::string const &extra) const
{
    std::string out;
    for (size_t i = 0; i < _shapes.size(); ++i)
        out += _shapes[i] + extra + _ShapePv(i) + " AttributeEnd\n";
    return out;
}

// Per point: vertex / varying (and uniform, one per point) values; constant
// ones go on every sphere.
std::vector<std::string> HdPbrtPoints::_ShapePrimvars(PrimvarList const &pvs,
                                                      std::set<std::string> *found) const
{
    PrimvarList names, perPoint;
    std::vector<std::vector<float>> constant, varying;
    for (const auto &pv : pvs) {
        HdInterpolation interp;
        std::vector<float> v;
        if (!HdPbrt_FetchPrimvar(_sd, GetId(), pv.first, pv.second, &interp, &v))
            continue;
        const size_t count = v.size() / pv.second;
        if (count == _numPoints && _numPoints > 1) {
            perPoint.push_back(pv);
            varying.push_back(std::move(v));
        } else if (count >= 1) {
            v.resize(pv.second);
            names.push_back(pv);
            constant.push_back(std::move(v));
        } else {
            continue;
        }
        found->insert(pv.first);
    }
    if (names.empty() && perPoint.empty())
        return {};
    if (perPoint.empty())
        return {HdPbrt_ShapePrimvarParams(names, constant, 1)};
    PrimvarList all = names;
    all.insert(all.end(), perPoint.begin(), perPoint.end());
    std::vector<std::string> out;
    out.reserve(_shapes.size());
    std::vector<std::vector<float>> values = constant;
    values.resize(all.size());
    for (size_t i = 0; i < _shapes.size(); ++i) {
        const size_t p = size_t(_pointIndex[i]);
        for (size_t k = 0; k < perPoint.size(); ++k) {
            const int w = perPoint[k].second;
            values[names.size() + k].assign(varying[k].begin() + p * w,
                                            varying[k].begin() + (p + 1) * w);
        }
        out.push_back(HdPbrt_ShapePrimvarParams(all, values, 1));
    }
    return out;
}

PXR_NAMESPACE_CLOSE_SCOPE
