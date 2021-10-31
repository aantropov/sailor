#pragma once
#include <functional>
#include <mutex>
#include <atomic>
#include <thread>
#include "Defines.h"
#include <glm/glm/glm.hpp>
#include "Core/RefPtr.hpp"
#include "GfxDevice.h"
#include "Core/Singleton.hpp"
#include "Jobsystem/JobSystem.h"
#include "Framework/Framework.h"
#include "GfxDevice/Vulkan/VulkanApi.h"
#include "RHI/Mesh.h"

namespace Sailor::RHI
{
	class Renderer : public TSingleton<Renderer>
	{
	public:
		static constexpr uint32_t MaxFramesInQueue = 2;

		static SAILOR_API void Initialize(class Window const* pViewport, RHI::EMsaaSamples msaaSamples, bool bIsDebug);
		SAILOR_API ~Renderer() override;

		void SAILOR_API FixLostDevice();
		bool SAILOR_API PushFrame(const FrameState& frame);

		uint32_t SAILOR_API GetNumFrames() const { return m_numFrames.load(); }
		uint32_t SAILOR_API GetSmoothFps() const { return m_pureFps.load(); }

		static SAILOR_API TUniquePtr<RHI::IGfxDevice>& GetDriver() { return m_pInstance->m_driverInstance; }

	protected:

		std::atomic<bool> m_bForceStop = false;
		std::atomic<uint32_t> m_pureFps = 0u;
		std::atomic<uint32_t> m_numFrames = 0u;

		class Window const* m_pViewport;

		TUniquePtr<RHI::IGfxDevice> m_driverInstance;
	};
};