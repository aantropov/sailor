struct IUnknown; // Workaround for "combaseapi.h(229): error C2187: syntax error: 'identifier' was unexpected here" when using /permissive-

#include <chrono>
#include <set>
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
#include "Tasks/Scheduler.h"
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

#include "Components/TestComponent.h"
#include "Components/MeshRendererComponent.h"
#include "Engine/Gameobject.h"
#include "Engine/World.h"
#include "Engine/EngineLoop.h"

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

VulkanDevice::VulkanDevice(Window* pViewport, RHI::EMsaaSamples requestMsaa)
{
	// Create Win32 surface
	CreateWin32Surface(pViewport);

	// Pick & Create device
	m_physicalDevice = VulkanApi::PickPhysicalDevice(m_surface);

	// Calculate max anisotropy
	vkGetPhysicalDeviceProperties(m_physicalDevice, &m_physicalDeviceProperties);
	m_maxAllowedMsaaSamples = CalculateMaxAllowedMSAASamples(m_physicalDeviceProperties.limits.framebufferColorSampleCounts & m_physicalDeviceProperties.limits.framebufferDepthSampleCounts);
	m_currentMsaaSamples = (VkSampleCountFlagBits)(std::min((uint8_t)requestMsaa, (uint8_t)m_maxAllowedMsaaSamples));
	m_bSupportsMultiDrawIndirect = m_physicalDeviceProperties.limits.maxDrawIndirectCount > 1;

	CreateLogicalDevice(m_physicalDevice);

	SAILOR_LOG("maxDescriptorSetSampledImages = %d", (int32_t)m_physicalDeviceProperties.limits.maxDescriptorSetSampledImages);
	SAILOR_LOG("maxSamplerAnisotropy = %.2f", m_physicalDeviceProperties.limits.maxSamplerAnisotropy);
	SAILOR_LOG("bufferImageGranularity = %d", (int32_t)m_physicalDeviceProperties.limits.bufferImageGranularity);
	SAILOR_LOG("m_maxAllowedMSAASamples = %d, requestedMSAASamples = %d", m_maxAllowedMsaaSamples, m_currentMsaaSamples);
	SAILOR_LOG("m_bSupportsMultiDrawIndirect = %d", (int32_t)m_bSupportsMultiDrawIndirect);

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
	CreateSwapchain(pViewport);

	// Create graphics
	CreateDefaultRenderPass();
	CreateFrameDependencies();
	CreateFrameSyncSemaphores();

	m_bIsSwapChainOutdated = false;

	// Get dynamic rendering extension
	pVkCmdBeginRenderingKHR = (PFN_vkCmdBeginRenderingKHR)vkGetDeviceProcAddr(m_device, "vkCmdBeginRenderingKHR");
	pVkCmdEndRenderingKHR = (PFN_vkCmdEndRenderingKHR)vkGetDeviceProcAddr(m_device, "vkCmdEndRenderingKHR");
}

void VulkanDevice::vkCmdBeginRenderingKHR(VkCommandBuffer commandBuffer, const VkRenderingInfo* pRenderingInfo)
{
	pVkCmdBeginRenderingKHR(commandBuffer, pRenderingInfo);
}

void VulkanDevice::vkCmdEndRenderingKHR(VkCommandBuffer commandBuffer)
{
	pVkCmdEndRenderingKHR(commandBuffer);
}

VulkanDevice::~VulkanDevice()
{
	vkDestroyDevice(m_device, nullptr);
}

void VulkanDevice::BeginConditionalDestroy()
{
	//Clear dependencies
	WaitIdle();

	CleanupSwapChain();

	m_oldSwapchain.Clear();
	m_swapchain.Clear();
	m_commandPool.Clear();

	// 1 Main, 1 Render, 2 RHI
	check(m_threadContext.Num() <= 4);

	for (auto& pair : m_threadContext)
	{
		pair.m_second.Clear();
	}

	m_renderFinishedSemaphores.Clear();
	m_imageAvailableSemaphores.Clear();
	m_syncImages.Clear();
	m_syncFences.Clear();

	m_frameDeps.Clear();

	m_samplers.Clear();
	m_pipelineBuilder.Clear();
	m_surface.Clear();
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
	auto& res = m_threadContext.At_Lock(threadId);
	if (!res)
	{
		res = CreateThreadContext();

#ifndef _SHIPPING
		VkDescriptorPool pool = *res->m_descriptorPool;
		SetDebugName(VkObjectType::VK_OBJECT_TYPE_DESCRIPTOR_POOL, (uint64_t)pool, Utils::GetCurrentThreadName());
#endif 
		// We don't want to create ThreadContext for each thread, only for Main, RHI and Render
		check(m_threadContext.Num() <= App::GetSubmodule<Tasks::Scheduler>()->GetNumRHIThreads() + 2);
	}
	m_threadContext.Unlock(threadId);

	return *res;
}

ThreadContext& VulkanDevice::GetCurrentThreadContext()
{
	return GetOrAddThreadContext(GetCurrentThreadId());
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

TBlockAllocator<class GlobalVulkanMemoryAllocator, class VulkanMemoryPtr>& VulkanDevice::GetMemoryAllocator(VkMemoryPropertyFlags properties, VkMemoryRequirements requirements)
{
	uint64_t hash{};
	HashCombine(hash, properties, requirements.memoryTypeBits);

	auto& pAllocator = m_memoryAllocators.At_Lock(hash);

	if (!pAllocator)
	{
		const size_t AverageElementSize = 512 * 512 * 16;
		const size_t BlockSize = 32 * AverageElementSize;
		const size_t ReservedSize = 64 * AverageElementSize;

		pAllocator = TUniquePtr<VulkanDeviceMemoryAllocator>::Make(BlockSize, AverageElementSize, ReservedSize);
	}

	auto& vulkanAllocator = pAllocator->GetGlobalAllocator();
	vulkanAllocator.SetMemoryProperties(properties);
	vulkanAllocator.SetMemoryRequirements(requirements);

	m_memoryAllocators.Unlock(hash);

	return *pAllocator;
}

void VulkanDevice::vkCmdDebugMarkerBegin(VulkanCommandBufferPtr cmdBuffer, const VkDebugMarkerMarkerInfoEXT* markerInfo)
{
#ifndef _SHIPPING
	m_pCmdDebugMarkerBegin(*cmdBuffer, markerInfo);
#endif
}

void VulkanDevice::vkCmdDebugMarkerEnd(VulkanCommandBufferPtr cmdBuffer)
{
#ifndef _SHIPPING
	m_pCmdDebugMarkerEnd(*cmdBuffer);
#endif
}

void VulkanDevice::SetDebugName(VkObjectType type, uint64_t objectHandle, const std::string& name)
{
#ifndef _SHIPPING
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

	VkResult submitResult = VK_SUCCESS;

	if (commandBuffer->GetCommandPool()->GetQueueFamilyIndex() == m_queueFamilies.m_computeFamily.value_or(-1))
	{
		submitResult = m_computeQueue->Submit(submitInfo, fence);
	}
	else if (commandBuffer->GetCommandPool()->GetQueueFamilyIndex() == m_queueFamilies.m_transferFamily.value_or(-1))
	{
		submitResult = m_transferQueue->Submit(submitInfo, fence);
	}
	else
	{
		submitResult = m_graphicsQueue->Submit(submitInfo, fence);
	}

	_freea(waits);
	_freea(signals);
	_freea(waitStages);

	m_numSubmittedCommandBuffersAcc++;

	if (submitResult == VK_ERROR_DEVICE_LOST)
	{
		m_bIsDeviceLost = true;
		return false;
	}

	VK_CHECK(submitResult);

	return submitResult == VK_SUCCESS;
}

void VulkanDevice::CreateDefaultRenderPass()
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
	context->m_computeCommandPool = VulkanCommandPoolPtr::Make(VulkanDevicePtr(this), queueFamilyIndices.m_computeFamily.value(), VK_COMMAND_POOL_CREATE_TRANSIENT_BIT);

	auto descriptorSizes = TVector
	{
		VulkanApi::CreateDescriptorPoolSize(VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 32),
		VulkanApi::CreateDescriptorPoolSize(VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 64)
	};

	context->m_descriptorPool = VulkanDescriptorPoolPtr::Make(VulkanDevicePtr(this), 8096, descriptorSizes);

	context->m_stagingBufferAllocator = TSharedPtr<VulkanBufferAllocator>::Make(1024 * 1024, 1024 * 512, 2 * 1024 * 1024);
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

bool VulkanDevice::RecreateSwapchain(Window* pViewport)
{
	if (pViewport->GetWidth() == 0 || pViewport->GetHeight() == 0)
	{
		return false;
	}

	WaitIdle();

	CleanupSwapChain();

	CreateSwapchain(pViewport);
	CreateDefaultRenderPass();

	m_frameDeps.Clear();

	CreateFrameDependencies();

	m_bIsSwapChainOutdated = false;

	assert(m_swapchain);

	const auto& depthView = m_swapchain->GetDepthBufferView();
	const auto& name = "RecreateSwapchain: Initialize DepthStencil RenderTarget";

	RHICommandListPtr cmdList = Sailor::RHI::Renderer::GetDriver()->CreateCommandList(false, RHI::ECommandListQueue::Graphics);

	Renderer::GetDriver()->SetDebugName(cmdList, name);
	Renderer::GetDriverCommands()->BeginCommandList(cmdList, true);

	for (const auto& sc : m_swapchain->GetImageViews())
	{
		cmdList->m_vulkan.m_commandBuffer->ImageMemoryBarrier(sc, sc->m_format, VK_IMAGE_LAYOUT_UNDEFINED, sc->GetImage()->m_defaultLayout);
	}

	assert(depthView);
	cmdList->m_vulkan.m_commandBuffer->ImageMemoryBarrier(depthView, depthView->m_format, VK_IMAGE_LAYOUT_UNDEFINED, depthView->GetImage()->m_defaultLayout);

	Renderer::GetDriverCommands()->EndCommandList(cmdList);

	App::GetSubmodule<Tasks::Scheduler>()->Run(Sailor::Tasks::CreateTask(name, [cmdList = std::move(cmdList)]()
		{
			Renderer::GetDriver()->SubmitCommandList(cmdList, Sailor::RHI::RHIFencePtr::Make());
		}, Sailor::Tasks::EThreadType::Render));

	return true;
}

template<typename T>
void AddFeature(TVector<TVector<uint8_t>>& bytes, typename TFunction<void, T&>::type init)
{
	T newFeature{};
	newFeature.pNext = nullptr;

	init(newFeature);

	TVector<uint8_t> byteRepresentation(reinterpret_cast<uint8_t*>(&newFeature), sizeof(T));
	bytes.Emplace(std::move(byteRepresentation));

	if (bytes.Num() > 1)
	{
		// Link previous
		((T*)(bytes[bytes.Num() - 2].GetData()))->pNext = bytes[bytes.Num() - 1].GetData();
	}
}


void VulkanDevice::CreateFrameDependencies()
{
	const TVector<VulkanImageViewPtr>& swapChainImageViews = m_swapchain->GetImageViews();

	if (m_frameDeps.Num() < swapChainImageViews.Num())
	{
		m_frameDeps.AddDefault(swapChainImageViews.Num() - m_frameDeps.Num());
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

	TVector<const char*> deviceExtensions;
	TVector<const char*> instanceExtensions;
	VulkanApi::GetRequiredExtensions(deviceExtensions, instanceExtensions);

#ifndef _SHIPPING
	deviceExtensions.Add(VK_EXT_DEBUG_MARKER_EXTENSION_NAME);
#endif

	TVector<TVector<uint8_t>> features;

	/*
	AddFeature<VkPhysicalDeviceDynamicRenderingUnusedAttachmentsFeaturesEXT>(features, [](auto& unusedAttachments)
		{
			unusedAttachments.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DYNAMIC_RENDERING_UNUSED_ATTACHMENTS_FEATURES_EXT;
			unusedAttachments.dynamicRenderingUnusedAttachments = VK_TRUE;
		});
	*/
	// We are using multiply blending
	AddFeature<VkPhysicalDeviceBlendOperationAdvancedFeaturesEXT>(features, [](auto& features)
		{
			features.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_BLEND_OPERATION_ADVANCED_FEATURES_EXT;
			features.advancedBlendCoherentOperations = VK_TRUE;
		});

	// Create device that supports VK_EXT_shader_atomic_float (GL_EXT_shader_atomic_float)
	// this allows to perform atomic operations on storage buffers
	AddFeature<VkPhysicalDeviceShaderAtomicFloatFeaturesEXT>(features, [](auto& floatFeatures)
		{
			floatFeatures.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_ATOMIC_FLOAT_FEATURES_EXT;
			floatFeatures.shaderBufferFloat32AtomicAdd = true;
		});

	AddFeature<VkPhysicalDeviceFeatures2 >(features, [&](auto& physicalDeviceFeatures2)
		{
			physicalDeviceFeatures2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
			vkGetPhysicalDeviceFeatures2(m_physicalDevice, &physicalDeviceFeatures2);
			physicalDeviceFeatures2.features.sampleRateShading = VK_FALSE;
			physicalDeviceFeatures2.features.multiDrawIndirect = VK_TRUE;

#ifdef SAILOR_VULKAN_MSAA_IMPACTS_TEXTURE_SAMPLING
			physicalDeviceFeatures2.features.sampleRateShading = VK_TRUE;
#endif
		});

	AddFeature<VkPhysicalDeviceVulkan13Features>(features, [&](auto& core13)
		{
			core13.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES;
			core13.maintenance4 = true;
			core13.dynamicRendering = true;
			core13.synchronization2 = true;
		});

	AddFeature<VkPhysicalDeviceVulkan11Features>(features, [&](auto& core11)
		{
			core11.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES;
			core11.shaderDrawParameters = true;
		});

	AddFeature<VkPhysicalDeviceVulkan12Features>(features, [&](auto& core12)
		{
			core12.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES;
			core12.samplerFilterMinmax = VK_TRUE;
			core12.descriptorBindingPartiallyBound = VK_TRUE;
			core12.descriptorBindingSampledImageUpdateAfterBind = VK_TRUE;
			core12.descriptorBindingStorageBufferUpdateAfterBind = VK_TRUE;
			core12.descriptorBindingUniformBufferUpdateAfterBind = VK_TRUE;
			core12.descriptorBindingStorageImageUpdateAfterBind = VK_TRUE;
			core12.descriptorBindingVariableDescriptorCount = VK_TRUE;
			core12.descriptorIndexing = VK_TRUE;
			core12.descriptorBindingUpdateUnusedWhilePending = VK_TRUE;
		});

	VkDeviceCreateInfo createInfo{ VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO };
	createInfo.enabledExtensionCount = static_cast<uint32_t>(deviceExtensions.Num());
	createInfo.ppEnabledExtensionNames = deviceExtensions.GetData();
	createInfo.queueCreateInfoCount = static_cast<uint32_t>(queueCreateInfos.Num());
	createInfo.pQueueCreateInfos = queueCreateInfos.GetData();
	createInfo.pEnabledFeatures = VK_NULL_HANDLE;

	createInfo.pNext = features[0].GetData();

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

	VkQueue computeQueue;
	vkGetDeviceQueue(m_device, m_queueFamilies.m_computeFamily.value(), 0, &computeQueue);
	m_computeQueue = VulkanQueuePtr::Make(computeQueue, m_queueFamilies.m_computeFamily.value(), 0);
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

void VulkanDevice::CreateSwapchain(Window* pViewport)
{
	m_oldSwapchain = m_swapchain;

	m_swapchain = VulkanSwapchainPtr::Make(
		VulkanDevicePtr(this),
		pViewport->GetWidth(),
		pViewport->GetHeight(),
		pViewport->IsVsyncRequested(),
		m_oldSwapchain);

	const float height = (float)m_swapchain->GetExtent().height;
	const float width = (float)m_swapchain->GetExtent().width;

	pViewport->SetRenderArea(ivec2(m_swapchain->GetExtent().width, m_swapchain->GetExtent().height));

	// Flip Y-axis to point up in viewport
	// https://www.saschawillems.de/blog/2019/03/29/flipping-the-vulkan-viewport/
	//m_viewport.y = height;
	//m_viewport.height = -height;

	m_pCurrentFrameViewport = new VulkanStateViewport(0.0f, height,
		width, -height,
		{ 0,0 },
		{ (uint32_t)width, (uint32_t)height },
		0.0f, 1.0f);

	m_bNeedToTransitSwapchainToPresent = true;
}

void VulkanDevice::CleanupSwapChain()
{
	m_renderPass.Clear();
}

void VulkanDevice::WaitIdlePresentQueue()
{
	m_presentQueue->WaitIdle();
}

void VulkanDevice::WaitIdle()
{
	m_graphicsQueue->WaitIdle();
	m_computeQueue->WaitIdle();
	m_transferQueue->WaitIdle();

	vkDeviceWaitIdle(m_device);
}

bool VulkanDevice::ShouldFixLostDevice(const Win32::Window* pViewport)
{
	SAILOR_PROFILE_FUNCTION();

	if (IsSwapChainOutdated() || m_bIsDeviceLost || m_swapchain == nullptr)
	{
		return true;
	}

	const auto& m_swapChainSupportDetails = m_swapchain->GetSwapchainSupportDetails();

	const VkExtent2D swapchainExtent = VulkanApi::ChooseSwapExtent(m_swapChainSupportDetails.m_capabilities, pViewport->GetWidth(), pViewport->GetHeight());
	return m_swapchain && (swapchainExtent.width != m_swapchain->GetExtent().width || swapchainExtent.height != m_swapchain->GetExtent().height);
}

void VulkanDevice::FixLostDevice(Win32::Window* pViewport)
{
	RecreateSwapchain(pViewport);
	m_bIsDeviceLost = false;
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
	// Wait while we recreate swapchain from main thread to sync with Win32Api
	if (m_bIsSwapChainOutdated)
	{
		return false;
	}

	// Wait while GPU is finishing frame
	m_syncFences[m_currentFrame]->Wait();

	VkResult result = m_swapchain->AcquireNextImage(UINT64_MAX, m_imageAvailableSemaphores[m_currentFrame], VulkanFencePtr(), m_currentSwapchainImageIndex);

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
	if (m_syncImages[m_currentSwapchainImageIndex] && *m_syncImages[m_currentSwapchainImageIndex] != VK_NULL_HANDLE)
	{
		// Wait while previous frame frees the image
		m_syncImages[m_currentSwapchainImageIndex]->Wait();
	}
	// Mark the image as now being in use by this frame
	m_syncImages[m_currentSwapchainImageIndex] = m_syncFences[m_currentFrame];

	return true;
}

bool VulkanDevice::PresentFrame(const FrameState& state, const TVector<VulkanCommandBufferPtr>& primaryCommandBuffers, const TVector<VulkanSemaphorePtr>& semaphoresToWait)
{
	//////////////////////////////////////////////////
	if (!m_pCurrentFrameViewport &&
		m_pCurrentFrameViewport->GetViewport().width != m_swapchain->GetExtent().width ||
		abs(m_pCurrentFrameViewport->GetViewport().height) != m_swapchain->GetExtent().height)
	{
		const float width = (float)m_swapchain->GetExtent().width;
		const float height = (float)m_swapchain->GetExtent().height;

		m_pCurrentFrameViewport = new VulkanStateViewport(0.0f, height,
			width, -height,
			{ 0,0 },
			{ (uint32_t)width, (uint32_t)height },
			0.0f, 1.0f);
	}

	// Clear previous frame deps
	m_frameDeps[m_currentFrame].Clear();

	TVector<VkCommandBuffer> commandBuffers;

	if (m_bNeedToTransitSwapchainToPresent)
	{
		VulkanCommandBufferPtr transitCmd = CreateCommandBuffer(RHI::ECommandListQueue::Graphics);

		SetDebugName(VkObjectType::VK_OBJECT_TYPE_COMMAND_BUFFER, (uint64_t)(VkCommandBuffer)*transitCmd, "VulkanDevice::PresentFrame::TransitSwapchainToPresent");

		transitCmd->BeginCommandList();
		for (const auto& swapchain : m_swapchain->GetImageViews())
		{
			transitCmd->ImageMemoryBarrier(swapchain, swapchain->m_format, VK_IMAGE_LAYOUT_UNDEFINED, swapchain->GetImage()->m_defaultLayout);
		}

		transitCmd->ImageMemoryBarrier(m_swapchain->GetDepthBufferView(),
			m_swapchain->GetDepthBufferView()->m_format, VK_IMAGE_LAYOUT_UNDEFINED,
			m_swapchain->GetDepthBufferView()->GetImage()->m_defaultLayout);

		transitCmd->EndCommandList();

		commandBuffers.Add(*transitCmd);
		m_frameDeps[m_currentFrame].Add(transitCmd);
		m_bNeedToTransitSwapchainToPresent = false;
	}

	if (primaryCommandBuffers.Num() > 0)
	{
		for (const auto& cmdBuffer : primaryCommandBuffers)
		{
			commandBuffers.Add(*cmdBuffer);
			m_frameDeps[m_currentFrame].Add(cmdBuffer);
		}
	}

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
	VkResult presentResult = VK_SUCCESS;
	VkResult submitResult = VK_SUCCESS;
	if (commandBuffers.Num() > 0)
	{
		VkSubmitInfo submitInfo{};
		submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;

		submitInfo.waitSemaphoreCount = static_cast<uint32_t>(waitSemaphores.Num());
		submitInfo.pWaitSemaphores = &waitSemaphores[0];

		VkPipelineStageFlags* waitStages = reinterpret_cast<VkPipelineStageFlags*>(_malloca(waitSemaphores.Num() * sizeof(VkPipelineStageFlags)));

		for (uint32_t i = 0; i < waitSemaphores.Num(); i++)
		{
			waitStages[i] = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
		}

		submitInfo.pWaitDstStageMask = waitStages;

		submitInfo.commandBufferCount = static_cast<uint32_t>(commandBuffers.Num());
		submitInfo.pCommandBuffers = &commandBuffers[0];

		VkSemaphore signalSemaphores[] = { *m_renderFinishedSemaphores[m_currentFrame] };
		submitInfo.signalSemaphoreCount = 1;
		submitInfo.pSignalSemaphores = signalSemaphores;

		m_syncFences[m_currentFrame]->Reset();

		//TODO: Transfer queue for transfer family command lists
		submitResult = m_graphicsQueue->Submit(submitInfo, m_syncFences[m_currentFrame]);

		m_numSubmittedCommandBuffersAcc += (uint32_t)commandBuffers.Num();

		_freea(waitStages);

		VkPresentInfoKHR presentInfo{};
		presentInfo.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;

		presentInfo.waitSemaphoreCount = 1;
		presentInfo.pWaitSemaphores = signalSemaphores;

		VkSwapchainKHR swapChains[] = { *m_swapchain };
		presentInfo.swapchainCount = 1;
		presentInfo.pSwapchains = swapChains;
		presentInfo.pImageIndices = &m_currentSwapchainImageIndex;
		presentInfo.pResults = nullptr; // Optional

		presentResult = m_presentQueue->Present(presentInfo);
	}
	else
	{
		VkPresentInfoKHR presentInfo{};
		presentInfo.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;

		presentInfo.waitSemaphoreCount = 0;
		VkSwapchainKHR swapChains[] = { *m_swapchain };
		presentInfo.swapchainCount = 1;
		presentInfo.pSwapchains = swapChains;
		presentInfo.pImageIndices = &m_currentSwapchainImageIndex;
		presentInfo.pResults = nullptr; // Optional

		presentResult = m_presentQueue->Present(presentInfo);
	}

	m_currentFrame = (m_currentFrame + 1) % VulkanApi::MaxFramesInFlight;

	m_numSubmittedCommandBuffers = m_numSubmittedCommandBuffersAcc;
	m_numSubmittedCommandBuffersAcc = 0;

	if (submitResult == VK_ERROR_DEVICE_LOST)
	{
		m_bIsDeviceLost = true;
	}

	if (presentResult == VK_ERROR_OUT_OF_DATE_KHR || presentResult == VK_SUBOPTIMAL_KHR)
	{
		m_bIsSwapChainOutdated = true;
	}
	else if (presentResult != VK_SUCCESS)
	{
		SAILOR_LOG("Failed to present swap chain image!");
		return false;
	}

	return submitResult == VK_SUCCESS && presentResult == VK_SUCCESS;
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