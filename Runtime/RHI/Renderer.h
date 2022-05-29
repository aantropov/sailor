#pragma once
#include <functional>
#include <atomic>
#include <thread>
#include <glm/glm/glm.hpp>

#include "Core/Defines.h"
#include "Types.h"
#include "Memory/RefPtr.hpp"
#include "Memory/UniquePtr.hpp"
#include "Core/Submodule.h"
#include "Jobsystem/JobSystem.h"
#include "GraphicsDriver.h"

namespace Sailor
{
	class FrameState;
}

namespace Sailor::RHI
{
	typedef TRefPtr<class RHIBuffer> RHIBufferPtr;
	typedef TRefPtr<class RHICommandList> RHICommandListPtr;
	typedef TRefPtr<class RHIFence> RHIFencePtr;
	typedef TRefPtr<class RHIMesh> RHIMeshPtr;
	typedef TRefPtr<class RHITexture> RHITexturePtr;
	typedef TRefPtr<class RHIMaterial> RHIMaterialPtr;
	typedef TRefPtr<class RHIShader> RHIShaderPtr;
	typedef TRefPtr<class RHIShaderBinding> RHIShaderBindingPtr;
	typedef TRefPtr<class RHIShaderBindingSet> RHIShaderBindingSetPtr;
	typedef TRefPtr<class RHISemaphore> RHISemaphorePtr;
	typedef TRefPtr<class RHIVertexDescription> RHIVertexDescriptionPtr;

	class Renderer : public TSubmodule<Renderer>
	{
	public:
		static constexpr uint32_t MaxFramesInQueue = 2;

		SAILOR_API Renderer(class Win32::Window const* pViewport, RHI::EMsaaSamples msaaSamples, bool bIsDebug);
		SAILOR_API ~Renderer() override;

		SAILOR_API void FixLostDevice();
		SAILOR_API bool PushFrame(const Sailor::FrameState& frame);

		SAILOR_API uint32_t GetNumFrames() const { return m_numFrames.load(); }
		SAILOR_API uint32_t GetSmoothFps() const { return m_pureFps.load(); }

		SAILOR_API static TUniquePtr<IGraphicsDriver>& GetDriver();
		SAILOR_API static IGraphicsDriverCommands* GetDriverCommands();

	protected:

		RHI::RHICommandListPtr DrawTestScene(const Sailor::FrameState& frame);

		std::atomic<bool> m_bForceStop = false;
		std::atomic<uint32_t> m_pureFps = 0u;
		std::atomic<uint32_t> m_numFrames = 0u;

		class Win32::Window const* m_pViewport;

		TUniquePtr<IGraphicsDriver> m_driverInstance;
	};
};