#include <mutex>
#include "Renderer.h"
#include "GfxDevice/Vulkan/VulkanApi.h"
#include "GfxDevice/Vulkan/VulkanDevice.h"
#include "JobSystem/JobSystem.h"

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
		WaitIdle();
		GfxDevice::Vulkan::VulkanApi::GetInstance()->GetMainDevice()->FixLostDevice(m_pViewport);
	}
}

bool Renderer::PushFrame(const FrameState& frame)
{
	SAILOR_PROFILE_FUNCTION();

	std::scoped_lock<std::mutex> lk(queueMutex);

	if (m_renderingJob)
	{
		m_renderingJob->Wait();
	}

	if (m_bForceStop)
	{
		return false;
	}

	m_renderingJob = JobSystem::Scheduler::CreateJob("Render Frame",
		[this]() {

		if (!m_pViewport->IsIconic())
		{
			static uint32_t totalFramesCount = 0U;
			static int64_t totalTime = 0U;

			SAILOR_PROFILE_BLOCK("Present Frame");

			if (GfxDevice::Vulkan::VulkanApi::PresentFrame())
			{
				totalFramesCount++;

				if (Utils::GetCurrentTimeMicro() - totalTime > 1000000)
				{
					m_smoothFps = totalFramesCount;
					totalFramesCount = 0;
					totalTime = Utils::GetCurrentTimeMicro();
				}
			}
			else
			{
				m_smoothFps = 0;
			}

			SAILOR_PROFILE_END_BLOCK();
		}
		else
		{
			m_smoothFps = 0;
		}

	}, Sailor::JobSystem::EThreadType::Rendering);

	JobSystem::Scheduler::GetInstance()->Run(m_renderingJob);

	return true;
}

void Renderer::WaitIdle()
{
	m_renderingJob->Wait();
	GfxDevice::Vulkan::VulkanApi::WaitIdle();
}
