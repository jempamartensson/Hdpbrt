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
#include <map>
#include <cstdlib>
#include <unistd.h>

PXR_NAMESPACE_OPEN_SCOPE

TF_DEFINE_PRIVATE_TOKENS(_tokens,
    (st)(uv)(st0)(UVMap)(map1));

HdPbrtMesh::HdPbrtMesh(SdfPath const &id, HdPbrtRenderDelegate *delegate)
    : HdMesh(id), HdPbrtGeom(delegate)
{
}

HdPbrtMesh::~HdPbrtMesh()
{
    if (getenv("HDPBRT_KEEP_TMP"))
        return;
    if (!_plyPath.empty())
        unlink(_plyPath.c_str());
    for (const auto &f : _pvFiles)
        if (!f.second.empty())
            unlink(f.second.c_str());
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
           HdChangeTracker::DirtyInstancer | HdChangeTracker::DirtyCategories | HdChangeTracker::DirtyMaterialId |
           HdChangeTracker::DirtySubdivTags | HdChangeTracker::DirtyDisplayStyle;
}

namespace {

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

}  // namespace

void HdPbrtMesh::Sync(HdSceneDelegate *sceneDelegate, HdRenderParam *renderParam,
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
    _SyncInstancing(sceneDelegate);

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
    const size_t numBasePoints = points.size();

    // Texture coordinates (fetched before subdivision, which refines them).
    HdPbrtPrimvar uvPv = HdPbrt_FindPrimvar(sd, id, {_tokens->st, _tokens->uv, _tokens->st0,
                                          _tokens->UVMap, _tokens->map1}, true);

    // Subdivision: catmullClark and loop meshes are refined uniformly and then
    // treated as plain polygons (smooth normals; authored normals are ignored,
    // as for any subdivision surface).
    std::shared_ptr<HdPbrtSubdiv> subdiv;
    const int subdivLevel = std::min(
        6, _delegate->GetSetting<int>(HdPbrtSettingsTokens->subdivLevel, 2));
    if (subdivLevel > 0 && !points.empty()) {
        // Face-varying uvs: weld equal values per vertex into OpenSubdiv's
        // value indices, so shared corners refine smoothly and seams stay.
        VtIntArray fvIndices;
        VtVec2fArray fvValues;
        const VtIntArray &fvi = topology.GetFaceVertexIndices();
        const bool uvFV = uvPv.valid && uvPv.interp == HdInterpolationFaceVarying &&
                          uvPv.value.IsHolding<VtVec2fArray>() &&
                          uvPv.value.UncheckedGet<VtVec2fArray>().size() == fvi.size();
        if (uvFV) {
            const VtVec2fArray &uv = uvPv.value.UncheckedGet<VtVec2fArray>();
            std::map<std::pair<int, std::pair<float, float>>, int> weld;
            fvIndices.resize(fvi.size());
            for (size_t c = 0; c < fvi.size(); ++c) {
                auto key = std::make_pair(fvi[c], std::make_pair(uv[c][0], uv[c][1]));
                auto it = weld.find(key);
                if (it == weld.end()) {
                    it = weld.emplace(key, int(fvValues.size())).first;
                    fvValues.push_back(uv[c]);
                }
                fvIndices[c] = it->second;
            }
        }
        // Creases, corners and interpolation rules come separately from the
        // topology.
        topology.SetSubdivTags(sd->GetSubdivTags(id));
        subdiv = HdPbrtSubdiv::Create(topology, subdivLevel, uvFV ? &fvIndices : nullptr, id);
        if (subdiv) {
            VtVec3fArray refined = subdiv->RefinePoints(points);
            if (refined.empty()) {
                subdiv.reset();
            } else {
                points = refined;
                if (uvFV) {
                    uvPv.value = VtValue(subdiv->RefineFaceVaryingUV(fvValues));
                } else if (uvPv.valid && uvPv.value.IsHolding<VtVec2fArray>() &&
                           (uvPv.interp == HdInterpolationVertex ||
                            uvPv.interp == HdInterpolationVarying)) {
                    uvPv.value = VtValue(subdiv->RefineVertexUV(
                        uvPv.value.UncheckedGet<VtVec2fArray>(),
                        uvPv.interp == HdInterpolationVarying));
                } else if (uvPv.valid && uvPv.interp != HdInterpolationConstant) {
                    uvPv = HdPbrtPrimvar();  // uniform uvs: not refined
                }
                topology = subdiv->RefinedTopology();
            }
        }
    }

    HdMeshUtil util(&topology, id);
    VtVec3iArray tris;
    VtIntArray primParams;
    util.ComputeTriangleIndices(&tris, &primParams);

    // Drop triangles with out-of-range indices (bad topology).
    const size_t numTrisBeforeDrop = tris.size();
    {
        VtVec3iArray good;
        VtIntArray goodParams;
        good.reserve(tris.size());
        const int n = (int)points.size();
        for (size_t i = 0; i < tris.size(); ++i) {
            const GfVec3i &t = tris[i];
            if (t[0] >= 0 && t[1] >= 0 && t[2] >= 0 && t[0] < n && t[1] < n && t[2] < n) {
                good.push_back(t);
                if (i < primParams.size())
                    goodParams.push_back(primParams[i]);
            }
        }
        tris = good;
        primParams = goodParams;
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
        fprintf(stderr, "hdPbrt: %s scheme %s points %zu (subdivided %d)\n", id.GetText(),
                topology.GetScheme().GetText(), points.size(), subdiv ? subdivLevel : 0);
    }

    // Normals.
    HdPbrtPrimvar nPv =
        subdiv ? HdPbrtPrimvar() : HdPbrt_FindPrimvar(sd, id, {HdTokens->normals});
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

    _SyncDisplayColor(sd);

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
                 sa.values[k].UncheckedGet<VtVec3fArray>().size() == numBasePoints;
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
            motion->subdiv = subdiv;
            motion->uvs = plyUV;
            if (debug)
                fprintf(stderr, "hdPbrt: %s deforms (%zu point samples, %g..%g)\n",
                        id.GetText(), sa.count, sa.times[0], sa.times[sa.count - 1]);
        }
    }

    _PvSource pvs;
    pvs.sd = sd;
    pvs.source = source;
    pvs.corner = corner;
    pvs.unweld = unweld;
    pvs.tris = tris;
    pvs.primParams = primParams;
    pvs.topology = topology;
    pvs.subdiv = subdiv;
    pvs.numBasePoints = numBasePoints;
    if (numTrisBeforeDrop != tris.size())
        pvs.primParams.clear();  // face-varying data no longer lines up

    std::lock_guard<std::mutex> lock(_mutex);
    if (!_plyPath.empty())
        unlink(_plyPath.c_str());  // a running pbrt keeps its open handle
    _pv = std::move(pvs);
    for (const auto &f : _pvFiles)
        if (!f.second.empty())
            unlink(f.second.c_str());
    _pvFiles.clear();
    _plyPath = path;
    _hasUV = hasUV;
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
        VtVec3fArray pts = _PointsAt(mo.points, times[e]);
        if (mo.subdiv)
            pts = mo.subdiv->RefinePoints(pts);
        if (pts.empty())
            return;
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

std::string HdPbrtMesh::_PrimvarFile(std::string const &name, int width) const
{
    const std::string key = name + "#" + std::to_string(width);
    auto cached = _pvFiles.find(key);
    if (cached != _pvFiles.end())
        return cached->second;
    std::string &file = _pvFiles[key];
    const _PvSource &pv = _pv;
    if (!pv.sd || pv.source.empty())
        return file;
    SdfPath const &id = GetId();
    auto warn = [&](const char *why) {
        if (_pvWarned.insert(key).second)
            TF_WARN("hdPbrt: %s primvar %s: %s; the material uses its default.",
                    id.GetText(), name.c_str(), why);
    };
    // The primvar and its interpolation (Houdini's Cd is displayColor).
    HdInterpolation interp = HdInterpolationCount;
    std::vector<float> vals;
    bool unsupported = false;
    if (!HdPbrt_FetchPrimvar(pv.sd, id, name, width, &interp, &vals, &unsupported)) {
        if (unsupported)
            warn("unsupported value type");
        return file;  // absent: the default, silently
    }
    const size_t count = vals.size() / width;
    const size_t nv = pv.source.size();
    std::vector<float> out(nv * width, 0.f);
    auto at = [&](size_t e, int k) { return vals[e * width + k]; };
    // Per-corner values (face-varying, uniform): to PLY vertices, averaged
    // over the corners when the mesh isn't unwelded.
    auto fromCorners = [&](const std::vector<float> &cv) {
        if (pv.unweld) {
            for (size_t v = 0; v < nv; ++v)
                for (int k = 0; k < width; ++k)
                    out[v * width + k] = cv[pv.corner[v] * width + k];
            return;
        }
        std::vector<int> n(nv, 0);
        for (size_t t = 0; t < pv.tris.size(); ++t)
            for (int c = 0; c < 3; ++c) {
                const int v = pv.tris[t][c];
                if (v < 0 || size_t(v) >= nv)
                    continue;
                for (int k = 0; k < width; ++k)
                    out[v * width + k] += cv[(t * 3 + c) * width + k];
                ++n[v];
            }
        for (size_t v = 0; v < nv; ++v)
            if (n[v])
                for (int k = 0; k < width; ++k)
                    out[v * width + k] /= float(n[v]);
    };
    if (interp == HdInterpolationConstant) {
        for (size_t v = 0; v < nv; ++v)
            for (int k = 0; k < width; ++k)
                out[v * width + k] = at(0, k);
    } else if (interp == HdInterpolationVertex || interp == HdInterpolationVarying) {
        if (count != pv.numBasePoints) {
            warn("wrong element count");
            return file;
        }
        if (pv.subdiv) {
            // Refine three channels at a time as points.
            std::vector<float> refined;
            size_t nref = 0;
            for (int c0 = 0; c0 < width; c0 += 3) {
                VtVec3fArray chunk(count);
                for (size_t e = 0; e < count; ++e)
                    for (int k = 0; k < 3; ++k)
                        chunk[e][k] = c0 + k < width ? at(e, c0 + k) : 0.f;
                VtVec3fArray r = pv.subdiv->RefinePoints(chunk);
                if (r.empty()) {
                    warn("subdivision failed");
                    return file;
                }
                nref = r.size();
                refined.resize(nref * width);
                for (size_t e = 0; e < nref; ++e)
                    for (int k = 0; k < 3 && c0 + k < width; ++k)
                        refined[e * width + c0 + k] = r[e][k];
            }
            vals.swap(refined);
        }
        for (size_t v = 0; v < nv; ++v) {
            const size_t e = size_t(pv.source[v]);
            if (e * width + width > vals.size())
                continue;
            for (int k = 0; k < width; ++k)
                out[v * width + k] = vals[e * width + k];
        }
    } else if (interp == HdInterpolationUniform) {
        if (pv.subdiv || pv.primParams.size() != pv.tris.size()) {
            warn("uniform primvars on subdivided meshes aren't supported");
            return file;
        }
        std::vector<float> cv(pv.tris.size() * 3 * width);
        for (size_t t = 0; t < pv.tris.size(); ++t) {
            const size_t f =
                size_t(HdMeshUtil::DecodeFaceIndexFromCoarseFaceParam(pv.primParams[t]));
            for (int c = 0; c < 3; ++c)
                for (int k = 0; k < width; ++k)
                    cv[(t * 3 + c) * width + k] = f < count ? at(f, k) : 0.f;
        }
        fromCorners(cv);
    } else {  // face-varying
        if (pv.subdiv || pv.primParams.size() != pv.tris.size()) {
            warn("face-varying primvars on subdivided meshes aren't supported");
            return file;
        }
        VtVec4fArray v4(count);
        for (size_t e = 0; e < count; ++e)
            for (int k = 0; k < 4; ++k)
                v4[e][k] = k < width ? at(e, k) : 0.f;
        HdMeshTopology topo = pv.topology;
        HdMeshUtil util(&topo, id);
        VtVec4fArray tri = _Triangulated(util, v4, HdTypeFloatVec4);
        if (tri.size() != pv.tris.size() * 3) {
            warn("face-varying data doesn't match the triangles");
            return file;
        }
        std::vector<float> cv(tri.size() * width);
        for (size_t c = 0; c < tri.size(); ++c)
            for (int k = 0; k < width; ++k)
                cv[c * width + k] = tri[c][k];
        fromCorners(cv);
    }
    std::string path = _delegate->NewTempPath("primvar", ".raw");
    FILE *f = fopen(path.c_str(), "wb");
    if (!f || fwrite(out.data(), sizeof(float), out.size(), f) != out.size()) {
        if (f)
            fclose(f);
        warn("cannot write its file");
        return file;
    }
    fclose(f);
    file = path;
    return file;
}

std::string HdPbrtMesh::_Shapes(std::string const &extra) const
{
    if (_plyPath.empty())
        return "";
    // Deformation blur: the poses at the geo sample times (pbrt patches 5/6:
    // filename = first pose, motionfilenames = the rest).
    const GfVec2f shutter = _delegate->GetShutter();
    const bool blur = shutter[0] != shutter[1];
    if (blur && _motion)
        _UpdateMotionPlys(HdPbrt_ShutterTimes(
            shutter,
            std::max(2, _delegate->GetSetting<int>(HdPbrtSettingsTokens->geoSamples, 2))));
    const bool deforming = blur && _motion && _motionPlys.size() >= 2;
    std::ostringstream shape;
    shape << "Shape \"plymesh\" \"string filename\" "
          << HdPbrt_Quote(deforming ? _motionPlys[0] : _plyPath);
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
    shape << extra << "\n";
    return shape.str();
}

PXR_NAMESPACE_CLOSE_SCOPE
