// SPDX-License-Identifier: Apache-2.0
#include "hdPbrt.h"
#include "pxr/imaging/hd/rendererPluginRegistry.h"

PXR_NAMESPACE_OPEN_SCOPE

TF_REGISTRY_FUNCTION(TfType)
{
    HdRendererPluginRegistry::Define<HdPbrtRendererPlugin>();
}

HdRenderDelegate *HdPbrtRendererPlugin::CreateRenderDelegate()
{
    return new HdPbrtRenderDelegate();
}

HdRenderDelegate *HdPbrtRendererPlugin::CreateRenderDelegate(HdRenderSettingsMap const &settingsMap)
{
    return new HdPbrtRenderDelegate(settingsMap);
}

void HdPbrtRendererPlugin::DeleteRenderDelegate(HdRenderDelegate *renderDelegate)
{
    delete renderDelegate;
}

#if PXR_VERSION >= 2608
bool HdPbrtRendererPlugin::IsSupported(const HdRendererCreateArgsSchema &, std::string *) const
{
    return true;
}
#elif PXR_VERSION >= 2505
bool HdPbrtRendererPlugin::IsSupported(HdRendererCreateArgs const &, std::string *) const
{
    return true;
}
#else
bool HdPbrtRendererPlugin::IsSupported(bool) const { return true; }
#endif

PXR_NAMESPACE_CLOSE_SCOPE
