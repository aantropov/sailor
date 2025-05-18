#pragma once
#include <functional>
#include <atomic>
#include <thread>

#include "Core/Defines.h"
#include "Math/Math.h"
#include "Engine/Types.h"
#include "RHI/Types.h"
#include "Containers/ConcurrentMap.h"
#include "Memory/RefPtr.hpp"
#include "Memory/SharedPtr.hpp"
#include "Memory/UniquePtr.hpp"
#include "Memory/ObjectPtr.hpp"
#include "Core/Submodule.h"
#include "Tasks/Scheduler.h"
#include "GraphicsDriver.h"
#include "SceneView.h"
#include "Platform/Window.h"

namespace Sailor
{
	class FrameState;
}

namespace Sailor::RHI
{
	class Renderer : public TSubmodule<Renderer>
	{
	public:

		// TODO: Move to RHI::Constants?
		static constexpr uint32_t GPUCullingGroupSize = 256;

		static constexpr uint32_t MaxFramesInQueue = 2;

               SAILOR_API Renderer(class Platform::Window* pViewport, RHI::EMsaaSamples msaaSamples, bool bIsDebug);
		SAILOR_API ~Renderer() override;

		SAILOR_API RHI::EMsaaSamples GetMsaaSamples() const { return m_msaaSamples; }
		SAILOR_API RHI::EFormat GetColorFormat() const;
		SAILOR_API RHI::EFormat GetDepthFormat() const;

		SAILOR_API void FixLostDevice();
		SAILOR_API bool PushFrame(const Sailor::FrameState& frame);

		SAILOR_API const Stats& GetStats() const { return m_stats; }

		SAILOR_API static TUniquePtr<IGraphicsDriver>& GetDriver();
		SAILOR_API static IGraphicsDriverCommands* GetDriverCommands();

		SAILOR_API RHISceneViewPtr GetOrAddSceneView(WorldPtr worldPtr);
		SAILOR_API void RemoveSceneView(WorldPtr worldPtr);

		SAILOR_API void BeginConditionalDestroy();
		SAILOR_API void RefreshFrameGraph() { m_bFrameGraphOutdated = true; }

		SAILOR_API FrameGraphPtr GetFrameGraph() { return m_frameGraph; }

		SAILOR_API static void MemoryStats();

	protected:

		RHI::EMsaaSamples m_msaaSamples;

		std::atomic<bool> m_bFrameGraphOutdated = false;
		std::atomic<bool> m_bForceStop = false;

		RHI::Stats m_stats{};

               class Platform::Window* m_pViewport;

		FrameGraphPtr m_frameGraph{};
		TConcurrentMap<WorldPtr, TList<TPair<RHISceneViewPtr,bool>>, 4, ERehashPolicy::Never> m_cachedSceneViews{};
		TUniquePtr<IGraphicsDriver> m_driverInstance{};
		Tasks::ITaskPtr m_previousRenderFrame{};
	};
};