// SPDX-License-Identifier: Apache-2.0
#include "hdPbrt.h"
#include "pxr/base/gf/transform.h"
#include "pxr/base/gf/rotation.h"
#include "pxr/base/gf/quatd.h"

#include "pxr/imaging/hd/meshUtil.h"
#include "pxr/imaging/hd/smoothNormals.h"
#include "pxr/imaging/hd/vertexAdjacency.h"
#include "pxr/imaging/hd/sceneDelegate.h"
#include "pxr/imaging/hd/renderIndex.h"
#include "pxr/imaging/hd/tokens.h"
#include "pxr/imaging/pxOsd/tokens.h"

#include <cstdio>
#include <cstdlib>
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
        if (HdChangeTracker::IsTransformDirty(*dirtyBits, id)) {
            // Motion blur: sample over a window around the current time and
            // resample at the shutter when the scene is written.
            const float w = HdPbrt_SampleWindow;
            (void)w;
            HdPbrt_SampleXform(
                [&](float a, float b, HdTimeSampleArray<GfMatrix4d, 4> *sa) {
                    sceneDelegate->SampleTransform(id, a, b, sa);
                },
                &_xform);
            if (_xform.count == 0) {
                _xform.Resize(1);
                _xform.times[0] = 0.f;
                _xform.values[0] = sceneDelegate->GetTransform(id);
            }
            static const bool debug = getenv("HDPBRT_DEBUG") != nullptr;
            if (debug && _xform.count > 1)
                fprintf(stderr, "hdPbrt %s: %zu transform samples (%g..%g)\n", id.GetText(),
                        _xform.count, _xform.times[0], _xform.times[_xform.count - 1]);
        }
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
        bool instanced = !GetInstancerId().IsEmpty();
        if (instanced) {
            if (HdInstancer *instancer =
                    sceneDelegate->GetRenderIndex().GetInstancer(GetInstancerId()))
                static_cast<HdPbrtInstancer *>(instancer)->UpdateInstanceIndices(id);
        }
        std::lock_guard<std::mutex> lock(_mutex);
        _instanced = instanced;
    }

    *dirtyBits &= ~HdChangeTracker::AllSceneDirtyBits;
}

// Write a binary PLY (positions, optional normals and uvs, triangles).
static bool _WritePlyFile(const std::string &path, const std::string &comment,
                          const std::vector<GfVec3f> &p, const std::vector<GfVec3f> &n,
                          const std::vector<GfVec2f> &uv, const std::vector<int32_t> &indices)
{
    FILE *f = fopen(path.c_str(), "wb");
    if (!f)
        return false;
    const bool hasN = !n.empty(), hasUV = !uv.empty();
    const size_t numTris = indices.size() / 3;
    fprintf(f, "ply\nformat binary_little_endian 1.0\n");
    fprintf(f, "comment hdPbrt %s\n", comment.c_str());
    fprintf(f, "element vertex %zu\n", p.size());
    fprintf(f, "property float x\nproperty float y\nproperty float z\n");
    if (hasN)
        fprintf(f, "property float nx\nproperty float ny\nproperty float nz\n");
    if (hasUV)
        fprintf(f, "property float u\nproperty float v\n");
    fprintf(f, "element face %zu\n", numTris);
    fprintf(f, "property list uchar int vertex_indices\nend_header\n");
    std::vector<float> vbuf;
    vbuf.reserve(p.size() * (3 + (hasN ? 3 : 0) + (hasUV ? 2 : 0)));
    for (size_t i = 0; i < p.size(); ++i) {
        vbuf.insert(vbuf.end(), {p[i][0], p[i][1], p[i][2]});
        if (hasN)
            vbuf.insert(vbuf.end(), {n[i][0], n[i][1], n[i][2]});
        if (hasUV)
            vbuf.insert(vbuf.end(), {uv[i][0], uv[i][1]});
    }
    fwrite(vbuf.data(), sizeof(float), vbuf.size(), f);
    std::vector<uint8_t> fbuf(numTris * 13);
    for (size_t t = 0; t < numTris; ++t) {
        uint8_t *d = &fbuf[t * 13];
        d[0] = 3;
        memcpy(d + 1, &indices[t * 3], 12);
    }
    fwrite(fbuf.data(), 1, fbuf.size(), f);
    return fclose(f) == 0;
}

// Points at time t from samples (linear between neighbors; nearest when the
// sizes differ).
static VtVec3fArray _PointsAt(const HdTimeSampleArray<VtVec3fArray, 4> &sa, float t)
{
    if (sa.count == 1 || t <= sa.times[0])
        return sa.values[0];
    if (t >= sa.times[sa.count - 1])
        return sa.values[sa.count - 1];
    size_t i = 0;
    while (i + 1 < sa.count && sa.times[i + 1] < t)
        ++i;
    const VtVec3fArray &a = sa.values[i], &b = sa.values[i + 1];
    const float t0 = sa.times[i], t1 = sa.times[i + 1];
    const float u = t1 > t0 ? (t - t0) / (t1 - t0) : 0.f;
    if (a.size() != b.size())
        return u < 0.5f ? a : b;
    VtVec3fArray r(a.size());
    for (size_t k = 0; k < a.size(); ++k)
        r[k] = a[k] + u * (b[k] - a[k]);
    return r;
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

    // PLY vertex arrays. Face-varying data unwelds the mesh into three
    // vertices per triangle.
    std::vector<int> source;  // PLY vertex -> point index
    std::vector<size_t> corner;
    std::vector<int32_t> indices(numTris * 3);
    if (unweld) {
        for (size_t t = 0; t < numTris; ++t)
            for (int k = 0; k < 3; ++k) {
                source.push_back(tris[t][k]);
                corner.push_back(t * 3 + k);
                indices[t * 3 + k] = int32_t(t * 3 + k);
            }
    } else {
        for (size_t v = 0; v < points.size(); ++v) {
            source.push_back(int(v));
            corner.push_back(v);
        }
        for (size_t t = 0; t < numTris; ++t)
            for (int k = 0; k < 3; ++k)
                indices[t * 3 + k] = tris[t][k];
    }
    std::vector<GfVec3f> plyP(source.size()), plyN;
    std::vector<GfVec2f> plyUV;
    for (size_t v = 0; v < source.size(); ++v)
        plyP[v] = points[source[v]];
    if (hasN) {
        plyN.resize(source.size());
        for (size_t v = 0; v < source.size(); ++v)
            plyN[v] = nFaceVarying ? normals[corner[v]] : normals[source[v]];
    }
    if (hasUV) {
        plyUV.resize(source.size());
        for (size_t v = 0; v < source.size(); ++v)
            plyUV[v] = uvFaceVarying ? uvs[corner[v]] : uvs[source[v]];
    }

    std::string path;
    size_t writtenTris = 0;
    if (!tris.empty()) {
        path = _delegate->NewTempPath("mesh", ".ply");
        if (_WritePlyFile(path, id.GetString(), plyP, plyN, plyUV, indices)) {
            writtenTris = numTris;
        } else {
            TF_WARN("hdPbrt: cannot write %s", path.c_str());
            path.clear();
        }
    }

    // Deformation blur: keep the time-sampled points (when they really move
    // and keep their count) and what's needed to rebuild the PLY from them.
    std::unique_ptr<_Motion> motion;
    if (writtenTris) {
        const float w = HdPbrt_SampleWindow;
        HdTimeSampleArray<VtValue, 4> sa;
        sd->SamplePrimvar(id, HdTokens->points, -w, w, &sa);
        bool moving = false, ok = sa.count > 1;
        for (size_t k = 0; ok && k < sa.count; ++k) {
            ok = sa.values[k].IsHolding<VtVec3fArray>() &&
                 sa.values[k].UncheckedGet<VtVec3fArray>().size() == points.size();
            if (ok && k > 0)
                moving |= sa.values[k].UncheckedGet<VtVec3fArray>() !=
                          sa.values[0].UncheckedGet<VtVec3fArray>();
        }
        if (ok && moving) {
            motion = std::make_unique<_Motion>();
            motion->points.Resize(sa.count);
            for (size_t k = 0; k < sa.count; ++k) {
                motion->points.times[k] = sa.times[k];
                motion->points.values[k] = sa.values[k].UncheckedGet<VtVec3fArray>();
            }
            motion->source = source;
            motion->indices = indices;
            const bool smooth = hasN && nVertex && !nPv.valid;
            motion->smoothNormals = smooth;
            if (smooth)
                motion->topology = topology;
            else if (hasN)
                motion->normals.assign(plyN.begin(), plyN.end());
            motion->uvs = plyUV;
            if (debug)
                fprintf(stderr, "hdPbrt: %s deforms (%zu point samples, %g..%g)\n",
                        id.GetText(), sa.count, sa.times[0], sa.times[sa.count - 1]);
        }
    }

    std::lock_guard<std::mutex> lock(_mutex);
    if (!_plyPath.empty())
        unlink(_plyPath.c_str());  // a running pbrt keeps its open handle
    _plyPath = path;
    _numTriangles = writtenTris;
    _motion = std::move(motion);
    for (const std::string &m : _motionPlys)
        unlink(m.c_str());
    _motionPlys.clear();
    _motionTimes.clear();
}

void HdPbrtMesh::_UpdateMotionPlys(std::vector<float> const &times) const
{
    if (!_motion || times.size() < 2)
        return;
    if (times == _motionTimes && _motionPlys.size() == times.size())
        return;
    for (const std::string &m : _motionPlys)
        unlink(m.c_str());
    _motionPlys.clear();
    _motionTimes = times;
    const _Motion &mo = *_motion;
    std::vector<std::string> paths;
    for (size_t e = 0; e < times.size(); ++e) {
        const VtVec3fArray pts = _PointsAt(mo.points, times[e]);
        std::vector<GfVec3f> p(mo.source.size()), n;
        for (size_t v = 0; v < mo.source.size(); ++v)
            p[v] = pts[mo.source[v]];
        if (mo.smoothNormals) {
            HdMeshTopology topo = mo.topology;
            Hd_VertexAdjacency adjacency;
            adjacency.BuildAdjacencyTable(&topo);
            VtVec3fArray sn = Hd_SmoothNormals::ComputeSmoothNormals(&adjacency,
                                                                     (int)pts.size(),
                                                                     pts.cdata());
            n.resize(mo.source.size());
            for (size_t v = 0; v < mo.source.size(); ++v)
                n[v] = sn[mo.source[v]];
        } else if (!mo.normals.empty()) {
            n.assign(mo.normals.begin(), mo.normals.end());
        }
        paths.push_back(_delegate->NewTempPath("motion", ".ply"));
        if (!_WritePlyFile(paths.back(), GetId().GetString(), p, n, mo.uvs, mo.indices)) {
            TF_WARN("hdPbrt: cannot write %s", paths.back().c_str());
            for (const std::string &q : paths)
                unlink(q.c_str());
            _motionTimes.clear();
            return;
        }
    }
    _motionPlys = paths;
}

GfMatrix4d HdPbrt_InterpolateXform(const GfMatrix4d &a, const GfMatrix4d &b, double u)
{
    if (a == b || u <= 0)
        return a;
    if (u >= 1)
        return b;
    GfTransform ta(a), tb(b);
    GfTransform r;
    r.SetScale(ta.GetScale() + u * (tb.GetScale() - ta.GetScale()));
    r.SetTranslation(ta.GetTranslation() + u * (tb.GetTranslation() - ta.GetTranslation()));
    r.SetPivotPosition(ta.GetPivotPosition() +
                       u * (tb.GetPivotPosition() - ta.GetPivotPosition()));
    r.SetPivotOrientation(ta.GetPivotOrientation());
    r.SetScaleOrientation(ta.GetScaleOrientation());
    const GfQuatd qa = ta.GetRotation().GetQuat(), qb = tb.GetRotation().GetQuat();
    r.SetRotation(GfRotation(GfSlerp(u, qa, qb)));
    return r.GetMatrix();
}

GfMatrix4d HdPbrt_XformAt(const HdTimeSampleArray<GfMatrix4d, 4> &sa, float t)
{
    if (sa.count == 0)
        return GfMatrix4d(1.0);
    if (sa.count == 1 || t <= sa.times[0])
        return sa.values[0];
    if (t >= sa.times[sa.count - 1])
        return sa.values[sa.count - 1];
    size_t i = 0;
    while (i + 1 < sa.count && sa.times[i + 1] < t)
        ++i;
    const float t0 = sa.times[i], t1 = sa.times[i + 1];
    const double u = t1 > t0 ? double(t - t0) / double(t1 - t0) : 0.0;
    return HdPbrt_InterpolateXform(sa.values[i], sa.values[i + 1], u);
}

// pbrt transform text for keys at the shutter times: a plain Transform when
// they're all equal, pbrt's own two-key ActiveTransform pair for two keys.
// For more keys it returns "" and *motionAttr gets the Attribute line for
// pbrt patch 6 (N-key instance motion; it must precede an ObjectInstance).
static std::string _MotionTransform(const std::vector<GfMatrix4d> &keys,
                                    const std::vector<float> &times,
                                    std::string *motionAttr)
{
    bool moving = false;
    for (size_t k = 1; k < keys.size(); ++k)
        moving |= keys[k] != keys[0];
    if (!moving)
        return "Transform " + HdPbrt_Matrix(keys[0]);
    if (keys.size() == 2)
        return "ActiveTransform StartTime Transform " + HdPbrt_Matrix(keys[0]) +
               " ActiveTransform EndTime Transform " + HdPbrt_Matrix(keys[1]) +
               " ActiveTransform All";
    std::string m = "Attribute \"shape\" \"float hdpbrtmotionxforms\" [";
    for (const GfMatrix4d &k : keys) {
        std::string one = HdPbrt_Matrix(k);  // "[a b ...]"
        m += " " + one.substr(1, one.size() - 2);
    }
    m += " ] \"float hdpbrtmotiontimes\" [";
    char buf[32];
    for (float t : times) {
        snprintf(buf, sizeof(buf), " %.9g", t);
        m += buf;
    }
    m += " ]";
    *motionAttr = m;
    return "";
}

// N evenly spaced times over the shutter (N >= 2), or {open} without blur.
static std::vector<float> _ShutterTimes(GfVec2f shutter, int n)
{
    if (shutter[0] == shutter[1] || n < 2)
        return {shutter[0]};
    std::vector<float> t(n);
    for (int k = 0; k < n; ++k)
        t[k] = shutter[0] + (shutter[1] - shutter[0]) * float(k) / float(n - 1);
    return t;
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
    // Transform keys over the shutter (one without motion blur).
    const GfVec2f shutter = _delegate->GetShutter();
    const bool blur = shutter[0] != shutter[1];
    const std::vector<float> xTimes = _ShutterTimes(
        shutter, std::max(2, _delegate->GetSetting<int>(HdPbrtSettingsTokens->xformSamples, 2)));
    const size_t nx = xTimes.size();
    std::vector<GfMatrix4d> xf(nx);
    for (size_t k = 0; k < nx; ++k)
        xf[k] = HdPbrt_XformAt(_xform, xTimes[k]);
    std::vector<VtMatrix4dArray> inst;
    if (_instanced) {
        if (auto *pi = static_cast<HdPbrtInstancer *>(index.GetInstancer(GetInstancerId())))
            for (size_t k = 0; k < nx; ++k)
                inst.push_back(pi->ComputeInstanceTransforms(GetId(), xTimes[k]));
        if (inst.empty() || inst[0].empty())
            return;
        for (size_t k = 1; k < nx; ++k)
            if (inst[k].size() != inst[0].size())  // the instance count changes
                inst[k] = inst[0];
    }

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
    const bool emissive = mat.emission[0] > 0 || mat.emission[1] > 0 || mat.emission[2] > 0;

    // Deformation blur: the poses at the geo sample times (pbrt patches 5/6:
    // filename = first pose, motionfilenames = the rest).
    if (blur && _motion)
        _UpdateMotionPlys(_ShutterTimes(
            shutter,
            std::max(2, _delegate->GetSetting<int>(HdPbrtSettingsTokens->geoSamples, 2))));
    const bool deforming = blur && _motion && _motionPlys.size() >= 2;
    std::ostringstream shape;
    // hdpbrtprimid: Hydra's prim id, streamed back by pbrt for the primId AOV
    // (selection).
    shape << "Shape \"plymesh\" \"string filename\" "
          << HdPbrt_Quote(deforming ? _motionPlys[0] : _plyPath) << " \"integer hdpbrtprimid\" "
          << GetPrimId();
    if (deforming) {
        shape << " \"string motionfilenames\" [";
        for (size_t k = 1; k < _motionPlys.size(); ++k)
            shape << " " << HdPbrt_Quote(_motionPlys[k]);
        shape << " ] \"float motiontimes\" [";
        char buf[32];
        for (float t : _motionTimes) {
            snprintf(buf, sizeof(buf), " %.9g", t);
            shape << buf;
        }
        shape << " ]";
    }
    float alpha = mat.opacity * _displayOpacity;
    if (!mat.opacityTexture.empty())
        shape << " \"texture alpha\" " << HdPbrt_Quote(mat.opacityTexture);
    else if (alpha < 0.999f)
        shape << " \"float alpha\" " << std::max(alpha, 0.f);
    shape << "\n";

    os << "# " << GetId().GetString() << "\n";
    if (!_instanced) {
        std::string motionAttr;
        // Area lights can't be instanced in pbrt: emissive meshes get two keys.
        const std::vector<GfMatrix4d> keys =
            emissive && nx > 2 ? std::vector<GfMatrix4d>{xf.front(), xf.back()} : xf;
        const std::vector<float> keyTimes =
            emissive && nx > 2 ? std::vector<float>{xTimes.front(), xTimes.back()} : xTimes;
        const std::string xform = _MotionTransform(keys, keyTimes, &motionAttr);
        if (motionAttr.empty()) {
            os << "AttributeBegin\n  " << xform << "\n  " << matLine;
            if (emissive)
                os << "  AreaLightSource \"diffuse\" \"rgb L\" " << HdPbrt_Rgb(mat.emission)
                   << "\n";
            os << "  " << shape.str() << "AttributeEnd\n";
        } else {
            // N-key transform motion only exists for object instances.
            std::string name = "hdpbrt_obj_" + std::to_string(ctx.objectCounter++);
            os << "ObjectBegin " << HdPbrt_Quote(name) << "\n  Identity\n  " << matLine << "  "
               << shape.str() << "ObjectEnd\nAttributeBegin " << motionAttr
               << " ObjectInstance " << HdPbrt_Quote(name) << " AttributeEnd\n";
        }
        ++ctx.numMeshes;
    } else {
        std::string name = "hdpbrt_obj_" + std::to_string(ctx.objectCounter++);
        os << "ObjectBegin " << HdPbrt_Quote(name) << "\n  Identity\n  " << matLine << "  "
           << shape.str() << "ObjectEnd\n";
        // hdpbrtinstanceid: the instance index Hydra expects in the instanceId
        // AOV (flattened over nested instancers, see HdPbrtInstancer).
        std::vector<GfMatrix4d> keys(nx);
        for (size_t i = 0; i < inst[0].size(); ++i) {
            for (size_t k = 0; k < nx; ++k)
                keys[k] = xf[k] * inst[k][i];
            std::string motionAttr;
            const std::string xform = _MotionTransform(keys, xTimes, &motionAttr);
            os << "AttributeBegin " << xform << (xform.empty() ? "" : " ") << motionAttr
               << (motionAttr.empty() ? "" : " ")
               << "Attribute \"shape\" \"integer hdpbrtinstanceid\" " << i
               << " ObjectInstance " << HdPbrt_Quote(name) << " AttributeEnd\n";
        }
        ctx.numInstances += inst[0].size();
    }
}

PXR_NAMESPACE_CLOSE_SCOPE
