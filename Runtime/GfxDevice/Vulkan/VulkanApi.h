#pragma once
#include <cstdint>
#include <optional>
#include <string>
#include <vector>
#include <vulkan/vulkan_core.h>
#include "Sailor.h"
#include "Core/Singleton.hpp"

#define VK_CHECK(call) \
	do { \
		VkResult result_ = call; \
		assert(result_ == VK_SUCCESS); \
	} while (0)

#define NUM_ELEMENTS(array) (sizeof(array) / sizeof(array[0]))


class Sailor::Window;

namespace Sailor::GfxDevice::Vulkan
{
	class VulkanApi : public TSingleton<VulkanApi>
	{
	public:
		const int MAX_FRAMES_IN_FLIGHT = 2;
		
		struct QueueFamilyIndices
		{
			std::optional<uint32_t> m_graphicsFamily;
			std::optional<uint32_t> m_presentFamily;

			SAILOR_API bool IsComplete() const { return m_graphicsFamily.has_value() && m_presentFamily.has_value(); }
		};

		struct SwapChainSupportDetails
		{
			VkSurfaceCapabilitiesKHR m_capabilities;
			std::vector<VkSurfaceFormatKHR> m_formats;
			std::vector<VkPresentModeKHR> m_presentModes;
		};

		static SAILOR_API void Initialize(const Window* pViewport, bool bIsDebug);

		static SAILOR_API uint32_t GetNumSupportedExtensions();
		static SAILOR_API void PrintSupportedExtensions();

		static SAILOR_API bool CheckDeviceExtensionSupport(VkPhysicalDevice device);
		static SAILOR_API bool CheckValidationLayerSupport(const std::vector<const char*>& validationLayers);

		static SAILOR_API bool IsDeviceSuitable(VkPhysicalDevice device);
		static SAILOR_API int32_t GetDeviceScore(VkPhysicalDevice device);

		static SAILOR_API void GetRequiredDeviceExtensions(std::vector<const char*>& requiredDeviceExtensions) { requiredDeviceExtensions = { VK_KHR_SWAPCHAIN_EXTENSION_NAME }; }

		static SAILOR_API VkShaderModule CreateShaderModule(const std::vector<uint32_t>& inCode);

		virtual SAILOR_API ~VulkanApi() override;

		void SAILOR_API DrawFrame(Window* pViewport);

		static SAILOR_API void WaitIdle();

	
	private:

		static SAILOR_API bool SetupDebugCallback();
		static SAILOR_API VkResult CreateDebugUtilsMessengerEXT(VkInstance instance, const VkDebugUtilsMessengerCreateInfoEXT* pCreateInfo, const	VkAllocationCallbacks* pAllocator, VkDebugUtilsMessengerEXT* pDebugMessenger);
		static SAILOR_API void DestroyDebugUtilsMessengerEXT(VkInstance instance, VkDebugUtilsMessengerEXT debugMessenger, const	VkAllocationCallbacks* pAllocator);

		static SAILOR_API VkPhysicalDevice PickPhysicalDevice();

		static SAILOR_API QueueFamilyIndices FindQueueFamilies(VkPhysicalDevice device);
		static SAILOR_API SwapChainSupportDetails QuerySwapChainSupport(VkPhysicalDevice device);

		static SAILOR_API VkSurfaceFormatKHR ChooseSwapSurfaceFormat(const std::vector<VkSurfaceFormatKHR>& availableFormats);
		static SAILOR_API VkPresentModeKHR ÑhooseSwapPresentMode(const std::vector<VkPresentModeKHR>& availablePresentModes, bool bVSync);
		static SAILOR_API VkExtent2D ChooseSwapExtent(const VkSurfaceCapabilitiesKHR& capabilities, const Window* pViewport);

		SAILOR_API void CreateLogicalDevice(VkPhysicalDevice physicalDevice);
		SAILOR_API void CreateWin32Surface(const Window* pViewport);
		SAILOR_API void CreateSwapchain(const Window* pViewport);
		SAILOR_API bool RecreateSwapchain(Window* pViewport);
		SAILOR_API void CreateGraphicsPipeline();
		SAILOR_API void CreateRenderPass();
		SAILOR_API void CreateFramebuffers();
		SAILOR_API void CreateCommandPool();
		SAILOR_API void CreateCommandBuffers();
		SAILOR_API void CreateFrameSyncSemaphores();

		SAILOR_API void CleanupSwapChain();
		
		__forceinline static SAILOR_API VkInstance& GetVkInstance() { return m_pInstance->m_vkInstance; }

		bool bIsEnabledValidationLayers = false;
		VkInstance m_vkInstance = 0;

		// Command pool
		VkCommandPool m_commandPool;
		std::vector<std::unique_ptr<class VulkanCommandBuffer>> m_commandBuffers;

		// Render Pass
		VkRenderPass m_renderPass = 0;
		VkPipelineLayout m_pipelineLayout = 0;
		VkPipeline m_graphicsPipeline = 0;

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
		std::vector<VkFramebuffer> m_swapChainFramebuffers;
		VkExtent2D m_swapChainExtent;

		// Choosen swapchain formats
		VkSurfaceFormatKHR m_surfaceFormat;
		VkPresentModeKHR m_presentMode;

		// Frame sync
		std::vector<VkSemaphore> m_imageAvailableSemaphores;
		std::vector<VkSemaphore> m_renderFinishedSemaphores;
		std::vector<VkFence> m_syncFences;
		std::vector<VkFence> m_syncImages;
		size_t m_currentFrame = 0;

		std::atomic<bool> bIsFramebufferResizedThisFrame = false;
	};
}
