#include <mutex>
#include "Renderer.h"
#include "Mesh.h"
#include "GfxDevice/Vulkan/VulkanApi.h"
#include "GfxDevice/Vulkan/VulkanDevice.h"
#include "JobSystem/JobSystem.h"
#include "Memory/MemoryBlockAllocator.hpp"
#include "GfxDevice/Vulkan/VulkanDeviceMemory.h"

using namespace Sailor;
using namespace Sailor::RHI;

void Renderer::Initialize(Window const* pViewport, RHI::EMsaaSamples msaaSamples, bool bIsDebug)
{
	if (m_pInstance != nullptr)
	{
		SAILOR_LOG("Renderer already initialized!");
		return;
	}

	m_pInstance = new Renderer();
	m_pInstance->m_pViewport = pViewport;

#if defined(VULKAN)
	GfxDevice::Vulkan::VulkanApi::Initialize(pViewport, msaaSamples, bIsDebug);
	m_pInstance->m_vkInstance = GfxDevice::Vulkan::VulkanApi::GetInstance();

	m_pInstance->m_vkInstance->GetMainDevice()->CreateVertexBuffer();
	m_pInstance->m_vkInstance->GetMainDevice()->CreateGraphicsPipeline();
#endif
}

Renderer::~Renderer()
{
#if defined(VULKAN)
	m_vkInstance->Shutdown();
#endif
}

void Renderer::FixLostDevice()
{
#if defined(VULKAN)
	if (m_vkInstance->GetMainDevice()->ShouldFixLostDevice(m_pViewport))
	{
		SAILOR_PROFILE_BLOCK("Fix lost device");

		m_vkInstance->WaitIdle();
		m_vkInstance->GetMainDevice()->FixLostDevice(m_pViewport);

		SAILOR_PROFILE_END_BLOCK();
	}
#endif
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

		static Utils::Timer timer;
		timer.Start();

		do
		{
			static uint32_t totalFramesCount = 0U;

			SAILOR_PROFILE_BLOCK("Present Frame");

#if defined(VULKAN)
			if (m_vkInstance->PresentFrame(frame, &frame.GetCommandBuffers()))
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
#endif
			SAILOR_PROFILE_END_BLOCK();

		} while (m_pViewport->IsIconic());

	}, Sailor::JobSystem::EThreadType::Rendering);

	JobSystem::Scheduler::GetInstance()->Run(renderingJob);

	SAILOR_PROFILE_END_BLOCK();

	return true;
}

TRefPtr<RHI::Mesh> Sailor::RHI::Renderer::CreateMesh(const std::vector<RHI::Vertex>& vertices, const std::vector<uint32_t>& indices)
{
	TRefPtr<RHI::Mesh> res = TRefPtr<RHI::Mesh>::Make();

#if defined(VULKAN)

	const VkDeviceSize bufferSize = sizeof(vertices[0]) * vertices.size();
	const VkDeviceSize indexBufferSize = sizeof(indices[0]) * indices.size();

	res->m_indicesNum = indexBufferSize;
	res->m_verticesNum = bufferSize;

	res->m_vulkan.m_vertexBuffer = VulkanApi::CreateBuffer_Immediate(m_vkInstance->GetMainDevice(),
		reinterpret_cast<void const*>(&vertices[0]),
		bufferSize,
		VK_BUFFER_USAGE_VERTEX_BUFFER_BIT);

	res->m_vulkan.m_indexBuffer = VulkanApi::CreateBuffer_Immediate(m_vkInstance->GetMainDevice(),
		reinterpret_cast<void const*>(&indices[0]),
		indexBufferSize,
		VK_BUFFER_USAGE_INDEX_BUFFER_BIT);

#endif

	return res;
}
