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

		static void CreateInstance(const Window* viewport);
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
	};
}