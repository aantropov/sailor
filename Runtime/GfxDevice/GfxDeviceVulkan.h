#pragma once
#include <cstdint>
#include <string>
#include <vector>
#include <vulkan/vulkan_core.h>

namespace Sailor
{
	class Window;

	class GfxDeviceVulkan
	{
	public:	
		
		static void CreateInstance(const Window* viewport, bool bInIsEnabledValidationLayers);
		static void Shutdown();

		static uint32_t GetNumSupportedExtensions();
		static void PrintSupportedExtensions();

		static bool CheckValidationLayerSupport(const std::vector<const char*>& validationLayers);
		static void SetupDebugCallback();

	private:

		GfxDeviceVulkan() = default;
		~GfxDeviceVulkan() = default;

		static GfxDeviceVulkan* instance;
		static VkInstance vkInstance;
		static VkDebugUtilsMessengerEXT debugMessenger;

		static VkResult CreateDebugUtilsMessengerEXT(VkInstance instance, const VkDebugUtilsMessengerCreateInfoEXT* pCreateInfo, const	VkAllocationCallbacks* pAllocator, VkDebugUtilsMessengerEXT* pDebugMessenger);
		static void DestroyDebugUtilsMessengerEXT(VkInstance instance, VkDebugUtilsMessengerEXT debugMessenger, const	VkAllocationCallbacks* pAllocator);

		bool bIsEnabledValidationLayers = false;
	};
}