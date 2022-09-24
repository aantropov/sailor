#include "Core/LogMacros.h"
#include "Tasks/Scheduler.h"
#include "RenderDocApi.h"
#include <libloaderapi.h>

using namespace Sailor;

RenderDocApi::RenderDocApi()
{
	GetRenderDocAPI();
}

void RenderDocApi::SetActiveWindow(void* device, void* wndHandle)
{
#ifdef BUILD_WITH_RENDER_DOC
	if (g_pRenderDocAPI)
	{
		g_pRenderDocAPI->SetActiveWindow(device, wndHandle);
	}
#endif
}

void RenderDocApi::StartCapture()
{
#ifdef BUILD_WITH_RENDER_DOC
	if (g_pRenderDocAPI)
	{
		g_pRenderDocAPI->StartFrameCapture(NULL, NULL);
	}
#endif
}

void RenderDocApi::StopCapture()
{
#ifdef BUILD_WITH_RENDER_DOC
	if (g_pRenderDocAPI)
	{
		g_pRenderDocAPI->EndFrameCapture(NULL, NULL);
	}
#endif
}

void RenderDocApi::TriggerCapture()
{
#ifdef BUILD_WITH_RENDER_DOC
	if (g_pRenderDocAPI)
	{
		g_pRenderDocAPI->TriggerCapture();
		g_pRenderDocAPI->LaunchReplayUI(1, nullptr);
	}
#endif
}

RENDERDOC_API_1_4_2* RenderDocApi::GetRenderDocAPI()
{
#ifdef BUILD_WITH_RENDER_DOC
	SAILOR_LOG("Try to initialize RenderDocApi");

	if (!g_pRenderDocAPI)
	{
		const std::string renderDocDll = "C:/Program Files/RenderDoc/renderdoc.dll";

		LoadLibrary(renderDocDll.c_str());
		HMODULE mod = GetModuleHandleA(renderDocDll.c_str());
		if (!mod)
		{
			SAILOR_LOG("Cannot find renderdoc.dll");
			return nullptr;
		}

		pRENDERDOC_GetAPI RENDERDOC_GetAPI = (pRENDERDOC_GetAPI)GetProcAddress(mod, "RENDERDOC_GetAPI");
		const int ret = RENDERDOC_GetAPI(eRENDERDOC_API_Version_1_1_2, (void**)&g_pRenderDocAPI);
		if (ret != 1)
		{
			SAILOR_LOG("Cannot initialize RenderDoc");
			return nullptr;
		}

		g_pRenderDocAPI->MaskOverlayBits(RENDERDOC_OverlayBits::eRENDERDOC_Overlay_All, RENDERDOC_OverlayBits::eRENDERDOC_Overlay_All);
	}

	return g_pRenderDocAPI;
#else
	return nullptr
#endif
}
