#include "VulkanCapabilityOverrides.h"
#include <vulkan/vulkan.h>
#include <cstring>

namespace Sailor::Tests
{
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

	VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL GetDeviceProc(VkDevice device, const char* name)
	{
		auto& overrides = GetVulkanCapabilityOverrides();
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

	VKAPI_ATTR VkResult VKAPI_CALL CreateSampler(VkDevice device, const VkSamplerCreateInfo* info,
		const VkAllocationCallbacks* allocator, VkSampler* sampler)
	{
		++GetVulkanCapabilityOverrides().samplerCreateCalls;
		return vkCreateSampler(device, info, allocator, sampler);
	}

	VKAPI_ATTR VkResult VKAPI_CALL CreateBuffer(VkDevice device, const VkBufferCreateInfo* info,
		const VkAllocationCallbacks* allocator, VkBuffer* buffer)
	{
		++GetVulkanCapabilityOverrides().bufferCreateCalls;
		return vkCreateBuffer(device, info, allocator, buffer);
	}

	// dyld interposes other images, leaving this library's calls to Vulkan intact.
	__attribute__((used, section("__DATA,__interpose,interposing")))
	const struct { const void* replacement; const void* original; } interpose[] = {
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
		{ reinterpret_cast<const void*>(&CreateSampler), reinterpret_cast<const void*>(&vkCreateSampler) },
		{ reinterpret_cast<const void*>(&CreateBuffer), reinterpret_cast<const void*>(&vkCreateBuffer) }
	};
}
