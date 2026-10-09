#include "VulkanCapabilityOverrides.h"
#include "ScopeExit.h"
#include <vulkan/vulkan.h>
#include <cstring>
#include <utility>

namespace
{
	thread_local std::vector<Sailor::Tests::VulkanBufferWrite>* g_bufferWrites = nullptr;
	thread_local std::vector<VkImageMemoryBarrier>* g_imageBarriers = nullptr;
	thread_local std::vector<Sailor::Tests::VulkanComputeInputEvent>* g_computeInputs = nullptr;
	thread_local Sailor::Tests::VulkanDescriptorAllocationFailure* g_descriptorFailure = nullptr;
}

namespace Sailor::Tests
{
	std::vector<VkImageMemoryBarrier> CaptureVulkanImageBarriers(const std::function<void()>& record)
	{
		std::vector<VkImageMemoryBarrier> barriers;
		auto* previous = std::exchange(g_imageBarriers, &barriers);
		ScopeExit restore([previous]() { g_imageBarriers = previous; });
		record();
		return barriers;
	}

	VulkanDescriptorAllocationFailure RefuseSecondVulkanDescriptorAllocation(const std::function<void()>& record)
	{
		VulkanDescriptorAllocationFailure failure;
		auto previous = std::exchange(g_descriptorFailure, &failure);
		try
		{
			record();
		}
		catch (...)
		{
			g_descriptorFailure = previous;
			throw;
		}
		g_descriptorFailure = previous;
		return failure;
	}

	std::vector<VulkanComputeInputEvent> CaptureVulkanComputeInputs(const std::function<void()>& record)
	{
		std::vector<VulkanComputeInputEvent> events;
		auto previous = std::exchange(g_computeInputs, &events);
		try { record(); }
		catch (...)
		{
			g_computeInputs = previous;
			throw;
		}
		g_computeInputs = previous;
		return events;
	}

	std::vector<VulkanBufferWrite> CaptureVulkanBufferWrites(const std::function<void()>& record)
	{
		std::vector<VulkanBufferWrite> writes;
		auto previous = std::exchange(g_bufferWrites, &writes);
		try { record(); }
		catch (...)
		{
			g_bufferWrites = previous;
			throw;
		}
		g_bufferWrites = previous;
		return writes;
	}

	VulkanCapabilityOverrides& GetVulkanCapabilityOverrides()
	{
		static VulkanCapabilityOverrides overrides;
		return overrides;
	}
}

namespace
{
	using Sailor::Tests::GetVulkanCapabilityOverrides;
	using Sailor::Tests::MissingVulkanFeature;
	using Sailor::Tests::ValidationLayerInventory;

	VKAPI_ATTR VkResult VKAPI_CALL EnumerateLayers(uint32_t* count, VkLayerProperties* properties)
	{
		auto& overrides = GetVulkanCapabilityOverrides();
		const auto inventory = overrides.validationLayers.load();
		if (inventory == ValidationLayerInventory::Native) return vkEnumerateInstanceLayerProperties(count, properties);
		++overrides.layerEnumerationCalls;
		const char* names[2]{};
		uint32_t available = 0;
		if (inventory == ValidationLayerInventory::Primary || inventory == ValidationLayerInventory::Both)
			names[available++] = "VK_LAYER_KHRONOS_validation";
		if (inventory == ValidationLayerInventory::Compatibility || inventory == ValidationLayerInventory::Both)
			names[available++] = "VK_LAYER_KHRONOS_synchronization2";
		if (!properties)
		{
			*count = available;
			return VK_SUCCESS;
		}
		const uint32_t written = *count < available ? *count : available;
		for (uint32_t i = 0; i < written; ++i)
		{
			properties[i] = {};
			std::strcpy(properties[i].layerName, names[i]);
			properties[i].specVersion = VK_API_VERSION_1_3;
		}
		*count = written;
		return written == available ? VK_SUCCESS : VK_INCOMPLETE;
	}

	VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL GetDeviceProc(VkDevice device, const char* name)
	{
		auto& overrides = GetVulkanCapabilityOverrides();
		if (const auto submit = overrides.queueSubmit.load(); submit && std::strcmp(name, "vkQueueSubmit") == 0)
			return reinterpret_cast<PFN_vkVoidFunction>(submit);
		if (const auto wait = overrides.waitForFences.load(); wait && std::strcmp(name, "vkWaitForFences") == 0)
			return reinterpret_cast<PFN_vkVoidFunction>(wait);
		if (const auto status = overrides.getFenceStatus.load(); status && std::strcmp(name, "vkGetFenceStatus") == 0)
			return reinterpret_cast<PFN_vkVoidFunction>(status);
		if (std::strcmp(name, "vkCmdBeginRendering") == 0 || std::strcmp(name, "vkCmdEndRendering") == 0)
			++overrides.coreRenderingLookups;
		if (std::strcmp(name, "vkCmdBeginRenderingKHR") == 0 || std::strcmp(name, "vkCmdEndRenderingKHR") == 0)
			++overrides.khrRenderingLookups;
		const uint32_t missing = overrides.missingRenderingCommands.load();
		if (((missing & 1u) && (std::strcmp(name, "vkCmdBeginRendering") == 0 || std::strcmp(name, "vkCmdBeginRenderingKHR") == 0)) ||
			((missing & 2u) && (std::strcmp(name, "vkCmdEndRendering") == 0 || std::strcmp(name, "vkCmdEndRenderingKHR") == 0)))
		{
			++overrides.hiddenRenderingCommands;
			return nullptr;
		}
		return vkGetDeviceProcAddr(device, name);
	}

	VKAPI_ATTR VkResult VKAPI_CALL EnumerateInstanceVersion(uint32_t* version)
	{
		*version = GetVulkanCapabilityOverrides().loaderApiVersion;
		return VK_SUCCESS;
	}

	VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL GetInstanceProc(VkInstance instance, const char* name)
	{
		if (GetVulkanCapabilityOverrides().loaderApiVersion && std::strcmp(name, "vkEnumerateInstanceVersion") == 0)
			return reinterpret_cast<PFN_vkVoidFunction>(&EnumerateInstanceVersion);
		return vkGetInstanceProcAddr(instance, name);
	}

	VKAPI_ATTR void VKAPI_CALL GetProperties(VkPhysicalDevice device, VkPhysicalDeviceProperties* properties)
	{
		vkGetPhysicalDeviceProperties(device, properties);
		if (const uint32_t version = GetVulkanCapabilityOverrides().deviceApiVersion) properties->apiVersion = version;
	}

	VKAPI_ATTR void VKAPI_CALL GetProperties2(VkPhysicalDevice device, VkPhysicalDeviceProperties2* properties)
	{
		vkGetPhysicalDeviceProperties2(device, properties);
		if (const uint32_t version = GetVulkanCapabilityOverrides().deviceApiVersion) properties->properties.apiVersion = version;
	}

	VKAPI_ATTR void VKAPI_CALL GetFeatures(VkPhysicalDevice device, VkPhysicalDeviceFeatures2* features)
	{
		vkGetPhysicalDeviceFeatures2(device, features);
		auto& overrides = GetVulkanCapabilityOverrides();
		++overrides.featureQueries;
		const auto missing = overrides.missingFeature.load();
		if (missing == MissingVulkanFeature::Anisotropy) features->features.samplerAnisotropy = VK_FALSE;
		if (missing == MissingVulkanFeature::FirstInstance) features->features.drawIndirectFirstInstance = VK_FALSE;
		if (missing == MissingVulkanFeature::IndependentBlend) features->features.independentBlend = VK_FALSE;
		for (auto* next = static_cast<VkBaseOutStructure*>(features->pNext); next; next = next->pNext)
		{
			if (next->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES)
			{
				auto& core12 = *reinterpret_cast<VkPhysicalDeviceVulkan12Features*>(next);
				if (missing == MissingVulkanFeature::RuntimeArray) core12.runtimeDescriptorArray = VK_FALSE;
				if (missing == MissingVulkanFeature::SampledImageIndexing) core12.shaderSampledImageArrayNonUniformIndexing = VK_FALSE;
				if (missing == MissingVulkanFeature::VariableDescriptorCount) core12.descriptorBindingVariableDescriptorCount = VK_FALSE;
				if (missing == MissingVulkanFeature::PartiallyBound) core12.descriptorBindingPartiallyBound = VK_FALSE;
			}
			if (next->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DYNAMIC_RENDERING_FEATURES && missing == MissingVulkanFeature::DynamicRendering)
				reinterpret_cast<VkPhysicalDeviceDynamicRenderingFeatures*>(next)->dynamicRendering = VK_FALSE;
		}
	}

	VKAPI_ATTR VkResult VKAPI_CALL CreateDevice(VkPhysicalDevice physicalDevice, const VkDeviceCreateInfo* info,
		const VkAllocationCallbacks* allocator, VkDevice* device)
	{
		auto& overrides = GetVulkanCapabilityOverrides();
		++overrides.deviceCreateCalls;
		if (const auto result = overrides.deviceCreationResult.load(); result != VK_SUCCESS)
		{
			*device = VK_NULL_HANDLE;
			return result;
		}
		overrides.enabledKhrRendering = false;
		for (uint32_t i = 0; i < info->enabledExtensionCount; ++i)
			if (std::strcmp(info->ppEnabledExtensionNames[i], VK_KHR_DYNAMIC_RENDERING_EXTENSION_NAME) == 0)
				overrides.enabledKhrRendering = true;
		return vkCreateDevice(physicalDevice, info, allocator, device);
	}

	VKAPI_ATTR void VKAPI_CALL DestroyDevice(VkDevice device, const VkAllocationCallbacks* allocator)
	{
		++GetVulkanCapabilityOverrides().deviceDestroyCalls;
		vkDestroyDevice(device, allocator);
	}

	VKAPI_ATTR VkResult VKAPI_CALL CreateInstance(const VkInstanceCreateInfo* info,
		const VkAllocationCallbacks* allocator, VkInstance* instance)
	{
		auto& overrides = GetVulkanCapabilityOverrides();
		++overrides.instanceCreateCalls;
		overrides.instanceTarget = info->pApplicationInfo->apiVersion;
		if (overrides.validationLayers != ValidationLayerInventory::Native)
		{
			overrides.requestedLayerCount = info->enabledLayerCount;
			overrides.requestedPrimaryValidation = false;
			overrides.requestedCompatibilityLayer = false;
			overrides.requestedDebugMessenger = false;
			for (uint32_t i = 0; i < info->enabledLayerCount; ++i)
			{
				if (std::strcmp(info->ppEnabledLayerNames[i], "VK_LAYER_KHRONOS_validation") == 0)
					overrides.requestedPrimaryValidation = true;
				if (std::strcmp(info->ppEnabledLayerNames[i], "VK_LAYER_KHRONOS_synchronization2") == 0)
					overrides.requestedCompatibilityLayer = true;
			}
			for (auto* next = static_cast<const VkBaseInStructure*>(info->pNext); next; next = next->pNext)
			{
				if (next->sType == VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT)
					overrides.requestedDebugMessenger = reinterpret_cast<const VkDebugUtilsMessengerCreateInfoEXT*>(next)->pfnUserCallback != nullptr;
			}
			// Observe configuration without asking a real driver to load fictional layers.
			*instance = VK_NULL_HANDLE;
			return VK_ERROR_INITIALIZATION_FAILED;
		}
		return vkCreateInstance(info, allocator, instance);
	}

	VKAPI_ATTR void VKAPI_CALL DestroyInstance(VkInstance instance, const VkAllocationCallbacks* allocator)
	{
		++GetVulkanCapabilityOverrides().instanceDestroyCalls;
		vkDestroyInstance(instance, allocator);
	}

	VKAPI_ATTR void VKAPI_CALL DestroySurface(VkInstance instance, VkSurfaceKHR surface, const VkAllocationCallbacks* allocator)
	{
		++GetVulkanCapabilityOverrides().surfaceDestroyCalls;
		vkDestroySurfaceKHR(instance, surface, allocator);
	}

	VKAPI_ATTR VkResult VKAPI_CALL GetSurfaceCapabilities(VkPhysicalDevice device, VkSurfaceKHR surface,
		VkSurfaceCapabilitiesKHR* capabilities)
	{
		const auto result = vkGetPhysicalDeviceSurfaceCapabilitiesKHR(device, surface, capabilities);
		const uint32_t limit = GetVulkanCapabilityOverrides().swapchainImageLimit;
		// Restrict the surface to a supported count; creation, acquisition and presentation stay native.
		if (result == VK_SUCCESS && limit != 0 && limit >= capabilities->minImageCount &&
			(capabilities->maxImageCount == 0 || limit < capabilities->maxImageCount))
		{
			capabilities->maxImageCount = limit;
		}
		return result;
	}

	VKAPI_ATTR VkResult VKAPI_CALL AcquireNextImage(VkDevice device, VkSwapchainKHR swapchain,
		uint64_t timeout, VkSemaphore semaphore, VkFence fence, uint32_t* imageIndex)
	{
		const int32_t index = GetVulkanCapabilityOverrides().acquiredImageIndex;
		if (index >= 0)
		{
			// The image-table fixture never submits or presents these modeled acquisitions.
			*imageIndex = static_cast<uint32_t>(index);
			return VK_SUCCESS;
		}
		return vkAcquireNextImageKHR(device, swapchain, timeout, semaphore, fence, imageIndex);
	}

	VKAPI_ATTR VkResult VKAPI_CALL CreateSampler(VkDevice device, const VkSamplerCreateInfo* info,
		const VkAllocationCallbacks* allocator, VkSampler* sampler)
	{
		++GetVulkanCapabilityOverrides().samplerCreateCalls;
		return vkCreateSampler(device, info, allocator, sampler);
	}

	VKAPI_ATTR VkResult VKAPI_CALL CreateBuffer(VkDevice device, const VkBufferCreateInfo* info,
		const VkAllocationCallbacks* allocator, VkBuffer* buffer)
	{
		const auto result = vkCreateBuffer(device, info, allocator, buffer);
		if (result == VK_SUCCESS) ++GetVulkanCapabilityOverrides().bufferCreateCalls;
		return result;
	}

	VKAPI_ATTR void VKAPI_CALL DestroyBuffer(VkDevice device, VkBuffer buffer, const VkAllocationCallbacks* allocator)
	{
		if (buffer) ++GetVulkanCapabilityOverrides().bufferDestroyCalls;
		vkDestroyBuffer(device, buffer, allocator);
	}

	VKAPI_ATTR VkResult VKAPI_CALL CreateImage(VkDevice device, const VkImageCreateInfo* info,
		const VkAllocationCallbacks* allocator, VkImage* image)
	{
		const auto result = vkCreateImage(device, info, allocator, image);
		if (result == VK_SUCCESS) ++GetVulkanCapabilityOverrides().imageCreateCalls;
		return result;
	}

	VKAPI_ATTR void VKAPI_CALL DestroyImage(VkDevice device, VkImage image, const VkAllocationCallbacks* allocator)
	{
		if (image) ++GetVulkanCapabilityOverrides().imageDestroyCalls;
		vkDestroyImage(device, image, allocator);
	}

	VKAPI_ATTR VkResult VKAPI_CALL CreateFence(VkDevice device, const VkFenceCreateInfo* info,
		const VkAllocationCallbacks* allocator, VkFence* fence)
	{
		const auto result = vkCreateFence(device, info, allocator, fence);
		if (result == VK_SUCCESS) ++GetVulkanCapabilityOverrides().fenceCreateCalls;
		return result;
	}

	VKAPI_ATTR void VKAPI_CALL DestroyFence(VkDevice device, VkFence fence, const VkAllocationCallbacks* allocator)
	{
		if (fence) ++GetVulkanCapabilityOverrides().fenceDestroyCalls;
		vkDestroyFence(device, fence, allocator);
	}

	VKAPI_ATTR void VKAPI_CALL CopyBufferToImage(VkCommandBuffer command, VkBuffer buffer, VkImage image,
		VkImageLayout layout, uint32_t count, const VkBufferImageCopy* regions)
	{
		uint32_t layers = 0;
		for (uint32_t i = 0; i < count; ++i) layers += regions[i].imageSubresource.layerCount;
		GetVulkanCapabilityOverrides().lastUploadLayers = layers;
		vkCmdCopyBufferToImage(command, buffer, image, layout, count, regions);
	}

	VKAPI_ATTR void VKAPI_CALL CopyBuffer(VkCommandBuffer command, VkBuffer source, VkBuffer destination,
		uint32_t count, const VkBufferCopy* regions)
	{
		if (g_computeInputs)
			for (uint32_t i = 0; i < count; ++i)
				g_computeInputs->push_back({ Sailor::Tests::VulkanComputeInputEvent::Kind::Copy,
					command, destination, regions[i].dstOffset, regions[i].size });
		if (g_bufferWrites)
			for (uint32_t i = 0; i < count; ++i)
				g_bufferWrites->push_back({ destination, regions[i].dstOffset, regions[i].size });
		vkCmdCopyBuffer(command, source, destination, count, regions);
	}

	VKAPI_ATTR void VKAPI_CALL UpdateBuffer(VkCommandBuffer command, VkBuffer destination,
		VkDeviceSize offset, VkDeviceSize size, const void* data)
	{
		if (g_computeInputs) g_computeInputs->push_back({ Sailor::Tests::VulkanComputeInputEvent::Kind::Copy, command, destination, offset, size });
		if (g_bufferWrites) g_bufferWrites->push_back({ destination, offset, size });
		vkCmdUpdateBuffer(command, destination, offset, size, data);
	}

	VKAPI_ATTR void VKAPI_CALL PipelineBarrier(VkCommandBuffer command, VkPipelineStageFlags sourceStage,
		VkPipelineStageFlags destinationStage, VkDependencyFlags flags, uint32_t memoryCount,
		const VkMemoryBarrier* memory, uint32_t bufferCount, const VkBufferMemoryBarrier* buffers,
		uint32_t imageCount, const VkImageMemoryBarrier* images)
	{
		if (g_imageBarriers && imageCount > 0)
		{
			g_imageBarriers->insert(g_imageBarriers->end(), images, images + imageCount);
		}
		if (g_computeInputs)
		{
			for (uint32_t i = 0; i < memoryCount; ++i)
				g_computeInputs->push_back({ Sailor::Tests::VulkanComputeInputEvent::Kind::Barrier, command,
					VK_NULL_HANDLE, 0, 0, sourceStage, destinationStage, memory[i].srcAccessMask, memory[i].dstAccessMask });
			for (uint32_t i = 0; i < bufferCount; ++i)
				g_computeInputs->push_back({ Sailor::Tests::VulkanComputeInputEvent::Kind::Barrier, command,
					buffers[i].buffer, buffers[i].offset, buffers[i].size, sourceStage, destinationStage,
					buffers[i].srcAccessMask, buffers[i].dstAccessMask });
		}
		vkCmdPipelineBarrier(command, sourceStage, destinationStage, flags, memoryCount, memory, bufferCount, buffers, imageCount, images);
	}

	VKAPI_ATTR void VKAPI_CALL Dispatch(VkCommandBuffer command, uint32_t x, uint32_t y, uint32_t z)
	{
		if (g_computeInputs) g_computeInputs->push_back({ Sailor::Tests::VulkanComputeInputEvent::Kind::Dispatch, command });
		vkCmdDispatch(command, x, y, z);
	}

	VKAPI_ATTR VkResult VKAPI_CALL AllocateDescriptorSets(VkDevice device, const VkDescriptorSetAllocateInfo* info,
		VkDescriptorSet* sets)
	{
		if (g_descriptorFailure && ++g_descriptorFailure->m_numAllocations == 2)
		{
			g_descriptorFailure->m_failedLayout = info->pSetLayouts[0];
			for (uint32_t i = 0; i < info->descriptorSetCount; ++i)
			{
				sets[i] = VK_NULL_HANDLE;
			}
			// Pool exhaustion retries internally; host OOM reaches the caller directly.
			return VK_ERROR_OUT_OF_HOST_MEMORY;
		}
		const auto result = vkAllocateDescriptorSets(device, info, sets);
		if (g_descriptorFailure && g_descriptorFailure->m_numAllocations == 1 && result == VK_SUCCESS)
		{
			g_descriptorFailure->m_firstSet = sets[0];
		}
		return result;
	}

	VKAPI_ATTR void VKAPI_CALL UpdateDescriptorSets(VkDevice device, uint32_t writeCount, const VkWriteDescriptorSet* writes,
		uint32_t copyCount, const VkCopyDescriptorSet* copies)
	{
		vkUpdateDescriptorSets(device, writeCount, writes, copyCount, copies);
		if (g_descriptorFailure && g_descriptorFailure->m_numAllocations == 1 && writeCount == 1)
		{
			g_descriptorFailure->m_bFirstStorageWritten = writes[0].dstSet == g_descriptorFailure->m_firstSet &&
				writes[0].dstBinding == 0 && writes[0].descriptorType == VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
		}
	}

	// dyld interposes other images, leaving this library's calls to Vulkan intact.
	__attribute__((used, section("__DATA,__interpose,interposing")))
	const struct { const void* replacement; const void* original; } interpose[] = {
		{ reinterpret_cast<const void*>(&EnumerateLayers), reinterpret_cast<const void*>(&vkEnumerateInstanceLayerProperties) },
		{ reinterpret_cast<const void*>(&GetDeviceProc), reinterpret_cast<const void*>(&vkGetDeviceProcAddr) },
		{ reinterpret_cast<const void*>(&GetInstanceProc), reinterpret_cast<const void*>(&vkGetInstanceProcAddr) },
		{ reinterpret_cast<const void*>(&GetProperties), reinterpret_cast<const void*>(&vkGetPhysicalDeviceProperties) },
		{ reinterpret_cast<const void*>(&GetProperties2), reinterpret_cast<const void*>(&vkGetPhysicalDeviceProperties2) },
		{ reinterpret_cast<const void*>(&GetFeatures), reinterpret_cast<const void*>(&vkGetPhysicalDeviceFeatures2) },
		{ reinterpret_cast<const void*>(&CreateDevice), reinterpret_cast<const void*>(&vkCreateDevice) },
		{ reinterpret_cast<const void*>(&DestroyDevice), reinterpret_cast<const void*>(&vkDestroyDevice) },
		{ reinterpret_cast<const void*>(&CreateInstance), reinterpret_cast<const void*>(&vkCreateInstance) },
		{ reinterpret_cast<const void*>(&DestroyInstance), reinterpret_cast<const void*>(&vkDestroyInstance) },
		{ reinterpret_cast<const void*>(&DestroySurface), reinterpret_cast<const void*>(&vkDestroySurfaceKHR) },
		{ reinterpret_cast<const void*>(&GetSurfaceCapabilities), reinterpret_cast<const void*>(&vkGetPhysicalDeviceSurfaceCapabilitiesKHR) },
		{ reinterpret_cast<const void*>(&AcquireNextImage), reinterpret_cast<const void*>(&vkAcquireNextImageKHR) },
		{ reinterpret_cast<const void*>(&CreateSampler), reinterpret_cast<const void*>(&vkCreateSampler) },
		{ reinterpret_cast<const void*>(&CreateBuffer), reinterpret_cast<const void*>(&vkCreateBuffer) },
		{ reinterpret_cast<const void*>(&DestroyBuffer), reinterpret_cast<const void*>(&vkDestroyBuffer) },
		{ reinterpret_cast<const void*>(&CreateImage), reinterpret_cast<const void*>(&vkCreateImage) },
		{ reinterpret_cast<const void*>(&DestroyImage), reinterpret_cast<const void*>(&vkDestroyImage) },
		{ reinterpret_cast<const void*>(&CreateFence), reinterpret_cast<const void*>(&vkCreateFence) },
		{ reinterpret_cast<const void*>(&DestroyFence), reinterpret_cast<const void*>(&vkDestroyFence) },
		{ reinterpret_cast<const void*>(&CopyBufferToImage), reinterpret_cast<const void*>(&vkCmdCopyBufferToImage) },
		{ reinterpret_cast<const void*>(&CopyBuffer), reinterpret_cast<const void*>(&vkCmdCopyBuffer) },
		{ reinterpret_cast<const void*>(&PipelineBarrier), reinterpret_cast<const void*>(&vkCmdPipelineBarrier) },
		{ reinterpret_cast<const void*>(&Dispatch), reinterpret_cast<const void*>(&vkCmdDispatch) },
		{ reinterpret_cast<const void*>(&AllocateDescriptorSets), reinterpret_cast<const void*>(&vkAllocateDescriptorSets) },
		{ reinterpret_cast<const void*>(&UpdateDescriptorSets), reinterpret_cast<const void*>(&vkUpdateDescriptorSets) },
		{ reinterpret_cast<const void*>(&UpdateBuffer), reinterpret_cast<const void*>(&vkCmdUpdateBuffer) }
	};
}
