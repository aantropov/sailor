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

		struct SwapChainSupportDetails
		{
			VkSurfaceCapabilitiesKHR capabilities;
			std::vector<VkSurfaceFormatKHR> formats;
			std::vector<VkPresentModeKHR> presentModes;
		};

		static void CreateInstance(const Window* viewport, bool bInIsEnabledValidationLayers);
		static void Shutdown();

		static uint32_t GetNumSupportedExtensions();
		static void PrintSupportedExtensions();

		static bool CheckDeviceExtensionSupport(VkPhysicalDevice device);
		static bool CheckValidationLayerSupport(const std::vector<const char*>& validationLayers);

		static bool IsDeviceSuitable(VkPhysicalDevice device);
		static int GetDeviceScore(VkPhysicalDevice device);

		static void GetRequiredDeviceExtensions(std::vector<const char*>& requiredDeviceExtensions) { requiredDeviceExtensions = { VK_KHR_SWAPCHAIN_EXTENSION_NAME }; }

	private:

		GfxDeviceVulkan() = default;
		~GfxDeviceVulkan() = default;

		static GfxDeviceVulkan* instance;

		static bool SetupDebugCallback();
		static VkResult CreateDebugUtilsMessengerEXT(VkInstance instance, const VkDebugUtilsMessengerCreateInfoEXT* pCreateInfo, const	VkAllocationCallbacks* pAllocator, VkDebugUtilsMessengerEXT* pDebugMessenger);
		static void DestroyDebugUtilsMessengerEXT(VkInstance instance, VkDebugUtilsMessengerEXT debugMessenger, const	VkAllocationCallbacks* pAllocator);

		static VkPhysicalDevice PickPhysicalDevice();

		static QueueFamilyIndices FindQueueFamilies(VkPhysicalDevice device);
		static SwapChainSupportDetails QuerySwapChainSupport(VkPhysicalDevice device);

		static VkSurfaceFormatKHR ChooseSwapSurfaceFormat(const std::vector<VkSurfaceFormatKHR>& availableFormats);
		static VkPresentModeKHR ÑhooseSwapPresentMode(const std::vector<VkPresentModeKHR>& availablePresentModes);
		static VkExtent2D ChooseSwapExtent(const VkSurfaceCapabilitiesKHR& capabilities, const Window* viewport);

		void CreateLogicalDevice(VkPhysicalDevice physicalDevice);
		void CreateWin32Surface(const Window* viewport);
		void CreateSwapchain(const Window* viewport);

		__forceinline static VkInstance& GetVkInstance() { return instance->vkInstance; }

		bool bIsEnabledValidationLayers = false;
		VkInstance vkInstance = 0;

		// Queuees
		VkQueue graphicsQueue = 0;
		VkQueue presentQueue = 0;

		VkSurfaceKHR surface = 0;
		
		VkDebugUtilsMessengerEXT debugMessenger = 0;

		VkPhysicalDevice mainPhysicalDevice = 0;
		VkDevice device = 0;
		QueueFamilyIndices queueFamilies;

		// Swapchain
		VkSwapchainKHR swapChain = 0;
		std::vector<VkImage> swapChainImages;
		std::vector<VkImageView> swapChainImageViews;

		// Choosen swapchain formats
		VkSurfaceFormatKHR surfaceFormat;		
		VkPresentModeKHR presentMode;
		VkExtent2D extent;
	};
}