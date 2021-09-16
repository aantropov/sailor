#pragma once
#include <cstdint>
#include <optional>
#include <string>
#include <vector>
#include <cassert>
#include <iostream>
#include <cstdint>
#include <vulkan/vulkan_core.h>
#include "Sailor.h"
#include "Core/RefPtr.hpp"
#include "Core/Singleton.hpp"

class Sailor::Win32::Window;
using namespace Sailor::Win32;

namespace Sailor::GfxDevice::Vulkan
{
	class VulkanRenderPass;
	class VulkanSurface;
	class VulkanDevice;
	class VulkanImageView;
	class VulkanImage;
	class VulkanBuffer;
	class VulkanDeviceMemory;

#define VK_CHECK(call) \
	do { \
		VkResult result_ = call; \
		assert(result_ == VK_SUCCESS); \
	} while (0)

#define NUM_ELEMENTS(array) (sizeof(array) / sizeof(array[0]))

	struct VulkanQueueFamilyIndices
	{
		std::optional<uint32_t> m_graphicsFamily;
		std::optional<uint32_t> m_presentFamily;
		std::optional<uint32_t> m_transferFamily;

		SAILOR_API bool IsComplete() const { return m_graphicsFamily.has_value() && m_presentFamily.has_value() && m_transferFamily.has_value(); }
	};

	struct SwapChainSupportDetails
	{
		VkSurfaceCapabilitiesKHR m_capabilities;
		std::vector<VkSurfaceFormatKHR> m_formats;
		std::vector<VkPresentModeKHR> m_presentModes;
	};

	class RHIVertexFactoryPositionColor
	{
	public:

		static SAILOR_API VkVertexInputBindingDescription GetBindingDescription();
		static SAILOR_API std::array<VkVertexInputAttributeDescription, 2> GetAttributeDescriptions();
	};

	class VulkanApi : public TSingleton<VulkanApi>
	{
	public:
		static constexpr int MaxFramesInFlight = 2;

		static SAILOR_API void Initialize(const Window* pViewport, bool bIsDebug);
		virtual SAILOR_API ~VulkanApi() override;

		static void SAILOR_API DrawFrame();
		static void SAILOR_API WaitIdle();
		SAILOR_API TRefPtr<VulkanDevice> GetMainDevice() const;

		bool SAILOR_API IsEnabledValidationLayers() const { return bIsEnabledValidationLayers; }
		__forceinline static SAILOR_API VkInstance& GetVkInstance() { return m_pInstance->m_vkInstance; }

		static SAILOR_API VulkanQueueFamilyIndices FindQueueFamilies(VkPhysicalDevice device, TRefPtr<VulkanSurface> surface);
		static SAILOR_API SwapChainSupportDetails QuerySwapChainSupport(VkPhysicalDevice device, TRefPtr<VulkanSurface> surface);

		static SAILOR_API VkSurfaceFormatKHR ChooseSwapSurfaceFormat(const std::vector<VkSurfaceFormatKHR>& availableFormats);
		static SAILOR_API VkPresentModeKHR ÑhooseSwapPresentMode(const std::vector<VkPresentModeKHR>& availablePresentModes, bool bVSync);
		static SAILOR_API VkExtent2D ChooseSwapExtent(const VkSurfaceCapabilitiesKHR& capabilities, uint32_t width, uint32_t height);

		static SAILOR_API VkPhysicalDevice PickPhysicalDevice(TRefPtr<VulkanSurface> surface);
		static SAILOR_API void GetRequiredExtensions(std::vector<const char*>& requiredDeviceExtensions, std::vector<const char*>& requiredInstanceExtensions) { requiredDeviceExtensions = { VK_KHR_SWAPCHAIN_EXTENSION_NAME }; }

		static SAILOR_API VkAttachmentDescription GetDefaultColorAttachment(VkFormat imageFormat);
		static SAILOR_API VkAttachmentDescription GetDefaultDepthAttachment(VkFormat depthFormat);

		static SAILOR_API TRefPtr<VulkanRenderPass> CreateRenderPass(TRefPtr<VulkanDevice> device, VkFormat imageFormat, VkFormat depthFormat);
		static SAILOR_API TRefPtr<VulkanRenderPass> CreateMSSRenderPass(TRefPtr<VulkanDevice> device, VkFormat imageFormat, VkFormat depthFormat, VkSampleCountFlagBits samples);
		static SAILOR_API TRefPtr<VulkanImageView> CreateImageView(TRefPtr<VulkanDevice> device, TRefPtr<VulkanImage> image, VkImageAspectFlags aspectFlags);
		static SAILOR_API uint32_t FindMemoryByType(VkPhysicalDevice physicalDevice, uint32_t typeFilter, VkMemoryPropertyFlags properties);

		static SAILOR_API void CreateBuffer(TRefPtr<VulkanDevice> device, VkDeviceSize in_size, VkBufferUsageFlags in_usage, VkSharingMode in_sharingMode, VkMemoryPropertyFlags properties,
			TRefPtr<VulkanBuffer>& outBuffer, TRefPtr<VulkanDeviceMemory>& outDeviceMemory);

		//Immediate context
		static SAILOR_API TRefPtr<VulkanBuffer> CreateBuffer_Immediate(TRefPtr<VulkanDevice> device, const void* pData, VkDeviceSize in_size, VkBufferUsageFlags in_usage, VkSharingMode in_sharingMode = VkSharingMode::VK_SHARING_MODE_CONCURRENT);
		static SAILOR_API void CopyBuffer_Immediate(TRefPtr<VulkanDevice> device, TRefPtr<VulkanBuffer>  src, TRefPtr<VulkanBuffer> dst, VkDeviceSize size);
		//Immediate context

		static SAILOR_API VkImageAspectFlags ComputeAspectFlagsForFormat(VkFormat format);

	private:

		static SAILOR_API uint32_t GetNumSupportedExtensions();
		static SAILOR_API void PrintSupportedExtensions();

		static SAILOR_API bool CheckDeviceExtensionSupport(VkPhysicalDevice device);
		static SAILOR_API bool CheckValidationLayerSupport(const std::vector<const char*>& validationLayers);

		static SAILOR_API bool IsDeviceSuitable(VkPhysicalDevice device, TRefPtr<VulkanSurface> surface);
		static SAILOR_API int32_t GetDeviceScore(VkPhysicalDevice device);

		static SAILOR_API bool SetupDebugCallback();
		static SAILOR_API VkResult CreateDebugUtilsMessengerEXT(VkInstance instance, const VkDebugUtilsMessengerCreateInfoEXT* pCreateInfo, const	VkAllocationCallbacks* pAllocator, VkDebugUtilsMessengerEXT* pDebugMessenger);
		static SAILOR_API void DestroyDebugUtilsMessengerEXT(VkInstance instance, VkDebugUtilsMessengerEXT debugMessenger, const	VkAllocationCallbacks* pAllocator);

		VkDebugUtilsMessengerEXT m_debugMessenger = 0;
		bool bIsEnabledValidationLayers = false;

		VkInstance m_vkInstance = 0;
		TRefPtr<VulkanDevice> m_device;
	};
}
