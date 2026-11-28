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
#include "pxr/imaging/hd/basisCurves.h"
#include "pxr/imaging/hd/points.h"
#include "pxr/imaging/hd/volume.h"
#include "pxr/imaging/hd/field.h"
#include "pxr/imaging/hd/camera.h"
#include "pxr/base/gf/range3d.h"
#include "pxr/imaging/hd/renderDelegate.h"
#include "pxr/imaging/hd/renderPass.h"
#include "pxr/imaging/hd/rendererPlugin.h"
#include "pxr/imaging/hd/timeSampleArray.h"
#include "pxr/imaging/hd/vtBufferSource.h"

#include "session.h"

#include <atomic>
#include <cmath>
#include <chrono>
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
    ((depthOfField, "pbrt:depthOfField"))       \
    ((motionBlur, "pbrt:motionBlur"))           \
    ((xformSamples, "pbrt:xformSamples"))       \
    ((geoSamples, "pbrt:geoSamples"))           \
    ((subdivLevel, "pbrt:subdivLevel"))         \
    ((volumeDensity, "pbrt:volumeDensity"))     \
    ((volumeAlbedo, "pbrt:volumeAlbedo"))       \
    ((volumeTemperatureScale, "pbrt:volumeTemperatureScale")) \
    ((exportPath, "pbrt:exportPath"))           \
    ((materialPreview, "pbrt:materialPreview")) \
    ((displacement, "pbrt:displacement"))       \
    ((dicingRate, "pbrt:dicingRate"))           \
    ((dicingMaxTriangles, "pbrt:dicingMaxTriangles")) \
    ((pbrtExecutable, "pbrt:executable"))       \
    ((workingSpace, "pbrt:workingSpace"))

TF_DECLARE_PUBLIC_TOKENS(HdPbrtSettingsTokens, HDPBRT_SETTINGS_TOKENS);

// The rendering (working) color space: what RGB numbers in the scene mean
// (material and light colors, displayColor, COP images, raw textures) and what
// the color AOV holds. ACEScg by default, as Houdini/Karma and Blender with an
// ACES setup; "lin_rec709" (pbrt:workingSpace) for the Typhoon comparisons.
// Images are read in their own color space (file metadata, the shader's color
// space, else sRGB primaries, as Karma and Blender assume) and pbrt converts.
struct HdPbrtWorkingSpace {
    std::string pbrtName = "acescg";  // pbrt ColorSpace / "colorspace" name
    bool aces = true;
    // Linear Rec.709 -> working space (Bradford D65 -> ACES white for ACEScg,
    // as OCIO's and Karma's conversion); identity for lin_rec709.
    GfVec3f FromRec709(GfVec3f const &c) const;
    // Relative luminance weights of the working space's primaries.
    GfVec3f Luminance() const;
    float Luminance(GfVec3f const &c) const;
    static HdPbrtWorkingSpace FromSetting(std::string const &name);
};

// Instancer primvars and transforms are sampled over this window (frames
// relative to the current time) and resampled at the shutter times.
constexpr float HdPbrt_SampleWindow = 1.f;

// A time-sampled transform at time t: between neighboring samples, the
// translation, rotation (slerp) and scale are interpolated separately so
// rotations don't shrink. Clamps outside the samples; identity if empty.
GfMatrix4d HdPbrt_XformAt(const HdTimeSampleArray<GfMatrix4d, 4> &sa, float t);
GfMatrix4d HdPbrt_InterpolateXform(const GfMatrix4d &a, const GfMatrix4d &b, double u);

// Sample a transform over HdPbrt_SampleWindow for motion blur. sample(start,
// end, &sa) wraps the scene delegate's SampleTransform/SampleInstancerTransform.
// A time-varying transform is evaluated on a 1/16-frame grid: USD composes the
// transform exactly at each time (a rotateY orbit follows its arc), which
// interpolating authored samples of the composed matrix can't do. Delegates
// that only return authored samples (legacy path) are resampled instead.
template <typename F>
void HdPbrt_SampleXform(F &&sample, HdTimeSampleArray<GfMatrix4d, 4> *out)
{
    const float w = HdPbrt_SampleWindow;
    sample(-w, w, out);
    if (out->count < 2)
        return;
    bool varies = false;
    for (size_t k = 1; k < out->count; ++k)
        varies |= out->values[k] != out->values[0];
    if (!varies)
        return;
    const int n = int(32 * w);
    HdTimeSampleArray<GfMatrix4d, 4> dense;
    dense.Resize(n + 1);
    for (int k = 0; k <= n; ++k) {
        const float t = -w + 2.f * w * float(k) / float(n);
        // A zero-width interval counts as not time-varying; a tiny one has
        // its edges evaluated exactly.
        HdTimeSampleArray<GfMatrix4d, 4> one;
        sample(t, t + 1e-3f, &one);
        bool exact = false;
        for (size_t j = 0; j < one.count && !exact; ++j)
            if (std::fabs(one.times[j] - t) < 1e-4f) {
                dense.values[k] = one.values[j];
                exact = true;
            }
        if (!exact)
            dense.values[k] = HdPbrt_XformAt(*out, t);
        dense.times[k] = t;
    }
    *out = dense;
}

// Tabulate an IES profile (angle scale and normalization applied) as the
// one-channel theta x phi table pbrt's light shaping reads (patch 8).
bool HdPbrt_WriteIESTable(const std::string &iesPath, float angleScale, bool normalize,
                          const std::string &outPfm);

// Thin-lens parameters for pbrt's perspective camera, in world units.
// lensRadius 0 = pinhole (no depth of field).
struct HdPbrtLens {
    float lensRadius = 0.f, focusDistance = 0.f;
    std::string sensor;  // Film sensor parameters (camera pbrt:sensor), or ""
    bool operator==(HdPbrtLens const &o) const {
        return lensRadius == o.lensRadius && focusDistance == o.focusDistance &&
               sensor == o.sensor;
    }
    bool operator!=(HdPbrtLens const &o) const { return !(*this == o); }
};

class HdPbrtRenderDelegate;
class HdPbrtMesh;
class HdPbrtGeom;
class HdPbrtLight;

// Format helpers shared by the scene writers.
std::string HdPbrt_Quote(const std::string &s);
std::string HdPbrt_Matrix(const GfMatrix4d &m);
std::string HdPbrt_Rgb(const GfVec3f &c);

struct HdPbrtWriteContext {
    std::set<SdfPath> texturesWritten;
    // Screen-space dicing of displaced meshes (pbrt patch 39): the size of a
    // pixel (radians at the camera, or world units for an orthographic
    // camera), and the camera position in world space.
    float dicePixel = 0.f;
    bool diceOrtho = false;
    GfVec3d cameraPos{0, 0, 0};
    std::string diceView;  // " \"float hdpbrtdiceview\" [...] \"float hdpbrtdicewindow\" [...]"
    int objectCounter = 0;
    int volumeCounter = 0;
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
// to pbrt's diffuse, hdpbrtlayered, conductor and dielectric materials.
struct HdPbrtMaterialDesc {
    std::string type = "diffuse";
    GfVec3f baseColor{0.18f, 0.18f, 0.18f};
    std::string baseColorTexture;    // named pbrt texture, or empty
    bool baseColorFromPrimvar = false;  // use the mesh's displayColor
    std::string otherParams;         // remaining pbrt parameters
    bool writeReflectance = true;    // false: no "reflectance" (conductor with eta/k)
    std::string mediumName;          // interior medium (defined in `textures`), or empty
    std::string textures;            // Texture directives this material needs
    GfVec3f emission{0, 0, 0};
    // Varying emission (image or primvar; color x weight): a spectrum texture
    // for the area light ("hdpbrtltexture", pbrt patch 46), or empty.
    std::string emissionTexture;
    float opacity = 1.f;
    std::string opacityTexture;
    TfToken uvPrimvar;
    // Volume shading (Karma pyro shader): parameters for pbrt's nanovdb
    // medium (pbrt patch 35) and the fields it reads. Empty: not a pyro material.
    std::string volumeParams;
    TfToken densityField, fireField;
    // Infrared reflectance (pbrt patch 38): the diffuse reflectance above the
    // red edge (nm), for infrared renders; < 0: the RGB fit continues.
    float irReflectance = -1.f;
    float irEdge = 720.f;
    // True displacement (pbrt patch 39): a float texture along the normal,
    // displacement = scale * (texture + offset).
    std::string displacementTexture;
    float displacementScale = 1.f, displacementOffset = 0.f;
    // Vector displacement (MaterialX ND_displacement_vector3): the texture's
    // RGB in (dPdu, dPdv, N) space, d = vectorScale * rgb + vectorBias.
    bool displacementVector = false;
    GfVec3f displacementVectorScale{1.f}, displacementVectorBias{0.f};
    // Procedural displacement (pbrt patch 41): a compiled MaterialX graph
    // (mtlxGraph.cpp) in place of the texture, its images (file, encoding).
    std::string displacementGraph;
    std::vector<std::string> displacementGraphImages;
    // Primvars the graph reads (geompropvalue / geomcolor): name, width.
    std::vector<std::pair<std::string, int>> displacementGraphPrimvars;
    // Primvars the surface textures read ("hdpbrtprimvar" textures): name,
    // width. Meshes write them per vertex; other shapes get the defaults.
    std::vector<std::pair<std::string, int>> surfacePrimvars;
};

class HdPbrtMaterial final : public HdMaterial {
public:
    HdPbrtMaterial(SdfPath const &id, HdPbrtRenderDelegate *delegate);
    void Sync(HdSceneDelegate *sceneDelegate, HdRenderParam *renderParam,
              HdDirtyBits *dirtyBits) override;
    HdDirtyBits GetInitialDirtyBitsMask() const override { return AllDirty; }

    // The description to render: the preview one under pbrt:materialPreview.
    HdPbrtMaterialDesc GetDesc() const;

private:
    HdPbrtMaterialDesc _Build(VtValue const &materialResource) const;
    void _BuildDisplacement(HdMaterialNetworkMap const &map, HdPbrtMaterialDesc *result) const;

    HdPbrtRenderDelegate *_delegate;
    mutable std::mutex _mutex;
    HdPbrtMaterialDesc _desc;
    HdPbrtMaterialDesc _previewDesc;  // from the universal context (UsdPreviewSurface)
};

// True while HdPbrtMaterial fetches its preview network (material.cpp).
// Procedural displacement (pbrt patch 41): compiles the MaterialX graph
// driving `disp` (ND_displacement_float / _vector3: displacement x scale) into
// a pbrt "hdpbrtgraph" program; false (with the reason) for unsupported graphs.
bool HdPbrt_CompileDisplacementGraph(const HdMaterialNetwork2 &net, const HdMaterialNode2 &disp,
                                     HdPbrtRenderDelegate *delegate, std::string *program,
                                     std::vector<std::string> *images,
                                     std::vector<std::pair<std::string, int>> *primvars,
                                     std::string *error);
bool HdPbrt_PreviewContexts();

// Karma pyro shader node -> HdPbrtMaterialDesc volume fields (material.cpp).
bool HdPbrt_PyroVolume(const HdMaterialNetwork2 &net, const HdMaterialNode2 &n,
                       SdfPath const &id, HdPbrtMaterialDesc *d);

// Writes the pbrt "Material" line for a material (or the default material).
// irReflectance >= 0 overrides the material's (a geometry primvar).
// displayColorTexture: a texture of the shape's per-vertex displayColor,
// used in place of the constant when the base color reads displayColor.
std::string HdPbrt_MaterialLine(const HdPbrtMaterialDesc &m,
                                const GfVec3f *displayColor, float irReflectance = -1.f,
                                const std::string &displayColorTexture = std::string());
// The shared "hdpbrtprimvar" texture of displayColor (HdPbrt_MaterialLine).
extern const char *const HdPbrt_DisplayColorTexture;
HdPbrtMaterialDesc HdPbrt_DefaultMaterial();

// ---------------------------------------------------------------------------
class HdPbrtInstancer final : public HdInstancer {
public:
    HdPbrtInstancer(HdSceneDelegate *delegate, SdfPath const &id);
    ~HdPbrtInstancer() override;
    void Sync(HdSceneDelegate *sceneDelegate, HdRenderParam *renderParam,
              HdDirtyBits *dirtyBits) override;
    // Fetch and cache the instance indices for a prototype (and up the
    // parent instancer chain). Call while syncing the prototype.
    void UpdateInstanceIndices(SdfPath const &prototypeId);
    // Instance transforms (flattened over parent instancers) at a time
    // relative to the current frame, within HdPbrt_SampleWindow, from the
    // cached samples and indices (no scene delegate calls).
    VtMatrix4dArray ComputeInstanceTransforms(SdfPath const &prototypeId, float time = 0.f);
    // Light-linking categories per instance, flattened like the transforms
    // (a nested instance is in its own and its parents' collections).
    std::vector<VtArray<TfToken>> ComputeInstanceCategories(SdfPath const &prototypeId);

private:
    std::mutex _mutex;
    std::map<SdfPath, VtIntArray> _indices;
    std::vector<VtArray<TfToken>> _categories;  // by instance index
    VtArray<TfToken> _ownCategories;  // the instancer's own: all its instances
    std::map<TfToken, HdTimeSampleArray<VtValue, 4>> _primvars;
    HdTimeSampleArray<GfMatrix4d, 4> _xform;
    bool _visible = true;
};

// Primvar lookup shared by the geometry prims: the first of names found (by
// interpolation, face-varying first), indexed primvars expanded. anyVec2:
// fall back to any float2 primvar (texture coordinates).
struct HdPbrtPrimvar {
    VtValue value;
    HdInterpolation interp = HdInterpolationConstant;
    bool valid = false;
};
HdPbrtPrimvar HdPbrt_FindPrimvar(HdSceneDelegate *sd, SdfPath const &id,
                                 const std::vector<TfToken> &names, bool anyVec2 = false);
GfVec3f HdPbrt_AverageColor(VtValue const &v, bool *ok);
// Primvar `name` (Houdini's Cd falls back to displayColor) as `width` floats
// per element, with its interpolation. False when the prim doesn't have it
// (*unsupported: it has it, in a type that can't be read as floats).
bool HdPbrt_FetchPrimvar(HdSceneDelegate *sd, SdfPath const &id, std::string const &name,
                         int width, HdInterpolation *interp, std::vector<float> *vals,
                         bool *unsupported = nullptr);
// pbrt shape parameters for per-shape primvars (pbrt patch 44): `count`
// values (x width) of each primvar in `values`, named by `names`.
std::string HdPbrt_ShapePrimvarParams(std::vector<std::pair<std::string, int>> const &names,
                                      std::vector<std::vector<float>> const &values, int count);
// N evenly spaced times over the shutter (N >= 2), or {open} without blur.
std::vector<float> HdPbrt_ShutterTimes(GfVec2f shutter, int n);

// ---------------------------------------------------------------------------
// Uniform subdivision of a catmullClark or loop mesh (OpenSubdiv).
class HdPbrtSubdiv {
public:
    // nullptr when the scheme isn't refined or the topology is invalid.
    // fvarIndices: one face-varying channel (uv value index per face corner).
    static std::shared_ptr<HdPbrtSubdiv> Create(HdMeshTopology const &topology, int level,
                                                VtIntArray const *fvarIndices,
                                                SdfPath const &id);
    ~HdPbrtSubdiv();
    // The refined faces (holes dropped), same scheme and orientation.
    HdMeshTopology const &RefinedTopology() const { return _topology; }
    VtVec3fArray RefinePoints(VtVec3fArray const &points) const;
    VtVec2fArray RefineVertexUV(VtVec2fArray const &uv, bool varying) const;
    // values per face-varying value index; result per refined face corner.
    VtVec2fArray RefineFaceVaryingUV(VtVec2fArray const &values) const;

private:
    HdPbrtSubdiv();
    struct _Impl;
    std::unique_ptr<_Impl> _impl;
    HdMeshTopology _topology;
    bool _hasFVar = false;
};

// ---------------------------------------------------------------------------
// Shared by the geometry rprims (mesh, curves, points): transform samples,
// visibility, display color and instancing, and writing the shapes with their
// material, motion keys and instances. The rprim supplies its pbrt shapes.
class HdPbrtGeom {
public:
    explicit HdPbrtGeom(HdPbrtRenderDelegate *delegate);
    virtual ~HdPbrtGeom();

    // Append this prim's pbrt description to the scene.
    virtual void Write(std::ostream &os, HdRenderIndex &index, TfTokenVector const &renderTags,
                       HdPbrtWriteContext &ctx) const;

protected:
    virtual HdRprim const &_Prim() const = 0;
    // pbrt Shape lines in object space ("" for nothing); extra goes on every
    // Shape (prim id, alpha). Called with _mutex held.
    virtual std::string _Shapes(std::string const &extra) const = 0;
    // Emissive materials become area lights on these shapes (meshes).
    virtual bool _CanEmit() const { return false; }
    // Displaced by the material's displacement (meshes with uvs).
    virtual bool _CanDisplace() const { return false; }
    // A file of `width` floats per pbrt vertex of primvar `name` for
    // displacement graphs ("" when the prim lacks it). Called with _mutex held.
    virtual std::string _PrimvarFile(std::string const &name, int width) const { return {}; }
    // Primvars for "hdpbrtprimvar" textures on shapes other than meshes
    // (pbrt "hdpbrtshapeprimvars"): one parameter string per pbrt Shape line
    // that _Shapes writes, or a single one for all; `found` gets the names
    // the prim has. Called with _mutex held, when the scene is written.
    using PrimvarList = std::vector<std::pair<std::string, int>>;
    virtual std::vector<std::string> _ShapePrimvars(PrimvarList const &pvs,
                                                    std::set<std::string> *found) const {
        return {};
    }
    // Constant primvars only, the same for every shape (implicit prims).
    std::vector<std::string> _ConstantShapePrimvars(PrimvarList const &pvs,
                                                    std::set<std::string> *found) const;
    // The shape primvar parameters of pbrt Shape line i (for _Shapes).
    std::string const &_ShapePv(size_t i) const;

    void _SyncTransform(HdSceneDelegate *sd);
    void _SyncInstancing(HdSceneDelegate *sd);  // after the instancer synced
    void _SyncDisplayColor(HdSceneDelegate *sd);
    void _SyncCategories(HdSceneDelegate *sd);  // light-linking collections

    HdPbrtRenderDelegate *_delegate;
    mutable std::mutex _mutex;
    // Transform samples over HdPbrt_SampleWindow, resampled at the shutter
    // when the scene is written.
    HdTimeSampleArray<GfMatrix4d, 4> _xform;
    bool _visible = true;
    GfVec3f _displayColor{0.18f, 0.18f, 0.18f};
    bool _hasDisplayColor = false;
    bool _displayColorVarying = false;  // not constant (meshes: per-vertex texture)
    float _displayOpacity = 1.f;
    float _irReflectance = -1.f;  // primvar pbrt:irReflectance, < 0: none
    // Per-object dicing primvars (pbrt:dicingRate, karma:object:dicingquality,
    // pbrt:dicingMaxTriangles), < 0: none (the render settings).
    float _dicingRate = -1.f, _dicingQuality = -1.f;
    int _dicingMaxTriangles = -1;
    mutable bool _warnedNoUV = false;  // displacement without uvs
    bool _instanced = false;
    VtArray<TfToken> _categories;
    // The scene delegate from the last sync (_SyncDisplayColor), for primvars
    // fetched when the scene is written.
    HdSceneDelegate *_sd = nullptr;
    mutable std::vector<std::string> _shapePv;  // see _ShapePrimvars
};

// ---------------------------------------------------------------------------
class HdPbrtMesh final : public HdMesh, public HdPbrtGeom {
public:
    HdPbrtMesh(SdfPath const &id, HdPbrtRenderDelegate *delegate);
    ~HdPbrtMesh() override;

    HdDirtyBits GetInitialDirtyBitsMask() const override;
    void Sync(HdSceneDelegate *sceneDelegate, HdRenderParam *renderParam,
              HdDirtyBits *dirtyBits, TfToken const &reprToken) override;
    void Finalize(HdRenderParam *renderParam) override;

protected:
    HdRprim const &_Prim() const override { return *this; }
    std::string _Shapes(std::string const &extra) const override;
    bool _CanEmit() const override { return true; }
    bool _CanDisplace() const override { return _hasUV; }
    std::string _PrimvarFile(std::string const &name, int width) const override;
    HdDirtyBits _PropagateDirtyBits(HdDirtyBits bits) const override { return bits; }
    void _InitRepr(TfToken const &reprToken, HdDirtyBits *dirtyBits) override {}

private:
    void _WritePly(HdSceneDelegate *sceneDelegate, HdDirtyBits dirtyBits);
    // Deformation blur: (re)write the PLYs of the poses at the given times
    // when they changed. Called with _mutex held.
    void _UpdateMotionPlys(std::vector<float> const &times) const;

    std::string _plyPath;
    bool _hasUV = false;
    size_t _numTriangles = 0;
    int _plyVersion = 0;

    // Deformation blur (only for meshes whose points are time-sampled): the
    // points over HdPbrt_SampleWindow and how to rebuild the PLY from them.
    struct _Motion {
        HdTimeSampleArray<VtVec3fArray, 4> points;
        std::vector<int> source;       // PLY vertex -> point index
        std::vector<int32_t> indices;  // PLY triangle vertex indices
        VtVec3fArray normals;          // per PLY vertex: authored (static) normals
        bool smoothNormals = false;    // recompute vertex normals per pose
        HdMeshTopology topology;       // for smoothNormals (refined when subdivided)
        std::shared_ptr<HdPbrtSubdiv> subdiv;  // refines each pose's points
        std::vector<GfVec2f> uvs;      // per PLY vertex (empty: none)
    };
    std::unique_ptr<_Motion> _motion;
    mutable std::vector<float> _motionTimes;
    mutable std::vector<std::string> _motionPlys;

    // Primvars for displacement graphs, fetched when a material asks for
    // them (the scene delegate is used outside Sync, while the scene is
    // written) and mapped like the PLY's vertices.
    struct _PvSource {
        HdSceneDelegate *sd = nullptr;
        std::vector<int> source;      // PLY vertex -> point index
        std::vector<size_t> corner;   // PLY vertex -> triangle corner (unwelded)
        bool unweld = false;
        VtVec3iArray tris;            // triangles (refined points)
        VtIntArray primParams;        // per triangle: coarse face
        HdMeshTopology topology;      // triangulated (refined when subdivided)
        std::shared_ptr<HdPbrtSubdiv> subdiv;
        size_t numBasePoints = 0;
    };
    _PvSource _pv;
    mutable std::map<std::string, std::string> _pvFiles;  // name#width -> file
    mutable std::set<std::string> _pvWarned;
};

// ---------------------------------------------------------------------------
class HdPbrtCurves final : public HdBasisCurves, public HdPbrtGeom {
public:
    HdPbrtCurves(SdfPath const &id, HdPbrtRenderDelegate *delegate);
    ~HdPbrtCurves() override;
    HdDirtyBits GetInitialDirtyBitsMask() const override;
    void Sync(HdSceneDelegate *sceneDelegate, HdRenderParam *renderParam,
              HdDirtyBits *dirtyBits, TfToken const &reprToken) override;
    void Finalize(HdRenderParam *renderParam) override;

protected:
    HdRprim const &_Prim() const override { return *this; }
    std::string _Shapes(std::string const &extra) const override;
    std::vector<std::string> _ShapePrimvars(PrimvarList const &pvs,
                                            std::set<std::string> *found) const override;
    HdDirtyBits _PropagateDirtyBits(HdDirtyBits bits) const override { return bits; }
    void _InitRepr(TfToken const &reprToken, HdDirtyBits *dirtyBits) override {}

private:
    void _BuildShapes(HdSceneDelegate *sceneDelegate);
    std::vector<std::string> _shapes;  // one pbrt curve per USD curve, object space
    // Per pbrt curve, for shape primvars: its USD curve, the point index of
    // each of its vertices and its Bezier segment count.
    struct _CurveShape {
        size_t curve = 0;
        std::vector<int> points;
        int segments = 0;
    };
    std::vector<_CurveShape> _curveShapes;
    size_t _numPoints = 0, _numCurves = 0;
    TfToken _curveType, _curveBasis, _curveWrap;
};

// ---------------------------------------------------------------------------
class HdPbrtPoints final : public HdPoints, public HdPbrtGeom {
public:
    HdPbrtPoints(SdfPath const &id, HdPbrtRenderDelegate *delegate);
    ~HdPbrtPoints() override;
    HdDirtyBits GetInitialDirtyBitsMask() const override;
    void Sync(HdSceneDelegate *sceneDelegate, HdRenderParam *renderParam,
              HdDirtyBits *dirtyBits, TfToken const &reprToken) override;
    void Finalize(HdRenderParam *renderParam) override;

protected:
    HdRprim const &_Prim() const override { return *this; }
    std::string _Shapes(std::string const &extra) const override;
    std::vector<std::string> _ShapePrimvars(PrimvarList const &pvs,
                                            std::set<std::string> *found) const override;
    HdDirtyBits _PropagateDirtyBits(HdDirtyBits bits) const override { return bits; }
    void _InitRepr(TfToken const &reprToken, HdDirtyBits *dirtyBits) override {}

private:
    std::vector<std::string> _shapes;  // one sphere per point, object space
    std::vector<int> _pointIndex;      // per sphere: its point (shape primvars)
    size_t _numPoints = 0;
};

// ---------------------------------------------------------------------------
// UsdGeom Sphere / Cylinder / Cone / Capsule as exact pbrt shapes (implicit.cpp).
class HdPbrtImplicit final : public HdRprim, public HdPbrtGeom {
public:
    HdPbrtImplicit(TfToken const &type, SdfPath const &id, HdPbrtRenderDelegate *delegate);
    ~HdPbrtImplicit() override;
    HdDirtyBits GetInitialDirtyBitsMask() const override;
    void Sync(HdSceneDelegate *sceneDelegate, HdRenderParam *renderParam,
              HdDirtyBits *dirtyBits, TfToken const &reprToken) override;
    void Finalize(HdRenderParam *renderParam) override;
    TfTokenVector const &GetBuiltinPrimvarNames() const override;

protected:
    HdRprim const &_Prim() const override { return *this; }
    std::string _Shapes(std::string const &extra) const override;
    std::vector<std::string> _ShapePrimvars(PrimvarList const &pvs,
                                            std::set<std::string> *found) const override;
    bool _CanEmit() const override { return true; }
    HdDirtyBits _PropagateDirtyBits(HdDirtyBits bits) const override { return bits; }
    void _InitRepr(TfToken const &reprToken, HdDirtyBits *dirtyBits) override {}

private:
    void _SyncShape(HdSceneDelegate *sd);
    TfToken _type;
    std::string _axis;                 // rotation from +z to the prim's axis
    std::vector<std::string> _shapes;  // object-space pbrt shapes
};

// ---------------------------------------------------------------------------
// Camera that keeps its transform samples (over HdPbrt_SampleWindow) for
// camera motion blur.
class HdPbrtCamera final : public HdCamera {
public:
    explicit HdPbrtCamera(SdfPath const &id);
    void Sync(HdSceneDelegate *sceneDelegate, HdRenderParam *renderParam,
              HdDirtyBits *dirtyBits) override;
    HdTimeSampleArray<GfMatrix4d, 4> GetTransformSamples() const;
    // pbrt Film parameters for the camera's spectral response (pbrt:sensor).
    std::string GetSensorParams() const;

private:
    mutable std::mutex _mutex;
    HdTimeSampleArray<GfMatrix4d, 4> _xform;
    std::string _sensor;
};

// ---------------------------------------------------------------------------
// A volume field (openvdbAsset, or houdiniFieldAsset with a file on disk).
class HdPbrtField final : public HdField {
public:
    explicit HdPbrtField(SdfPath const &id);
    void Sync(HdSceneDelegate *sceneDelegate, HdRenderParam *renderParam,
              HdDirtyBits *dirtyBits) override;
    HdDirtyBits GetInitialDirtyBitsMask() const override { return AllDirty; }
    void Get(std::string *filePath, std::string *gridName) const {
        std::lock_guard<std::mutex> l(_mutex);
        *filePath = _filePath;
        *gridName = _gridName;
    }

private:
    mutable std::mutex _mutex;
    std::string _filePath, _gridName;
};

// ---------------------------------------------------------------------------
// A volume: its density field as a pbrt nanovdb medium in an interface box.
class HdPbrtVolume final : public HdVolume, public HdPbrtGeom {
public:
    HdPbrtVolume(SdfPath const &id, HdPbrtRenderDelegate *delegate);
    ~HdPbrtVolume() override;
    HdDirtyBits GetInitialDirtyBitsMask() const override;
    void Sync(HdSceneDelegate *sceneDelegate, HdRenderParam *renderParam,
              HdDirtyBits *dirtyBits, TfToken const &reprToken) override;
    void Finalize(HdRenderParam *renderParam) override;
    void Write(std::ostream &os, HdRenderIndex &index, TfTokenVector const &renderTags,
               HdPbrtWriteContext &ctx) const override;

protected:
    HdRprim const &_Prim() const override { return *this; }
    std::string _Shapes(std::string const &) const override { return ""; }
    HdDirtyBits _PropagateDirtyBits(HdDirtyBits bits) const override { return bits; }
    void _InitRepr(TfToken const &reprToken, HdDirtyBits *dirtyBits) override {}

private:
    std::string _nvdb;   // converted density (+ temperature) grids
    GfRange3d _bbox;     // density's active voxels, volume object space
    bool _hasTemperature = false;
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
    HdPbrtLens _lens;
    float _exposureScale = 1.f;  // camera exposure (UsdGeomCamera exposure:*)
    GfMatrix4d _viewOpen{0.0}, _viewClose{0.0};  // world-to-view at shutter open/close
    GfRect2i _dataWindow;
    TfTokenVector _renderTags;
    uint64_t _imageVersion = 0;
    std::vector<float> _image, _depth, _primIds, _instanceIds, _alpha;
    static void _ToNdcDepth(std::vector<float> *zs, GfMatrix4d const &proj);
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
    void Register(HdPbrtGeom *g);
    void Unregister(HdPbrtGeom *g);
    void Register(HdPbrtLight *l);
    void Unregister(HdPbrtLight *l);

    const std::string &GetTempDir() const { return _tempDir; }
    std::string NewTempPath(const std::string &stem, const std::string &ext);
    // pbrt:workingSpace (default ACEScg); read when prims sync.
    HdPbrtWorkingSpace GetWorkingSpace() const;

    // Convert a lat-long environment image to pbrt's equal-area square
    // layout (cached). Returns an empty string on failure.
    std::string GetEquiAreaTexture(const std::string &latlong);
    // Convert an OpenVDB file's density grid (and temperature grid, if named)
    // to NanoVDB for pbrt's nanovdb medium with hdpbrt_vdb2nvdb (cached).
    // bbox: the density's active voxels in the file's world space.
    // Light linking: each light-/shadow-link collection a light uses gets a
    // bit (0..63; -1 for none or when out of bits). LinkParam gives the
    // " \"integer hdpbrtlinks\" [...]" shape parameter for a prim's
    // categories, or "" when no light uses linking.
    int LinkBit(TfToken const &collection);
    std::string LinkParam(VtArray<TfToken> const &categories);
    // HdPbrt_WriteIESTable, cached per file, mtime, angle scale and normalize.
    std::string GetIESTable(const std::string &ies, float angleScale, bool normalize);
    // A Houdini op: image (COP node, Houdini flavor) cooked into a float PFM.
    // Each op: path keeps its file; when the node changes, PollHoudiniImages
    // rewrites it and restarts the render (Houdini's viewport doesn't resync
    // anything when a COP changes, so the scene text can't depend on it).
    // Empty on failure (warns).
    std::string GetHoudiniImage(const std::string &opPath);
    // The same for a dome light: the image in pbrt's equal-area layout.
    std::string GetHoudiniEquiArea(const std::string &opPath);
    // Called by the render pass on every execute (at most 4 checks a second).
    void PollHoudiniImages();
    bool GetNanoVDB(const std::string &vdb, const std::string &densityGrid,
                    const std::string &temperatureGrid, std::string *nvdb, GfRange3d *bbox);

    // Write the full pbrt scene to a file and start rendering it.
    bool StartRender(HdRenderIndex *index, const GfMatrix4d &view,
                     const GfMatrix4d &proj, int width, int height,
                     TfTokenVector const &renderTags, HdPbrtLens const &lens,
                     const GfMatrix4d *viewClose = nullptr, float exposureScale = 1.f);
    void StopRender() { _session.Stop(); }
    hdpbrt::PbrtSession &GetSession() { return _session; }
    int GetSpp() const;

    template <typename T>
    T GetSetting(TfToken const &key, T const &def) const;

    // Motion blur shutter interval in frames relative to the current time
    // (open == close: no blur). Set by the render pass from the camera; the
    // prims sample their transforms over it when they sync.
    GfVec2f GetShutter() const { return _shutter; }
    void SetShutter(GfVec2f s) { _shutter = s; }

private:
    void _Init();
    std::string _PbrtExecutable() const;

    std::unique_ptr<HdPbrtRenderParam> _renderParam;
    HdRenderSettingDescriptorList _settingDescriptors;
    std::mutex _registryMutex;
    std::set<HdPbrtGeom *> _geoms;
    std::set<HdPbrtLight *> _lights;
    std::string _tempDir;
    std::atomic<int> _tempCounter{0};
    std::mutex _texMutex;
    std::map<std::string, std::string> _equiAreaCache;
    std::map<std::string, std::pair<std::string, GfRange3d>> _nanoVdbCache;
    std::map<std::string, std::string> _iesCache;
    struct _HoudiniImage {
        std::string version, file;   // file: stable for the op: path
        std::string equiArea;        // stable too, when a dome uses it
        int opid = -1;
        long long nodeVersion = -1;  // HdPbrt_HoudiniImageVersion at the last cook
        bool ok = false;
    };
    bool _CookHoudiniImage(const std::string &opPath, _HoudiniImage &img);  // _texMutex held
    bool _MakeEquiArea(const std::string &latlong, const std::string &out);
    std::map<std::string, _HoudiniImage> _houdiniImages;
    std::chrono::steady_clock::time_point _lastHoudiniPoll;
    std::mutex _linkMutex;
    std::map<TfToken, int> _linkBits;
    hdpbrt::PbrtSession _session;
    int _renderCount = 0;
    GfVec2f _shutter{0.f, 0.f};

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

// Depth AOV convention (reversed in Houdini). See renderPass.cpp.
bool HdPbrt_ReversedDepth();
float HdPbrt_FarDepth();

PXR_NAMESPACE_CLOSE_SCOPE

#endif
