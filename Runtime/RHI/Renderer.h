#pragma once
#include <functional>
#include <atomic>
#include <thread>
#include <glm/glm/glm.hpp>

#include "Core/Defines.h"
#include "Engine/Types.h"
#include "RHI/Types.h"
#include "Containers/ConcurrentMap.h"
#include "Memory/RefPtr.hpp"
#include "Memory/SharedPtr.hpp"
#include "Memory/UniquePtr.hpp"
#include "Memory/ObjectPtr.hpp"
#include "Core/Submodule.h"
#include "Jobsystem/JobSystem.h"
#include "GraphicsDriver.h"
#include "SceneView.h"

namespace Sailor
{
	class FrameState;
}

namespace Sailor::RHI
{
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

		SAILOR_API RHISceneViewPtr& GetOrAddSceneView(WorldPtr worldPtr);
		SAILOR_API void RemoveSceneView(WorldPtr worldPtr);

	protected:

		RHI::RHICommandListPtr DrawTestScene(const Sailor::FrameState& frame);

		std::atomic<bool> m_bForceStop = false;
		std::atomic<uint32_t> m_pureFps = 0u;
		std::atomic<uint32_t> m_numFrames = 0u;

		class Win32::Window const* m_pViewport;

		FrameGraphPtr m_frameGraph;
		TConcurrentMap<WorldPtr, RHISceneViewPtr> m_cachedSceneViews;
		TUniquePtr<IGraphicsDriver> m_driverInstance;
	};
};