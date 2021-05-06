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
			std::optional<uint32_t> m_graphicsFamily;
			std::optional<uint32_t> m_presentFamily;

			SAILOR_API bool IsComplete() const { return m_graphicsFamily.has_value() && m_presentFamily.has_value();; }

		};

		struct SwapChainSupportDetails
		{
			VkSurfaceCapabilitiesKHR m_capabilities;
			std::vector<VkSurfaceFormatKHR> m_formats;
			std::vector<VkPresentModeKHR> m_presentModes;
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
		
		__forceinline static SAILOR_API VkInstance& GetVkInstance() { return m_pInstance->m_vkInstance; }

		bool bIsEnabledValidationLayers = false;
		VkInstance m_vkInstance = 0;

		// Queuees
		VkQueue m_graphicsQueue = 0;
		VkQueue m_presentQueue = 0;

		VkSurfaceKHR m_surface = 0;
		
		VkDebugUtilsMessengerEXT m_debugMessenger = 0;

		VkPhysicalDevice m_mainPhysicalDevice = 0;
		VkDevice m_device = 0;
		QueueFamilyIndices m_queueFamilies;

		// Swapchain
		VkSwapchainKHR m_swapChain = 0;
		std::vector<VkImage> m_swapChainImages;
		std::vector<VkImageView> m_swapChainImageViews;

		// Choosen swapchain formats
		VkSurfaceFormatKHR m_surfaceFormat;		
		VkPresentModeKHR m_presentMode;
		VkExtent2D m_extent;
	};
}
