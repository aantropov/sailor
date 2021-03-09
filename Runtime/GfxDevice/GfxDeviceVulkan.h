#pragma once
#include <cstdint>
#include <optional>
#include <string>
#include <vector>
#include <vulkan/vulkan_core.h>

namespace Sailor
{
	class Window;

	class GfxDeviceVulkan
	{
	public:

		struct QueueFamilyIndices
		{
			std::optional<uint32_t> graphicsFamily;
			std::optional<uint32_t> presentFamily;

			bool IsComplete() const { return graphicsFamily.has_value() && presentFamily.has_value();; }

		};

		static void CreateInstance(const Window* viewport, bool bInIsEnabledValidationLayers);
		static void Shutdown();

		static uint32_t GetNumSupportedExtensions();
		static void PrintSupportedExtensions();

		static bool CheckValidationLayerSupport(const std::vector<const char*>& validationLayers);
		static bool IsDeviceSuitable(VkPhysicalDevice device);
		static int GetDeviceScore(VkPhysicalDevice device);

		static QueueFamilyIndices FindQueueFamilies(VkPhysicalDevice device);

	private:

		GfxDeviceVulkan() = default;
		~GfxDeviceVulkan() = default;

		static GfxDeviceVulkan* instance;
		
		static VkResult CreateDebugUtilsMessengerEXT(VkInstance instance, const VkDebugUtilsMessengerCreateInfoEXT* pCreateInfo, const	VkAllocationCallbacks* pAllocator, VkDebugUtilsMessengerEXT* pDebugMessenger);
		static void DestroyDebugUtilsMessengerEXT(VkInstance instance, VkDebugUtilsMessengerEXT debugMessenger, const	VkAllocationCallbacks* pAllocator);

		static bool SetupDebugCallback();
		static VkPhysicalDevice PickPhysicalDevice();
		static void CreateLogicalDevice(VkPhysicalDevice physicalDevice);

		__forceinline static VkInstance& GetVkInstance() { return instance->vkInstance; }
		
		bool bIsEnabledValidationLayers = false;
		VkInstance vkInstance = 0;
		VkDebugUtilsMessengerEXT debugMessenger = 0;
		VkPhysicalDevice mainPhysicalDevice = 0;
		VkDevice device = 0;
		QueueFamilyIndices queueFamilies;
		VkSurfaceKHR surface = 0;
		VkQueue graphicsQueue = 0;
		VkQueue presentQueue = 0;

	};
}