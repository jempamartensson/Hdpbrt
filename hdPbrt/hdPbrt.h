// hdPbrt: a Hydra render delegate for pbrt-v4.
//
// The delegate keeps a light-weight copy of the Hydra scene (meshes are written
// to binary PLY files as they sync), writes a pbrt-v4 scene description when a
// render starts, and runs the pbrt executable in a child process. pbrt streams
// its progressive image back over its display-server socket protocol.
//
// SPDX-License-Identifier: Apache-2.0
#ifndef HDPBRT_H
#define HDPBRT_H

#include "pxr/pxr.h"
#include "pxr/base/gf/matrix4d.h"
#include "pxr/base/gf/rect2i.h"
#include "pxr/base/gf/vec2f.h"
#include "pxr/base/gf/vec3i.h"
#include "pxr/base/gf/vec3f.h"
#include "pxr/base/tf/staticTokens.h"
#include "pxr/base/vt/array.h"
#include "pxr/imaging/hd/instancer.h"
#include "pxr/imaging/hd/light.h"
#include "pxr/imaging/hd/material.h"
#include "pxr/imaging/hd/mesh.h"
#include "pxr/imaging/hd/renderDelegate.h"
#include "pxr/imaging/hd/renderPass.h"
#include "pxr/imaging/hd/rendererPlugin.h"
#include "pxr/imaging/hd/vtBufferSource.h"

#include "session.h"

#include <atomic>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <sstream>
#include <string>

PXR_NAMESPACE_OPEN_SCOPE

#define HDPBRT_SETTINGS_TOKENS                  \
    ((pixelSamples, "pbrt:pixelSamples"))       \
    ((maxDepth, "pbrt:maxDepth"))               \
    ((integrator, "pbrt:integrator"))           \
    ((sampler, "pbrt:sampler"))                 \
    ((threads, "pbrt:threads"))                 \
    ((regularize, "pbrt:regularize"))           \
    ((exportPath, "pbrt:exportPath"))           \
    ((pbrtExecutable, "pbrt:executable"))

TF_DECLARE_PUBLIC_TOKENS(HdPbrtSettingsTokens, HDPBRT_SETTINGS_TOKENS);

class HdPbrtRenderDelegate;
class HdPbrtMesh;
class HdPbrtLight;

// Format helpers shared by the scene writers.
std::string HdPbrt_Quote(const std::string &s);
std::string HdPbrt_Matrix(const GfMatrix4d &m);
std::string HdPbrt_Rgb(const GfVec3f &c);

struct HdPbrtWriteContext {
    std::set<SdfPath> texturesWritten;
    int objectCounter = 0;
    size_t numMeshes = 0, numInstances = 0;
};

class HdPbrtRenderParam : public HdRenderParam {
public:
    explicit HdPbrtRenderParam(HdPbrtRenderDelegate *d) : delegate(d) {}
    void Edit() { ++sceneVersion; }
    std::atomic<int> sceneVersion{1};
    HdPbrtRenderDelegate *delegate;
};

// ---------------------------------------------------------------------------
// Material: UsdPreviewSurface / MaterialX standard_surface / OpenPBR mapped
// to pbrt's diffuse, coateddiffuse, conductor and dielectric materials.
struct HdPbrtMaterialDesc {
    std::string type = "coateddiffuse";
    GfVec3f baseColor{0.18f, 0.18f, 0.18f};
    std::string baseColorTexture;    // named pbrt texture, or empty
    bool baseColorFromPrimvar = false;  // use the mesh's displayColor
    std::string otherParams;         // remaining pbrt parameters
    std::string textures;            // Texture directives this material needs
    GfVec3f emission{0, 0, 0};
    float opacity = 1.f;
    std::string opacityTexture;
    TfToken uvPrimvar;
};

class HdPbrtMaterial final : public HdMaterial {
public:
    explicit HdPbrtMaterial(SdfPath const &id);
    void Sync(HdSceneDelegate *sceneDelegate, HdRenderParam *renderParam,
              HdDirtyBits *dirtyBits) override;
    HdDirtyBits GetInitialDirtyBitsMask() const override { return AllDirty; }

    HdPbrtMaterialDesc GetDesc() const {
        std::lock_guard<std::mutex> l(_mutex);
        return _desc;
    }

private:
    mutable std::mutex _mutex;
    HdPbrtMaterialDesc _desc;
};

// Writes the pbrt "Material" line for a material (or the default material).
std::string HdPbrt_MaterialLine(const HdPbrtMaterialDesc &m,
                                const GfVec3f *displayColor);
HdPbrtMaterialDesc HdPbrt_DefaultMaterial();

// ---------------------------------------------------------------------------
class HdPbrtInstancer final : public HdInstancer {
public:
    HdPbrtInstancer(HdSceneDelegate *delegate, SdfPath const &id);
    ~HdPbrtInstancer() override;
    void Sync(HdSceneDelegate *sceneDelegate, HdRenderParam *renderParam,
              HdDirtyBits *dirtyBits) override;
    VtMatrix4dArray ComputeInstanceTransforms(SdfPath const &prototypeId);

private:
    std::mutex _mutex;
    std::map<TfToken, VtValue> _primvars;
    bool _visible = true;
};

// ---------------------------------------------------------------------------
class HdPbrtMesh final : public HdMesh {
public:
    HdPbrtMesh(SdfPath const &id, HdPbrtRenderDelegate *delegate);
    ~HdPbrtMesh() override;

    HdDirtyBits GetInitialDirtyBitsMask() const override;
    void Sync(HdSceneDelegate *sceneDelegate, HdRenderParam *renderParam,
              HdDirtyBits *dirtyBits, TfToken const &reprToken) override;
    void Finalize(HdRenderParam *renderParam) override;

    // Append this mesh's pbrt description to the scene.
    void Write(std::ostream &os, HdRenderIndex &index,
               TfTokenVector const &renderTags, HdPbrtWriteContext &ctx) const;

protected:
    HdDirtyBits _PropagateDirtyBits(HdDirtyBits bits) const override { return bits; }
    void _InitRepr(TfToken const &reprToken, HdDirtyBits *dirtyBits) override {}

private:
    void _WritePly(HdSceneDelegate *sceneDelegate, HdDirtyBits dirtyBits);

    HdPbrtRenderDelegate *_delegate;
    mutable std::mutex _mutex;
    std::string _plyPath;
    size_t _numTriangles = 0;
    GfMatrix4d _xform{1.0};
    bool _visible = true;
    GfVec3f _displayColor{0.18f, 0.18f, 0.18f};
    bool _hasDisplayColor = false;
    float _displayOpacity = 1.f;
    VtMatrix4dArray _instanceXforms;
    bool _instanced = false;
    int _plyVersion = 0;
};

// ---------------------------------------------------------------------------
class HdPbrtLight final : public HdLight {
public:
    HdPbrtLight(SdfPath const &id, TfToken const &lightType,
                HdPbrtRenderDelegate *delegate);
    ~HdPbrtLight() override;
    void Sync(HdSceneDelegate *sceneDelegate, HdRenderParam *renderParam,
              HdDirtyBits *dirtyBits) override;
    HdDirtyBits GetInitialDirtyBitsMask() const override { return AllDirty; }
    void Finalize(HdRenderParam *renderParam) override;

    // Append this light's pbrt description to the scene.
    void Write(std::ostream &os) const;
    bool IsDome() const { return _type == HdPrimTypeTokens->domeLight; }

private:
    HdPbrtRenderDelegate *_delegate;
    TfToken _type;
    mutable std::mutex _mutex;
    std::string _text;
};

// ---------------------------------------------------------------------------
class HdPbrtRenderBuffer;

class HdPbrtRenderPass final : public HdRenderPass {
public:
    HdPbrtRenderPass(HdRenderIndex *index, HdRprimCollection const &collection,
                     HdPbrtRenderDelegate *delegate);
    ~HdPbrtRenderPass() override;
    bool IsConverged() const override;

protected:
    void _Execute(HdRenderPassStateSharedPtr const &renderPassState,
                  TfTokenVector const &renderTags) override;

private:
    HdPbrtRenderDelegate *_delegate;
    int _lastSceneVersion = -1;
    int _lastSettingsVersion = -1;
    GfMatrix4d _view{0.0}, _proj{0.0};
    GfRect2i _dataWindow;
    TfTokenVector _renderTags;
    uint64_t _imageVersion = 0;
    std::vector<float> _image;
    bool _converged = false;
    bool _started = false;
    bool _pending = false;
    double _changeTime = 0, _lastStart = 0;
};

// ---------------------------------------------------------------------------
class HdPbrtRenderDelegate final : public HdRenderDelegate {
public:
    HdPbrtRenderDelegate();
    explicit HdPbrtRenderDelegate(HdRenderSettingsMap const &settingsMap);
    ~HdPbrtRenderDelegate() override;

    HdRenderParam *GetRenderParam() const override { return _renderParam.get(); }
    const TfTokenVector &GetSupportedRprimTypes() const override;
    const TfTokenVector &GetSupportedSprimTypes() const override;
    const TfTokenVector &GetSupportedBprimTypes() const override;
    HdResourceRegistrySharedPtr GetResourceRegistry() const override;
    HdRenderSettingDescriptorList GetRenderSettingDescriptors() const override;
    VtDictionary GetRenderStats() const override;

    HdRenderPassSharedPtr CreateRenderPass(HdRenderIndex *index,
                                           HdRprimCollection const &collection) override;
    HdInstancer *CreateInstancer(HdSceneDelegate *delegate, SdfPath const &id) override;
    void DestroyInstancer(HdInstancer *instancer) override;
    HdRprim *CreateRprim(TfToken const &typeId, SdfPath const &rprimId) override;
    void DestroyRprim(HdRprim *rPrim) override;
    HdSprim *CreateSprim(TfToken const &typeId, SdfPath const &sprimId) override;
    HdSprim *CreateFallbackSprim(TfToken const &typeId) override;
    void DestroySprim(HdSprim *sPrim) override;
    HdBprim *CreateBprim(TfToken const &typeId, SdfPath const &bprimId) override;
    HdBprim *CreateFallbackBprim(TfToken const &typeId) override;
    void DestroyBprim(HdBprim *bPrim) override;
    void CommitResources(HdChangeTracker *tracker) override {}
    HdAovDescriptor GetDefaultAovDescriptor(TfToken const &name) const override;
    TfTokenVector GetMaterialRenderContexts() const override;
    TfToken GetMaterialBindingPurpose() const override { return HdTokens->full; }
    void SetRenderSetting(TfToken const &key, VtValue const &value) override;

    // --- used by the prims and the render pass ---
    void Register(HdPbrtMesh *m);
    void Unregister(HdPbrtMesh *m);
    void Register(HdPbrtLight *l);
    void Unregister(HdPbrtLight *l);

    const std::string &GetTempDir() const { return _tempDir; }
    std::string NewTempPath(const std::string &stem, const std::string &ext);

    // Convert a lat-long environment image to pbrt's equal-area square
    // layout (cached). Returns an empty string on failure.
    std::string GetEquiAreaTexture(const std::string &latlong);

    // Write the full pbrt scene to a file and start rendering it.
    bool StartRender(HdRenderIndex *index, const GfMatrix4d &view,
                     const GfMatrix4d &proj, int width, int height,
                     TfTokenVector const &renderTags);
    void StopRender() { _session.Stop(); }
    hdpbrt::PbrtSession &GetSession() { return _session; }
    int GetSpp() const;

    template <typename T>
    T GetSetting(TfToken const &key, T const &def) const;

private:
    void _Init();
    std::string _PbrtExecutable() const;

    std::unique_ptr<HdPbrtRenderParam> _renderParam;
    HdRenderSettingDescriptorList _settingDescriptors;
    std::mutex _registryMutex;
    std::set<HdPbrtMesh *> _meshes;
    std::set<HdPbrtLight *> _lights;
    std::string _tempDir;
    std::atomic<int> _tempCounter{0};
    std::mutex _texMutex;
    std::map<std::string, std::string> _equiAreaCache;
    hdpbrt::PbrtSession _session;
    int _renderCount = 0;

    static std::mutex _resourceRegistryMutex;
    static std::atomic_int _counterResourceRegistry;
    static HdResourceRegistrySharedPtr _resourceRegistry;
};

// ---------------------------------------------------------------------------
class HdPbrtRendererPlugin final : public HdRendererPlugin {
public:
    HdPbrtRendererPlugin() = default;
    ~HdPbrtRendererPlugin() override = default;
    HdRenderDelegate *CreateRenderDelegate() override;
    HdRenderDelegate *CreateRenderDelegate(HdRenderSettingsMap const &settingsMap) override;
    void DeleteRenderDelegate(HdRenderDelegate *renderDelegate) override;
#if PXR_VERSION >= 2608
    bool IsSupported(const HdRendererCreateArgsSchema &args,
                     std::string *reasonWhyNot = nullptr) const override;
#elif PXR_VERSION >= 2505
    bool IsSupported(HdRendererCreateArgs const &args,
                     std::string *reasonWhyNot = nullptr) const override;
#else
    bool IsSupported(bool gpuEnabled = true) const override;
#endif
};

PXR_NAMESPACE_CLOSE_SCOPE

#endif
