#pragma once
#include <functional>
#include <mutex>
#include <atomic>
#include <thread>
#include "Defines.h"
#include <glm/glm/glm.hpp>
#include "Core/RefPtr.hpp"
#include "Core/Singleton.hpp"
#include "Jobsystem/JobSystem.h"
#include "Framework/Framework.h"
#include "GfxDevice/Vulkan/VulkanApi.h"

namespace Sailor
{
	class Renderer : public TSingleton<Renderer>
	{
	public:
		static constexpr uint8_t MaxFramesInQueue = 2;

		static SAILOR_API void Initialize(class Window const* pViewport, RHI::EMsaaSamples msaaSamples, bool bIsDebug);
		SAILOR_API ~Renderer() override;

		bool SAILOR_API IsRunning() const;
		void SAILOR_API RunRenderLoop();
		void SAILOR_API StopRenderLoop();

		void SAILOR_API FixLostDevice();

		void SAILOR_API PushFrame(const FrameState& frame);
		uint32_t SAILOR_API GetNumFrames() const { return m_numFrames.load(); }
		uint32_t SAILOR_API GetSmoothFps() const { return m_smoothFps.load(); }

	protected:

		void SAILOR_API RenderLoop_RenderThread();
		
		bool SAILOR_API TryPopFrame(FrameState& frame);

		std::mutex m_queueMutex;
		std::mutex m_waitFinishRendering;
		std::condition_variable m_onPushFrame;
		std::condition_variable m_onPopFrame;

		std::array<FrameState, MaxFramesInQueue> m_frames;

		TSharedPtr<class JobSystem::Job> m_renderingJob;
		std::atomic<bool> m_bForceStop = false;
		std::atomic<uint32_t> m_smoothFps = 0u;
		std::atomic<uint32_t> m_numFrames = 0u;

		class Window const* m_pViewport;
	};
};