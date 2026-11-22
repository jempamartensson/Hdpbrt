// SPDX-License-Identifier: Apache-2.0
#include "hdPbrt.h"
#include "renderBuffer.h"

#include "pxr/imaging/hd/camera.h"
#include "pxr/imaging/hd/renderIndex.h"
#include "pxr/imaging/hd/resourceRegistry.h"
#include "pxr/imaging/hd/tokens.h"
#include "pxr/base/tf/getenv.h"
#include "pxr/base/tf/stringUtils.h"
#include "pxr/base/tf/pathUtils.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cerrno>
#include <csignal>
#include <dlfcn.h>
#include <fcntl.h>
#include <filesystem>
#include <fstream>
#include <spawn.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>

extern char **environ;

PXR_NAMESPACE_OPEN_SCOPE

TF_DEFINE_PUBLIC_TOKENS(HdPbrtSettingsTokens, HDPBRT_SETTINGS_TOKENS);

// --- formatting helpers ------------------------------------------------------

std::string HdPbrt_Quote(const std::string &s)
{
    std::string r = "\"";
    for (char c : s) {
        if (c == '"' || c == '\\')
            r += '\\';
        r += c;
    }
    return r + "\"";
}

std::string HdPbrt_Matrix(const GfMatrix4d &m)
{
    // pbrt's Transform takes the matrix in the same element order as
    // GfMatrix4d's row-vector storage (translation in elements 12..14).
    char buf[512];
    const double *d = m.GetArray();
    int n = snprintf(buf, sizeof(buf), "[");
    for (int i = 0; i < 16; ++i)
        n += snprintf(buf + n, sizeof(buf) - n, "%s%.9g", i ? " " : "", d[i]);
    snprintf(buf + n, sizeof(buf) - n, "]");
    return buf;
}

std::string HdPbrt_Rgb(const GfVec3f &c)
{
    char buf[128];
    snprintf(buf, sizeof(buf), "[%.7g %.7g %.7g]", c[0], c[1], c[2]);
    return buf;
}

// --- delegate ----------------------------------------------------------------

static const TfTokenVector _supportedRprims = {HdPrimTypeTokens->mesh};
static const TfTokenVector _supportedSprims = {
    HdPrimTypeTokens->camera,      HdPrimTypeTokens->material,
    HdPrimTypeTokens->distantLight, HdPrimTypeTokens->sphereLight,
    HdPrimTypeTokens->rectLight,   HdPrimTypeTokens->diskLight,
    HdPrimTypeTokens->cylinderLight, HdPrimTypeTokens->domeLight};
static const TfTokenVector _supportedBprims = {HdPrimTypeTokens->renderBuffer};

HdPbrtRenderDelegate::HdPbrtRenderDelegate() : HdRenderDelegate() { _Init(); }

HdPbrtRenderDelegate::HdPbrtRenderDelegate(HdRenderSettingsMap const &settingsMap)
    : HdRenderDelegate(settingsMap)
{
    _Init();
}

void HdPbrtRenderDelegate::_Init()
{
    _renderParam = std::make_unique<HdPbrtRenderParam>(this);

    auto add = [&](const char *name, TfToken const &key, VtValue def) {
        _settingDescriptors.push_back({name, key, def});
        if (_settingsMap.find(key) == _settingsMap.end())
            _settingsMap[key] = def;
    };
    add("Pixel Samples", HdPbrtSettingsTokens->pixelSamples, VtValue(256));
    add("Max Depth", HdPbrtSettingsTokens->maxDepth, VtValue(5));
    add("Integrator (volpath, path, bdpt, simplepath, ambientocclusion, ...)",
        HdPbrtSettingsTokens->integrator, VtValue(std::string("volpath")));
    add("Sampler (zsobol, sobol, halton, pmj02bn, independent, ...)",
        HdPbrtSettingsTokens->sampler, VtValue(std::string("zsobol")));
    add("Threads (0 = all but two)", HdPbrtSettingsTokens->threads, VtValue(0));
    add("Regularize BSDFs", HdPbrtSettingsTokens->regularize, VtValue(false));
    add("Depth of Field (camera fStop / focusDistance)", HdPbrtSettingsTokens->depthOfField,
        VtValue(true));
    add("Motion Blur (camera shutter)", HdPbrtSettingsTokens->motionBlur, VtValue(true));
    add("Transform Motion Samples", HdPbrtSettingsTokens->xformSamples, VtValue(2));
    add("Deformation Motion Samples", HdPbrtSettingsTokens->geoSamples, VtValue(2));
    add("Export .pbrt Scene To", HdPbrtSettingsTokens->exportPath, VtValue(std::string()));
    _PopulateDefaultSettings(_settingDescriptors);

    std::string base = TfGetenv("HDPBRT_TMPDIR");
    if (base.empty())
        base = std::filesystem::temp_directory_path().string();
    // Remove directories left behind by crashed processes (hdpbrt-<pid>-*).
    std::error_code ec;
    for (auto &e : std::filesystem::directory_iterator(base, ec)) {
        const std::string name = e.path().filename().string();
        if (!TfStringStartsWith(name, "hdpbrt-"))
            continue;
        pid_t pid = (pid_t)atoi(name.c_str() + 7);
        if (pid > 0 && pid != getpid() && kill(pid, 0) != 0 && errno == ESRCH)
            std::filesystem::remove_all(e.path(), ec);
    }
    std::string tmpl = base + "/hdpbrt-" + std::to_string(getpid()) + "-XXXXXX";
    std::vector<char> buf(tmpl.begin(), tmpl.end());
    buf.push_back(0);
    if (mkdtemp(buf.data()))
        _tempDir = buf.data();
    else
        TF_RUNTIME_ERROR("hdPbrt: cannot create a temp directory in %s", base.c_str());
}

HdPbrtRenderDelegate::~HdPbrtRenderDelegate()
{
    _session.Stop();
    if (!_tempDir.empty() && TfGetenv("HDPBRT_KEEP_TMP").empty()) {
        std::error_code ec;
        std::filesystem::remove_all(_tempDir, ec);
    }
}

const TfTokenVector &HdPbrtRenderDelegate::GetSupportedRprimTypes() const { return _supportedRprims; }
const TfTokenVector &HdPbrtRenderDelegate::GetSupportedSprimTypes() const { return _supportedSprims; }
const TfTokenVector &HdPbrtRenderDelegate::GetSupportedBprimTypes() const { return _supportedBprims; }

HdResourceRegistrySharedPtr HdPbrtRenderDelegate::GetResourceRegistry() const
{
    static HdResourceRegistrySharedPtr registry = std::make_shared<HdResourceRegistry>();
    return registry;
}

HdRenderSettingDescriptorList HdPbrtRenderDelegate::GetRenderSettingDescriptors() const
{
    return _settingDescriptors;
}

void HdPbrtRenderDelegate::SetRenderSetting(TfToken const &key, VtValue const &value)
{
    // Accept "pixelSamples" as well as "pbrt:pixelSamples" (Blender passes
    // whatever the add-on returns; USD render settings use the namespaced name).
    TfToken k = key;
    if (!TfStringStartsWith(key.GetString(), "pbrt:")) {
        TfToken ns("pbrt:" + key.GetString());
        for (auto &d : _settingDescriptors)
            if (d.key == ns)
                k = ns;
    }
    HdRenderDelegate::SetRenderSetting(k, value);
}

template <typename T>
T HdPbrtRenderDelegate::GetSetting(TfToken const &key, T const &def) const
{
    VtValue v = GetRenderSetting(key);
    if (v.IsHolding<T>())
        return v.UncheckedGet<T>();
    if constexpr (std::is_same_v<T, int>) {
        if (v.IsHolding<int64_t>()) return (int)v.UncheckedGet<int64_t>();
        if (v.IsHolding<float>()) return (int)v.UncheckedGet<float>();
        if (v.IsHolding<double>()) return (int)v.UncheckedGet<double>();
        if (v.IsHolding<bool>()) return (int)v.UncheckedGet<bool>();
    }
    if constexpr (std::is_same_v<T, bool>) {
        if (v.IsHolding<int>()) return v.UncheckedGet<int>() != 0;
        if (v.IsHolding<int64_t>()) return v.UncheckedGet<int64_t>() != 0;
    }
    if constexpr (std::is_same_v<T, std::string>) {
        if (v.IsHolding<TfToken>()) return v.UncheckedGet<TfToken>().GetString();
        if (v.IsHolding<SdfAssetPath>()) return v.UncheckedGet<SdfAssetPath>().GetAssetPath();
    }
    return def;
}
// Used from the render pass too.
template bool HdPbrtRenderDelegate::GetSetting<bool>(TfToken const &, bool const &) const;
template int HdPbrtRenderDelegate::GetSetting<int>(TfToken const &, int const &) const;
template std::string HdPbrtRenderDelegate::GetSetting<std::string>(TfToken const &,
                                                                   std::string const &) const;

int HdPbrtRenderDelegate::GetSpp() const
{
    return std::max(1, GetSetting<int>(HdPbrtSettingsTokens->pixelSamples, 256));
}

VtDictionary HdPbrtRenderDelegate::GetRenderStats() const
{
    VtDictionary d;
    d["rendererName"] = std::string("pbrt-v4");
    d["totalClockTime"] = _session.ElapsedSeconds();
    d["percentDone"] = _session.IsFinished() ? 100.0 : 0.0;
    return d;
}

HdAovDescriptor HdPbrtRenderDelegate::GetDefaultAovDescriptor(TfToken const &name) const
{
    if (name == HdAovTokens->color)
        return HdAovDescriptor(HdFormatFloat32Vec4, false, VtValue(GfVec4f(0, 0, 0, 1)));
    // Houdini's viewport composites the delegate image using its depth AOV and
    // draws nothing without one; selection uses the id AOVs. Depth is NDC z
    // mapped to [0, 1] (as hdEmbree and Storm), from pbrt's first-hit depth;
    // reversed in Houdini (see HdPbrtRenderPass::_ToNdcDepth).
    if (name == HdAovTokens->depth)
        return HdAovDescriptor(HdFormatFloat32, false, VtValue(HdPbrt_FarDepth()));
    if (name == HdAovTokens->primId || name == HdAovTokens->instanceId ||
        name == HdAovTokens->elementId)
        return HdAovDescriptor(HdFormatInt32, false, VtValue(-1));
    return HdAovDescriptor();
}

TfTokenVector HdPbrtRenderDelegate::GetMaterialRenderContexts() const
{
    return {TfToken("mtlx"), TfToken()};
}

HdRenderPassSharedPtr HdPbrtRenderDelegate::CreateRenderPass(HdRenderIndex *index,
                                                             HdRprimCollection const &collection)
{
    return std::make_shared<HdPbrtRenderPass>(index, collection, this);
}

HdInstancer *HdPbrtRenderDelegate::CreateInstancer(HdSceneDelegate *delegate, SdfPath const &id)
{
    return new HdPbrtInstancer(delegate, id);
}

void HdPbrtRenderDelegate::DestroyInstancer(HdInstancer *instancer) { delete instancer; }

HdRprim *HdPbrtRenderDelegate::CreateRprim(TfToken const &typeId, SdfPath const &rprimId)
{
    if (typeId == HdPrimTypeTokens->mesh)
        return new HdPbrtMesh(rprimId, this);
    return nullptr;
}

void HdPbrtRenderDelegate::DestroyRprim(HdRprim *rPrim)
{
    _renderParam->Edit();
    delete rPrim;
}

HdSprim *HdPbrtRenderDelegate::CreateSprim(TfToken const &typeId, SdfPath const &sprimId)
{
    if (typeId == HdPrimTypeTokens->camera)
        return new HdCamera(sprimId);
    if (typeId == HdPrimTypeTokens->material)
        return new HdPbrtMaterial(sprimId);
    if (std::find(_supportedSprims.begin(), _supportedSprims.end(), typeId) !=
        _supportedSprims.end())
        return new HdPbrtLight(sprimId, typeId, this);
    return nullptr;
}

HdSprim *HdPbrtRenderDelegate::CreateFallbackSprim(TfToken const &typeId)
{
    return CreateSprim(typeId, SdfPath::EmptyPath());
}

void HdPbrtRenderDelegate::DestroySprim(HdSprim *sPrim)
{
    _renderParam->Edit();
    delete sPrim;
}

HdBprim *HdPbrtRenderDelegate::CreateBprim(TfToken const &typeId, SdfPath const &bprimId)
{
    if (typeId == HdPrimTypeTokens->renderBuffer)
        return new HdPbrtRenderBuffer(bprimId);
    return nullptr;
}

HdBprim *HdPbrtRenderDelegate::CreateFallbackBprim(TfToken const &typeId)
{
    return CreateBprim(typeId, SdfPath::EmptyPath());
}

void HdPbrtRenderDelegate::DestroyBprim(HdBprim *bPrim) { delete bPrim; }

void HdPbrtRenderDelegate::Register(HdPbrtMesh *m)
{
    std::lock_guard<std::mutex> l(_registryMutex);
    _meshes.insert(m);
}
void HdPbrtRenderDelegate::Unregister(HdPbrtMesh *m)
{
    std::lock_guard<std::mutex> l(_registryMutex);
    _meshes.erase(m);
}
void HdPbrtRenderDelegate::Register(HdPbrtLight *l)
{
    if (l->GetId().IsEmpty())
        return;  // fallback prim
    std::lock_guard<std::mutex> g(_registryMutex);
    _lights.insert(l);
}
void HdPbrtRenderDelegate::Unregister(HdPbrtLight *l)
{
    std::lock_guard<std::mutex> g(_registryMutex);
    _lights.erase(l);
}

std::string HdPbrtRenderDelegate::NewTempPath(const std::string &stem, const std::string &ext)
{
    return _tempDir + "/" + stem + "_" + std::to_string(++_tempCounter) + ext;
}

std::string HdPbrtRenderDelegate::_PbrtExecutable() const
{
    std::string exe = GetSetting<std::string>(HdPbrtSettingsTokens->pbrtExecutable, "");
    if (!exe.empty())
        return exe;
    exe = TfGetenv("HDPBRT_PBRT_EXECUTABLE");
    if (!exe.empty())
        return exe;
    Dl_info info;
    if (dladdr((void *)&HdPbrt_Quote, &info) && info.dli_fname) {
        std::filesystem::path dir = std::filesystem::path(info.dli_fname).parent_path();
        for (auto cand : {dir / "pbrt", dir / ".." / "bin" / "pbrt"})
            if (access(cand.c_str(), X_OK) == 0)
                return cand.string();
    }
    return "/usr/local/bin/pbrt";
}

static bool _RunTool(const std::vector<std::string> &args, const std::string &logFile)
{
    std::vector<char *> argv;
    for (auto &a : args)
        argv.push_back(const_cast<char *>(a.c_str()));
    argv.push_back(nullptr);
    std::vector<std::string> envs;
    for (char **e = environ; e && *e; ++e)
        if (strncmp(*e, "LD_LIBRARY_PATH=", 16) && strncmp(*e, "LD_PRELOAD=", 11))
            envs.emplace_back(*e);
    std::vector<char *> envp;
    for (auto &e : envs)
        envp.push_back(const_cast<char *>(e.c_str()));
    envp.push_back(nullptr);
    posix_spawn_file_actions_t fa;
    posix_spawn_file_actions_init(&fa);
    posix_spawn_file_actions_addopen(&fa, 1, logFile.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
    posix_spawn_file_actions_adddup2(&fa, 1, 2);
    pid_t pid;
    int rc = posix_spawn(&pid, args[0].c_str(), &fa, nullptr, argv.data(), envp.data());
    posix_spawn_file_actions_destroy(&fa);
    if (rc != 0)
        return false;
    int status = 0;
    waitpid(pid, &status, 0);
    return WIFEXITED(status) && WEXITSTATUS(status) == 0;
}

std::string HdPbrtRenderDelegate::GetEquiAreaTexture(const std::string &latlong)
{
    struct stat st;
    if (stat(latlong.c_str(), &st) != 0)
        return {};
    std::string ext = TfStringToLower(TfGetExtension(latlong));
    if (ext != "exr" && ext != "hdr" && ext != "pfm" && ext != "png")
        return {};
    std::string key = latlong + "@" + std::to_string((long long)st.st_mtime);
    std::lock_guard<std::mutex> l(_texMutex);
    auto it = _equiAreaCache.find(key);
    if (it != _equiAreaCache.end())
        return it->second;
    std::string exe = _PbrtExecutable();
    std::string imgtool = std::filesystem::path(exe).parent_path() / "imgtool";
    std::string out = NewTempPath("env", ".exr");
    bool ok = _RunTool({imgtool, "makeequiarea", latlong, "--outfile", out},
                       NewTempPath("imgtool", ".log"));
    if (!ok) {
        TF_WARN("hdPbrt: imgtool makeequiarea failed for %s", latlong.c_str());
        out.clear();
    }
    _equiAreaCache[key] = out;
    return out;
}

bool HdPbrtRenderDelegate::StartRender(HdRenderIndex *index, const GfMatrix4d &view,
                                       const GfMatrix4d &proj, int width, int height,
                                       TfTokenVector const &renderTags,
                                       HdPbrtLens const &lens)
{
    if (width <= 0 || height <= 0 || _tempDir.empty())
        return false;
    _session.Stop();

    const int n = ++_renderCount;
    const std::string sceneFile = _tempDir + "/scene_" + std::to_string(n) + ".pbrt";
    const std::string pfm = _tempDir + "/render_" + std::to_string(n) + ".pfm";
    const std::string log = _tempDir + "/pbrt.log";
    // Previous render's files are no longer in use.
    if (n > 1) {
        unlink((_tempDir + "/scene_" + std::to_string(n - 1) + ".pbrt").c_str());
        unlink((_tempDir + "/render_" + std::to_string(n - 1) + ".pfm").c_str());
    }

    // Lights, written once here so the Film knows whether the background is
    // an environment (dome, or the fallback white one) or empty. With an
    // empty background pbrt sends coverage as alpha, so the host's viewport
    // background shows through.
    std::string lightText;
    bool hasEnvironment = false;
    {
        std::lock_guard<std::mutex> l(_registryMutex);
        std::ostringstream ls;
        ls.imbue(std::locale::classic());
        size_t numLights = 0;
        for (HdPbrtLight *light : _lights) {
            std::ostringstream one;
            one.imbue(std::locale::classic());
            light->Write(one);
            if (one.str().find("Light") != std::string::npos) {  // skip invisible lights
                ++numLights;
                if (light->IsDome())
                    hasEnvironment = true;
            }
            ls << one.str();
        }
        if (numLights == 0) {
            ls << "# no lights in the scene: add a uniform white environment\n"
                  "LightSource \"infinite\" \"rgb L\" [1 1 1]\n";
            hasEnvironment = true;
        }
        lightText = ls.str();
    }

    std::ostringstream os;
    os.imbue(std::locale::classic());
    os << "# pbrt-v4 scene written by hdPbrt\n";
    os << "Film \"rgb\" \"string filename\" " << HdPbrt_Quote(pfm)
       << " \"integer xresolution\" " << width << " \"integer yresolution\" " << height
       << " \"bool hdpbrtdepth\" true \"bool hdpbrtids\" true";
    if (!hasEnvironment)
        os << " \"bool hdpbrtalpha\" true";
    os << "\n";
    os << "Sampler " << HdPbrt_Quote(GetSetting<std::string>(HdPbrtSettingsTokens->sampler,
                                                             "zsobol"))
       << " \"integer pixelsamples\" " << GetSpp() << "\n";
    os << "Integrator "
       << HdPbrt_Quote(GetSetting<std::string>(HdPbrtSettingsTokens->integrator, "volpath"))
       << " \"integer maxdepth\" "
       << std::max(1, GetSetting<int>(HdPbrtSettingsTokens->maxDepth, 5));
    if (GetSetting<bool>(HdPbrtSettingsTokens->regularize, false))
        os << " \"bool regularize\" true";
    os << "\n";

    // Camera. USD camera space looks down -Z (right-handed); pbrt's looks
    // down +Z, so flip Z. The projection matrix (which already includes
    // the viewport's conform/window policy) gives the screen window.
    GfMatrix4d flip(1.0);
    flip.SetScale(GfVec3d(1, 1, -1));
    os << "Transform " << HdPbrt_Matrix(view * flip) << "\n";
    const bool ortho = std::fabs(proj[3][3] - 1.0) < 1e-6;
    double sx0, sx1, sy0, sy1;
    if (ortho) {
        sx0 = (-1 - proj[3][0]) / proj[0][0];
        sx1 = (1 - proj[3][0]) / proj[0][0];
        sy0 = (-1 - proj[3][1]) / proj[1][1];
        sy1 = (1 - proj[3][1]) / proj[1][1];
        os << "Camera \"orthographic\"";
    } else {
        sx0 = (-1 + proj[2][0]) / proj[0][0];
        sx1 = (1 + proj[2][0]) / proj[0][0];
        sy0 = (-1 + proj[2][1]) / proj[1][1];
        sy1 = (1 + proj[2][1]) / proj[1][1];
        os << "Camera \"perspective\" \"float fov\" 90";
        if (lens.lensRadius > 0.f && lens.focusDistance > 0.f) {
            char l[128];
            snprintf(l, sizeof(l), " \"float lensradius\" %.9g \"float focaldistance\" %.9g",
                     lens.lensRadius, lens.focusDistance);
            os << l;
        }
    }
    char sw[256];
    snprintf(sw, sizeof(sw), " \"float screenwindow\" [%.9g %.9g %.9g %.9g]", sx0, sx1, sy0,
             sy1);
    os << sw;
    // Motion blur: pbrt samples ray times over the camera shutter, and the
    // prims' animated transforms span the same interval (TransformTimes).
    const GfVec2f shutter = _shutter;
    if (shutter[0] < shutter[1]) {
        char sh[128];
        snprintf(sh, sizeof(sh), " \"float shutteropen\" %.9g \"float shutterclose\" %.9g",
                 shutter[0], shutter[1]);
        os << sh;
    }
    os << "\n";
    if (shutter[0] < shutter[1]) {
        char tt[96];
        snprintf(tt, sizeof(tt), "TransformTimes %.9g %.9g\n", shutter[0], shutter[1]);
        os << tt;
    }

    os << "WorldBegin\n";
    HdPbrtWriteContext ctx;
    {
        os << lightText;
        std::lock_guard<std::mutex> l(_registryMutex);
        for (HdPbrtMesh *mesh : _meshes)
            mesh->Write(os, *index, renderTags, ctx);
    }

    const std::string text = os.str();
    {
        std::ofstream f(sceneFile);
        f << text;
        if (!f) {
            TF_RUNTIME_ERROR("hdPbrt: cannot write %s", sceneFile.c_str());
            return false;
        }
    }
    std::string exportPath = GetSetting<std::string>(HdPbrtSettingsTokens->exportPath, "");
    if (!exportPath.empty()) {
        std::ofstream f(exportPath);
        f << text;
    }

    int threads = GetSetting<int>(HdPbrtSettingsTokens->threads, 0);
    if (threads <= 0)
        threads = std::max(1, (int)std::thread::hardware_concurrency() - 2);
    return _session.Start(_PbrtExecutable(), sceneFile, pfm, log, width, height, threads);
}

PXR_NAMESPACE_CLOSE_SCOPE
