// SPDX-License-Identifier: Apache-2.0
// Uniform subdivision of catmullClark and loop meshes with OpenSubdiv, via
// USD's PxOsdRefinerFactory (which maps creases, corners, holes, boundary and
// face-varying interpolation rules from the USD topology).
#include "hdPbrt.h"

#include "pxr/imaging/pxOsd/refinerFactory.h"
#include "pxr/imaging/pxOsd/tokens.h"

#include <opensubdiv/far/primvarRefiner.h>
#include <opensubdiv/far/topologyRefiner.h>

PXR_NAMESPACE_OPEN_SCOPE

using namespace OpenSubdiv;

namespace {

template <int N>
struct _Value {
    float v[N];
    void Clear(void * = nullptr) {
        for (int i = 0; i < N; ++i)
            v[i] = 0.f;
    }
    void AddWithWeight(_Value const &s, float w) {
        for (int i = 0; i < N; ++i)
            v[i] += w * s.v[i];
    }
};

}  // namespace

struct HdPbrtSubdiv::_Impl {
    PxOsdTopologyRefinerSharedPtr refiner;
    int level = 0;
};

HdPbrtSubdiv::HdPbrtSubdiv() : _impl(new _Impl) {}
HdPbrtSubdiv::~HdPbrtSubdiv() = default;

std::shared_ptr<HdPbrtSubdiv> HdPbrtSubdiv::Create(HdMeshTopology const &topology, int level,
                                                   VtIntArray const *fvarIndices,
                                                   SdfPath const &id)
{
    const TfToken scheme = topology.GetScheme();
    if (level <= 0 || (scheme != PxOsdOpenSubdivTokens->catmullClark &&
                       scheme != PxOsdOpenSubdivTokens->loop))
        return nullptr;
    PxOsdTopologyRefinerSharedPtr r;
    if (fvarIndices)
        r = PxOsdRefinerFactory::Create(topology.GetPxOsdMeshTopology(), {*fvarIndices},
                                        TfToken(id.GetString()));
    else
        r = PxOsdRefinerFactory::Create(topology.GetPxOsdMeshTopology(), TfToken(id.GetString()));
    if (!r || r->GetLevel(0).GetNumFaces() == 0)
        return nullptr;
    Far::TopologyRefiner::UniformOptions opts(level);
    opts.fullTopologyInLastLevel = true;
    r->RefineUniform(opts);

    std::shared_ptr<HdPbrtSubdiv> s(new HdPbrtSubdiv);
    s->_impl->refiner = r;
    s->_impl->level = level;
    s->_hasFVar = fvarIndices != nullptr;

    // The last level's faces (minus holes) as a plain topology. The scheme
    // is kept so smooth normals get computed for it; it isn't refined again.
    const Far::TopologyLevel &last = r->GetLevel(level);
    VtIntArray counts, indices;
    counts.reserve(last.GetNumFaces());
    for (int f = 0; f < last.GetNumFaces(); ++f) {
        if (last.IsFaceHole(f))
            continue;
        Far::ConstIndexArray fv = last.GetFaceVertices(f);
        counts.push_back(fv.size());
        for (int k = 0; k < fv.size(); ++k)
            indices.push_back(fv[k]);
    }
    s->_topology = HdMeshTopology(scheme, topology.GetOrientation(), counts, indices);
    return s;
}

template <int N, typename T>
static VtArray<T> _Refine(const Far::TopologyRefiner &r, int level, VtArray<T> const &src,
                          bool varying)
{
    const int n0 = r.GetLevel(0).GetNumVertices();
    if (int(src.size()) != n0)
        return {};
    std::vector<_Value<N>> buf(r.GetNumVerticesTotal());
    for (int i = 0; i < n0; ++i)
        for (int k = 0; k < N; ++k)
            buf[i].v[k] = src[i][k];
    Far::PrimvarRefiner pr(r);
    _Value<N> *s = buf.data();
    for (int l = 1; l <= level; ++l) {
        _Value<N> *d = s + r.GetLevel(l - 1).GetNumVertices();
        if (varying)
            pr.InterpolateVarying(l, s, d);
        else
            pr.Interpolate(l, s, d);
        s = d;
    }
    const int nl = r.GetLevel(level).GetNumVertices();
    VtArray<T> out(nl);
    for (int i = 0; i < nl; ++i)
        for (int k = 0; k < N; ++k)
            out[i][k] = s[i].v[k];
    return out;
}

VtVec3fArray HdPbrtSubdiv::RefinePoints(VtVec3fArray const &points) const
{
    return _Refine<3>(*_impl->refiner, _impl->level, points, false);
}

VtVec2fArray HdPbrtSubdiv::RefineVertexUV(VtVec2fArray const &uv, bool varying) const
{
    return _Refine<2>(*_impl->refiner, _impl->level, uv, varying);
}

VtVec2fArray HdPbrtSubdiv::RefineFaceVaryingUV(VtVec2fArray const &values) const
{
    const Far::TopologyRefiner &r = *_impl->refiner;
    const int level = _impl->level;
    if (!_hasFVar || int(values.size()) != r.GetLevel(0).GetNumFVarValues(0))
        return {};
    int total = 0;
    for (int l = 0; l <= level; ++l)
        total += r.GetLevel(l).GetNumFVarValues(0);
    std::vector<_Value<2>> buf(total);
    for (size_t i = 0; i < values.size(); ++i)
        buf[i].v[0] = values[i][0], buf[i].v[1] = values[i][1];
    Far::PrimvarRefiner pr(r);
    _Value<2> *s = buf.data();
    for (int l = 1; l <= level; ++l) {
        _Value<2> *d = s + r.GetLevel(l - 1).GetNumFVarValues(0);
        pr.InterpolateFaceVarying(l, s, d, 0);
        s = d;
    }
    // Per refined face corner, in the order of RefinedTopology().
    const Far::TopologyLevel &last = r.GetLevel(level);
    VtVec2fArray out;
    out.reserve(_topology.GetFaceVertexIndices().size());
    for (int f = 0; f < last.GetNumFaces(); ++f) {
        if (last.IsFaceHole(f))
            continue;
        Far::ConstIndexArray fv = last.GetFaceFVarValues(f, 0);
        for (int k = 0; k < fv.size(); ++k)
            out.push_back(GfVec2f(s[fv[k]].v[0], s[fv[k]].v[1]));
    }
    return out;
}

PXR_NAMESPACE_CLOSE_SCOPE
