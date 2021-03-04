#include "GfxDeviceVulkan.h"
#include "vulkan/vulkan.h"

uint32_t GfxDeviceVulkan::GetNumSupportedExtensions()
{
	uint32_t extensionCount;
	vkEnumerateInstanceExtensionProperties(nullptr, &extensionCount,
		nullptr);
	return extensionCount;
}
