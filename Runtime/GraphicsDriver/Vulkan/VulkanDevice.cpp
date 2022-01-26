struct IUnknown; // Workaround for "combaseapi.h(229): error C2187: syntax error: 'identifier' was unexpected here" when using /permissive-

#include <chrono>
#include <set>
#include <assert.h>
#include <map>
#include "Containers/Vector.h"
#include <optional>
#include <wtypes.h>
#include <vulkan/vulkan.h>
#include <vulkan/vulkan_win32.h>
#include "AssetRegistry/AssetRegistry.h"
#include "AssetRegistry/Material/MaterialImporter.h"
#include "AssetRegistry/Shader/ShaderCompiler.h"
#include "AssetRegistry/Texture/TextureImporter.h"
#include "AssetRegistry/Model/ModelImporter.h"
#include "Platform/Win32/Window.h"
#include "Math/Math.h"
#include "VulkanDevice.h"
#include "VulkanApi.h"
#include "VulkanRenderPass.h"
#include "VulkanSwapchain.h"
#include "VulkanCommandBuffer.h"
#include "VulkanCommandPool.h"
#include "VulkanSemaphore.h"
#include "VulkanFence.h"
#include "VulkanQueue.h"
#include "VulkanImage.h"
#include "VulkanImageView.h"
#include "VulkanFrameBuffer.h"
#include "VulkanDeviceMemory.h"
#include "VulkanBuffer.h"
#include "VulkanShaderModule.h"
#include "VulkanPipeline.h"
#include "VulkanDescriptors.h"
#include "VulkanPipileneStates.h"
#include "JobSystem/JobSystem.h"
#include "Platform/Win32/Input.h"
#include "Winuser.h"
#include "Engine/EngineLoop.h"
#include "Memory/MemoryBlockAllocator.hpp"
#include "Memory/MemoryPoolAllocator.hpp"
#include "RHI/Types.h"
#include "RHI/Mesh.h"
#include "RHI/Buffer.h"
#include "RHI/Material.h"
#include "RHI/Shader.h"
#include "RHI/CommandList.h"
#include "Containers/Map.h"

using namespace glm;
using namespace Sailor;
using namespace Sailor::Win32;
using namespace Sailor::RHI;
using namespace Sailor::GraphicsDriver::Vulkan;

VkSampleCountFlagBits CalculateMaxAllowedMSAASamples(VkSampleCountFlags counts)
{
	if (counts & VK_SAMPLE_COUNT_64_BIT) { return VK_SAMPLE_COUNT_64_BIT; }
	if (counts & VK_SAMPLE_COUNT_32_BIT) { return VK_SAMPLE_COUNT_32_BIT; }
	if (counts & VK_SAMPLE_COUNT_16_BIT) { return VK_SAMPLE_COUNT_16_BIT; }
	if (counts & VK_SAMPLE_COUNT_8_BIT) { return VK_SAMPLE_COUNT_8_BIT; }
	if (counts & VK_SAMPLE_COUNT_4_BIT) { return VK_SAMPLE_COUNT_4_BIT; }
	if (counts & VK_SAMPLE_COUNT_2_BIT) { return VK_SAMPLE_COUNT_2_BIT; }

	return VK_SAMPLE_COUNT_1_BIT;
}

VulkanDevice::VulkanDevice(const Window* pViewport, RHI::EMsaaSamples requestMsaa)
{
	// Create Win32 surface
	CreateWin32Surface(pViewport);

	// Pick & Create device
	m_physicalDevice = VulkanApi::PickPhysicalDevice(m_surface);
	CreateLogicalDevice(m_physicalDevice);

	// Calculate max anisotropy
	VkPhysicalDeviceProperties properties{};
	vkGetPhysicalDeviceProperties(m_physicalDevice, &properties);
	m_maxAllowedAnisotropy = properties.limits.maxSamplerAnisotropy;
	m_maxAllowedMsaaSamples = CalculateMaxAllowedMSAASamples(properties.limits.framebufferColorSampleCounts & properties.limits.framebufferDepthSampleCounts);
	m_currentMsaaSamples = (VkSampleCountFlagBits)(std::min((uint8_t)requestMsaa, (uint8_t)m_maxAllowedMsaaSamples));

	SAILOR_LOG("m_maxAllowedAnisotropy = %.2f", m_maxAllowedAnisotropy);
	SAILOR_LOG("m_maxAllowedMSAASamples = %d, requestedMSAASamples = %d", m_maxAllowedMsaaSamples, m_currentMsaaSamples);

	// Cache samplers & states
	m_samplers = TUniquePtr<VulkanSamplerCache>::Make(VulkanDevicePtr(this));
	m_pipelineBuilder = TUniquePtr<VulkanPipelineStateBuilder>::Make(VulkanDevicePtr(this));

	// Cache memory requirements
	{
		m_minStorageBufferOffsetAlignment = properties.limits.minStorageBufferOffsetAlignment;
		m_minUboOffsetAlignment = properties.limits.minUniformBufferOffsetAlignment;
		properties.limits.optimalBufferCopyOffsetAlignment;
		VulkanBufferPtr stagingBuffer = VulkanBufferPtr::Make(VulkanDevicePtr(this),
			1024,
			VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
			VK_SHARING_MODE_CONCURRENT);

		stagingBuffer->Compile();
		m_memoryRequirements_StagingBuffer = stagingBuffer->GetMemoryRequirements();
		stagingBuffer.Clear();
	}

	// Create swapchain	CreateCommandPool();
	CreateSwapchain(pViewport);

	// Create graphics
	CreateRenderPass();
	CreateFramebuffers();
	CreateCommandBuffers();
	CreateFrameSyncSemaphores();

	m_bIsSwapChainOutdated = false;
}

VulkanDevice::~VulkanDevice()
{
	vkDestroyDevice(m_device, nullptr);

	m_surface.Clear();
}

void VulkanDevice::Shutdown()
{
	//Clear dependencies
	WaitIdle();

	CleanupSwapChain();

	m_swapchain.Clear();

	m_commandBuffers.Clear();
	m_commandPool.Clear();

	for (auto& pair : m_threadContext)
	{
		pair.m_second.Clear();
	}

	m_renderFinishedSemaphores.Clear();
	m_imageAvailableSemaphores.Clear();
	m_syncImages.Clear();
	m_syncFences.Clear();

	m_presentQueue.Clear();
	m_graphicsQueue.Clear();
	m_transferQueue.Clear();

	m_samplers.Clear();
	m_pipelineBuilder.Clear();
	m_threadContext.Clear();

	m_memoryAllocators.Clear();
}

ThreadContext& VulkanDevice::GetOrCreateThreadContext(DWORD threadId)
{
	auto res = m_threadContext.Find(threadId);
	if (res != m_threadContext.end())
	{
		return *(*res).m_second;
	}

	return *(m_threadContext[threadId] = CreateThreadContext());
}

ThreadContext& VulkanDevice::GetCurrentThreadContext()
{
	return GetOrCreateThreadContext(GetCurrentThreadId());
}

VulkanSurfacePtr VulkanDevice::GetSurface() const
{
	return m_surface;
}

VkFormat VulkanDevice::GetDepthFormat() const
{
	return VulkanApi::SelectFormatByFeatures(
		m_physicalDevice,
		{ VK_FORMAT_D32_SFLOAT_S8_UINT, VK_FORMAT_D24_UNORM_S8_UINT, VK_FORMAT_D32_SFLOAT },
		VK_IMAGE_TILING_OPTIMAL,
		VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT
	);
}

TBlockAllocator<class GlobalVulkanMemoryAllocator, class VulkanMemoryPtr>& VulkanDevice::GetMemoryAllocator(VkMemoryPropertyFlags properties, VkMemoryRequirements requirements)
{
	uint64_t hash = properties | ((uint64_t)requirements.memoryTypeBits) << 32;

	auto& pAllocator = m_memoryAllocators[hash];

	if (!pAllocator)
	{
		pAllocator = TUniquePtr<VulkanDeviceMemoryAllocator>::Make(1024 * 1024 * 64, 1024 * 512, 32 * 1024 * 1024);
	}

	auto& vulkanAllocator = pAllocator->GetGlobalAllocator();

	vulkanAllocator.SetMemoryProperties(properties);
	vulkanAllocator.SetMemoryRequirements(requirements);

	return *pAllocator;
}

bool VulkanDevice::IsMipsSupported(VkFormat format) const
{
	VkFormatProperties formatProperties;
	vkGetPhysicalDeviceFormatProperties(m_physicalDevice, format, &formatProperties);

	if (!(formatProperties.optimalTilingFeatures & VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT))
	{
		return false;
	}

	return true;
}

VulkanCommandBufferPtr VulkanDevice::CreateCommandBuffer(bool bOnlyTransferQueue)
{
	return VulkanCommandBufferPtr::Make(VulkanDevicePtr(this),
		bOnlyTransferQueue ? GetCurrentThreadContext().m_transferCommandPool : GetCurrentThreadContext().m_commandPool);
}

void VulkanDevice::SubmitCommandBuffer(VulkanCommandBufferPtr commandBuffer,
	VulkanFencePtr fence,
	TVector<VulkanSemaphorePtr> signalSemaphores,
	TVector<VulkanSemaphorePtr> waitSemaphores)
{
	SAILOR_PROFILE_FUNCTION();

	VkSubmitInfo submitInfo{};
	submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
	submitInfo.commandBufferCount = 1;
	submitInfo.pCommandBuffers = { commandBuffer->GetHandle() };

	VkSemaphore* waits = reinterpret_cast<VkSemaphore*>(_malloca(waitSemaphores.Num() * sizeof(VkSemaphore)));
	VkPipelineStageFlags* waitStages = reinterpret_cast<VkPipelineStageFlags*>(_malloca(waitSemaphores.Num() * sizeof(VkPipelineStageFlags)));
	VkSemaphore* signals = reinterpret_cast<VkSemaphore*>(_malloca(signalSemaphores.Num() * sizeof(VkSemaphore)));

	for (uint32_t i = 0; i < waitSemaphores.Num(); i++)
	{
		waits[i] = *waitSemaphores[i];
		waitStages[i] = waitSemaphores[i]->PipelineStageFlags();
	}

	for (uint32_t i = 0; i < signalSemaphores.Num(); i++)
	{
		signals[i] = *signalSemaphores[i];
	}

	submitInfo.signalSemaphoreCount = static_cast<uint32_t>(signalSemaphores.Num());
	submitInfo.pSignalSemaphores = &signals[0];

	submitInfo.waitSemaphoreCount = static_cast<uint32_t>(waitSemaphores.Num());
	submitInfo.pWaitSemaphores = &waits[0];
	submitInfo.pWaitDstStageMask = &waitStages[0];

	if (commandBuffer->GetCommandPool()->GetQueueFamilyIndex() == m_queueFamilies.m_transferFamily.value_or(-1))
	{
		VK_CHECK(m_transferQueue->Submit(submitInfo, fence));
	}
	else
	{
		VK_CHECK(m_graphicsQueue->Submit(submitInfo, fence));
	}

	_freea(waits);
	_freea(signals);
	_freea(waitStages);
}

void VulkanDevice::CreateRenderPass()
{
	VkFormat depthFormat = VK_FORMAT_D32_SFLOAT_S8_UINT; // VK_FORMAT_D24_UNORM_S8_UINT or VK_FORMAT_D32_SFLOAT_S8_UINT or VK_FORMAT_D24_SFLOAT_S8_UINT
	m_renderPass = VulkanApi::CreateMSSRenderPass(VulkanDevicePtr(this), m_swapchain->GetImageFormat(), depthFormat, (VkSampleCountFlagBits)m_currentMsaaSamples);
}

TUniquePtr<ThreadContext> VulkanDevice::CreateThreadContext()
{
	TUniquePtr<ThreadContext> context = TUniquePtr<ThreadContext>::Make();

	VulkanQueueFamilyIndices queueFamilyIndices = VulkanApi::FindQueueFamilies(m_physicalDevice, m_surface);
	context->m_commandPool = VulkanCommandPoolPtr::Make(VulkanDevicePtr(this), queueFamilyIndices.m_graphicsFamily.value(), VK_COMMAND_POOL_CREATE_TRANSIENT_BIT);
	context->m_transferCommandPool = VulkanCommandPoolPtr::Make(VulkanDevicePtr(this), queueFamilyIndices.m_transferFamily.value(), VK_COMMAND_POOL_CREATE_TRANSIENT_BIT);

	auto descriptorSizes = TVector
	{
		VulkanApi::CreateDescriptorPoolSize(VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1024),
		VulkanApi::CreateDescriptorPoolSize(VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1024)
	};

	context->m_descriptorPool = VulkanDescriptorPoolPtr::Make(VulkanDevicePtr(this), 1024, descriptorSizes);

	context->m_stagingBufferAllocator = TSharedPtr<VulkanBufferAllocator>::Make(1024 * 1024, 2048, 2 * 1024 * 1024);
	context->m_stagingBufferAllocator->GetGlobalAllocator().SetUsage(VK_BUFFER_USAGE_TRANSFER_SRC_BIT);
	//m_stagingBufferAllocator->GetGlobalAllocator().SetSharingMode(VK_SHARING_MODE_EXCLUSIVE);
	context->m_stagingBufferAllocator->GetGlobalAllocator().SetMemoryProperties(VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);

	return context;
}

void VulkanDevice::CreateFrameSyncSemaphores()
{
	m_syncImages.Resize(m_swapchain->GetImageViews().Num());

	for (size_t i = 0; i < VulkanApi::MaxFramesInFlight; i++)
	{
		m_imageAvailableSemaphores.Add(VulkanSemaphorePtr::Make(VulkanDevicePtr(this)));
		m_renderFinishedSemaphores.Add(VulkanSemaphorePtr::Make(VulkanDevicePtr(this)));
		m_syncFences.Add(VulkanFencePtr::Make(VulkanDevicePtr(this), VK_FENCE_CREATE_SIGNALED_BIT));
	}
}

bool VulkanDevice::RecreateSwapchain(const Window* pViewport)
{
	if (pViewport->GetWidth() == 0 || pViewport->GetHeight() == 0)
	{
		return false;
	}

	WaitIdle();

	CleanupSwapChain();

	CreateSwapchain(pViewport);
	CreateRenderPass();
	CreateFramebuffers();

	m_bIsSwapChainOutdated = false;
	return true;
}

void VulkanDevice::CreateFramebuffers()
{
	TVector<VulkanImageViewPtr>& swapChainImageViews = m_swapchain->GetImageViews();

	m_swapChainFramebuffers.Resize(swapChainImageViews.Num());

	TVector<VulkanImageViewPtr> attachments;

	uint8_t swapchainIndex = 1;
	if ((VkSampleCountFlagBits)m_currentMsaaSamples != VK_SAMPLE_COUNT_1_BIT)
	{
		attachments = { m_swapchain->GetColorBufferView(), nullptr, m_swapchain->GetDepthBufferView() };
	}
	else
	{
		swapchainIndex = 0;
		attachments = { nullptr, m_swapchain->GetDepthBufferView() };
	}

	for (size_t i = 0; i < swapChainImageViews.Num(); i++)
	{
		attachments[swapchainIndex] = swapChainImageViews[i];
		m_swapChainFramebuffers[i] = VulkanFramebufferPtr::Make(
			m_renderPass,
			attachments,
			m_swapchain->GetExtent().width,
			m_swapchain->GetExtent().height,
			1);
	}
}

void VulkanDevice::CreateCommandBuffers()
{
	// We use internal independent command pool with reset + to handle present command lists
	VulkanQueueFamilyIndices queueFamilyIndices = VulkanApi::FindQueueFamilies(m_physicalDevice, m_surface);
	m_commandPool = VulkanCommandPoolPtr::Make(VulkanDevicePtr(this), queueFamilyIndices.m_graphicsFamily.value(), VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT);

	for (int i = 0; i < m_swapChainFramebuffers.Num(); i++)
	{
		// Command buffer would be released fine if we initialize render in the main thread
		m_commandBuffers.Add(VulkanCommandBufferPtr::Make(VulkanDevicePtr(this), m_commandPool, VK_COMMAND_BUFFER_LEVEL_PRIMARY));
	}
}

void VulkanDevice::CreateLogicalDevice(VkPhysicalDevice physicalDevice)
{
	m_queueFamilies = VulkanApi::FindQueueFamilies(physicalDevice, m_surface);

	TVector<VkDeviceQueueCreateInfo> queueCreateInfos;
	std::set<uint32_t> uniqueQueueFamilies = { m_queueFamilies.m_graphicsFamily.value(),
		m_queueFamilies.m_presentFamily.value(),
		m_queueFamilies.m_transferFamily.value() };

	float queuePriority = 1.0f;

	for (uint32_t queueFamily : uniqueQueueFamilies)
	{
		VkDeviceQueueCreateInfo queueCreateInfo{ VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO };
		queueCreateInfo.queueFamilyIndex = queueFamily;
		queueCreateInfo.queueCount = 1;
		queueCreateInfo.pQueuePriorities = &queuePriority;
		queueCreateInfos.Add(queueCreateInfo);
	}

	VkPhysicalDeviceFeatures deviceFeatures{};
	deviceFeatures.samplerAnisotropy = VK_TRUE;

#ifdef SAILOR_MSAA_IMPACTS_TEXTURE_SAMPLING
	deviceFeatures.sampleRateShading = VK_TRUE;
#endif

	VkDeviceCreateInfo createInfo{ VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO };

	TVector<const char*> deviceExtensions;
	TVector<const char*> instanceExtensions;
	VulkanApi::GetRequiredExtensions(deviceExtensions, instanceExtensions);

	createInfo.enabledExtensionCount = static_cast<uint32_t>(deviceExtensions.Num());
	createInfo.ppEnabledExtensionNames = deviceExtensions.GetData();

	createInfo.queueCreateInfoCount = static_cast<uint32_t>(queueCreateInfos.Num());
	createInfo.pQueueCreateInfos = queueCreateInfos.GetData();
	createInfo.pEnabledFeatures = &deviceFeatures;

	// Compatibility with older Vulkan drivers
	const TVector<const char*> validationLayers = { "VK_LAYER_KHRONOS_validation" };
	if (VulkanApi::GetInstance()->IsEnabledValidationLayers())
	{
		createInfo.enabledLayerCount = static_cast<uint32_t>(validationLayers.Num());
		createInfo.ppEnabledLayerNames = validationLayers.GetData();
	}
	else
	{
		createInfo.enabledLayerCount = 0;
	}

	VK_CHECK(vkCreateDevice(physicalDevice, &createInfo, nullptr, &m_device));

	// Create queues
	VkQueue presentQueue;
	vkGetDeviceQueue(m_device, m_queueFamilies.m_presentFamily.value(), 0, &presentQueue);
	m_presentQueue = VulkanQueuePtr::Make(presentQueue, m_queueFamilies.m_presentFamily.value(), 0);

	VkQueue graphicsQueue;
	vkGetDeviceQueue(m_device, m_queueFamilies.m_graphicsFamily.value(), 0, &graphicsQueue);
	m_graphicsQueue = VulkanQueuePtr::Make(graphicsQueue, m_queueFamilies.m_graphicsFamily.value(), 0);

	VkQueue transferQueue;
	vkGetDeviceQueue(m_device, m_queueFamilies.m_transferFamily.value(), 0, &transferQueue);
	m_transferQueue = VulkanQueuePtr::Make(transferQueue, m_queueFamilies.m_transferFamily.value(), 0);
}

void VulkanDevice::CreateWin32Surface(const Window* viewport)
{
	VkWin32SurfaceCreateInfoKHR createInfoWin32{ VK_STRUCTURE_TYPE_WIN32_SURFACE_CREATE_INFO_KHR };
	createInfoWin32.hwnd = viewport->GetHWND();
	createInfoWin32.hinstance = viewport->GetHINSTANCE();
	VkSurfaceKHR surface;
	VK_CHECK(vkCreateWin32SurfaceKHR(VulkanApi::GetVkInstance(), &createInfoWin32, nullptr, &surface));

	m_surface = VulkanSurfacePtr::Make(surface, VulkanApi::GetVkInstance());
}

void VulkanDevice::CreateSwapchain(const Window* viewport)
{
	VulkanSwapchainPtr oldSwapchain = m_swapchain;
	m_swapchain.Clear();

	m_swapchain = VulkanSwapchainPtr::Make(
		VulkanDevicePtr(this),
		viewport->GetWidth(),
		viewport->GetHeight(),
		viewport->IsVsyncRequested(),
		oldSwapchain);
}

void VulkanDevice::CleanupSwapChain()
{
	m_swapChainFramebuffers.Clear();
	m_renderPass.Clear();
}

void VulkanDevice::WaitIdlePresentQueue()
{
	m_presentQueue->WaitIdle();
}

void VulkanDevice::WaitIdle()
{
	m_graphicsQueue->WaitIdle();
	vkDeviceWaitIdle(m_device);
}

bool VulkanDevice::ShouldFixLostDevice(const Win32::Window* pViewport)
{
	return IsSwapChainOutdated() || (m_swapchain && (pViewport->GetWidth() != m_swapchain->GetExtent().width ||
		pViewport->GetHeight() != m_swapchain->GetExtent().height));
}

void VulkanDevice::FixLostDevice(const Win32::Window* pViewport)
{
	RecreateSwapchain(pViewport);
}

bool VulkanDevice::PresentFrame(const FrameState& state, TVector<VulkanCommandBufferPtr> primaryCommandBuffers,
	TVector<VulkanCommandBufferPtr> secondaryCommandBuffers,
	TVector<VulkanSemaphorePtr> semaphoresToWait)
{
	// Wait while we recreate swapchain from main thread to sync with Win32Api
	if (m_bIsSwapChainOutdated)
	{
		return false;
	}

	// Wait while GPU is finishing frame
	m_syncFences[m_currentFrame]->Wait();

	uint32_t imageIndex;
	VkResult result = m_swapchain->AcquireNextImage(UINT64_MAX, m_imageAvailableSemaphores[m_currentFrame], VulkanFencePtr(), imageIndex);

	if (result == VK_ERROR_OUT_OF_DATE_KHR)
	{
		m_bIsSwapChainOutdated = true;
		return false;
	}
	else if (result != VK_SUCCESS && result != VK_SUBOPTIMAL_KHR)
	{
		SAILOR_LOG("Failed to acquire swap chain image!");
		return false;
	}

	// Check if a previous frame is using this image (i.e. there is its fence to wait on)
	if (m_syncImages[imageIndex] && *m_syncImages[imageIndex] != VK_NULL_HANDLE)
	{
		// Wait while previous frame frees the image
		m_syncImages[imageIndex]->Wait();
	}
	// Mark the image as now being in use by this frame
	m_syncImages[imageIndex] = m_syncFences[m_currentFrame];

	VkSubmitInfo submitInfo{};
	submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;

	//////////////////////////////////////////////////
	VulkanStateViewportPtr pStateViewport = new VulkanStateViewport((float)m_swapchain->GetExtent().width, (float)m_swapchain->GetExtent().height);

	TVector<VkCommandBuffer> commandBuffers;
	if (primaryCommandBuffers.Num() > 0)
	{
		for (auto cmdBuffer : primaryCommandBuffers)
		{
			commandBuffers.Add(*cmdBuffer);
		}
	}

	m_commandBuffers[imageIndex]->BeginCommandList();
	{
		m_commandBuffers[imageIndex]->BeginRenderPass(m_renderPass, m_swapChainFramebuffers[imageIndex], m_swapchain->GetExtent());

		if (secondaryCommandBuffers.Num() > 0)
		{
			for (auto cmdBuffer : secondaryCommandBuffers)
			{
				m_commandBuffers[imageIndex]->Execute(cmdBuffer);
			}
		}

		// Temporary

		if (auto engineLoop = App::GetSubmodule<EngineLoop>())
		{
			if (engineLoop->GetTestMesh())
			{
				auto pModel = engineLoop->GetTestMesh().Lock();

				auto& perInstanceBinding = engineLoop->GetPerInstanceBinding();

				SAILOR_PROFILE_BLOCK("Render meshes");
				for (uint32_t index = 0; index < pModel->GetMeshes().Num(); index++)
				{
					if (pModel->IsReady())
					{
						SAILOR_PROFILE_BLOCK("Get data");
						auto& material = pModel->GetMaterials()[index].Lock()->GetRHI();
						bool bIsMaterialReady = pModel->GetMaterials()[index] && pModel->GetMaterials()[index].Lock()->IsReady();
						auto& mesh = pModel->GetMeshes()[index];
						SAILOR_PROFILE_END_BLOCK();

						if (mesh && mesh->IsReady() && bIsMaterialReady && material && material->m_vulkan.m_pipeline && perInstanceBinding && perInstanceBinding->m_vulkan.m_descriptorSet)
						{
							SAILOR_PROFILE_BLOCK("Bind pipelines");
							m_commandBuffers[imageIndex]->BindPipeline(material->m_vulkan.m_pipeline);
							SAILOR_PROFILE_END_BLOCK();

							m_commandBuffers[imageIndex]->SetViewport(pStateViewport);
							m_commandBuffers[imageIndex]->SetScissor(pStateViewport);

							SAILOR_PROFILE_BLOCK("Bind buffers");
							m_commandBuffers[imageIndex]->BindVertexBuffers({ mesh->m_vertexBuffer->m_vulkan.m_buffer });
							m_commandBuffers[imageIndex]->BindIndexBuffer(mesh->m_indexBuffer->m_vulkan.m_buffer);
							SAILOR_PROFILE_END_BLOCK();

							// TODO: Parse missing descriptor sets
							TVector<VulkanDescriptorSetPtr> sets;
							if (state.GetFrameBinding()->GetShaderBindings().Num())
							{
								sets.Add(state.GetFrameBinding()->m_vulkan.m_descriptorSet);
							}
							if (perInstanceBinding->GetShaderBindings().Num())
							{
								sets.Add(perInstanceBinding->m_vulkan.m_descriptorSet);
							}
							if (material->GetBindings()->GetShaderBindings().Num())
							{
								sets.Add(material->GetBindings()->m_vulkan.m_descriptorSet);
							}

							SAILOR_PROFILE_BLOCK("Bind descriptor sets");
							m_commandBuffers[imageIndex]->BindDescriptorSet(material->m_vulkan.m_pipeline->m_layout, sets);
							SAILOR_PROFILE_END_BLOCK();

							SAILOR_PROFILE_BLOCK("Draw Indexed");
							m_commandBuffers[imageIndex]->DrawIndexed(mesh->m_indexBuffer->m_vulkan.m_buffer, 1, 0, 0, perInstanceBinding->GetOrCreateShaderBinding("data")->GetStorageInstanceIndex());
							SAILOR_PROFILE_END_BLOCK();

							SAILOR_PROFILE_END_BLOCK();
						}
					}
				}
				SAILOR_PROFILE_END_BLOCK();
			}
		}

		m_commandBuffers[imageIndex]->EndRenderPass();
	}
	m_commandBuffers[imageIndex]->EndCommandList();
	commandBuffers.Add(*m_commandBuffers[imageIndex]->GetHandle());

	TVector<VkSemaphore> waitSemaphores;
	if (semaphoresToWait.Num() > 0)
	{
		//waitSemaphores.reserve(semaphoresToWait.size() + 1);
		for (auto semaphore : semaphoresToWait)
		{
			waitSemaphores.Add(*semaphore);
			m_commandBuffers[imageIndex]->AddDependency(semaphore);
		}
	}

	waitSemaphores.Add(*m_imageAvailableSemaphores[m_currentFrame]);

	///////////////////////////////////////////////////

	VkPipelineStageFlags* waitStages = reinterpret_cast<VkPipelineStageFlags*>(_malloca(waitSemaphores.Num() * sizeof(VkPipelineStageFlags)));

	for (uint32_t i = 0; i < waitSemaphores.Num(); i++)
	{
		waitStages[i] = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
	}

	submitInfo.waitSemaphoreCount = static_cast<uint32_t>(waitSemaphores.Num());
	submitInfo.pWaitSemaphores = &waitSemaphores[0];
	submitInfo.pWaitDstStageMask = waitStages;

	submitInfo.commandBufferCount = static_cast<uint32_t>(commandBuffers.Num());
	submitInfo.pCommandBuffers = &commandBuffers[0];

	VkSemaphore signalSemaphores[] = { *m_renderFinishedSemaphores[m_currentFrame] };
	submitInfo.signalSemaphoreCount = 1;
	submitInfo.pSignalSemaphores = signalSemaphores;

	m_syncFences[m_currentFrame]->Reset();

	//TODO: Transfer queue for transfer family command lists
	VK_CHECK(m_graphicsQueue->Submit(submitInfo, m_syncFences[m_currentFrame]));

	_freea(waitStages);

	VkPresentInfoKHR presentInfo{};
	presentInfo.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;

	presentInfo.waitSemaphoreCount = 1;
	presentInfo.pWaitSemaphores = signalSemaphores;

	VkSwapchainKHR swapChains[] = { *m_swapchain };
	presentInfo.swapchainCount = 1;
	presentInfo.pSwapchains = swapChains;
	presentInfo.pImageIndices = &imageIndex;
	presentInfo.pResults = nullptr; // Optional

	result = m_presentQueue->Present(presentInfo);

	if (result == VK_ERROR_OUT_OF_DATE_KHR || result == VK_SUBOPTIMAL_KHR)
	{
		m_bIsSwapChainOutdated = true;
	}
	else if (result != VK_SUCCESS)
	{
		SAILOR_LOG("Failed to present swap chain image!");
		return false;
	}

	m_currentFrame = (m_currentFrame + 1) % VulkanApi::MaxFramesInFlight;

	return true;
}
