#pragma once
#include <cstdint>
#include <optional>
#include <string>
#include <vector>
#include <vulkan/vulkan_core.h>
#include "Sailor.h"
#include "Singleton.hpp"

namespace Sailor
{
	class Window;

	class GfxDeviceVulkan : public Singleton<GfxDeviceVulkan>
	{
	public:

		struct QueueFamilyIndices
		{
			std::optional<uint32_t> graphicsFamily;
			std::optional<uint32_t> presentFamily;

			SAILOR_API bool IsComplete() const { return graphicsFamily.has_value() && presentFamily.has_value();; }

		};

		struct SwapChainSupportDetails
		{
			VkSurfaceCapabilitiesKHR capabilities;
			std::vector<VkSurfaceFormatKHR> formats;
			std::vector<VkPresentModeKHR> presentModes;
		};

		static SAILOR_API void Initialize(const Window* viewport, bool bInIsEnabledValidationLayers);

		static SAILOR_API uint32_t GetNumSupportedExtensions();
		static SAILOR_API void PrintSupportedExtensions();

		static SAILOR_API bool CheckDeviceExtensionSupport(VkPhysicalDevice device);
		static SAILOR_API bool CheckValidationLayerSupport(const std::vector<const char*>& validationLayers);

		static SAILOR_API bool IsDeviceSuitable(VkPhysicalDevice device);
		static SAILOR_API int GetDeviceScore(VkPhysicalDevice device);

		static SAILOR_API void GetRequiredDeviceExtensions(std::vector<const char*>& requiredDeviceExtensions) { requiredDeviceExtensions = { VK_KHR_SWAPCHAIN_EXTENSION_NAME }; }

		virtual SAILOR_API ~GfxDeviceVulkan() override;

	private:

		static SAILOR_API bool SetupDebugCallback();
		static SAILOR_API VkResult CreateDebugUtilsMessengerEXT(VkInstance instance, const VkDebugUtilsMessengerCreateInfoEXT* pCreateInfo, const	VkAllocationCallbacks* pAllocator, VkDebugUtilsMessengerEXT* pDebugMessenger);
		static SAILOR_API void DestroyDebugUtilsMessengerEXT(VkInstance instance, VkDebugUtilsMessengerEXT debugMessenger, const	VkAllocationCallbacks* pAllocator);

		static SAILOR_API VkPhysicalDevice PickPhysicalDevice();

		static SAILOR_API QueueFamilyIndices FindQueueFamilies(VkPhysicalDevice device);
		static SAILOR_API SwapChainSupportDetails QuerySwapChainSupport(VkPhysicalDevice device);

		static SAILOR_API VkSurfaceFormatKHR ChooseSwapSurfaceFormat(const std::vector<VkSurfaceFormatKHR>& availableFormats);
		static SAILOR_API VkPresentModeKHR ÑhooseSwapPresentMode(const std::vector<VkPresentModeKHR>& availablePresentModes);
		static SAILOR_API VkExtent2D ChooseSwapExtent(const VkSurfaceCapabilitiesKHR& capabilities, const Window* viewport);

		SAILOR_API void CreateLogicalDevice(VkPhysicalDevice physicalDevice);
		SAILOR_API void CreateWin32Surface(const Window* viewport);
		SAILOR_API void CreateSwapchain(const Window* viewport);
		SAILOR_API void CreateGraphicsPipeline();
		
		__forceinline static SAILOR_API VkInstance& GetVkInstance() { return instance->vkInstance; }

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
