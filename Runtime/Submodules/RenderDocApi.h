#pragma once
#include "Core/Submodule.h"

#ifdef SAILOR_BUILD_WITH_RENDER_DOC
#include "renderdoc/renderdoc/api/app/renderdoc_app.h"
#endif

namespace Sailor
{
	class RenderDocApi : public TSubmodule<RenderDocApi>
	{
	public:

		RenderDocApi();

		SAILOR_API bool IsCapturing() const;
		SAILOR_API bool IsConnected() const;

		SAILOR_API void LaunchRenderDocApp();

		SAILOR_API void SetActiveWindow(void* device, void* wndHandle);

		SAILOR_API void TriggerCapture();

		SAILOR_API void StartCapture();
		SAILOR_API void StopCapture();

		SAILOR_API uint32_t GetNumCaptures() const;

	protected:

#ifdef SAILOR_BUILD_WITH_RENDER_DOC
		RENDERDOC_API_1_4_2* GetRenderDocAPI();
		RENDERDOC_API_1_4_2* g_pRenderDocAPI = nullptr;
#endif // SAILOR_BUILD_WITH_RENDER_DOC

	};
}