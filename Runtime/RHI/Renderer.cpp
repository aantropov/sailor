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

	TSharedPtr<Sailor::JobSystem::Job> renderingJob = JobSystem::Scheduler::CreateJob("Rendering Loop",
		[this]() {
		m_bIsRunning = true;
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
				m_smoothFps = (float)totalFramesCount;
				totalFramesCount = 0;
				totalTime = 0;
			}

			SAILOR_PROFILE_END_BLOCK();
		}
		GfxDevice::Vulkan::VulkanApi::WaitIdle();
		m_bForceStop = false;
		m_renderLoopStopped.notify_all();
	}, Sailor::JobSystem::EThreadType::Rendering);

	JobSystem::Scheduler::GetInstance()->Run(renderingJob);
}

void Renderer::StopRenderLoop()
{
	m_bForceStop = true;
	std::unique_lock<std::mutex> lk(m_renderLoopActive);
	m_renderLoopStopped.wait(lk);
	m_bIsRunning = false;
}

void Renderer::RenderLoop_RenderThread()
{
}
