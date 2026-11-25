// SPDX-License-Identifier: Apache-2.0
// Converts the implicit cube and plane to (bilinear) meshes for hdPbrt.
// Spheres, cylinders, cones and capsules stay implicit: HdPbrtImplicit writes
// them as exact pbrt shapes (USD's meshes for them are coarse Catmull-Clark
// cages that subdivision shrinks).
#include "pxr/pxr.h"
#include "pxr/imaging/hd/retainedDataSource.h"
#include "pxr/imaging/hd/sceneIndexPlugin.h"
#include "pxr/imaging/hd/sceneIndexPluginRegistry.h"
#include "pxr/imaging/hd/tokens.h"
#include "pxr/imaging/hdsi/implicitSurfaceSceneIndex.h"
#include "pxr/imaging/hdsi/lightLinkingSceneIndex.h"
#include "pxr/imaging/hdsi/velocityMotionResolvingSceneIndex.h"

PXR_NAMESPACE_OPEN_SCOPE

class HdPbrt_ImplicitSurfaceSceneIndexPlugin : public HdSceneIndexPlugin {
public:
    HdPbrt_ImplicitSurfaceSceneIndexPlugin() = default;

protected:
    HdSceneIndexBaseRefPtr _AppendSceneIndex(const HdSceneIndexBaseRefPtr &inputScene,
                                             const HdContainerDataSourceHandle &) override
    {
        HdDataSourceBaseHandle const toMesh =
            HdRetainedTypedSampledDataSource<TfToken>::New(
                HdsiImplicitSurfaceSceneIndexTokens->toMesh);
        HdContainerDataSourceHandle const args = HdRetainedContainerDataSource::New(
            HdPrimTypeTokens->cube, toMesh, HdPrimTypeTokens->plane, toMesh);
        return HdsiImplicitSurfaceSceneIndex::New(inputScene, args);
    }
};

// Light linking: resolves the lights' lightLink/shadowLink collections into
// category ids on the lights and categories on the geometry (as hdPrman does).
class HdPbrt_LightLinkingSceneIndexPlugin : public HdSceneIndexPlugin {
public:
    HdPbrt_LightLinkingSceneIndexPlugin() = default;

protected:
    HdSceneIndexBaseRefPtr _AppendSceneIndex(const HdSceneIndexBaseRefPtr &inputScene,
                                             const HdContainerDataSourceHandle &) override
    {
        return HdsiLightLinkingSceneIndex::New(inputScene, nullptr);
    }
};

// Velocity motion blur: resolves points velocities/accelerations (and point
// instancer velocities) into time samples, using the stage's
// timeCodesPerSecond, so the usual deformation and instance blur applies to
// meshes whose point count changes every frame (simulations). As hdPrman.
class HdPbrt_VelocityMotionSceneIndexPlugin : public HdSceneIndexPlugin {
public:
    HdPbrt_VelocityMotionSceneIndexPlugin() = default;

protected:
    HdSceneIndexBaseRefPtr _AppendSceneIndex(const HdSceneIndexBaseRefPtr &inputScene,
                                             const HdContainerDataSourceHandle &) override
    {
        return HdsiVelocityMotionResolvingSceneIndex::New(inputScene, nullptr);
    }
};

TF_REGISTRY_FUNCTION(TfType)
{
    HdSceneIndexPluginRegistry::Define<HdPbrt_VelocityMotionSceneIndexPlugin>();
    HdSceneIndexPluginRegistry::Define<HdPbrt_ImplicitSurfaceSceneIndexPlugin>();
    HdSceneIndexPluginRegistry::Define<HdPbrt_LightLinkingSceneIndexPlugin>();
}

TF_REGISTRY_FUNCTION(HdSceneIndexPlugin)
{
    HdSceneIndexPluginRegistry::GetInstance().RegisterSceneIndexForRenderer(
        "pbrt", TfToken("HdPbrt_ImplicitSurfaceSceneIndexPlugin"), nullptr, 0,
        HdSceneIndexPluginRegistry::InsertionOrderAtStart);
    HdSceneIndexPluginRegistry::GetInstance().RegisterSceneIndexForRenderer(
        "pbrt", TfToken("HdPbrt_VelocityMotionSceneIndexPlugin"), nullptr, 2,
        HdSceneIndexPluginRegistry::InsertionOrderAtStart);
    // After procedurals that might create lights (hdPrman uses phase 4 too).
    HdSceneIndexPluginRegistry::GetInstance().RegisterSceneIndexForRenderer(
        "pbrt", TfToken("HdPbrt_LightLinkingSceneIndexPlugin"), nullptr, 4,
        HdSceneIndexPluginRegistry::InsertionOrderAtStart);
}

PXR_NAMESPACE_CLOSE_SCOPE
