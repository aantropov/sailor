#include "Renderer.h"
#include "GfxDevice/Vulkan/VulkanApi.h"
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

void Renderer::RunRenderLoop()
{
	m_bForceStop = false;

	m_renderingJob = JobSystem::Scheduler::CreateJob("Rendering Loop",
		[this]() {
		while (!m_bForceStop)
		{
			static float totalFramesCount = 0.0f;
			static float totalTime = 0.0f;

			SAILOR_PROFILE_BLOCK("Render Frame"); 

			const float beginFrameTime = (float)GetTickCount();

			GfxDevice::Vulkan::VulkanApi::DrawFrame(m_pViewport);

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
		GfxDevice::Vulkan::VulkanApi::WaitIdle();
		m_bForceStop = false;
	}, Sailor::JobSystem::EThreadType::Rendering);

	JobSystem::Scheduler::GetInstance()->Run(m_renderingJob);
}

void Renderer::StopRenderLoop()
{
	m_bForceStop = true;
	m_renderingJob->Wait();
}

bool Renderer::IsRunning() const
{
	return !m_renderingJob->IsFinished() && !m_renderingJob->IsReadyToStart();
}

void Renderer::RenderLoop_RenderThread()
{
}
