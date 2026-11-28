// SPDX-License-Identifier: Apache-2.0
// BasisCurves -> pbrt "curve" shapes (one per curve). bspline and cubic
// bezier map directly; catmullRom and linear curves are converted to bezier
// segments. pbrt has one start and one end width per curve, so per-vertex
// widths become a taper from the first to the last width.
#include "hdPbrt.h"

#include "pxr/imaging/hd/basisCurvesTopology.h"
#include "pxr/imaging/hd/sceneDelegate.h"
#include "pxr/imaging/hd/renderIndex.h"
#include "pxr/imaging/hd/tokens.h"

#include <cstdio>

PXR_NAMESPACE_OPEN_SCOPE

HdPbrtCurves::HdPbrtCurves(SdfPath const &id, HdPbrtRenderDelegate *delegate)
    : HdBasisCurves(id), HdPbrtGeom(delegate)
{
}

HdPbrtCurves::~HdPbrtCurves() = default;

void HdPbrtCurves::Finalize(HdRenderParam *renderParam)
{
    static_cast<HdPbrtRenderParam *>(renderParam)->Edit();
}

HdDirtyBits HdPbrtCurves::GetInitialDirtyBitsMask() const
{
    return HdChangeTracker::Clean | HdChangeTracker::InitRepr |
           HdChangeTracker::DirtyPoints | HdChangeTracker::DirtyTopology |
           HdChangeTracker::DirtyTransform | HdChangeTracker::DirtyVisibility |
           HdChangeTracker::DirtyPrimvar | HdChangeTracker::DirtyNormals |
           HdChangeTracker::DirtyWidths | HdChangeTracker::DirtyInstancer |
           HdChangeTracker::DirtyCategories | HdChangeTracker::DirtyMaterialId | HdChangeTracker::DirtyDisplayStyle;
}

void HdPbrtCurves::Sync(HdSceneDelegate *sceneDelegate, HdRenderParam *renderParam,
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
    if (HdChangeTracker::IsTopologyDirty(*dirtyBits, id) ||
        HdChangeTracker::IsAnyPrimvarDirty(*dirtyBits, id) ||
        (*dirtyBits & (HdChangeTracker::DirtyWidths | HdChangeTracker::DirtyNormals))) {
        _SyncDisplayColor(sceneDelegate);
        _BuildShapes(sceneDelegate);
    }

    _UpdateInstancer(sceneDelegate, dirtyBits);
    HdInstancer::_SyncInstancerAndParents(sceneDelegate->GetRenderIndex(), GetInstancerId());
    _SyncInstancing(sceneDelegate);

    *dirtyBits &= ~HdChangeTracker::AllSceneDirtyBits;
}

namespace {

// One curve's control points in pbrt's terms.
struct _PbrtCurve {
    std::vector<GfVec3f> cp;
    bool bspline = false;  // else cubic bezier
};

// Bezier control points of a uniform Catmull-Rom segment p1 -> p2.
void _CatmullRomToBezier(const GfVec3f &p0, const GfVec3f &p1, const GfVec3f &p2,
                         const GfVec3f &p3, std::vector<GfVec3f> *out)
{
    if (out->empty())
        out->push_back(p1);
    out->push_back(p1 + (p2 - p0) / 6.f);
    out->push_back(p2 - (p3 - p1) / 6.f);
    out->push_back(p2);
}

_PbrtCurve _Convert(std::vector<GfVec3f> p, TfToken const &type, TfToken const &basis,
                    TfToken const &wrap)
{
    _PbrtCurve c;
    const size_t n = p.size();
    if (n < 2)
        return c;
    const bool periodic = wrap == HdTokens->periodic;
    const bool pinned = wrap == HdTokens->pinned;
    if (type == HdTokens->linear || n < 4) {
        // Straight segments as cubic beziers (a, a + d/3, a + 2d/3, b).
        if (periodic)
            p.push_back(p[0]);
        c.cp.push_back(p[0]);
        for (size_t i = 0; i + 1 < p.size(); ++i) {
            const GfVec3f d = p[i + 1] - p[i];
            c.cp.push_back(p[i] + d / 3.f);
            c.cp.push_back(p[i] + d * (2.f / 3.f));
            c.cp.push_back(p[i + 1]);
        }
        return c;
    }
    if (basis == HdTokens->bspline) {
        c.bspline = true;
        if (periodic) {
            p.push_back(p[0]);
            p.push_back(p[1]);
            p.push_back(p[2]);
        } else if (pinned) {
            // Phantom points so the curve starts and ends at the end points.
            p.insert(p.begin(), 2.f * p[0] - p[1]);
            p.push_back(2.f * p[p.size() - 1] - p[p.size() - 2]);
        }
        c.cp = std::move(p);
        return c;
    }
    if (basis == HdTokens->catmullRom || basis == HdTokens->centripetalCatmullRom) {
        if (periodic) {
            for (size_t i = 0; i < n; ++i)
                _CatmullRomToBezier(p[(i + n - 1) % n], p[i], p[(i + 1) % n],
                                    p[(i + 2) % n], &c.cp);
        } else {
            if (pinned) {
                p.insert(p.begin(), 2.f * p[0] - p[1]);
                p.push_back(2.f * p[p.size() - 1] - p[p.size() - 2]);
            }
            for (size_t i = 1; i + 2 < p.size(); ++i)
                _CatmullRomToBezier(p[i - 1], p[i], p[i + 1], p[i + 2], &c.cp);
        }
        return c;
    }
    // Cubic bezier (and hermite, which isn't supported: treated as bezier).
    if (periodic)
        p.push_back(p[0]);
    const size_t segs = (p.size() - 1) / 3;
    p.resize(segs * 3 + 1);
    c.cp = std::move(p);
    return c;
}

std::string _Floats(const char *type, const char *name, const float *v, size_t n)
{
    std::string s = std::string(" \"") + type + " " + name + "\" [";
    char buf[32];
    for (size_t i = 0; i < n; ++i) {
        snprintf(buf, sizeof(buf), " %.7g", v[i]);
        s += buf;
    }
    return s + " ]";
}

}  // namespace

void HdPbrtCurves::_BuildShapes(HdSceneDelegate *sd)
{
    SdfPath const &id = GetId();
    const HdBasisCurvesTopology topo = GetBasisCurvesTopology(sd);
    VtVec3fArray points;
    VtValue pv = sd->Get(id, HdTokens->points);
    if (pv.IsHolding<VtVec3fArray>())
        points = pv.UncheckedGet<VtVec3fArray>();
    const VtIntArray &counts = topo.GetCurveVertexCounts();
    const VtIntArray &indices = topo.GetCurveIndices();

    // Widths: constant, per curve (uniform) or per vertex.
    HdPbrtPrimvar wPv = HdPbrt_FindPrimvar(sd, id, {HdTokens->widths});
    VtFloatArray widths;
    if (wPv.valid && wPv.value.IsHolding<VtFloatArray>())
        widths = wPv.value.UncheckedGet<VtFloatArray>();
    else if (wPv.valid && wPv.value.IsHolding<float>())
        widths = VtFloatArray(1, wPv.value.UncheckedGet<float>());
    // Normals make ribbons (oriented flat curves); without them, round tubes.
    HdPbrtPrimvar nPv = HdPbrt_FindPrimvar(sd, id, {HdTokens->normals});
    VtVec3fArray normals;
    if (nPv.valid && nPv.value.IsHolding<VtVec3fArray>())
        normals = nPv.value.UncheckedGet<VtVec3fArray>();

    std::vector<std::string> shapes;
    std::vector<_CurveShape> curveShapes;
    size_t vtx = 0;  // first vertex of the current curve
    for (size_t ci = 0; ci < counts.size(); ++ci) {
        const int count = counts[ci];
        if (count <= 0)
            continue;
        std::vector<GfVec3f> p(count);
        std::vector<int> pis(count);
        bool ok = true;
        for (int k = 0; k < count; ++k) {
            size_t vi = vtx + k;
            int pi = indices.empty() ? int(vi) : (vi < indices.size() ? indices[vi] : -1);
            if (pi < 0 || size_t(pi) >= points.size()) {
                ok = false;
                break;
            }
            p[k] = points[pi];
            pis[k] = pi;
        }
        auto vertexValue = [&](auto const &arr, size_t k, auto fallback) {
            if (arr.empty())
                return fallback;
            if (arr.size() == 1)
                return arr[0];
            if (arr.size() == counts.size())  // uniform
                return arr[ci];
            size_t vi = vtx + k;
            int pi = indices.empty() ? int(vi) : (vi < indices.size() ? indices[vi] : -1);
            return pi >= 0 && size_t(pi) < arr.size() ? arr[pi] : fallback;
        };
        const float w0 = vertexValue(widths, 0, 1.f);
        const float w1 = vertexValue(widths, size_t(count - 1), 1.f);
        vtx += count;
        if (!ok)
            continue;
        _PbrtCurve c = _Convert(std::move(p), topo.GetCurveType(), topo.GetCurveBasis(),
                                topo.GetCurveWrap());
        if (c.cp.size() < 4)
            continue;
        std::string s = "Shape \"curve\" \"string basis\" ";
        s += c.bspline ? "\"bspline\"" : "\"bezier\"";
        s += _Floats("point3", "P", c.cp.front().data(), c.cp.size() * 3);
        char wbuf[96];
        snprintf(wbuf, sizeof(wbuf), " \"float width0\" %.7g \"float width1\" %.7g",
                 std::max(w0, 0.f), std::max(w1, 0.f));
        s += wbuf;
        if (!normals.empty()) {
            // Ribbon: one normal per segment boundary, picked from the curve's
            // vertices at the matching fraction of its length.
            const size_t segs =
                c.bspline ? c.cp.size() - 3 : (c.cp.size() - 1) / 3;
            std::vector<GfVec3f> nb(segs + 1);
            for (size_t j = 0; j <= segs; ++j) {
                size_t k = segs ? size_t(std::lround(double(j) * (count - 1) / segs)) : 0;
                nb[j] = vertexValue(normals, k, GfVec3f(0, 0, 1));
            }
            s += " \"string type\" \"ribbon\"";
            s += _Floats("normal", "N", nb.front().data(), nb.size() * 3);
        } else {
            s += " \"string type\" \"cylinder\"";
        }
        shapes.push_back(std::move(s));
        _CurveShape cs;
        cs.curve = ci;
        cs.points = std::move(pis);
        cs.segments = int(c.bspline ? c.cp.size() - 3 : (c.cp.size() - 1) / 3);
        curveShapes.push_back(std::move(cs));
    }

    std::lock_guard<std::mutex> lock(_mutex);
    _shapes = std::move(shapes);
    _curveShapes = std::move(curveShapes);
    _numPoints = points.size();
    _numCurves = counts.size();
    _curveType = topo.GetCurveType();
    _curveBasis = topo.GetCurveBasis();
    _curveWrap = topo.GetCurveWrap();
}

std::string HdPbrtCurves::_Shapes(std::string const &extra) const
{
    std::string out;
    for (size_t i = 0; i < _shapes.size(); ++i)
        out += _shapes[i] + extra + _ShapePv(i) + "\n";
    return out;
}

// Per pbrt curve: each primvar's values at its Bezier segment boundaries
// (pbrt interpolates linearly within a segment). Vertex primvars go through
// the same basis conversion as the points (it is linear) and are evaluated
// at the boundaries; varying ones with one value per boundary are used as
// they are (else resampled by index); uniform and constant ones repeat.
std::vector<std::string> HdPbrtCurves::_ShapePrimvars(PrimvarList const &pvs,
                                                      std::set<std::string> *found) const
{
    struct Src {
        std::pair<std::string, int> pv;
        HdInterpolation interp;
        std::vector<float> v;
    };
    std::vector<Src> srcs;
    for (const auto &pv : pvs) {
        Src s{pv, HdInterpolationConstant, {}};
        if (!HdPbrt_FetchPrimvar(_sd, GetId(), pv.first, pv.second, &s.interp, &s.v) ||
            s.v.size() < size_t(pv.second))
            continue;
        found->insert(pv.first);
        srcs.push_back(std::move(s));
    }
    if (srcs.empty())
        return {};
    PrimvarList names;
    for (const Src &s : srcs)
        names.push_back(s.pv);
    // Linear resampling of m values to n (by index fraction).
    auto resample = [](const std::vector<float> &in, int w, size_t n) {
        const size_t m = in.size() / w;
        std::vector<float> out(n * w);
        for (size_t j = 0; j < n; ++j) {
            const double x = n > 1 ? double(j) * (m - 1) / (n - 1) : 0.0;
            const size_t a = std::min(size_t(x), m - 1), b = std::min(a + 1, m - 1);
            const float f = float(x - a);
            for (int k = 0; k < w; ++k)
                out[j * w + k] = in[a * w + k] * (1 - f) + in[b * w + k] * f;
        }
        return out;
    };
    // Varying values per curve, laid out curve after curve.
    std::vector<size_t> varyingStart;
    size_t varyingTotal = 0;
    {
        // Count per curve as USD defines it: linear = vertices; cubic
        // non-periodic: bezier (n - 4) / 3 + 2, others n - 2; periodic:
        // bezier n / 3, others n.
        const bool periodic = _curveWrap == HdTokens->periodic;
        const bool linear = _curveType == HdTokens->linear;
        const bool bezier = _curveBasis == HdTokens->bezier;
        std::vector<size_t> perCurve(_numCurves, 0);
        for (const _CurveShape &cs : _curveShapes) {
            const size_t n = cs.points.size();
            perCurve[cs.curve] = linear ? n
                                 : periodic ? (bezier ? n / 3 : n)
                                 : bezier ? (n >= 4 ? (n - 4) / 3 + 2 : n)
                                          : (n >= 2 ? n - 2 : n);
        }
        varyingStart.resize(_numCurves + 1, 0);
        for (size_t c = 0; c < _numCurves; ++c)
            varyingStart[c + 1] = varyingStart[c] + perCurve[c];
        varyingTotal = varyingStart[_numCurves];
    }
    std::vector<std::string> out;
    out.reserve(_curveShapes.size());
    for (const _CurveShape &cs : _curveShapes) {
        const size_t nb = size_t(cs.segments) + 1;
        std::vector<std::vector<float>> values;
        for (const Src &s : srcs) {
            const int w = s.pv.second;
            const size_t count = s.v.size() / w;
            std::vector<float> b;
            if ((s.interp == HdInterpolationVertex ||
                 (s.interp == HdInterpolationVarying && count != varyingTotal)) &&
                count == _numPoints) {
                // Through the basis conversion, three channels at a time.
                b.assign(nb * w, 0.f);
                for (int c0 = 0; c0 < w; c0 += 3) {
                    std::vector<GfVec3f> p(cs.points.size());
                    for (size_t k = 0; k < cs.points.size(); ++k)
                        for (int j = 0; j < 3; ++j)
                            p[k][j] = c0 + j < w ? s.v[size_t(cs.points[k]) * w + c0 + j] : 0.f;
                    _PbrtCurve c = _Convert(std::move(p), _curveType, _curveBasis, _curveWrap);
                    for (size_t seg = 0; seg < nb; ++seg) {
                        GfVec3f e;
                        if (c.bspline) {
                            const size_t i = std::min(seg, c.cp.size() - 3);
                            e = (c.cp[i] + 4.f * c.cp[i + 1] + c.cp[i + 2]) / 6.f;
                        } else {
                            e = c.cp[std::min(seg * 3, c.cp.size() - 1)];
                        }
                        for (int j = 0; j < 3 && c0 + j < w; ++j)
                            b[seg * w + c0 + j] = e[j];
                    }
                }
            } else if (s.interp == HdInterpolationVarying && count == varyingTotal) {
                const size_t a = varyingStart[cs.curve], e = varyingStart[cs.curve + 1];
                std::vector<float> in(s.v.begin() + a * w, s.v.begin() + e * w);
                if (_curveWrap == HdTokens->periodic && !in.empty())
                    in.insert(in.end(), in.begin(), in.begin() + w);  // closes the loop
                b = in.size() / w == nb ? in : (in.empty() ? std::vector<float>(nb * w, 0.f)
                                                           : resample(in, w, nb));
            } else {
                // Uniform (per curve) or constant.
                const size_t e = (s.interp == HdInterpolationUniform && count == _numCurves)
                                     ? cs.curve : 0;
                b.resize(nb * w);
                for (size_t j = 0; j < nb; ++j)
                    for (int k = 0; k < w; ++k)
                        b[j * w + k] = s.v[e * w + k];
            }
            values.push_back(std::move(b));
        }
        out.push_back(HdPbrt_ShapePrimvarParams(names, values, int(nb)));
    }
    return out;
}

PXR_NAMESPACE_CLOSE_SCOPE
