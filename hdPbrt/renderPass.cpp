// SPDX-License-Identifier: Apache-2.0
#include "hdPbrt.h"
#include "renderBuffer.h"

#include "pxr/imaging/hd/renderPassState.h"
#include "pxr/imaging/hd/renderIndex.h"
#include "pxr/imaging/hd/tokens.h"

#include <chrono>

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

void HdPbrtRenderPass::_Execute(HdRenderPassStateSharedPtr const &state,
                                TfTokenVector const &renderTags)
{
    // Find the color AOV.
    HdPbrtRenderBuffer *color = nullptr;
    HdRenderPassAovBindingVector bindings = state->GetAovBindings();
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
        else if (!pb->IsConverged()) {
            // Unsupported AOV: fill it with its clear value once.
            if (b.clearValue.IsHolding<float>()) {
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

    const bool changed = sceneVersion != _lastSceneVersion ||
                         settingsVersion != _lastSettingsVersion || view != _view ||
                         proj != _proj || dw != _dataWindow || renderTags != _renderTags;
    if (changed) {
        _pending = true;
        _lastSceneVersion = sceneVersion;
        _lastSettingsVersion = settingsVersion;
        _view = view;
        _proj = proj;
        _dataWindow = dw;
        _renderTags = renderTags;
        _changeTime = _Now();
        color->SetConverged(false);
    }
    // Debounce: while the camera is moving every frame, don't restart pbrt
    // more often than every 100 ms.
    if (_pending && (!_started || _Now() - _lastStart > 0.1)) {
        _pending = false;
        _started = _delegate->StartRender(GetRenderIndex(), _view, _proj, w, h, _renderTags);
        _lastStart = _Now();
        _imageVersion = 0;
    }

    hdpbrt::PbrtSession &session = _delegate->GetSession();
    int iw = 0, ih = 0;
    if (session.Snapshot(&_image, &iw, &ih, &_imageVersion) && iw == w && ih == h)
        color->WriteRGB(_image.data(), iw, ih, dw.GetMinX(), dw.GetMinY());

    _converged = !_pending && (!_started || session.IsFinished());
    color->SetConverged(_converged);
}

PXR_NAMESPACE_CLOSE_SCOPE
