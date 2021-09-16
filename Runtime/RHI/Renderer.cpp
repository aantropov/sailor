#include "Renderer.h"
#include "GfxDevice/Vulkan/VulkanApi.h"
#include "GfxDevice/Vulkan/VulkanDevice.h"
#include "JobSystem/JobSystem.h"

using namespace Sailor;

void Renderer::Initialize(Window const* pViewport, bool bIsDebug)
{
	if (m_pInstance != nullptr)
	{
		SAILOR_LOG("Renderer already initialized!");
		return;
	}

	m_pInstance = new Renderer();
	m_pInstance->m_pViewport = pViewport;

	GfxDevice::Vulkan::VulkanApi::Initialize(pViewport, bIsDebug);
}

Renderer::~Renderer()
{
	GfxDevice::Vulkan::VulkanApi::Shutdown();
}

void Renderer::FixLostDevice()
{
	GfxDevice::Vulkan::VulkanApi::GetInstance()->GetMainDevice()->FixLostDevice(m_pViewport);
}

void Renderer::RunRenderLoop()
{
	m_bForceStop = false;

	m_renderingJob = JobSystem::Scheduler::CreateJob("Rendering Loop",
		[this]() {
		while (!this->m_bForceStop)
		{
			if (!m_pViewport->IsIconic())
			{
				static float totalFramesCount = 0.0f;
				static float totalTime = 0.0f;

				SAILOR_PROFILE_BLOCK("Render Frame");

				const float beginFrameTime = (float)GetTickCount();
				
				GfxDevice::Vulkan::VulkanApi::DrawFrame();

				totalTime += (float)GetTickCount() - beginFrameTime;
				totalFramesCount++;

				if (totalTime > 1000)
				{
					m_smoothFps = (uint32_t)totalFramesCount;
					totalFramesCount = 0;
					totalTime = 0;
				}

				SAILOR_PROFILE_END_BLOCK();
			}
			else
			{
				m_smoothFps = 0;
			}
		}
		GfxDevice::Vulkan::VulkanApi::WaitIdle();
		m_bForceStop = false;
	}, Sailor::JobSystem::EThreadType::Rendering);

	JobSystem::Scheduler::GetInstance()->Run(m_renderingJob);
}

void Renderer::StopRenderLoop()
{
	if (!IsRunning())
	{
		return;
	}

	m_bForceStop = true;
	m_renderingJob->Wait();
}

bool Renderer::IsRunning() const
{
	return !m_renderingJob->IsFinished();
}

void Renderer::RenderLoop_RenderThread()
{
}
