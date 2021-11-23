#include <mutex>
#include "Types.h"
#include "Renderer.h"
#include "Mesh.h"
#include "GfxDevice.h"
#include "Framework/Framework.h"
#include "GfxDevice/Vulkan/VulkanApi.h"
#include "GfxDevice/Vulkan/VulkanDevice.h"
#include "JobSystem/JobSystem.h"
#include "Memory/MemoryBlockAllocator.hpp"
#include "GfxDevice/Vulkan/GfxDeviceVulkan.h"

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
				m_dependencies.pop_back();
				bShouldRemoveFromList = true;
			}
		}
	}
}

bool IDelayedInitialization::IsReady() const
{
	return m_dependencies.size() == 0;
}

void Renderer::Initialize(Win32::Window const* pViewport, RHI::EMsaaSamples msaaSamples, bool bIsDebug)
{
	if (m_pInstance != nullptr)
	{
		SAILOR_LOG("Renderer already initialized!");
		return;
	}

	m_pInstance = new Renderer();
	m_pInstance->m_pViewport = pViewport;

#if defined(VULKAN)
	m_pInstance->m_driverInstance = TUniquePtr<Sailor::GfxDevice::Vulkan::GfxDeviceVulkan>::Make();
	m_pInstance->m_driverInstance->Initialize(pViewport, msaaSamples, bIsDebug);
#endif
}

Renderer::~Renderer()
{
	Renderer::GetDriver()->WaitIdle();
	m_driverInstance.Clear();
}

TUniquePtr<IGfxDevice>& Renderer::GetDriver()
{
	return m_pInstance->m_driverInstance;
}

IGfxDeviceCommands* Renderer::GetDriverCommands()
{

#if defined(VULKAN)
	return dynamic_cast<IGfxDeviceCommands*>(m_pInstance->m_driverInstance.GetRawPtr());
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

	if (m_bForceStop || JobSystem::Scheduler::GetInstance()->GetNumRenderingJobs() > MaxFramesInQueue)
	{
		return false;
	}

	SAILOR_PROFILE_END_BLOCK();

	SAILOR_PROFILE_BLOCK("Push frame");

	TSharedPtr<class JobSystem::Job> preRenderingJob = JobSystem::Scheduler::CreateJob("Trace command lists & Track RHI resources",
		[this]()
		{
			this->GetDriver()->TrackResources_ThreadSafe();
		}, Sailor::JobSystem::EThreadType::Rendering);

	TSharedPtr<class JobSystem::Job> renderingJob = JobSystem::Scheduler::CreateJob("Render Frame",
		[this, frame]() 
		{

			auto frameInstance = frame;
			static Utils::Timer timer;
			timer.Start();

			SAILOR_PROFILE_BLOCK("Submit & Wait frame command list");
			std::vector<SemaphorePtr> waitFrameUpdate;
			for (uint32_t i = 0; i < frameInstance.NumCommandLists; i++)
			{
				if (auto pCommandList = frameInstance.GetCommandBuffer(i))
				{
					waitFrameUpdate.push_back(GetDriver()->CreateWaitSemaphore());
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

	JobSystem::Scheduler::GetInstance()->Run(preRenderingJob);
	JobSystem::Scheduler::GetInstance()->Run(renderingJob);

	SAILOR_PROFILE_END_BLOCK();

	return true;
}
