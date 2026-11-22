// SPDX-License-Identifier: Apache-2.0
// Camera sprim that also keeps transform samples for camera motion blur.
#include "hdPbrt.h"

#include "pxr/imaging/hd/sceneDelegate.h"

PXR_NAMESPACE_OPEN_SCOPE

HdPbrtCamera::HdPbrtCamera(SdfPath const &id) : HdCamera(id) {}

void HdPbrtCamera::Sync(HdSceneDelegate *sceneDelegate, HdRenderParam *renderParam,
                        HdDirtyBits *dirtyBits)
{
    if ((*dirtyBits & DirtyTransform) && !GetId().IsEmpty()) {
        HdTimeSampleArray<GfMatrix4d, 4> sa;
        HdPbrt_SampleXform(
            [&](float a, float b, HdTimeSampleArray<GfMatrix4d, 4> *s) {
                sceneDelegate->SampleTransform(GetId(), a, b, s);
            },
            &sa);
        std::lock_guard<std::mutex> l(_mutex);
        _xform = sa;
    }
    HdCamera::Sync(sceneDelegate, renderParam, dirtyBits);
}

HdTimeSampleArray<GfMatrix4d, 4> HdPbrtCamera::GetTransformSamples() const
{
    std::lock_guard<std::mutex> l(_mutex);
    return _xform;
}

PXR_NAMESPACE_CLOSE_SCOPE
