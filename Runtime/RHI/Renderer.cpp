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
		StopRenderLoop();
		GfxDevice::Vulkan::VulkanApi::GetInstance()->GetMainDevice()->FixLostDevice(m_pViewport);
		RunRenderLoop();
	}
}

void Renderer::PushFrame(const FrameState& frame)
{
	if (m_numFrames >= MaxFramesInQueue)
	{
		std::unique_lock<std::mutex> lk(m_waitFinishRendering);
		m_onPopFrame.wait(lk, [this]() { return GetNumFrames() < MaxFramesInQueue; });
	}

	{
		std::scoped_lock(m_queueMutex);
		m_frames[m_numFrames] = frame;
		m_numFrames++;
	}

	m_onPushFrame.notify_one();
}

bool Renderer::TryPopFrame(FrameState& frame)
{
	if (GetNumFrames() == 0)
	{
		return false;
	}

	{
		std::scoped_lock(m_queueMutex);
		frame = m_frames[m_numFrames - 1];
		m_numFrames--;
	}

	m_onPopFrame.notify_one();

	return true;
}

void Renderer::RunRenderLoop()
{
	m_bForceStop = false;

	m_renderingJob = JobSystem::Scheduler::CreateJob("Rendering Loop",
		[this]() {

		std::mutex threadExecutionMutex;
		while (!this->m_bForceStop)
		{
			FrameState frame;
			if (!TryPopFrame(frame))
			{
				std::unique_lock<std::mutex> lk(threadExecutionMutex);
				m_onPushFrame.wait(lk, [this, &frame]() {return TryPopFrame(frame) || this->m_bForceStop; });

				if (this->m_bForceStop)
				{
					break;
				}
			}

			if (!m_pViewport->IsIconic())
			{
				static uint32_t totalFramesCount = 0U;
				static int64_t totalTime = 0U;

				SAILOR_PROFILE_BLOCK("Render Frame");

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

			if (m_numFrames >= MaxFramesInQueue)
			{
				m_onPopFrame.notify_one();
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
	m_onPushFrame.notify_one();
	m_renderingJob->Wait();
}

bool Renderer::IsRunning() const
{
	return !m_renderingJob->IsFinished();
}

void Renderer::RenderLoop_RenderThread()
{
}
