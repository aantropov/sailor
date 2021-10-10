#include <mutex>
#include "Renderer.h"
#include "GfxDevice/Vulkan/VulkanApi.h"
#include "GfxDevice/Vulkan/VulkanDevice.h"
#include "JobSystem/JobSystem.h"
#include "Memory/MemoryBlockAllocator.hpp"
#include "GfxDevice/Vulkan/VulkanDeviceMemory.h"
#include "GfxDevice/Vulkan/VulkanMemory.hpp"

using namespace Sailor;

void Renderer::Initialize(Window const* pViewport, RHI::EMsaaSamples msaaSamples, bool bIsDebug)
{
	if (m_pInstance != nullptr)
	{
		SAILOR_LOG("Renderer already initialized!");
		return;
	}

	m_pInstance = new Renderer();
	m_pInstance->m_pViewport = pViewport;

	GfxDevice::Vulkan::VulkanApi::Initialize(pViewport, msaaSamples, bIsDebug);
}

Renderer::~Renderer()
{
	GfxDevice::Vulkan::VulkanApi::Shutdown();
}

void Renderer::FixLostDevice()
{
	if (GfxDevice::Vulkan::VulkanApi::GetInstance()->GetMainDevice()->ShouldFixLostDevice(m_pViewport))
	{
		SAILOR_PROFILE_BLOCK("Fix lost device");

		GfxDevice::Vulkan::VulkanApi::WaitIdle();
		GfxDevice::Vulkan::VulkanApi::GetInstance()->GetMainDevice()->FixLostDevice(m_pViewport);

		SAILOR_PROFILE_END_BLOCK();
	}
}

bool Renderer::PushFrame(const FrameState& frame)
{
	SAILOR_PROFILE_BLOCK("Wait for render thread");

	if (m_bForceStop || JobSystem::Scheduler::GetInstance()->GetNumRenderingJobs() > MaxFramesInQueue)
	{
		return false;
	}

	SAILOR_PROFILE_END_BLOCK();

	SAILOR_PROFILE_BLOCK("Push frame");

	TSharedPtr<class JobSystem::Job> renderingJob = JobSystem::Scheduler::CreateJob("Render Frame",
		[this, frame]() {

		do
		{
			static Utils::AccurateTimer timer;
			static uint32_t totalFramesCount = 0U;

			SAILOR_PROFILE_BLOCK("Present Frame");

			timer.Start();

			if (GfxDevice::Vulkan::VulkanApi::PresentFrame(frame))
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

	JobSystem::Scheduler::GetInstance()->Run(renderingJob);

	SAILOR_PROFILE_END_BLOCK();

	return true;
}
