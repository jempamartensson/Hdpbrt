// SPDX-License-Identifier: Apache-2.0
// Volumes: OpenVDB fields become a pbrt "nanovdb" medium inside an interface
// box. The density (and temperature) grids are converted to NanoVDB by the
// hdpbrt_vdb2nvdb tool next to pbrt (cached per file and grid).
#include "hdPbrt.h"

#include "pxr/imaging/hd/field.h"
#include "pxr/imaging/hd/sceneDelegate.h"
#include "pxr/imaging/hd/renderIndex.h"
#include "pxr/imaging/hd/tokens.h"
#include "pxr/usd/sdf/assetPath.h"

#include <cstdio>

PXR_NAMESPACE_OPEN_SCOPE

TF_DEFINE_PRIVATE_TOKENS(_tokens, (density)(temperature));

// --- field ------------------------------------------------------------------

HdPbrtField::HdPbrtField(SdfPath const &id) : HdField(id) {}

void HdPbrtField::Sync(HdSceneDelegate *sd, HdRenderParam *renderParam, HdDirtyBits *dirtyBits)
{
    if (*dirtyBits & DirtyParams) {
        VtValue fp = sd->Get(GetId(), HdFieldTokens->filePath);
        std::string path;
        if (fp.IsHolding<SdfAssetPath>()) {
            const SdfAssetPath &ap = fp.UncheckedGet<SdfAssetPath>();
            path = ap.GetResolvedPath().empty() ? ap.GetAssetPath() : ap.GetResolvedPath();
        } else if (fp.IsHolding<std::string>()) {
            path = fp.UncheckedGet<std::string>();
        }
        VtValue fn = sd->Get(GetId(), HdFieldTokens->fieldName);
        std::string grid;
        if (fn.IsHolding<TfToken>())
            grid = fn.UncheckedGet<TfToken>().GetString();
        else if (fn.IsHolding<std::string>())
            grid = fn.UncheckedGet<std::string>();
        std::lock_guard<std::mutex> l(_mutex);
        _filePath = path;
        _gridName = grid;
        if (renderParam)
            static_cast<HdPbrtRenderParam *>(renderParam)->Edit();
    }
    *dirtyBits = Clean;
}

// --- volume -----------------------------------------------------------------

HdPbrtVolume::HdPbrtVolume(SdfPath const &id, HdPbrtRenderDelegate *delegate)
    : HdVolume(id), HdPbrtGeom(delegate)
{
}

HdPbrtVolume::~HdPbrtVolume() = default;

void HdPbrtVolume::Finalize(HdRenderParam *renderParam)
{
    static_cast<HdPbrtRenderParam *>(renderParam)->Edit();
}

HdDirtyBits HdPbrtVolume::GetInitialDirtyBitsMask() const
{
    return HdChangeTracker::Clean | HdChangeTracker::InitRepr |
           HdChangeTracker::DirtyTransform | HdChangeTracker::DirtyVisibility |
           HdChangeTracker::DirtyVolumeField | HdChangeTracker::DirtyPrimvar |
           HdChangeTracker::DirtyMaterialId | HdChangeTracker::DirtyInstancer;
}

void HdPbrtVolume::Sync(HdSceneDelegate *sceneDelegate, HdRenderParam *renderParam,
                        HdDirtyBits *dirtyBits, TfToken const &reprToken)
{
    SdfPath const &id = GetId();
    static_cast<HdPbrtRenderParam *>(renderParam)->Edit();
    if (*dirtyBits & HdChangeTracker::DirtyMaterialId)
        SetMaterialId(sceneDelegate->GetMaterialId(id));
    if (HdChangeTracker::IsTransformDirty(*dirtyBits, id))
        _SyncTransform(sceneDelegate);
    {
        std::lock_guard<std::mutex> lock(_mutex);
        if (HdChangeTracker::IsVisibilityDirty(*dirtyBits, id))
            _UpdateVisibility(sceneDelegate, dirtyBits);
        _visible = IsVisible();
    }
    if (HdChangeTracker::IsAnyPrimvarDirty(*dirtyBits, id))
        _SyncDisplayColor(sceneDelegate);

    // Fields (always re-read: a field's file can change without the volume
    // being marked dirty). The density field is the one named "density", or
    // the first; "temperature" is optional.
    std::string densityFile, densityGrid, temperatureFile, temperatureGrid;
    for (const HdVolumeFieldDescriptor &fd : sceneDelegate->GetVolumeFieldDescriptors(id)) {
        auto *field = dynamic_cast<HdPbrtField *>(
            sceneDelegate->GetRenderIndex().GetBprim(fd.fieldPrimType, fd.fieldId));
        if (!field)
            continue;
        std::string file, grid;
        field->Get(&file, &grid);
        if (grid.empty())
            grid = fd.fieldName.GetString();
        if (fd.fieldName == _tokens->temperature) {
            temperatureFile = file;
            temperatureGrid = grid;
        } else if (fd.fieldName == _tokens->density || densityFile.empty()) {
            densityFile = file;
            densityGrid = grid;
        }
    }
    std::string nvdb;
    GfRange3d bbox;
    bool hasTemperature = false;
    if (densityFile.rfind("op:", 0) == 0) {
        TF_WARN("hdPbrt: %s: in-memory Houdini volumes (%s) aren't supported; write the "
                "VDB to disk (e.g. with a File Cache) to render it", id.GetText(),
                densityFile.c_str());
    } else if (!densityFile.empty()) {
        // The temperature grid only goes along when it's in the same file.
        const bool sameFile = !temperatureFile.empty() && temperatureFile == densityFile;
        hasTemperature = sameFile;
        if (!_delegate->GetNanoVDB(densityFile, densityGrid, sameFile ? temperatureGrid : "",
                                   &nvdb, &bbox))
            nvdb.clear();
    }
    {
        std::lock_guard<std::mutex> lock(_mutex);
        _nvdb = nvdb;
        _bbox = bbox;
        _hasTemperature = hasTemperature && !nvdb.empty();
    }

    _UpdateInstancer(sceneDelegate, dirtyBits);
    HdInstancer::_SyncInstancerAndParents(sceneDelegate->GetRenderIndex(), GetInstancerId());
    _SyncInstancing(sceneDelegate);
    *dirtyBits &= ~HdChangeTracker::AllSceneDirtyBits;
}

void HdPbrtVolume::Write(std::ostream &os, HdRenderIndex &index, TfTokenVector const &renderTags,
                         HdPbrtWriteContext &ctx) const
{
    std::lock_guard<std::mutex> lock(_mutex);
    if (!_visible || _nvdb.empty() || _bbox.IsEmpty())
        return;
    if (!renderTags.empty() &&
        std::find(renderTags.begin(), renderTags.end(), GetRenderTag()) == renderTags.end())
        return;
    if (_instanced) {
        static bool warned = false;
        if (!warned)
            TF_WARN("hdPbrt: instanced volumes aren't supported (%s)", GetId().GetText());
        warned = true;
        return;
    }
    // A pbrt medium lives in the coordinate system current at MakeNamedMedium:
    // the volume's transform (at shutter open; media don't move).
    const GfMatrix4d xf = HdPbrt_XformAt(_xform, _delegate->GetShutter()[0]);
    const float density =
        _delegate->GetSetting<float>(HdPbrtSettingsTokens->volumeDensity, 1.f);
    const float albedo = std::min(
        1.f, std::max(0.f, _delegate->GetSetting<float>(HdPbrtSettingsTokens->volumeAlbedo,
                                                        0.9f)));
    GfVec3f tint = _hasDisplayColor ? _displayColor : GfVec3f(1.f);
    const GfVec3f sigmaS = albedo * tint;
    const GfVec3f sigmaA = GfVec3f(1.f) - sigmaS;
    const float tempScale =
        _delegate->GetSetting<float>(HdPbrtSettingsTokens->volumeTemperatureScale, 0.f);

    const std::string name = "hdpbrt_vol_" + std::to_string(ctx.volumeCounter++);
    const GfVec3d lo = _bbox.GetMin(), hi = _bbox.GetMax();
    char box[512];
    // Outward-facing box: pbrt picks the medium a ray enters from the normal.
    snprintf(box, sizeof(box),
             "  Shape \"trianglemesh\" \"integer indices\" [0 2 1 0 3 2  4 5 6 4 6 7  "
             "0 5 4 0 1 5  3 6 2 3 7 6  0 7 3 0 4 7  1 6 5 1 2 6]\n"
             "    \"point3 P\" [%.9g %.9g %.9g  %.9g %.9g %.9g  %.9g %.9g %.9g  %.9g %.9g %.9g  "
             "%.9g %.9g %.9g  %.9g %.9g %.9g  %.9g %.9g %.9g  %.9g %.9g %.9g]\n",
             lo[0], lo[1], lo[2], hi[0], lo[1], lo[2], hi[0], hi[1], lo[2], lo[0], hi[1], lo[2],
             lo[0], lo[1], hi[2], hi[0], lo[1], hi[2], hi[0], hi[1], hi[2], lo[0], hi[1], hi[2]);
    os << "# " << GetId().GetString() << "\nAttributeBegin\n  Transform "
       << HdPbrt_Matrix(xf) << "\n  MakeNamedMedium " << HdPbrt_Quote(name)
       << " \"string type\" \"nanovdb\" \"string filename\" " << HdPbrt_Quote(_nvdb)
       << " \"rgb sigma_a\" " << HdPbrt_Rgb(sigmaA) << " \"rgb sigma_s\" " << HdPbrt_Rgb(sigmaS)
       << " \"float scale\" " << density;
    if (_hasTemperature && tempScale > 0.f)
        os << " \"float temperaturescale\" " << tempScale;
    else
        os << " \"float Lescale\" 0";
    os << "\n  MediumInterface " << HdPbrt_Quote(name) << " \"\"\n  Material \"interface\"\n"
       << box << "AttributeEnd\n";
}

PXR_NAMESPACE_CLOSE_SCOPE
