#pragma once
#include "Defines.h"
#include <glm/glm/glm.hpp>
#include "Core/RefPtr.hpp"
#include "Core/Singleton.hpp"
#include "GfxDevice/Vulkan/VulkanApi.h"

namespace Sailor
{
	class Renderer : public TSingleton<Renderer>
	{
	public:

		static SAILOR_API void Initialize(class Window const* pViewport, bool bIsDebug);
		SAILOR_API ~Renderer() override;

		bool SAILOR_API IsRunning() const { return m_bIsRunning; }
		void SAILOR_API RunRenderLoop();
		void SAILOR_API StopRenderLoop();

		uint32_t SAILOR_API GetSmoothFps() const { return m_smoothFps.load(); }

	protected:

		void SAILOR_API RenderLoop_RenderThread();
		
		std::condition_variable m_renderLoopStopped;
		std::mutex m_renderLoopActive;
		
		std::atomic<bool> m_bIsRunning = false;
		std::atomic<bool> m_bForceStop = false;
		std::atomic<uint32_t> m_smoothFps = 0u;

		class Window const* m_pViewport;
	};
};