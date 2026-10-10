struct IUnknown; // Workaround for "combaseapi.h(229): error C2187: syntax error: 'identifier' was unexpected here" when using /permissive-

#include <chrono>
#include "Sailor.h"
#include "Platform/Thread.h"
#include "Containers/Containers.h"
#include <cstdlib>
#include <set>
#include <map>
#include "Containers/Vector.h"
#include <optional>
#include <vulkan/vulkan.h>
#ifdef _WIN32
#include <wtypes.h>
#include <windows.h>
#include <vulkan/vulkan_win32.h>
#endif
#ifdef __APPLE__
#include "Platform/Mac/Window.h"
#include <vulkan/vulkan_metal.h>
#include <vulkan/vulkan_macos.h>
#endif
#include "AssetRegistry/AssetRegistry.h"
#include "AssetRegistry/Material/MaterialImporter.h"
#include "AssetRegistry/Shader/ShaderCompiler.h"
#include "AssetRegistry/Texture/TextureImporter.h"
#include "AssetRegistry/Model/ModelImporter.h"
#include "Platform/Window.h"
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
#include "VulkanFramebuffer.h"
#include "VulkanDeviceMemory.h"
#include "VulkanBuffer.h"
#include "VulkanShaderModule.h"
#include "VulkanPipeline.h"
#include "VulkanDescriptors.h"
#include "VulkanPipileneStates.h"
#include "Tasks/Scheduler.h"
#include "Platform/Win32/Input.h"
#if defined(_WIN32)
#include "Winuser.h"
#endif
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

#include "Components/TestComponent.h"
#include "Components/MeshRendererComponent.h"
#include "Engine/GameObject.h"
#include "Engine/World.h"
#include "Engine/EngineLoop.h"

using namespace glm;
using namespace Sailor;
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

VulkanDevice::VulkanDevice(Platform::Window* pViewport, RHI::EMsaaSamples requestMsaa)
	: m_maxFramesInFlight(App::GetGraphicsSettings().m_maxFramesInFlight)
{
	CreateSurface(pViewport);

	// Pick & Create device
	m_physicalDevice = VulkanApi::PickPhysicalDevice(m_surface);
	if (m_physicalDevice == VK_NULL_HANDLE)
	{
		SAILOR_LOG_ERROR("Failed to pick a Vulkan physical device.");
		return;
	}

	vkGetPhysicalDeviceProperties(m_physicalDevice, &m_physicalDeviceProperties);

	m_maxAllowedMsaaSamples = CalculateMaxAllowedMSAASamples(m_physicalDeviceProperties.limits.framebufferColorSampleCounts & m_physicalDeviceProperties.limits.framebufferDepthSampleCounts);
	m_currentMsaaSamples = (VkSampleCountFlagBits)(std::min((uint8_t)requestMsaa, (uint8_t)m_maxAllowedMsaaSamples));
	if (!CreateLogicalDevice(m_physicalDevice)) return;

	SAILOR_LOG("maxDescriptorSetSampledImages = %d", (int32_t)m_physicalDeviceProperties.limits.maxDescriptorSetSampledImages);
	SAILOR_LOG("maxSamplerAnisotropy = %.2f", m_physicalDeviceProperties.limits.maxSamplerAnisotropy);
	SAILOR_LOG("bufferImageGranularity = %d", (int32_t)m_physicalDeviceProperties.limits.bufferImageGranularity);
	SAILOR_LOG("m_maxAllowedMSAASamples = %d, requestedMSAASamples = %d", m_maxAllowedMsaaSamples, m_currentMsaaSamples);
	SAILOR_LOG("m_bSupportsMultiDrawIndirect = %d", (int32_t)m_bSupportsMultiDrawIndirect);
	SAILOR_LOG("maxFramesInFlight = %u", m_maxFramesInFlight);

	// Cache samplers & states
	m_samplers = TUniquePtr<VulkanSamplerCache>::Make(VulkanDevicePtr(this));
	m_pipelineBuilder = TUniquePtr<VulkanPipelineStateBuilder>::Make(VulkanDevicePtr(this));

	// Cache memory requirements
	{
		VulkanBufferPtr stagingBuffer = VulkanBufferPtr::Make(VulkanDevicePtr(this),
			1024,
			VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
			VK_SHARING_MODE_CONCURRENT);

		stagingBuffer->Compile();
		m_memoryRequirements_StagingBuffer = stagingBuffer->GetMemoryRequirements();
		stagingBuffer.Clear();
	}

	// Get debug extension
#ifndef _SHIPPING
	m_pSetDebugUtilsObjectNameEXT = (PFN_vkSetDebugUtilsObjectNameEXT)vkGetInstanceProcAddr(VulkanApi::GetInstance()->GetVkInstance(), "vkSetDebugUtilsObjectNameEXT");

	m_pCmdDebugMarkerBegin = (PFN_vkCmdDebugMarkerBeginEXT)vkGetDeviceProcAddr(m_device, "vkCmdDebugMarkerBeginEXT");
	m_pCmdDebugMarkerEnd = (PFN_vkCmdDebugMarkerEndEXT)vkGetDeviceProcAddr(m_device, "vkCmdDebugMarkerEndEXT");
	m_pCmdDebugMarkerInsert = (PFN_vkCmdDebugMarkerInsertEXT)vkGetDeviceProcAddr(m_device, "vkCmdDebugMarkerInsertEXT");
#endif

	// Create swapchain	CreateCommandPool();
	if (!CreateSwapchain(pViewport))
	{
		SAILOR_LOG_ERROR("Failed to create the initial Vulkan swapchain.");
		std::abort();
	}

	// Create graphics
	CreateDefaultRenderPass();
	CreateFrameDependencies();
	CreateFrameSyncSemaphores();

	m_bIsSwapChainOutdated = false;

}

void VulkanDevice::vkCmdBeginRenderingKHR(VkCommandBuffer commandBuffer, const VkRenderingInfo* pRenderingInfo)
{
	pVkCmdBeginRendering(commandBuffer, pRenderingInfo);
}

void VulkanDevice::vkCmdEndRenderingKHR(VkCommandBuffer commandBuffer)
{
	pVkCmdEndRendering(commandBuffer);
}

VulkanDevice::~VulkanDevice()
{
	if (m_device) vkDestroyDevice(m_device, nullptr);
}

bool VulkanDevice::BeginConditionalDestroy()
{
	// Keep device resources alive if pending work could not be drained.
	if (!m_bIsDeviceLost && (!ConsumeAcquiredImage() || WaitIdle() != VK_SUCCESS) && !m_bIsDeviceLost)
		return false;

	CleanupSwapChain();

	m_oldSwapchain.Clear();
	m_swapchain.Clear();
	m_commandPool.Clear();

	for (auto& pair : m_threadContext)
	{
		pair.m_second.Clear();
	}

	m_renderFinishedSemaphores.Clear();
	m_imageAvailableSemaphores.Clear();
	m_syncImages.Clear();
	m_syncFences.Clear();

	m_frameDeps.Clear();
	m_acquiredImageFlight.reset();

	m_samplers.Clear();
	m_pipelineBuilder.Clear();
	m_surface.Clear();
	return true;
}

void VulkanDevice::Shutdown()
{
	m_presentQueue.Clear();
	m_graphicsQueue.Clear();
	m_computeQueue.Clear();
	m_transferQueue.Clear();

	m_threadContext.Clear();
	m_memoryAllocators.Clear();
}

ThreadContext& VulkanDevice::GetOrAddThreadContext(DWORD threadId)
{
	// Resource creation is allowed on any thread; each caller owns its pools.
	auto& res = m_threadContext.At_Lock(threadId);
	if (!res)
	{
		res = CreateThreadContext();

#ifndef _SHIPPING
		VkDescriptorPool pool = *res->m_descriptorPool;
		SetDebugName(VkObjectType::VK_OBJECT_TYPE_DESCRIPTOR_POOL, (uint64_t)pool, Utils::GetCurrentThreadName().ToString());
#endif
	}
	m_threadContext.Unlock(threadId);

	return *res;
}

ThreadContext& VulkanDevice::GetCurrentThreadContext()
{
	// Main's pools follow its queue across bootstrap, engine-loop and shutdown threads.
	// The former owner can still allocate resources through its own native-thread context.
	const DWORD contextId = App::GetSubmodule<Tasks::Scheduler>()->IsMainThread() ? 0 : GetCurrentThreadId();
	return GetOrAddThreadContext(contextId);
}

VulkanSurfacePtr VulkanDevice::GetSurface() const
{
	return m_surface;
}

VkFormat VulkanDevice::GetColorFormat() const
{
	return m_swapchain->GetImageFormat();
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

VulkanDeviceMemoryAllocator& VulkanDevice::GetMemoryAllocator(VkMemoryPropertyFlags properties,
	VkMemoryRequirements requirements, EVulkanMemoryClass memoryClass)
{
	const MemoryAllocatorKey key{ properties, requirements.memoryTypeBits, memoryClass };
	auto& pAllocator = m_memoryAllocators.At_Lock(key);

	if (!pAllocator)
	{
		const size_t AverageElementSize = 512 * 512 * 16;
		const size_t BlockSize = 32 * AverageElementSize;
		const size_t ReservedSize = 64 * AverageElementSize;

		pAllocator = TUniquePtr<VulkanDeviceMemoryAllocator>::Make(BlockSize, AverageElementSize, ReservedSize);

		// Pool configuration stays immutable while allocations use their own lock.
		auto& vulkanAllocator = pAllocator->GetGlobalAllocator();
		vulkanAllocator.SetMemoryProperties(properties);
		vulkanAllocator.SetMemoryRequirements(requirements);
	}

	m_memoryAllocators.Unlock(key);

	return *pAllocator;
}

void VulkanDevice::vkCmdDebugMarkerBegin(VulkanCommandBufferPtr cmdBuffer, const VkDebugMarkerMarkerInfoEXT* markerInfo)
{
#ifndef _SHIPPING
	if (m_pCmdDebugMarkerBegin && m_pCmdDebugMarkerEnd)
	{
		m_pCmdDebugMarkerBegin(*cmdBuffer, markerInfo);
	}
#endif
}

void VulkanDevice::vkCmdDebugMarkerEnd(VulkanCommandBufferPtr cmdBuffer)
{
#ifndef _SHIPPING
	if (m_pCmdDebugMarkerBegin && m_pCmdDebugMarkerEnd)
	{
		m_pCmdDebugMarkerEnd(*cmdBuffer);
	}
#endif
}

void VulkanDevice::SetDebugName(VkObjectType type, uint64_t objectHandle, const std::string& name)
{
#ifndef _SHIPPING
	if (!m_pSetDebugUtilsObjectNameEXT)
	{
		return;
	}
	VkDebugUtilsObjectNameInfoEXT nameInfo = {};
	nameInfo.sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_OBJECT_NAME_INFO_EXT;
	nameInfo.objectType = type;
	nameInfo.objectHandle = (uint64_t)objectHandle;
	nameInfo.pObjectName = name.c_str();
	m_pSetDebugUtilsObjectNameEXT(m_device, &nameInfo);
#endif
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

VulkanCommandBufferPtr VulkanDevice::CreateCommandBuffer(RHI::ECommandListQueue queue)
{
	switch (queue)
	{
	case RHI::ECommandListQueue::Graphics:
		return VulkanCommandBufferPtr::Make(VulkanDevicePtr(this), GetCurrentThreadContext().m_commandPool);
		break;
	case RHI::ECommandListQueue::Transfer:
		return VulkanCommandBufferPtr::Make(VulkanDevicePtr(this), GetCurrentThreadContext().m_transferCommandPool);
		break;
	case RHI::ECommandListQueue::Compute:
		return VulkanCommandBufferPtr::Make(VulkanDevicePtr(this), GetCurrentThreadContext().m_computeCommandPool);
		break;
	}

	check(0);
	return nullptr;
}

bool VulkanDevice::SubmitCommandBuffer(VulkanCommandBufferPtr commandBuffer,
	VulkanFencePtr fence,
	TVector<VulkanSemaphorePtr> signalSemaphores,
	TVector<VulkanSemaphorePtr> waitSemaphores,
	const void* submitNext)
{
	SAILOR_PROFILE_FUNCTION();

	if (m_bIsDeviceLost) return false;

	VkSubmitInfo submitInfo{};
	submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
	submitInfo.pNext = submitNext;
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

	auto* queue = m_graphicsQueue.GetRawPtr();

	if (commandBuffer->GetCommandPool()->GetQueueFamilyIndex() == m_queueFamilies.m_computeFamily.value_or(-1))
	{
		queue = m_computeQueue.GetRawPtr();
	}
	else if (commandBuffer->GetCommandPool()->GetQueueFamilyIndex() == m_queueFamilies.m_transferFamily.value_or(-1))
	{
		queue = m_transferQueue.GetRawPtr();
	}
	const VkResult submitResult = queue->Submit(submitInfo, fence);

	_freea(waits);
	_freea(signals);
	_freea(waitStages);

	if (submitResult == VK_ERROR_DEVICE_LOST)
	{
		m_bIsDeviceLost = true;
		// DEVICE_LOST may still have submitted work. Drain it before the caller
		// releases the command's dependencies; a lost queue wait returns finitely.
		queue->WaitIdle();
	}

	if (submitResult != VK_SUCCESS)
	{
		SAILOR_LOG_ERROR("VulkanDevice::SubmitCommandBuffer failed: %d", static_cast<int>(submitResult));
		return false;
	}

	m_numSubmittedCommandBuffersAcc.fetch_add(1u, std::memory_order_relaxed);
	return true;
}

void VulkanDevice::CreateDefaultRenderPass()
{
	VkFormat depthFormat = VK_FORMAT_D32_SFLOAT_S8_UINT; // VK_FORMAT_D24_UNORM_S8_UINT or VK_FORMAT_D32_SFLOAT_S8_UINT or VK_FORMAT_D24_SFLOAT_S8_UINT
	m_renderPass = VulkanApi::CreateMSSRenderPass(VulkanDevicePtr(this), m_swapchain->GetImageFormat(), depthFormat, (VkSampleCountFlagBits)m_currentMsaaSamples);
}

TUniquePtr<ThreadContext> VulkanDevice::CreateThreadContext()
{
	TUniquePtr<ThreadContext> context = TUniquePtr<ThreadContext>::Make();

	check(m_queueFamilies.IsComplete());
	// Completed command buffers can be reset independently of other work in the pool.
	constexpr VkCommandPoolCreateFlags poolFlags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT | VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
	context->m_commandPool = VulkanCommandPoolPtr::Make(VulkanDevicePtr(this), m_queueFamilies.m_graphicsFamily.value(), poolFlags);
	context->m_transferCommandPool = VulkanCommandPoolPtr::Make(VulkanDevicePtr(this), m_queueFamilies.m_transferFamily.value(), poolFlags);
	context->m_computeCommandPool = VulkanCommandPoolPtr::Make(VulkanDevicePtr(this), m_queueFamilies.m_computeFamily.value(), poolFlags);

	auto descriptorSizes = TVector
	{
		VulkanApi::CreateDescriptorPoolSize(VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1024),
		VulkanApi::CreateDescriptorPoolSize(VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 8192),
		VulkanApi::CreateDescriptorPoolSize(VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 4096),
		VulkanApi::CreateDescriptorPoolSize(VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1024)
	};

	context->m_descriptorPool = VulkanDescriptorPoolPtr::Make(VulkanDevicePtr(this), 8192, descriptorSizes);

	context->m_stagingBufferAllocator = TSharedPtr<VulkanBufferAllocator>::Make(1024 * 1024, 1024 * 512, 2 * 1024 * 1024);
	context->m_stagingBufferAllocator->GetGlobalAllocator().SetUsage(VK_BUFFER_USAGE_TRANSFER_SRC_BIT);
	//m_stagingBufferAllocator->GetGlobalAllocator().SetSharingMode(VK_SHARING_MODE_EXCLUSIVE);
	context->m_stagingBufferAllocator->GetGlobalAllocator().SetMemoryProperties(VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);

	return context;
}

void VulkanDevice::CreateFrameSyncSemaphores()
{
	m_imageAvailableSemaphores.Clear();
	m_renderFinishedSemaphores.Clear();
	m_syncFences.Clear();
	m_syncImages.Clear();
	m_swapchainImagesInitialized.Clear();
	m_syncImages.Resize(m_swapchain->GetImageViews().Num());

	for (size_t i = 0; i < m_maxFramesInFlight; i++)
	{
		m_imageAvailableSemaphores.Add(VulkanSemaphorePtr::Make(VulkanDevicePtr(this)));
		m_syncFences.Add(VulkanFencePtr::Make(VulkanDevicePtr(this), VK_FENCE_CREATE_SIGNALED_BIT));
	}
	// Reacquiring the same image proves that presentation consumed its semaphore.
	for (size_t i = 0; i < m_syncImages.Num(); ++i)
	{
		m_renderFinishedSemaphores.Add(VulkanSemaphorePtr::Make(VulkanDevicePtr(this)));
		m_swapchainImagesInitialized.Add(false);
	}
	m_currentFrame = 0;
	m_currentSwapchainImageIndex = 0;
}

bool VulkanDevice::RecreateSwapchain(Platform::Window* pViewport)
{
	if (m_bIsDeviceLost || pViewport->GetWidth() == 0 || pViewport->GetHeight() == 0)
	{
		return false;
	}

	if (!ConsumeAcquiredImage() || WaitIdle() != VK_SUCCESS) return false;

	if (!CreateSwapchain(pViewport))
	{
		m_bIsSwapChainOutdated = true;
		return false;
	}

	CleanupSwapChain();
	CreateDefaultRenderPass();

	m_frameDeps.Clear();

	CreateFrameDependencies();
	CreateFrameSyncSemaphores();

	m_bIsSwapChainOutdated = false;
	m_bIsSwapChainSuboptimal = false;
	m_frameSubmissionError = VK_SUCCESS;
	m_bLastFrameSubmitSuccessful = false;

	return true;
}

void VulkanDevice::CreateFrameDependencies()
{
	m_frameDeps.Resize(m_maxFramesInFlight);
}

bool VulkanDevice::CreateLogicalDevice(VkPhysicalDevice physicalDevice)
{
	supportedDeviceExtensions = VulkanApi::GetSupportedDeviceExtensions(physicalDevice);
	VulkanDeviceFeatures features;
	features.Query(physicalDevice, supportedDeviceExtensions);
	if (const char* missing = features.GetMissingRequirement())
	{
		SAILOR_LOG_ERROR("Cannot create Vulkan device: required feature %s is unavailable.", missing);
		return false;
	}

	m_queueFamilies = VulkanApi::FindQueueFamilies(physicalDevice, m_surface);
	TVector<VkDeviceQueueCreateInfo> queueCreateInfos;
	TSet<uint32_t> uniqueQueueFamilies = { m_queueFamilies.m_graphicsFamily.value(),
		m_queueFamilies.m_presentFamily.value(),
		m_queueFamilies.m_transferFamily.value(),
		m_queueFamilies.m_computeFamily.value() };
	float queuePriority = 1.0f;
	for (uint32_t queueFamily : uniqueQueueFamilies)
	{
		VkDeviceQueueCreateInfo queueCreateInfo{ VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO };
		queueCreateInfo.queueFamilyIndex = queueFamily;
		queueCreateInfo.queueCount = 1;
		queueCreateInfo.pQueuePriorities = &queuePriority;
		queueCreateInfos.Add(queueCreateInfo);
	}

	auto deviceExtensions = VulkanApi::GetRequiredDeviceExtensions(features.m_apiVersion);
	const auto hasDeviceExtension = [&](const char* extensionName)
	{
		return supportedDeviceExtensions.Contains(std::string(extensionName));
	};
#ifndef _SHIPPING
	if (hasDeviceExtension(VK_EXT_DEBUG_MARKER_EXTENSION_NAME)) deviceExtensions.Add(VK_EXT_DEBUG_MARKER_EXTENSION_NAME);
#endif
#if defined(__APPLE__)
	m_bSupportsMetalObjects = hasDeviceExtension(VK_EXT_METAL_OBJECTS_EXTENSION_NAME);
	if (m_bSupportsMetalObjects) deviceExtensions.Add(VK_EXT_METAL_OBJECTS_EXTENSION_NAME);
#endif
	if (features.m_apiVersion < VK_API_VERSION_1_3)
	{
		for (const char* extension : { VK_KHR_MAINTENANCE_4_EXTENSION_NAME, VK_KHR_SYNCHRONIZATION_2_EXTENSION_NAME })
		{
			if (hasDeviceExtension(extension)) deviceExtensions.Add(extension);
		}
	}
	if (hasDeviceExtension(VK_EXT_SHADER_ATOMIC_FLOAT_EXTENSION_NAME))
	{
		deviceExtensions.Add(VK_EXT_SHADER_ATOMIC_FLOAT_EXTENSION_NAME);
	}

	VkPhysicalDeviceVulkan12Properties core12Properties{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_PROPERTIES };
	VkPhysicalDeviceProperties2 properties{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, &core12Properties };
	vkGetPhysicalDeviceProperties2(physicalDevice, &properties);
	m_depthStencilResolveProperties.supportedDepthResolveModes = core12Properties.supportedDepthResolveModes;
	m_depthStencilResolveProperties.supportedStencilResolveModes = core12Properties.supportedStencilResolveModes;
	m_depthStencilResolveProperties.independentResolveNone = core12Properties.independentResolveNone;
	m_depthStencilResolveProperties.independentResolve = core12Properties.independentResolve;

	features.Enable();
	const auto& core12 = features.m_core12;
	m_bSupportsDescriptorUpdateAfterBind =
		core12.descriptorIndexing &&
		core12.descriptorBindingPartiallyBound &&
		core12.descriptorBindingUpdateUnusedWhilePending &&
		core12.descriptorBindingSampledImageUpdateAfterBind &&
		core12.descriptorBindingStorageBufferUpdateAfterBind &&
		core12.descriptorBindingUniformBufferUpdateAfterBind &&
		core12.descriptorBindingStorageImageUpdateAfterBind;
	m_bSupportsHostQueryReset = core12.hostQueryReset == VK_TRUE;
	m_bSupportsSamplerFilterMinmax = core12.samplerFilterMinmax == VK_TRUE;
	m_bSupportsMultiDrawIndirect = features.m_base.features.multiDrawIndirect &&
		m_physicalDeviceProperties.limits.maxDrawIndirectCount > 1;

	VkDeviceCreateInfo createInfo{ VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO };
	createInfo.enabledExtensionCount = static_cast<uint32_t>(deviceExtensions.Num());
	createInfo.ppEnabledExtensionNames = deviceExtensions.GetData();
	createInfo.queueCreateInfoCount = static_cast<uint32_t>(queueCreateInfos.Num());
	createInfo.pQueueCreateInfos = queueCreateInfos.GetData();
	createInfo.pNext = &features.m_base;

	VkDevice device = VK_NULL_HANDLE;
	const VkResult result = vkCreateDevice(physicalDevice, &createInfo, nullptr, &device);
	if (result != VK_SUCCESS)
	{
		SAILOR_LOG_ERROR("Failed to create Vulkan device: %d", static_cast<int32_t>(result));
		return false;
	}

	const bool coreRendering = features.m_apiVersion >= VK_API_VERSION_1_3;
	const auto beginRendering = reinterpret_cast<PFN_vkCmdBeginRendering>(
		vkGetDeviceProcAddr(device, coreRendering ? "vkCmdBeginRendering" : "vkCmdBeginRenderingKHR"));
	const auto endRendering = reinterpret_cast<PFN_vkCmdEndRendering>(
		vkGetDeviceProcAddr(device, coreRendering ? "vkCmdEndRendering" : "vkCmdEndRenderingKHR"));
	if (!beginRendering || !endRendering)
	{
		SAILOR_LOG_ERROR("Required Vulkan %s dynamic rendering commands are unavailable (begin=%d, end=%d).",
			coreRendering ? "core" : "KHR", beginRendering != nullptr, endRendering != nullptr);
		vkDestroyDevice(device, nullptr);
		return false;
	}

	m_device = device;
	pVkCmdBeginRendering = beginRendering;
	pVkCmdEndRendering = endRendering;
	m_getFenceStatus = reinterpret_cast<PFN_vkGetFenceStatus>(vkGetDeviceProcAddr(m_device, "vkGetFenceStatus"));
	m_waitForFences = reinterpret_cast<PFN_vkWaitForFences>(vkGetDeviceProcAddr(m_device, "vkWaitForFences"));
	SAILOR_LOG("Vulkan dynamic rendering: %s", coreRendering ? "core 1.3" : "KHR 1.2");
	SAILOR_LOG("m_bSupportsDescriptorUpdateAfterBind = %d", static_cast<int32_t>(m_bSupportsDescriptorUpdateAfterBind));
	SAILOR_LOG("m_bSupportsHostQueryReset = %d", static_cast<int32_t>(m_bSupportsHostQueryReset));

	// A VkQueue requires external synchronization. Queue families commonly alias
	// the same family/index on MoltenVK, so all aliases must share one wrapper and
	// therefore one submission lock.
	auto getOrCreateQueue = [this](uint32_t queueFamilyIndex) -> VulkanQueuePtr
		{
			constexpr uint32_t queueIndex = 0;
			const VulkanQueuePtr queues[] = {
				m_graphicsQueue,
				m_computeQueue,
				m_transferQueue,
				m_presentQueue
			};

			for (const VulkanQueuePtr& queue : queues)
			{
				if (queue.IsValid() &&
					queue->QueueFamilyIndex() == queueFamilyIndex &&
					queue->QueueIndex() == queueIndex)
				{
					return queue;
				}
			}

			VkQueue queue = VK_NULL_HANDLE;
			vkGetDeviceQueue(m_device, queueFamilyIndex, queueIndex, &queue);
			return VulkanQueuePtr::Make(queue, queueFamilyIndex, queueIndex,
				reinterpret_cast<PFN_vkQueueSubmit>(vkGetDeviceProcAddr(m_device, "vkQueueSubmit")));
		};

	m_graphicsQueue = getOrCreateQueue(m_queueFamilies.m_graphicsFamily.value());
	m_computeQueue = getOrCreateQueue(m_queueFamilies.m_computeFamily.value());
	m_transferQueue = getOrCreateQueue(m_queueFamilies.m_transferFamily.value());
	m_presentQueue = getOrCreateQueue(m_queueFamilies.m_presentFamily.value());
	return true;
}

void VulkanDevice::CreateSurface(const Platform::Window* viewport)
{
#if defined(_WIN32)
	VkWin32SurfaceCreateInfoKHR createInfoWin32{ VK_STRUCTURE_TYPE_WIN32_SURFACE_CREATE_INFO_KHR };
	createInfoWin32.hwnd = static_cast<HWND>(viewport->GetNativeHandle());
	createInfoWin32.hinstance = reinterpret_cast<HINSTANCE>(
		GetWindowLongPtrW(createInfoWin32.hwnd, GWLP_HINSTANCE));
	VkSurfaceKHR surface;
	VK_CHECK(vkCreateWin32SurfaceKHR(VulkanApi::GetVkInstance(), &createInfoWin32, nullptr, &surface));
#elif defined(__APPLE__)
	VkSurfaceKHR surface = VK_NULL_HANDLE;

	const auto pfnCreateMacOSSurfaceMVK = reinterpret_cast<PFN_vkCreateMacOSSurfaceMVK>(vkGetInstanceProcAddr(VulkanApi::GetVkInstance(), "vkCreateMacOSSurfaceMVK"));
	if (pfnCreateMacOSSurfaceMVK != nullptr)
	{
		VkMacOSSurfaceCreateInfoMVK createInfoMacOS{ VK_STRUCTURE_TYPE_MACOS_SURFACE_CREATE_INFO_MVK };
		createInfoMacOS.pView = Mac::GetNativeView(viewport->GetNativeHandle());
		VK_CHECK(pfnCreateMacOSSurfaceMVK(VulkanApi::GetVkInstance(), &createInfoMacOS, nullptr, &surface));
	}
	else
	{
		VkMetalSurfaceCreateInfoEXT createInfoMetal{ VK_STRUCTURE_TYPE_METAL_SURFACE_CREATE_INFO_EXT };
		createInfoMetal.pLayer = Mac::GetMetalLayer(viewport->GetNativeHandle(), viewport->IsVsyncRequested());
		VK_CHECK(vkCreateMetalSurfaceEXT(VulkanApi::GetVkInstance(), &createInfoMetal, nullptr, &surface));
	}
#else
	VkSurfaceKHR surface = VK_NULL_HANDLE;
#endif
	m_surface = VulkanSurfacePtr::Make(surface, VulkanApi::GetVkInstance());
}

VulkanStateViewportPtr VulkanDevice::CreateSwapchainViewport() const
{
	const float width = (float)m_swapchain->GetExtent().width;
	const float height = (float)m_swapchain->GetExtent().height;

//#if defined(__APPLE__)
	// MoltenVK path expects shader-space Y handling; keep Vulkan viewport non-inverted.
	return new VulkanStateViewport(0.0f, height,
                                       width, -height,
                                       { 0,0 },
                                       { (uint32_t)width, (uint32_t)height },
                                       0.0f, 1.0f);
//#else
//	return new VulkanStateViewport(0.0f, 0.0f,
//		width, height,
//		{ 0,0 },
//		{ (uint32_t)width, (uint32_t)height },
//		0.0f, 1.0f);
//#endif
}

bool VulkanDevice::CreateSwapchain(Platform::Window* pViewport)
{
	VulkanQueueFamilyIndices indices = VulkanApi::FindQueueFamilies(GetPhysicalDevice(), m_surface);
	if (!indices.m_graphicsFamily.has_value() || !indices.m_presentFamily.has_value())
	{
		SAILOR_LOG_ERROR("VulkanDevice::CreateSwapchain skipped: queue family discovery failed. graphics=%d, present=%d, width=%u, height=%u",
			(int32_t)indices.m_graphicsFamily.has_value(),
			(int32_t)indices.m_presentFamily.has_value(),
			pViewport->GetWidth(),
			pViewport->GetHeight());
		return false;
	}

	auto replacement = VulkanSwapchainPtr::Make(
		VulkanDevicePtr(this),
		pViewport->GetWidth(),
		pViewport->GetHeight(),
		pViewport->IsVsyncRequested(),
		m_swapchain);
	if (static_cast<VkSwapchainKHR>(*replacement) == VK_NULL_HANDLE || !replacement->GetDepthBufferView())
	{
		m_bIsSwapChainOutdated = true;
		return false;
	}
	m_oldSwapchain = m_swapchain;
	m_swapchain = std::move(replacement);

	pViewport->SetRenderArea(ivec2(m_swapchain->GetExtent().width, m_swapchain->GetExtent().height));
	m_pCurrentFrameViewport = CreateSwapchainViewport();

	m_bDepthBufferInitialized = false;
	return true;
}

void VulkanDevice::CleanupSwapChain()
{
	m_renderPass.Clear();
}

void VulkanDevice::WaitIdlePresentQueue()
{
	m_presentQueue->WaitIdle();
}

VkResult VulkanDevice::WaitIdle()
{
	VkResult result = VK_SUCCESS;
	for (auto* queue : { m_graphicsQueue.GetRawPtr(), m_computeQueue.GetRawPtr(),
		m_transferQueue.GetRawPtr(), m_presentQueue.GetRawPtr() })
	{
		const VkResult queueResult = queue->WaitIdle();
		if (queueResult == VK_ERROR_DEVICE_LOST) m_bIsDeviceLost = true;
		if (queueResult != VK_SUCCESS) result = queueResult;
	}
	return result;
}

bool VulkanDevice::ShouldFixLostDevice(const Platform::Window* pViewport)
{
	SAILOR_PROFILE_FUNCTION();

	if (IsSwapChainOutdated() || m_bIsDeviceLost || m_swapchain == nullptr)
	{
		return true;
	}

	const auto& support = m_swapchain->GetSwapchainSupportDetails();
	if (m_bIsSwapChainSuboptimal.exchange(false))
	{
		// SUBOPTIMAL still presents successfully. MoltenVK can keep returning it
		// for a scaled drawable even after recreation. Rebuild only when the
		// surface would actually produce a different swapchain; otherwise every
		// frame would discard temporal history and all frame-graph resources.
		const auto current = VulkanApi::QuerySwapChainSupport(m_physicalDevice, m_surface);
		if (current.m_formats.IsEmpty() || current.m_presentModes.IsEmpty())
		{
			m_bIsSwapChainOutdated = true;
			return true;
		}
		const auto extent = VulkanApi::ChooseSwapExtent(current.m_capabilities, pViewport->GetWidth(), pViewport->GetHeight());
		const auto oldFormat = VulkanApi::ChooseSwapSurfaceFormat(support.m_formats);
		const auto newFormat = VulkanApi::ChooseSwapSurfaceFormat(current.m_formats);
		if (extent.width != m_swapchain->GetExtent().width || extent.height != m_swapchain->GetExtent().height ||
			current.m_capabilities.currentTransform != support.m_capabilities.currentTransform ||
			newFormat.format != oldFormat.format || newFormat.colorSpace != oldFormat.colorSpace)
		{
			m_bIsSwapChainOutdated = true;
			return true;
		}
	}
	const auto extent = VulkanApi::ChooseSwapExtent(support.m_capabilities, pViewport->GetWidth(), pViewport->GetHeight());
	return extent.width != m_swapchain->GetExtent().width || extent.height != m_swapchain->GetExtent().height;
}

bool VulkanDevice::FixLostDevice(Platform::Window* pViewport)
{
	return !m_bIsDeviceLost && RecreateSwapchain(pViewport);
}

VulkanImageViewPtr VulkanDevice::GetBackBuffer() const
{
	return m_swapchain->GetImageViews()[m_currentSwapchainImageIndex];
}

VulkanImageViewPtr VulkanDevice::GetDepthBuffer() const
{
	return m_swapchain->GetDepthBufferView();
}

bool VulkanDevice::AcquireNextImage()
{
	uint32_t flightSlot = 0u;
	bool bHasSwapchainImage = false;
	return BeginRenderSubmission(flightSlot, bHasSwapchainImage) && bHasSwapchainImage;
}

uint32_t VulkanDevice::GetMaxFramesInFlight() const
{
	return m_maxFramesInFlight;
}

bool VulkanDevice::BeginRenderSubmission(uint32_t& outFlightSlot, bool& outHasSwapchainImage)
{
	outFlightSlot = static_cast<uint32_t>(m_currentFrame);
	outHasSwapchainImage = false;

	if (m_bIsDeviceLost || m_frameSubmissionError != VK_SUCCESS ||
		m_currentFrame >= m_syncFences.Num() || !m_syncFences[m_currentFrame])
	{
		return false;
	}

	// The slot fence is the ownership boundary for every mutable resource retained
	// by this flight. Always wait it, including the editor/no-present path and an
	// outdated swapchain, before FrameGraph preparation can reuse the slot.
	const VkResult previousFrameResult = m_syncFences[m_currentFrame]->Wait();
	if (previousFrameResult != VK_SUCCESS)
	{
		if (previousFrameResult == VK_ERROR_DEVICE_LOST)
		{
			m_bIsDeviceLost = true;
		}
		return false;
	}

	// Wait while we recreate swapchain from main thread to sync with Win32Api.
	// The flight slot is still safely acquired even though there is no image.
	if (m_bIsSwapChainOutdated)
	{
		return true;
	}

	VkResult result = m_swapchain->AcquireNextImage(UINT64_MAX, m_imageAvailableSemaphores[m_currentFrame], VulkanFencePtr(), m_currentSwapchainImageIndex);

	if (result == VK_ERROR_OUT_OF_DATE_KHR)
	{
		m_bIsSwapChainOutdated = true;
		return true;
	}
	else if (result != VK_SUCCESS && result != VK_SUBOPTIMAL_KHR)
	{
		if (result == VK_ERROR_DEVICE_LOST) m_bIsDeviceLost = true;
		SAILOR_LOG("Failed to acquire swap chain image!");
		return false;
	}
	if (result == VK_SUBOPTIMAL_KHR) m_bIsSwapChainSuboptimal = true;
	m_acquiredImageFlight = static_cast<uint32_t>(m_currentFrame);

	// Check if a previous frame is using this image (i.e. there is its fence to wait on)
	if (m_syncImages[m_currentSwapchainImageIndex] && *m_syncImages[m_currentSwapchainImageIndex] != VK_NULL_HANDLE)
	{
		// Wait while previous frame frees the image
		const VkResult imageResult = m_syncImages[m_currentSwapchainImageIndex]->Wait();
		if (imageResult != VK_SUCCESS)
		{
			m_frameSubmissionError = imageResult;
			m_bIsSwapChainOutdated = true;
			if (imageResult == VK_ERROR_DEVICE_LOST) m_bIsDeviceLost = true;
			return false;
		}
	}
	// Mark the image as now being in use by this frame
	m_syncImages[m_currentSwapchainImageIndex] = m_syncFences[m_currentFrame];

	outHasSwapchainImage = true;
	return true;
}

void VulkanDevice::PrepareFrameCommands(const TVector<VulkanCommandBufferPtr>& primaryCommandBuffers,
	bool hasSwapchainImage, TVector<VkCommandBuffer>& outCommands)
{
	m_frameDeps[m_currentFrame].Clear(false);
	const bool initializeImage = hasSwapchainImage && !m_swapchainImagesInitialized[m_currentSwapchainImageIndex];
	if (initializeImage || !m_bDepthBufferInitialized)
	{
		VulkanCommandBufferPtr transitCmd = CreateCommandBuffer(RHI::ECommandListQueue::Graphics);
		SetDebugName(VK_OBJECT_TYPE_COMMAND_BUFFER, (uint64_t)(VkCommandBuffer)*transitCmd, "Initialize swapchain attachments"_h);
		transitCmd->BeginCommandList();
		if (initializeImage)
		{
			const auto image = GetBackBuffer();
			transitCmd->ImageMemoryBarrier(image, image->m_format, VK_IMAGE_LAYOUT_UNDEFINED, image->GetImage()->m_defaultLayout);
		}
		if (!m_bDepthBufferInitialized)
		{
			const auto depth = GetDepthBuffer();
			transitCmd->ImageMemoryBarrier(depth, depth->m_format, VK_IMAGE_LAYOUT_UNDEFINED, depth->GetImage()->m_defaultLayout);
		}
		transitCmd->EndCommandList();
		outCommands.Add(*transitCmd);
		m_frameDeps[m_currentFrame].Add(transitCmd);
	}

	for (const auto& command : primaryCommandBuffers)
	{
		outCommands.Add(*command);
		m_frameDeps[m_currentFrame].Add(command);
	}
}

bool VulkanDevice::SubmitFrame(const VkSubmitInfo& submitInfo)
{
	m_frameSubmissionError = m_syncFences[m_currentFrame]->Reset();
	if (m_frameSubmissionError == VK_SUCCESS)
	{
		m_frameSubmissionError = m_graphicsQueue->Submit(submitInfo, m_syncFences[m_currentFrame]);
	}
	m_bLastFrameSubmitSuccessful = m_frameSubmissionError == VK_SUCCESS;
	if (m_bLastFrameSubmitSuccessful)
	{
		m_numSubmittedCommandBuffersAcc.fetch_add(submitInfo.commandBufferCount, std::memory_order_relaxed);
		m_bDepthBufferInitialized = true;
		m_currentFrame = (m_currentFrame + 1) % m_maxFramesInFlight;
	}
	else
	{
		// No later frame can signal this slot's fence. Keep its dependencies until
		// recovery drains the queues and replaces the complete synchronization set.
		m_bIsSwapChainOutdated = true;
		if (m_frameSubmissionError == VK_ERROR_DEVICE_LOST) m_bIsDeviceLost = true;
		SAILOR_LOG_ERROR("Vulkan frame submission failed: %d", static_cast<int>(m_frameSubmissionError));
	}
	// RHI uploads can finish while Render publishes this frame's statistics.
	m_numSubmittedCommandBuffers = m_numSubmittedCommandBuffersAcc.exchange(0u, std::memory_order_relaxed);
	return m_bLastFrameSubmitSuccessful;
}

bool VulkanDevice::ConsumeAcquiredImage()
{
	if (!m_acquiredImageFlight) return true;

	// A refused frame did not consume the acquire semaphore. Queue its wait
	// before WaitIdle so recovery also drains the outstanding WSI acquisition.
	VkSemaphore semaphore = *m_imageAvailableSemaphores[*m_acquiredImageFlight];
	VkPipelineStageFlags stage = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
	VkSubmitInfo submitInfo{ VK_STRUCTURE_TYPE_SUBMIT_INFO };
	submitInfo.waitSemaphoreCount = 1;
	submitInfo.pWaitSemaphores = &semaphore;
	submitInfo.pWaitDstStageMask = &stage;
	const VkResult result = m_graphicsQueue->Submit(submitInfo);
	if (result != VK_SUCCESS)
	{
		m_frameSubmissionError = result;
		if (result == VK_ERROR_DEVICE_LOST) m_bIsDeviceLost = true;
		return false;
	}
	m_acquiredImageFlight.reset();
	return true;
}

bool VulkanDevice::PresentFrame(const FrameState& state, const TVector<VulkanCommandBufferPtr>& primaryCommandBuffers, const TVector<VulkanSemaphorePtr>& semaphoresToWait)
{
	m_bLastFrameSubmitSuccessful = false;
	if (m_bIsDeviceLost || m_frameSubmissionError != VK_SUCCESS) return false;

	if (!m_pCurrentFrameViewport ||
		(m_pCurrentFrameViewport->GetViewport().width != m_swapchain->GetExtent().width ||
			abs(m_pCurrentFrameViewport->GetViewport().height) != m_swapchain->GetExtent().height))
	{
		m_pCurrentFrameViewport = CreateSwapchainViewport();
	}

	TVector<VkCommandBuffer> commandBuffers;
	PrepareFrameCommands(primaryCommandBuffers, true, commandBuffers);

	TVector<VkSemaphore> waitSemaphores;
	if (semaphoresToWait.Num() > 0)
	{
		waitSemaphores.Reserve(semaphoresToWait.Num() + 1);
		for (const auto& semaphore : semaphoresToWait)
		{
			waitSemaphores.Add(*semaphore);
			m_frameDeps[m_currentFrame].Add(semaphore);
		}
	}

	waitSemaphores.Add(*m_imageAvailableSemaphores[m_currentFrame]);

	///////////////////////////////////////////////////
	VkSubmitInfo submitInfo{};
	submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;

	submitInfo.waitSemaphoreCount = static_cast<uint32_t>(waitSemaphores.Num());
	submitInfo.pWaitSemaphores = &waitSemaphores[0];

	VkPipelineStageFlags* waitStages = reinterpret_cast<VkPipelineStageFlags*>(_malloca(waitSemaphores.Num() * sizeof(VkPipelineStageFlags)));

	for (uint32_t i = 0; i < semaphoresToWait.Num(); i++)
	{
		waitStages[i] = semaphoresToWait[i]->PipelineStageFlags();
	}
	// The first use can be a layout transition or transfer, not just color output.
	waitStages[waitSemaphores.Num() - 1] = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;

	submitInfo.pWaitDstStageMask = waitStages;

	// A zero-command submit is intentional: it still consumes the acquired-image
	// and frame-chain semaphores before they are reused.
	submitInfo.commandBufferCount = static_cast<uint32_t>(commandBuffers.Num());
	submitInfo.pCommandBuffers = commandBuffers.Num() > 0 ? &commandBuffers[0] : nullptr;

	VkSemaphore signalSemaphores[] = { *m_renderFinishedSemaphores[m_currentSwapchainImageIndex] };
	submitInfo.signalSemaphoreCount = 1;
	submitInfo.pSignalSemaphores = signalSemaphores;

	const bool submitted = SubmitFrame(submitInfo);
	_freea(waitStages);
	if (!submitted) return false;
	m_acquiredImageFlight.reset();
	m_swapchainImagesInitialized[m_currentSwapchainImageIndex] = true;

	VkPresentInfoKHR presentInfo{};
	presentInfo.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
	presentInfo.waitSemaphoreCount = 1;
	presentInfo.pWaitSemaphores = signalSemaphores;
	VkSwapchainKHR swapChains[] = { *m_swapchain };
	presentInfo.swapchainCount = 1;
	presentInfo.pSwapchains = swapChains;
	presentInfo.pImageIndices = &m_currentSwapchainImageIndex;
	const VkResult presentResult = m_presentQueue->Present(presentInfo);

	if (presentResult == VK_ERROR_OUT_OF_DATE_KHR)
	{
		m_bIsSwapChainOutdated = true;
	}
	else if (presentResult == VK_SUBOPTIMAL_KHR)
	{
		m_bIsSwapChainSuboptimal = true;
	}
	else if (presentResult != VK_SUCCESS)
	{
		m_bIsSwapChainOutdated = true;
		m_frameSubmissionError = presentResult;
		if (presentResult == VK_ERROR_DEVICE_LOST) m_bIsDeviceLost = true;
		SAILOR_LOG_ERROR("Failed to present swap chain image: %d", static_cast<int>(presentResult));
		return false;
	}

	return presentResult == VK_SUCCESS || presentResult == VK_SUBOPTIMAL_KHR;
}

bool VulkanDevice::SubmitFrameWithoutPresent(const TVector<VulkanCommandBufferPtr>& primaryCommandBuffers, const TVector<VulkanSemaphorePtr>& semaphoresToWait)
{
	m_bLastFrameSubmitSuccessful = false;
	if (m_bIsDeviceLost || m_frameSubmissionError != VK_SUCCESS) return false;

	// AcquireNextImage normally waits this fence before a frame slot is reused.
	// When there is no swapchain image, preserve the same invariant before
	// releasing the slot dependencies or resetting its fence.
	const VkResult previousFrameResult = m_syncFences[m_currentFrame]->Wait();
	if (previousFrameResult != VK_SUCCESS)
	{
		if (previousFrameResult == VK_ERROR_DEVICE_LOST)
		{
			m_bIsDeviceLost = true;
		}
		return false;
	}

	TVector<VkCommandBuffer> commandBuffers;
	PrepareFrameCommands(primaryCommandBuffers, false, commandBuffers);

	TVector<VkSemaphore> waitSemaphores;
	if (semaphoresToWait.Num() > 0)
	{
		waitSemaphores.Reserve(semaphoresToWait.Num());
		for (const auto& semaphore : semaphoresToWait)
		{
			waitSemaphores.Add(*semaphore);
			m_frameDeps[m_currentFrame].Add(semaphore);
		}
	}

	VkSubmitInfo submitInfo{};
	submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
	submitInfo.waitSemaphoreCount = static_cast<uint32_t>(waitSemaphores.Num());
	submitInfo.pWaitSemaphores = waitSemaphores.Num() > 0 ? &waitSemaphores[0] : nullptr;

	VkPipelineStageFlags* waitStages = waitSemaphores.Num() > 0 ?
		reinterpret_cast<VkPipelineStageFlags*>(_malloca(waitSemaphores.Num() * sizeof(VkPipelineStageFlags))) : nullptr;
	for (uint32_t i = 0; i < waitSemaphores.Num(); i++)
	{
		waitStages[i] = semaphoresToWait[i]->PipelineStageFlags();
	}

	submitInfo.pWaitDstStageMask = waitStages;
	submitInfo.commandBufferCount = static_cast<uint32_t>(commandBuffers.Num());
	submitInfo.pCommandBuffers = commandBuffers.Num() > 0 ? &commandBuffers[0] : nullptr;
	submitInfo.signalSemaphoreCount = 0;
	submitInfo.pSignalSemaphores = nullptr;

	const bool submitted = SubmitFrame(submitInfo);

	if (waitStages)
	{
		_freea(waitStages);
	}

	return submitted;
}

void VulkanDevice::GetOccupiedVideoMemory(VkMemoryHeapFlags memFlags, size_t& outHeapBudget, size_t& outHeapUsage)
{
	VkPhysicalDeviceMemoryBudgetPropertiesEXT memBudget;
	memBudget.sType = VkStructureType::VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MEMORY_BUDGET_PROPERTIES_EXT;
	memBudget.pNext = nullptr;

	VkPhysicalDeviceMemoryProperties2 memProps = { };
	memProps.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MEMORY_PROPERTIES_2;
	memProps.pNext = &memBudget;

	vkGetPhysicalDeviceMemoryProperties2(m_physicalDevice, &memProps);

	outHeapBudget = 0;
	outHeapUsage = 0;

	for (uint32_t i = 0; i < 16; i++)
	{
		const auto flags = memProps.memoryProperties.memoryHeaps[i].flags;

		if (flags & memFlags)
		{
			outHeapBudget += memBudget.heapBudget[i];
			outHeapUsage += memBudget.heapUsage[i];
		}
	}
}
