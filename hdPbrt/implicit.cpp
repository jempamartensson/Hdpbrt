// SPDX-License-Identifier: Apache-2.0
// UsdGeom implicit surfaces (Sphere, Cylinder, Cone, Capsule) as exact pbrt
// shapes. Hydra's to-mesh conversion gives ~10-segment Catmull-Clark meshes,
// and subdividing those shrinks them (a radius-1 sphere came out 0.93-0.99)
// and rounds the cylinder and cone rims.
//   sphere            -> pbrt sphere
//   cylinder          -> pbrt cylinder + two disks
//   capsule           -> pbrt cylinder + two half spheres
//   cone, frustum     -> a 128-segment triangle mesh with exact normals + disks
//   tapered capsule   -> GeomUtil's capsule mesh, finely tessellated
// pbrt's shapes run along +z; the prim's axis rotates them.
#include "hdPbrt.h"

#include "pxr/imaging/geomUtil/capsuleMeshGenerator.h"
#include "pxr/imaging/hd/capsuleSchema.h"
#include "pxr/imaging/hd/coneSchema.h"
#include "pxr/imaging/hd/cylinderSchema.h"
#include "pxr/imaging/hd/sceneDelegate.h"
#include "pxr/imaging/hd/sphereSchema.h"
#include "pxr/imaging/hd/tokens.h"

#include <cmath>
#include <cstdio>

PXR_NAMESPACE_OPEN_SCOPE

namespace {

double _GetDouble(HdSceneDelegate *sd, SdfPath const &id, TfToken const &key, double fallback,
                  bool *found = nullptr)
{
    VtValue v = sd->Get(id, key);
    if (found)
        *found = true;
    if (v.IsHolding<double>())
        return v.UncheckedGet<double>();
    if (v.IsHolding<float>())
        return v.UncheckedGet<float>();
    if (found)
        *found = false;
    return fallback;
}

// Rotation taking pbrt's +z to the prim's axis.
std::string _AxisRotation(TfToken const &axis)
{
    if (axis == "X")
        return "Rotate 90 0 1 0 ";
    if (axis == "Y")
        return "Rotate -90 1 0 0 ";
    return "";
}

std::string _Fmt(const char *f, double a, double b = 0, double c = 0)
{
    char buf[160];
    snprintf(buf, sizeof(buf), f, a, b, c);
    return buf;
}

// Disk caps: the top faces +z, the bottom -z (normals matter for media and
// one-sided emission).
void _Caps(std::vector<std::string> *out, double h, double rBottom, double rTop)
{
    if (rTop > 0)
        out->push_back(_Fmt("Translate 0 0 %.9g Shape \"disk\" \"float radius\" %.9g", 0.5 * h,
                            rTop));
    if (rBottom > 0)
        out->push_back(_Fmt("Translate 0 0 %.9g Rotate 180 1 0 0 Shape \"disk\" \"float radius\" %.9g",
                            -0.5 * h, rBottom));
}

std::string _TriangleMesh(std::vector<GfVec3f> const &p, std::vector<GfVec3f> const &n,
                          std::vector<GfVec2f> const &uv, std::vector<int> const &idx)
{
    std::string s = "Shape \"trianglemesh\" \"point3 P\" [";
    char buf[96];
    for (auto const &v : p) {
        snprintf(buf, sizeof(buf), " %.7g %.7g %.7g", v[0], v[1], v[2]);
        s += buf;
    }
    s += " ] \"normal N\" [";
    for (auto const &v : n) {
        snprintf(buf, sizeof(buf), " %.6g %.6g %.6g", v[0], v[1], v[2]);
        s += buf;
    }
    s += " ]";
    if (!uv.empty()) {
        s += " \"point2 uv\" [";
        for (auto const &v : uv) {
            snprintf(buf, sizeof(buf), " %.6g %.6g", v[0], v[1]);
            s += buf;
        }
        s += " ]";
    }
    s += " \"integer indices\" [";
    for (int i : idx)
        s += " " + std::to_string(i);
    return s + " ]";
}

// Side of a cone frustum from radius rb at z = -h/2 to rt at z = h/2
// (rt = 0: a cone). Straight along z, so one band is exact; the shading
// normals are the surface's own.
std::string _FrustumSide(double h, double rb, double rt)
{
    const int N = 128;
    std::vector<GfVec3f> p, n;
    std::vector<GfVec2f> uv;
    std::vector<int> idx;
    const double nz = rb - rt;  // outward normal ~ (h cos, h sin, rb - rt)
    for (int i = 0; i <= N; ++i) {
        const double phi = 2 * M_PI * i / N, c = std::cos(phi), s = std::sin(phi);
        GfVec3f nn(float(h * c), float(h * s), float(nz));
        nn.Normalize();
        p.emplace_back(float(rb * c), float(rb * s), float(-0.5 * h));
        p.emplace_back(float(rt * c), float(rt * s), float(0.5 * h));
        n.push_back(nn);
        n.push_back(nn);
        uv.emplace_back(float(i) / N, 0.f);
        uv.emplace_back(float(i) / N, 1.f);
    }
    for (int i = 0; i < N; ++i) {
        const int b0 = 2 * i, t0 = 2 * i + 1, b1 = 2 * i + 2, t1 = 2 * i + 3;
        if (rb > 0)
            idx.insert(idx.end(), {b0, b1, t1});
        if (rt > 0)
            idx.insert(idx.end(), {b0, t1, t0});
    }
    return _TriangleMesh(p, n, uv, idx);
}

std::string _TaperedCapsule(double h, double rb, double rt)
{
    const size_t numRadial = 96, numCapAxial = 24;
    const size_t np = GeomUtilCapsuleMeshGenerator::ComputeNumPoints(numRadial, numCapAxial);
    std::vector<GfVec3f> p(np), n(np);
    GeomUtilCapsuleMeshGenerator::GeneratePoints(p.begin(), numRadial, numCapAxial, rb, rt, h);
    GeomUtilCapsuleMeshGenerator::GenerateNormals(n.begin(), numRadial, numCapAxial, rb, rt, h);
    PxOsdMeshTopology topo = GeomUtilCapsuleMeshGenerator::GenerateTopology(numRadial,
                                                                            numCapAxial);
    std::vector<int> idx;
    const VtIntArray &counts = topo.GetFaceVertexCounts();
    const VtIntArray &fvi = topo.GetFaceVertexIndices();
    const bool flip = topo.GetOrientation() == PxOsdOpenSubdivTokens->leftHanded;
    size_t base = 0;
    for (int c : counts) {
        for (int k = 1; k + 1 < c; ++k) {
            idx.push_back(fvi[base]);
            idx.push_back(fvi[base + (flip ? k + 1 : k)]);
            idx.push_back(fvi[base + (flip ? k : k + 1)]);
        }
        base += c;
    }
    return _TriangleMesh(p, n, {}, idx);
}

}  // namespace

HdPbrtImplicit::HdPbrtImplicit(TfToken const &type, SdfPath const &id,
                               HdPbrtRenderDelegate *delegate)
    : HdRprim(id), HdPbrtGeom(delegate), _type(type)
{
}

HdPbrtImplicit::~HdPbrtImplicit() = default;

void HdPbrtImplicit::Finalize(HdRenderParam *renderParam)
{
    static_cast<HdPbrtRenderParam *>(renderParam)->Edit();
}

TfTokenVector const &HdPbrtImplicit::GetBuiltinPrimvarNames() const
{
    static const TfTokenVector names;
    return names;
}

HdDirtyBits HdPbrtImplicit::GetInitialDirtyBitsMask() const
{
    return HdChangeTracker::Clean | HdChangeTracker::InitRepr | HdChangeTracker::DirtyPoints |
           HdChangeTracker::DirtyTransform | HdChangeTracker::DirtyVisibility |
           HdChangeTracker::DirtyPrimvar | HdChangeTracker::DirtyInstancer |
           HdChangeTracker::DirtyCategories | HdChangeTracker::DirtyMaterialId;
}

void HdPbrtImplicit::Sync(HdSceneDelegate *sd, HdRenderParam *renderParam,
                          HdDirtyBits *dirtyBits, TfToken const &reprToken)
{
    SdfPath const &id = GetId();
    static_cast<HdPbrtRenderParam *>(renderParam)->Edit();

    if (*dirtyBits & HdChangeTracker::DirtyMaterialId)
        SetMaterialId(sd->GetMaterialId(id));
    if (HdChangeTracker::IsTransformDirty(*dirtyBits, id))
        _SyncTransform(sd);
    if (*dirtyBits & HdChangeTracker::DirtyCategories)
        _SyncCategories(sd);
    {
        std::lock_guard<std::mutex> lock(_mutex);
        if (HdChangeTracker::IsVisibilityDirty(*dirtyBits, id))
            _UpdateVisibility(sd, dirtyBits);
        _visible = IsVisible();
    }
    if (HdChangeTracker::IsAnyPrimvarDirty(*dirtyBits, id) ||
        (*dirtyBits & HdChangeTracker::DirtyPoints)) {
        _SyncDisplayColor(sd);
        _SyncShape(sd);
    }

    _UpdateInstancer(sd, dirtyBits);
    HdInstancer::_SyncInstancerAndParents(sd->GetRenderIndex(), GetInstancerId());
    _SyncInstancing(sd);

    *dirtyBits &= ~HdChangeTracker::AllSceneDirtyBits;
}

void HdPbrtImplicit::_SyncShape(HdSceneDelegate *sd)
{
    SdfPath const &id = GetId();
    std::vector<std::string> shapes;
    std::string axis;
    if (_type == HdPrimTypeTokens->sphere) {
        const double r = _GetDouble(sd, id, HdSphereSchemaTokens->radius, 1.0);
        if (r > 0)
            shapes.push_back(_Fmt("Shape \"sphere\" \"float radius\" %.9g", r));
    } else {
        VtValue a = sd->Get(id, HdCylinderSchemaTokens->axis);  // same token for all
        axis = _AxisRotation(a.IsHolding<TfToken>() ? a.UncheckedGet<TfToken>() : TfToken("Z"));
        // Defaults per UsdGeom: cylinder and cone 2 x 1, capsule 1 x 0.5.
        const bool capsule = _type == HdPrimTypeTokens->capsule;
        const bool cone = _type == HdPrimTypeTokens->cone;
        const double h = std::max(0.0, _GetDouble(sd, id, HdCylinderSchemaTokens->height,
                                                  capsule ? 1.0 : 2.0));
        const double r = _GetDouble(sd, id, HdCylinderSchemaTokens->radius, capsule ? 0.5 : 1.0);
        double rb = r, rt = cone ? 0.0 : r;
        if (!cone) {
            // Cylinder_1 / Capsule_1: radiusTop / radiusBottom win over radius.
            bool hasTop = false, hasBottom = false;
            const double top = _GetDouble(sd, id, HdCylinderSchemaTokens->radiusTop, r, &hasTop);
            const double bottom =
                _GetDouble(sd, id, HdCylinderSchemaTokens->radiusBottom, r, &hasBottom);
            if (hasTop || hasBottom) {
                rt = top;
                rb = bottom;
            }
        }
        rb = std::max(0.0, rb);
        rt = std::max(0.0, rt);
        if (capsule) {
            if (rb == rt) {
                if (h > 0 && rb > 0)
                    shapes.push_back(_Fmt("Shape \"cylinder\" \"float radius\" %.9g \"float zmin\" "
                                          "%.9g \"float zmax\" %.9g",
                                          rb, -0.5 * h, 0.5 * h));
                if (rb > 0) {
                    shapes.push_back(_Fmt("Translate 0 0 %.9g Shape \"sphere\" \"float radius\" "
                                          "%.9g \"float zmin\" 0 \"float zmax\" %.9g",
                                          0.5 * h, rb, rb));
                    shapes.push_back(_Fmt("Translate 0 0 %.9g Shape \"sphere\" \"float radius\" "
                                          "%.9g \"float zmin\" %.9g \"float zmax\" 0",
                                          -0.5 * h, rb, -rb));
                }
            } else {
                shapes.push_back(_TaperedCapsule(h, rb, rt));
            }
        } else if (h > 0) {
            if (rb == rt) {
                if (rb > 0)
                    shapes.push_back(_Fmt("Shape \"cylinder\" \"float radius\" %.9g \"float zmin\" "
                                          "%.9g \"float zmax\" %.9g",
                                          rb, -0.5 * h, 0.5 * h));
            } else {
                shapes.push_back(_FrustumSide(h, rb, rt));
            }
            _Caps(&shapes, h, rb, rt);
        }
    }
    if (getenv("HDPBRT_DEBUG"))
        fprintf(stderr, "hdPbrt %s: %s, %zu pbrt shapes\n", id.GetText(), _type.GetText(),
                shapes.size());
    std::lock_guard<std::mutex> lock(_mutex);
    _axis = axis;
    _shapes = std::move(shapes);
}

std::string HdPbrtImplicit::_Shapes(std::string const &extra) const
{
    std::string out;
    for (const std::string &s : _shapes)
        out += "AttributeBegin " + _axis + s + extra + " AttributeEnd\n";
    return out;
}

PXR_NAMESPACE_CLOSE_SCOPE
