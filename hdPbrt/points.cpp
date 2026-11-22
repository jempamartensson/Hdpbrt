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
        }
        std::lock_guard<std::mutex> lock(_mutex);
        _shapes = std::move(shapes);
    }

    _UpdateInstancer(sceneDelegate, dirtyBits);
    HdInstancer::_SyncInstancerAndParents(sceneDelegate->GetRenderIndex(), GetInstancerId());
    _SyncInstancing(sceneDelegate);

    *dirtyBits &= ~HdChangeTracker::AllSceneDirtyBits;
}

std::string HdPbrtPoints::_Shapes(std::string const &extra) const
{
    std::string out;
    for (const std::string &s : _shapes)
        out += s + extra + " AttributeEnd\n";
    return out;
}

PXR_NAMESPACE_CLOSE_SCOPE
