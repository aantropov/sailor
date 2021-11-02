#include <mutex>
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
	m_driverInstance.Clear();
}

TUniquePtr<IGfxDevice>& Renderer::GetDriver() 
{
	return m_pInstance->m_driverInstance; 
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

	TSharedPtr<class JobSystem::Job> preRenderingJob = JobSystem::Scheduler::CreateJob("Trace Command Lists",
		[this]() {

		SAILOR_PROFILE_BLOCK("Pre frame rendering jobs");
		this->GetDriver()->TrackResources();
		SAILOR_PROFILE_END_BLOCK();
	});

	TSharedPtr<class JobSystem::Job> renderingJob = JobSystem::Scheduler::CreateJob("Render Frame",
		[this, frame]() {

		static Utils::Timer timer;
		timer.Start();

		do
		{
			static uint32_t totalFramesCount = 0U;

			SAILOR_PROFILE_BLOCK("Present Frame");

			if (m_driverInstance->PresentFrame(frame, &frame.GetCommandBuffers()))
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
