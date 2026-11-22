// SPDX-License-Identifier: Apache-2.0
// Converts implicit surfaces (sphere, cube, cone, ...) to meshes for hdPbrt,
// following USD's hdEmbree.
#include "pxr/pxr.h"
#include "pxr/imaging/hd/retainedDataSource.h"
#include "pxr/imaging/hd/sceneIndexPlugin.h"
#include "pxr/imaging/hd/sceneIndexPluginRegistry.h"
#include "pxr/imaging/hd/tokens.h"
#include "pxr/imaging/hdsi/implicitSurfaceSceneIndex.h"
#include "pxr/imaging/hdsi/lightLinkingSceneIndex.h"

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
            HdPrimTypeTokens->sphere, toMesh, HdPrimTypeTokens->cube, toMesh,
            HdPrimTypeTokens->cone, toMesh, HdPrimTypeTokens->cylinder, toMesh,
            HdPrimTypeTokens->capsule, toMesh, HdPrimTypeTokens->plane, toMesh);
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

TF_REGISTRY_FUNCTION(TfType)
{
    HdSceneIndexPluginRegistry::Define<HdPbrt_ImplicitSurfaceSceneIndexPlugin>();
    HdSceneIndexPluginRegistry::Define<HdPbrt_LightLinkingSceneIndexPlugin>();
}

TF_REGISTRY_FUNCTION(HdSceneIndexPlugin)
{
    HdSceneIndexPluginRegistry::GetInstance().RegisterSceneIndexForRenderer(
        "pbrt", TfToken("HdPbrt_ImplicitSurfaceSceneIndexPlugin"), nullptr, 0,
        HdSceneIndexPluginRegistry::InsertionOrderAtStart);
    // After procedurals that might create lights (hdPrman uses phase 4 too).
    HdSceneIndexPluginRegistry::GetInstance().RegisterSceneIndexForRenderer(
        "pbrt", TfToken("HdPbrt_LightLinkingSceneIndexPlugin"), nullptr, 4,
        HdSceneIndexPluginRegistry::InsertionOrderAtStart);
}

PXR_NAMESPACE_CLOSE_SCOPE
