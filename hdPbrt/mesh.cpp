// SPDX-License-Identifier: Apache-2.0
#include "hdPbrt.h"

#include "pxr/imaging/hd/meshUtil.h"
#include "pxr/imaging/hd/smoothNormals.h"
#include "pxr/imaging/hd/vertexAdjacency.h"
#include "pxr/imaging/hd/sceneDelegate.h"
#include "pxr/imaging/hd/renderIndex.h"
#include "pxr/imaging/hd/tokens.h"
#include "pxr/imaging/pxOsd/tokens.h"

#include <cstdio>
#include <unistd.h>

PXR_NAMESPACE_OPEN_SCOPE

TF_DEFINE_PRIVATE_TOKENS(_tokens,
    (st)(uv)(st0)(UVMap)(map1)(displayColor)(displayOpacity));

HdPbrtMesh::HdPbrtMesh(SdfPath const &id, HdPbrtRenderDelegate *delegate)
    : HdMesh(id), _delegate(delegate)
{
    _delegate->Register(this);
}

HdPbrtMesh::~HdPbrtMesh()
{
    _delegate->Unregister(this);
    if (!_plyPath.empty() && !getenv("HDPBRT_KEEP_TMP"))
        unlink(_plyPath.c_str());
}

void HdPbrtMesh::Finalize(HdRenderParam *renderParam)
{
    static_cast<HdPbrtRenderParam *>(renderParam)->Edit();
}

HdDirtyBits HdPbrtMesh::GetInitialDirtyBitsMask() const
{
    return HdChangeTracker::Clean | HdChangeTracker::InitRepr |
           HdChangeTracker::DirtyPoints | HdChangeTracker::DirtyTopology |
           HdChangeTracker::DirtyTransform | HdChangeTracker::DirtyVisibility |
           HdChangeTracker::DirtyPrimvar | HdChangeTracker::DirtyNormals |
           HdChangeTracker::DirtyInstancer | HdChangeTracker::DirtyMaterialId |
           HdChangeTracker::DirtySubdivTags | HdChangeTracker::DirtyDisplayStyle;
}

namespace {

struct _Primvar {
    VtValue value;
    HdInterpolation interp = HdInterpolationConstant;
    bool valid = false;
};

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

_Primvar _FindPrimvar(HdSceneDelegate *sd, SdfPath const &id,
                      const std::vector<TfToken> &names, bool anyVec2 = false)
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

template <typename T>
VtArray<T> _Triangulated(HdMeshUtil &util, VtArray<T> const &src, HdType type)
{
    VtValue out;
    HdMeshComputationResult r =
        util.ComputeTriangulatedFaceVaryingPrimvar(src.cdata(), (int)src.size(), type, &out);
    if (r == HdMeshComputationResult::Unchanged)  // already all triangles
        return src;
    if (r == HdMeshComputationResult::Success && out.IsHolding<VtArray<T>>())
        return out.UncheckedGet<VtArray<T>>();
    return {};
}

GfVec3f _AverageColor(VtValue const &v, bool *ok)
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

}  // namespace

void HdPbrtMesh::Sync(HdSceneDelegate *sceneDelegate, HdRenderParam *renderParam,
                      HdDirtyBits *dirtyBits, TfToken const &reprToken)
{
    SdfPath const &id = GetId();
    static_cast<HdPbrtRenderParam *>(renderParam)->Edit();

    if (*dirtyBits & HdChangeTracker::DirtyMaterialId)
        SetMaterialId(sceneDelegate->GetMaterialId(id));

    {
        std::lock_guard<std::mutex> lock(_mutex);
        if (HdChangeTracker::IsTransformDirty(*dirtyBits, id))
            _xform = sceneDelegate->GetTransform(id);
        if (HdChangeTracker::IsVisibilityDirty(*dirtyBits, id))
            _UpdateVisibility(sceneDelegate, dirtyBits);
        _visible = IsVisible();
    }

    const bool geomDirty =
        HdChangeTracker::IsPrimvarDirty(*dirtyBits, id, HdTokens->points) ||
        HdChangeTracker::IsTopologyDirty(*dirtyBits, id) ||
        HdChangeTracker::IsAnyPrimvarDirty(*dirtyBits, id) ||
        (*dirtyBits & HdChangeTracker::DirtyNormals) ||
        HdChangeTracker::IsDisplayStyleDirty(*dirtyBits, id);
    if (geomDirty)
        _WritePly(sceneDelegate, *dirtyBits);

    _UpdateInstancer(sceneDelegate, dirtyBits);
    HdInstancer::_SyncInstancerAndParents(sceneDelegate->GetRenderIndex(),
                                          GetInstancerId());
    {
        VtMatrix4dArray xforms;
        bool instanced = !GetInstancerId().IsEmpty();
        if (instanced) {
            HdInstancer *instancer =
                sceneDelegate->GetRenderIndex().GetInstancer(GetInstancerId());
            if (instancer)
                xforms = static_cast<HdPbrtInstancer *>(instancer)
                             ->ComputeInstanceTransforms(id);
        }
        std::lock_guard<std::mutex> lock(_mutex);
        _instanced = instanced;
        _instanceXforms = xforms;
    }

    *dirtyBits &= ~HdChangeTracker::AllSceneDirtyBits;
}

void HdPbrtMesh::_WritePly(HdSceneDelegate *sd, HdDirtyBits dirtyBits)
{
    SdfPath const &id = GetId();

    VtValue pointsValue = sd->Get(id, HdTokens->points);
    VtVec3fArray points;
    if (pointsValue.IsHolding<VtVec3fArray>())
        points = pointsValue.UncheckedGet<VtVec3fArray>();

    HdMeshTopology topology = GetMeshTopology(sd);
    HdMeshUtil util(&topology, id);
    VtVec3iArray tris;
    VtIntArray primParams;
    util.ComputeTriangleIndices(&tris, &primParams);

    // Drop triangles with out-of-range indices (bad topology).
    {
        VtVec3iArray good;
        good.reserve(tris.size());
        const int n = (int)points.size();
        for (const GfVec3i &t : tris)
            if (t[0] >= 0 && t[1] >= 0 && t[2] >= 0 && t[0] < n && t[1] < n && t[2] < n)
                good.push_back(t);
        tris = good;
    }

    static const bool debug = getenv("HDPBRT_DEBUG") != nullptr;
    if (debug) {
        for (HdInterpolation i : {HdInterpolationConstant, HdInterpolationUniform,
                                  HdInterpolationVarying, HdInterpolationVertex,
                                  HdInterpolationFaceVarying})
            for (const HdPrimvarDescriptor &pv : sd->GetPrimvarDescriptors(id, i))
                fprintf(stderr, "hdPbrt: %s primvar %s interp %d role %s type %s\n",
                        id.GetText(), pv.name.GetText(), int(i), pv.role.GetText(),
                        sd->Get(id, pv.name).GetTypeName().c_str());
        fprintf(stderr, "hdPbrt: %s scheme %s points %zu\n", id.GetText(),
                topology.GetScheme().GetText(), points.size());
    }

    // Normals.
    _Primvar nPv = _FindPrimvar(sd, id, {HdTokens->normals});
    VtVec3fArray normals;
    HdInterpolation nInterp = HdInterpolationVertex;
    if (nPv.valid && nPv.value.IsHolding<VtVec3fArray>()) {
        normals = nPv.value.UncheckedGet<VtVec3fArray>();
        nInterp = nPv.interp;
    }
    if (normals.empty()) {
        const TfToken scheme = topology.GetScheme();
        if (scheme != PxOsdOpenSubdivTokens->none &&
            scheme != PxOsdOpenSubdivTokens->bilinear && !points.empty()) {
            Hd_VertexAdjacency adjacency;
            adjacency.BuildAdjacencyTable(&topology);
            normals = Hd_SmoothNormals::ComputeSmoothNormals(&adjacency, (int)points.size(),
                                                             points.cdata());
            nInterp = HdInterpolationVertex;
        }
    }
    bool nVertex = (nInterp == HdInterpolationVertex || nInterp == HdInterpolationVarying) &&
                   normals.size() == points.size();
    bool nFaceVarying = nInterp == HdInterpolationFaceVarying;
    if (nFaceVarying)
        normals = _Triangulated(util, normals, HdTypeFloatVec3);

    // Texture coordinates.
    _Primvar uvPv = _FindPrimvar(sd, id, {_tokens->st, _tokens->uv, _tokens->st0,
                                          _tokens->UVMap, _tokens->map1}, true);
    VtVec2fArray uvs;
    bool uvVertex = false, uvFaceVarying = false;
    if (uvPv.valid && uvPv.value.IsHolding<VtVec2fArray>()) {
        uvs = uvPv.value.UncheckedGet<VtVec2fArray>();
        uvVertex = (uvPv.interp == HdInterpolationVertex ||
                    uvPv.interp == HdInterpolationVarying) && uvs.size() == points.size();
        uvFaceVarying = uvPv.interp == HdInterpolationFaceVarying;
        if (uvFaceVarying)
            uvs = _Triangulated(util, uvs, HdTypeFloatVec2);
    }
    // Face-varying arrays are per triangle corner, matched to the *original*
    // triangle list; recompute their indices if we dropped bad triangles.
    const size_t numTris = tris.size();
    if (nFaceVarying && normals.size() != numTris * 3)
        nFaceVarying = false;
    if (uvFaceVarying && uvs.size() != numTris * 3)
        uvFaceVarying = false;
    if (debug)
        fprintf(stderr, "hdPbrt: %s tris %zu normals %zu (fv %d v %d) uvs %zu (fv %d v %d)\n",
                id.GetText(), numTris, normals.size(), nFaceVarying, nVertex, uvs.size(),
                uvFaceVarying, uvVertex);
    const bool hasN = nVertex || nFaceVarying;
    const bool hasUV = uvVertex || uvFaceVarying;
    const bool unweld = nFaceVarying || uvFaceVarying;

    // Display color / opacity.
    {
        _Primvar c = _FindPrimvar(sd, id, {_tokens->displayColor});
        bool ok = false;
        GfVec3f col = c.valid ? _AverageColor(c.value, &ok) : GfVec3f(0.18f);
        _Primvar o = _FindPrimvar(sd, id, {_tokens->displayOpacity});
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

    std::string path;
    size_t writtenTris = 0;
    if (!tris.empty()) {
        path = _delegate->NewTempPath("mesh", ".ply");
        FILE *f = fopen(path.c_str(), "wb");
        if (!f) {
            TF_WARN("hdPbrt: cannot write %s", path.c_str());
            path.clear();
        } else {
            const size_t nv = unweld ? numTris * 3 : points.size();
            fprintf(f, "ply\nformat binary_little_endian 1.0\n");
            fprintf(f, "comment hdPbrt %s\n", id.GetText());
            fprintf(f, "element vertex %zu\n", nv);
            fprintf(f, "property float x\nproperty float y\nproperty float z\n");
            if (hasN)
                fprintf(f, "property float nx\nproperty float ny\nproperty float nz\n");
            if (hasUV)
                fprintf(f, "property float u\nproperty float v\n");
            fprintf(f, "element face %zu\n", numTris);
            fprintf(f, "property list uchar int vertex_indices\nend_header\n");

            std::vector<float> vbuf;
            vbuf.reserve(nv * (3 + (hasN ? 3 : 0) + (hasUV ? 2 : 0)));
            auto emit = [&](int pi, size_t corner) {
                const GfVec3f &p = points[pi];
                vbuf.insert(vbuf.end(), {p[0], p[1], p[2]});
                if (hasN) {
                    GfVec3f n = nFaceVarying ? normals[corner] : normals[pi];
                    vbuf.insert(vbuf.end(), {n[0], n[1], n[2]});
                }
                if (hasUV) {
                    GfVec2f t = uvFaceVarying ? uvs[corner] : uvs[pi];
                    vbuf.insert(vbuf.end(), {t[0], t[1]});
                }
            };
            if (unweld) {
                for (size_t t = 0; t < numTris; ++t)
                    for (int k = 0; k < 3; ++k)
                        emit(tris[t][k], t * 3 + k);
            } else {
                for (size_t i = 0; i < points.size(); ++i)
                    emit((int)i, i);
            }
            fwrite(vbuf.data(), sizeof(float), vbuf.size(), f);
            std::vector<uint8_t> fbuf(numTris * 13);
            for (size_t t = 0; t < numTris; ++t) {
                uint8_t *d = &fbuf[t * 13];
                d[0] = 3;
                int32_t idx[3];
                for (int k = 0; k < 3; ++k)
                    idx[k] = unweld ? int32_t(t * 3 + k) : tris[t][k];
                memcpy(d + 1, idx, 12);
            }
            fwrite(fbuf.data(), 1, fbuf.size(), f);
            fclose(f);
            writtenTris = numTris;
        }
    }

    std::lock_guard<std::mutex> lock(_mutex);
    if (!_plyPath.empty())
        unlink(_plyPath.c_str());  // a running pbrt keeps its open handle
    _plyPath = path;
    _numTriangles = writtenTris;
}

void HdPbrtMesh::Write(std::ostream &os, HdRenderIndex &index,
                       TfTokenVector const &renderTags, HdPbrtWriteContext &ctx) const
{
    std::lock_guard<std::mutex> lock(_mutex);
    if (!_visible || _plyPath.empty())
        return;
    if (!renderTags.empty() &&
        std::find(renderTags.begin(), renderTags.end(), GetRenderTag()) == renderTags.end())
        return;
    if (_instanced && _instanceXforms.empty())
        return;

    HdPbrtMaterialDesc mat = HdPbrt_DefaultMaterial();
    const SdfPath &matId = GetMaterialId();
    if (!matId.IsEmpty()) {
        if (auto *m = static_cast<HdPbrtMaterial *>(
                index.GetSprim(HdPrimTypeTokens->material, matId))) {
            mat = m->GetDesc();
            if (!mat.textures.empty() && ctx.texturesWritten.insert(matId).second)
                os << mat.textures;
        }
    }
    const std::string matLine =
        HdPbrt_MaterialLine(mat, _hasDisplayColor ? &_displayColor : nullptr);

    std::ostringstream shape;
    shape << "Shape \"plymesh\" \"string filename\" " << HdPbrt_Quote(_plyPath);
    float alpha = mat.opacity * _displayOpacity;
    if (!mat.opacityTexture.empty())
        shape << " \"texture alpha\" " << HdPbrt_Quote(mat.opacityTexture);
    else if (alpha < 0.999f)
        shape << " \"float alpha\" " << std::max(alpha, 0.f);
    shape << "\n";

    os << "# " << GetId().GetString() << "\n";
    if (!_instanced) {
        os << "AttributeBegin\n  Transform " << HdPbrt_Matrix(_xform) << "\n  " << matLine;
        if (mat.emission[0] > 0 || mat.emission[1] > 0 || mat.emission[2] > 0)
            os << "  AreaLightSource \"diffuse\" \"rgb L\" " << HdPbrt_Rgb(mat.emission)
               << "\n";
        os << "  " << shape.str() << "AttributeEnd\n";
        ++ctx.numMeshes;
    } else {
        std::string name = "hdpbrt_obj_" + std::to_string(ctx.objectCounter++);
        os << "ObjectBegin " << HdPbrt_Quote(name) << "\n  Identity\n  " << matLine << "  "
           << shape.str() << "ObjectEnd\n";
        for (const GfMatrix4d &ix : _instanceXforms)
            os << "AttributeBegin Transform " << HdPbrt_Matrix(_xform * ix)
               << " ObjectInstance " << HdPbrt_Quote(name) << " AttributeEnd\n";
        ctx.numInstances += _instanceXforms.size();
    }
}

PXR_NAMESPACE_CLOSE_SCOPE
