// SPDX-License-Identifier: Apache-2.0
#include "hdPbrt.h"
#include "renderBuffer.h"

#include "pxr/imaging/hd/camera.h"
#include "pxr/imaging/hd/renderPassState.h"
#include "pxr/imaging/hd/renderIndex.h"
#include "pxr/imaging/hd/tokens.h"

#include "pxr/base/tf/stringUtils.h"
#include <chrono>
#include <cmath>
#include <algorithm>
#include <cstdio>
#include <cstdlib>

PXR_NAMESPACE_OPEN_SCOPE

static double _Now()
{
    using namespace std::chrono;
    return duration<double>(steady_clock::now().time_since_epoch()).count();
}

HdPbrtRenderPass::HdPbrtRenderPass(HdRenderIndex *index, HdRprimCollection const &collection,
                                   HdPbrtRenderDelegate *delegate)
    : HdRenderPass(index, collection), _delegate(delegate) {}

HdPbrtRenderPass::~HdPbrtRenderPass() { _delegate->StopRender(); }

bool HdPbrtRenderPass::IsConverged() const { return _converged; }

// Houdini's Vulkan viewport composites with reversed depth (1 = near, 0 = far;
// with standard depth its grid and handles vanish entirely).
// HDPBRT_REVERSED_DEPTH=0/1 overrides the build default.
bool HdPbrt_ReversedDepth()
{
#ifdef HDPBRT_HOUDINI
    const bool def = true;
#else
    const bool def = false;
#endif
    static const bool reversed = getenv("HDPBRT_REVERSED_DEPTH")
                                     ? atoi(getenv("HDPBRT_REVERSED_DEPTH")) != 0
                                     : def;
    return reversed;
}

float HdPbrt_FarDepth() { return HdPbrt_ReversedDepth() ? 0.f : 1.f; }

// pbrt camera-space z (looking down +Z; +inf = no hit) -> NDC depth in [0, 1].
// USD view space looks down -Z, and Gf matrices multiply row vectors.
void HdPbrtRenderPass::_ToNdcDepth(std::vector<float> *zs, GfMatrix4d const &proj)
{
    const double a = proj[2][2], b = proj[3][2], c = proj[2][3], d = proj[3][3];
    for (float &z : *zs) {
        if (!std::isfinite(z)) {
            z = 1.f;
            continue;
        }
        const double zv = -double(z);
        const double w = zv * c + d;
        const double ndc = w != 0 ? (zv * a + b) / w : 1.0;
        z = float(std::min(1.0, std::max(0.0, 0.5 * ndc + 0.5)));
    }
    if (HdPbrt_ReversedDepth())
        for (float &z : *zs)
            z = 1.f - z;
}

void HdPbrtRenderPass::_Execute(HdRenderPassStateSharedPtr const &state,
                                TfTokenVector const &renderTags)
{
    // Find the color AOV.
    HdPbrtRenderBuffer *color = nullptr, *depth = nullptr;
    HdPbrtRenderBuffer *primId = nullptr, *instanceId = nullptr;
    HdRenderPassAovBindingVector bindings = state->GetAovBindings();
    static const bool debug = getenv("HDPBRT_DEBUG") != nullptr;
    if (debug) {
        const CameraUtilFraming &fr = state->GetFraming();
        fprintf(stderr, "hdPbrt execute: %zu bindings, framing valid %d dw %d,%d %dx%d\n",
                bindings.size(), int(fr.IsValid()), fr.dataWindow.GetMinX(),
                fr.dataWindow.GetMinY(), fr.dataWindow.GetWidth(), fr.dataWindow.GetHeight());
        for (auto &b : bindings) {
            HdRenderBuffer *rb = b.renderBuffer;
            if (!rb && !b.renderBufferId.IsEmpty())
                rb = static_cast<HdRenderBuffer *>(GetRenderIndex()->GetBprim(
                    HdPrimTypeTokens->renderBuffer, b.renderBufferId));
            fprintf(stderr, "  aov %s id %s rb %p ours %d fmt %d %ux%u clear %s\n",
                    b.aovName.GetText(), b.renderBufferId.GetText(), (void *)rb,
                    int(dynamic_cast<HdPbrtRenderBuffer *>(rb) != nullptr),
                    rb ? int(rb->GetFormat()) : -1, rb ? rb->GetWidth() : 0,
                    rb ? rb->GetHeight() : 0, TfStringify(b.clearValue).c_str());
        }
    }
    for (auto &b : bindings) {
        HdRenderBuffer *rb = b.renderBuffer;
        if (!rb && !b.renderBufferId.IsEmpty())
            rb = static_cast<HdRenderBuffer *>(GetRenderIndex()->GetBprim(
                HdPrimTypeTokens->renderBuffer, b.renderBufferId));
        auto *pb = dynamic_cast<HdPbrtRenderBuffer *>(rb);
        if (!pb)
            continue;
        if (b.aovName == HdAovTokens->color && !color)
            color = pb;
        else if (b.aovName == HdAovTokens->depth && !depth &&
                 HdGetComponentCount(pb->GetFormat()) == 1 &&
                 HdGetComponentFormat(pb->GetFormat()) == HdFormatFloat32) {
            depth = pb;
            if (!pb->IsConverged()) {
                float far = HdPbrt_FarDepth();
                pb->Clear(&far, 1);
                pb->SetConverged(true);
            }
        } else if ((b.aovName == HdAovTokens->primId ||
                    b.aovName == HdAovTokens->instanceId) &&
                   HdGetComponentCount(pb->GetFormat()) == 1 &&
                   HdGetComponentFormat(pb->GetFormat()) == HdFormatInt32) {
            // Selection: the ids of the nearest hit, streamed back by pbrt.
            (b.aovName == HdAovTokens->primId ? primId : instanceId) = pb;
            if (!pb->IsConverged()) {
                float none = -1.f;
                pb->Clear(&none, 1);
                pb->SetConverged(true);
            }
        } else if (!pb->IsConverged()) {
            // Unsupported AOV: fill it with its clear value once.
            if (b.clearValue.IsHolding<int>()) {
                float v = float(b.clearValue.UncheckedGet<int>());
                pb->Clear(&v, 1);
            } else if (b.clearValue.IsHolding<float>()) {
                float v = b.clearValue.UncheckedGet<float>();
                pb->Clear(&v, 1);
            } else if (b.clearValue.IsHolding<GfVec4f>()) {
                GfVec4f v = b.clearValue.UncheckedGet<GfVec4f>();
                pb->Clear(v.data(), 4);
            } else {
                float z[4] = {0, 0, 0, 0};
                pb->Clear(z, 4);
            }
            pb->SetConverged(true);
        }
    }
    if (!color) {
        _converged = true;
        return;
    }

    GfRect2i dw;
    const CameraUtilFraming &framing = state->GetFraming();
    if (framing.IsValid())
        dw = framing.dataWindow;
    else
        dw = GfRect2i(GfVec2i(0), int(color->GetWidth()), int(color->GetHeight()));
    const int w = dw.GetWidth(), h = dw.GetHeight();

    const GfMatrix4d view = state->GetWorldToViewMatrix();
    const GfMatrix4d proj = state->GetProjectionMatrix();
    const int sceneVersion = _delegate->GetRenderParam()
                                 ? static_cast<HdPbrtRenderParam *>(_delegate->GetRenderParam())
                                       ->sceneVersion.load()
                                 : 0;
    const int settingsVersion = _delegate->GetRenderSettingsVersion();

    // Depth of field from the active camera (USD: fStop 0 = off). HdCamera
    // already converts focal length to world units; pbrt's thin lens wants
    // the aperture radius, focalLength / (2 fStop).
    HdPbrtLens lens;
    GfVec2f shutter(0.f, 0.f);
    if (HdCamera const *cam = state->GetCamera()) {
        // Motion blur over the camera shutter (frames relative to now).
        const bool blur =
            _delegate->GetSetting<bool>(HdPbrtSettingsTokens->motionBlur, true) &&
            !_delegate->GetSetting<bool>(HdRenderSettingsPrimTokens->disableMotionBlur, false);
        if (blur && cam->GetShutterOpen() < cam->GetShutterClose())
            shutter = GfVec2f(float(cam->GetShutterOpen()), float(cam->GetShutterClose()));
        const bool ortho = std::fabs(proj[3][3] - 1.0) < 1e-6;
        const bool enabled =
            _delegate->GetSetting<bool>(HdPbrtSettingsTokens->depthOfField, true) &&
            !_delegate->GetSetting<bool>(HdRenderSettingsPrimTokens->disableDepthOfField, false);
        if (enabled && !ortho && cam->GetFStop() > 0.f && cam->GetFocusDistance() > 0.f &&
            cam->GetFocalLength() > 0.f) {
            lens.lensRadius = cam->GetFocalLength() / (2.f * cam->GetFStop());
            lens.focusDistance = cam->GetFocusDistance();
        }
        if (debug)
            fprintf(stderr, "hdPbrt camera %s: fStop %g focusDistance %g focalLength %g -> "
                            "lensradius %g\n",
                    cam->GetId().GetText(), cam->GetFStop(), cam->GetFocusDistance(),
                    cam->GetFocalLength(), lens.lensRadius);
        if (debug)
            fprintf(stderr, "hdPbrt camera shutter %g..%g -> %g..%g\n", cam->GetShutterOpen(),
                    cam->GetShutterClose(), shutter[0], shutter[1]);
    }
    // The prims keep time samples around the current frame; a new shutter
    // only means writing the scene again.
    const bool shutterChanged = shutter != _delegate->GetShutter();
    _delegate->SetShutter(shutter);

    const bool changed = sceneVersion != _lastSceneVersion ||
                         settingsVersion != _lastSettingsVersion || view != _view ||
                         proj != _proj || lens != _lens || shutterChanged ||
                         dw != _dataWindow ||
                         renderTags != _renderTags;
    if (changed) {
        _pending = true;
        _lastSceneVersion = sceneVersion;
        _lastSettingsVersion = settingsVersion;
        _view = view;
        _proj = proj;
        _lens = lens;
        _dataWindow = dw;
        _renderTags = renderTags;
        _changeTime = _Now();
        color->SetConverged(false);
    }
    // Debounce: while the camera is moving every frame, don't restart pbrt
    // more often than every 100 ms.
    if (_pending && (!_started || _Now() - _lastStart > 0.1)) {
        _pending = false;
        _started = _delegate->StartRender(GetRenderIndex(), _view, _proj, w, h, _renderTags,
                                          _lens);
        _lastStart = _Now();
        _imageVersion = 0;
    }

    hdpbrt::PbrtSession &session = _delegate->GetSession();
    int iw = 0, ih = 0;
    const bool wantIds = primId || instanceId;
    if (session.Snapshot(&_image, &iw, &ih, &_imageVersion, depth ? &_depth : nullptr,
                         wantIds ? &_primIds : nullptr, wantIds ? &_instanceIds : nullptr,
                         &_alpha)) {
        if (debug)
            fprintf(stderr, "hdPbrt snapshot %dx%d (want %dx%d) v%llu\n", iw, ih, w, h,
                    (unsigned long long)_imageVersion);
        if (iw == w && ih == h) {
            // Coverage is only sent when the scene has no visible environment;
            // pbrt's color is then already premultiplied (escaped rays are black).
            const bool hasAlpha = _alpha.size() == size_t(iw) * ih;
            color->WriteRGB(_image.data(), iw, ih, dw.GetMinX(), dw.GetMinY(),
                            hasAlpha ? _alpha.data() : nullptr);
            if (depth && _depth.size() == size_t(iw) * ih) {
                if (debug) {
                    float lo = 1e30f, hi = -1e30f; size_t hits = 0;
                    for (float z : _depth) if (std::isfinite(z)) { lo = std::min(lo, z); hi = std::max(hi, z); ++hits; }
                    fprintf(stderr, "hdPbrt depth: %zu hits, cam z %g..%g\n", hits, lo, hi);
                }
                _ToNdcDepth(&_depth, _proj);
                if (debug) {
                    float lo = 2, hi = -1;
                    for (float z : _depth) { lo = std::min(lo, z); hi = std::max(hi, z); }
                    fprintf(stderr, "hdPbrt depth: ndc01 %g..%g\n", lo, hi);
                }
                depth->WriteFloat(_depth.data(), iw, ih, dw.GetMinX(), dw.GetMinY());
            }
            // Ids are exact integers in float (pbrt sends them that way).
            if (primId && _primIds.size() == size_t(iw) * ih)
                primId->WriteFloat(_primIds.data(), iw, ih, dw.GetMinX(), dw.GetMinY());
            if (instanceId && _instanceIds.size() == size_t(iw) * ih)
                instanceId->WriteFloat(_instanceIds.data(), iw, ih, dw.GetMinX(),
                                       dw.GetMinY());
            if (debug && wantIds) {
                std::set<int> ids;
                for (float f : _primIds) ids.insert(int(f));
                fprintf(stderr, "hdPbrt ids: %zu primIds:", _primIds.size());
                for (int id : ids) fprintf(stderr, " %d", id);
                fprintf(stderr, "\n");
            }
        }
    }

    _converged = !_pending && (!_started || session.IsFinished());
    color->SetConverged(_converged);
}

PXR_NAMESPACE_CLOSE_SCOPE
