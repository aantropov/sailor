#include "Types.h"
#include "Renderer.h"
#include "Mesh.h"
#include "GraphicsDriver.h"
#include "Engine/EngineLoop.h"
#include "GraphicsDriver/Vulkan/VulkanApi.h"
#include "GraphicsDriver/Vulkan/VulkanDevice.h"
#include "JobSystem/JobSystem.h"
#include "Memory/MemoryBlockAllocator.hpp"
#include "GraphicsDriver/Vulkan/VulkanGraphicsDriver.h"

using namespace Sailor;
using namespace Sailor::RHI;

void IDelayedInitialization::TraceVisit(class TRefPtr<Resource> visitor, bool& bShouldRemoveFromList)
{
	bShouldRemoveFromList = false;

	if (auto fence = TRefPtr<RHI::Fence>(visitor.GetRawPtr()))
	{
		if (fence->IsFinished())
		{
			auto it = std::find_if(m_dependencies.begin(), m_dependencies.end(),
				[&fence](const auto& lhs)
				{
					return fence.GetRawPtr() == lhs.GetRawPtr();
				});

			if (it != std::end(m_dependencies))
			{
				std::iter_swap(it, m_dependencies.end() - 1);
				m_dependencies.RemoveLast();
				bShouldRemoveFromList = true;
			}
		}
	}
}

bool IDelayedInitialization::IsReady() const
{
	return m_dependencies.Num() == 0;
}

Renderer::Renderer(Win32::Window const* pViewport, RHI::EMsaaSamples msaaSamples, bool bIsDebug)
{
	m_pViewport = pViewport;

#if defined(VULKAN)
	m_driverInstance = TUniquePtr<Sailor::GraphicsDriver::Vulkan::VulkanGraphicsDriver>::Make();
	m_driverInstance->Initialize(pViewport, msaaSamples, bIsDebug);
#endif
}

Renderer::~Renderer()
{
	Renderer::GetDriver()->WaitIdle();
	m_driverInstance.Clear();
}

TUniquePtr<IGraphicsDriver>& Renderer::GetDriver()
{
	return App::GetSubmodule<Renderer>()->m_driverInstance;
}

IGraphicsDriverCommands* Renderer::GetDriverCommands()
{

#if defined(VULKAN)
	return dynamic_cast<IGraphicsDriverCommands*>(App::GetSubmodule<Renderer>()->m_driverInstance.GetRawPtr());
#endif

	return nullptr;
}

void Renderer::FixLostDevice()
{
	m_driverInstance->FixLostDevice(m_pViewport);
}

bool Renderer::PushFrame(const Sailor::FrameState& frame)
{
	SAILOR_PROFILE_BLOCK("Wait for render thread");

	if (m_bForceStop || App::GetSubmodule<JobSystem::Scheduler>()->GetNumRenderingJobs() > MaxFramesInQueue)
	{
		return false;
	}

	SAILOR_PROFILE_END_BLOCK();

	SAILOR_PROFILE_BLOCK("Push frame");

	TSharedPtr<class JobSystem::ITask> preRenderingJob = JobSystem::Scheduler::CreateTask("Trace command lists & Track RHI resources",
		[this]()
		{
			this->GetDriver()->TrackResources_ThreadSafe();
		}, Sailor::JobSystem::EThreadType::Rendering);

	TSharedPtr<class JobSystem::ITask> renderingJob = JobSystem::Scheduler::CreateTask("Render Frame",
		[this, frame]()
		{

			auto frameInstance = frame;
			static Utils::Timer timer;
			timer.Start();

			SAILOR_PROFILE_BLOCK("Submit & Wait frame command list");
			TVector<SemaphorePtr> waitFrameUpdate;
			for (uint32_t i = 0; i < frameInstance.NumCommandLists; i++)
			{
				if (auto pCommandList = frameInstance.GetCommandBuffer(i))
				{
					waitFrameUpdate.Add(GetDriver()->CreateWaitSemaphore());
					GetDriver()->SubmitCommandList(pCommandList, FencePtr::Make(), waitFrameUpdate[i]);
				}
			}
			SAILOR_PROFILE_END_BLOCK();

			do
			{
				static uint32_t totalFramesCount = 0U;

				SAILOR_PROFILE_BLOCK("Present Frame");

				if (m_driverInstance->PresentFrame(frame, nullptr, nullptr, waitFrameUpdate))
				{
					totalFramesCount++;
					timer.Stop();

					if (timer.ResultAccumulatedMs() > 1000)
					{
						m_pureFps = totalFramesCount;
						totalFramesCount = 0;
						timer.Clear();
					}
				}
				else
				{
					m_pureFps = 0;
				}

				SAILOR_PROFILE_END_BLOCK();

			} while (m_pViewport->IsIconic());

		}, Sailor::JobSystem::EThreadType::Rendering);

	renderingJob->Join(preRenderingJob);

	App::GetSubmodule<JobSystem::Scheduler>()->Run(preRenderingJob);
	App::GetSubmodule<JobSystem::Scheduler>()->Run(renderingJob);

	SAILOR_PROFILE_END_BLOCK();

	return true;
}
