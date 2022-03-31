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
	typedef TRefPtr<class Buffer> BufferPtr;
	typedef TRefPtr<class CommandList> CommandListPtr;
	typedef TRefPtr<class Fence> FencePtr;
	typedef TRefPtr<class Mesh> MeshPtr;
	typedef TRefPtr<class Texture> TexturePtr;
	typedef TRefPtr<class Material> MaterialPtr;
	typedef TRefPtr<class Shader> ShaderPtr;
	typedef TRefPtr<class ShaderBinding> ShaderBindingPtr;
	typedef TRefPtr<class ShaderBindingSet> ShaderBindingSetPtr;
	typedef TRefPtr<class Semaphore> SemaphorePtr;
	typedef TRefPtr<class VertexDescription> VertexDescriptionPtr;

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

		std::atomic<bool> m_bForceStop = false;
		std::atomic<uint32_t> m_pureFps = 0u;
		std::atomic<uint32_t> m_numFrames = 0u;

		class Win32::Window const* m_pViewport;

		TUniquePtr<IGraphicsDriver> m_driverInstance;
	};
};