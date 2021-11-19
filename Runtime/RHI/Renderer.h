#pragma once
#include <functional>
#include <mutex>
#include <atomic>
#include <thread>
#include <glm/glm/glm.hpp>

#include "Defines.h"
#include "Types.h"
#include "Core/RefPtr.hpp"
#include "Core/UniquePtr.hpp"
#include "Core/Singleton.hpp"
#include "Jobsystem/JobSystem.h"
#include "GfxDevice.h"

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

	class Renderer : public TSingleton<Renderer>
	{
	public:
		static constexpr uint32_t MaxFramesInQueue = 2;

		static SAILOR_API void Initialize(class Win32::Window const* pViewport, RHI::EMsaaSamples msaaSamples, bool bIsDebug);
		SAILOR_API ~Renderer() override;

		void SAILOR_API FixLostDevice();
		bool SAILOR_API PushFrame(const Sailor::FrameState& frame);

		uint32_t SAILOR_API GetNumFrames() const { return m_numFrames.load(); }
		uint32_t SAILOR_API GetSmoothFps() const { return m_pureFps.load(); }

		static SAILOR_API TUniquePtr<IGfxDevice>& GetDriver();
		static SAILOR_API IGfxDeviceCommands* GetDriverCommands();

	protected:

		std::atomic<bool> m_bForceStop = false;
		std::atomic<uint32_t> m_pureFps = 0u;
		std::atomic<uint32_t> m_numFrames = 0u;

		class Win32::Window const* m_pViewport;

		TUniquePtr<IGfxDevice> m_driverInstance;
	};
};