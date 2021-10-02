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
#include "RHI/RHIResource.h"
#include "Core/RefPtr.hpp"
#include "Core/Singleton.hpp"

namespace Sailor
{
	class Win32::Window;
	class FrameState;
}

using namespace Sailor::Win32;
using namespace Sailor;

namespace Sailor::GfxDevice::Vulkan
{
	class VulkanRenderPass;
	class VulkanSurface;
	class VulkanDevice;
	class VulkanImageView;
	class VulkanImage;
	class VulkanBuffer;
	class VulkanDeviceMemory;
	class VulkanCommandBuffer;
	class VulkanSemaphore;

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

	template<typename TVertex = RHI::Vertex>
	class VertexFactory
	{
	public:

		static SAILOR_API VkVertexInputBindingDescription GetBindingDescription();
		static SAILOR_API std::vector<VkVertexInputAttributeDescription> GetAttributeDescriptions();
	};

	class VulkanApi : public TSingleton<VulkanApi>
	{
	public:
		static constexpr int MaxFramesInFlight = 2;
		static constexpr VkClearDepthStencilValue DefaultClearDepthStencilValue{ 1.0f, 0 };
		static constexpr VkClearValue DefaultClearColor{ {0.0f,0.0f,0.0f,0.0f} };

		static SAILOR_API void Initialize(const Window* pViewport, RHI::EMsaaSamples msaaSamples, bool bIsDebug);
		virtual SAILOR_API ~VulkanApi() override;

		static bool SAILOR_API PresentFrame(const FrameState& state, const std::vector<TRefPtr<VulkanCommandBuffer>>* primaryCommandBuffers = nullptr,
			const std::vector<TRefPtr<VulkanCommandBuffer>>* secondaryCommandBuffers = nullptr,
			const std::vector<TRefPtr<VulkanSemaphore>>* waitSemaphores = nullptr);

		static void SAILOR_API WaitIdle();
		SAILOR_API TRefPtr<VulkanDevice> GetMainDevice() const;

		bool SAILOR_API IsEnabledValidationLayers() const { return bIsEnabledValidationLayers; }
		__forceinline static SAILOR_API VkInstance& GetVkInstance() { return m_pInstance->m_vkInstance; }

		static SAILOR_API VulkanQueueFamilyIndices FindQueueFamilies(VkPhysicalDevice device, TRefPtr<VulkanSurface> surface);

		static SAILOR_API VkFormat SelectFormatByFeatures(VkPhysicalDevice physicalDevice, const std::vector<VkFormat>& candidates, VkImageTiling tiling, VkFormatFeatureFlags features);
		static SAILOR_API bool HasStencilComponent(VkFormat format);

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

		static SAILOR_API VkDescriptorSetLayoutBinding CreateDescriptorSetLayoutBinding(
			uint32_t              binding = 0,
			VkDescriptorType      descriptorType = VkDescriptorType::VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
			uint32_t              descriptorCount = 1,
			VkShaderStageFlags    stageFlags = VK_SHADER_STAGE_ALL,
			const VkSampler* pImmutableSamplers = nullptr);

		static SAILOR_API VkDescriptorPoolSize CreateDescriptorPoolSize(VkDescriptorType type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, uint32_t count = 1);

		static SAILOR_API TRefPtr<VulkanBuffer> CreateBuffer(TRefPtr<VulkanDevice> device, VkDeviceSize size, VkBufferUsageFlags usage, VkMemoryPropertyFlags properties, VkSharingMode sharingMode = VkSharingMode::VK_SHARING_MODE_CONCURRENT);

		//Immediate context
		static SAILOR_API TRefPtr<VulkanBuffer> CreateBuffer_Immediate(TRefPtr<VulkanDevice> device, const void* pData, VkDeviceSize size, VkBufferUsageFlags usage, VkSharingMode sharingMode = VkSharingMode::VK_SHARING_MODE_CONCURRENT);
		static SAILOR_API void CopyBuffer_Immediate(TRefPtr<VulkanDevice> device, TRefPtr<VulkanBuffer>  src, TRefPtr<VulkanBuffer> dst, VkDeviceSize size);

		static SAILOR_API TRefPtr<VulkanImage> CreateImage_Immediate(
			TRefPtr<VulkanDevice> device,
			const void* pData,
			VkDeviceSize size,
			VkExtent3D extent,
			uint32_t mipLevels = 1,
			VkImageType type = VK_IMAGE_TYPE_2D,
			VkFormat format = VK_FORMAT_R8G8B8A8_SRGB,
			VkImageTiling tiling = VK_IMAGE_TILING_OPTIMAL,
			VkImageUsageFlags usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
			VkSharingMode sharingMode = VkSharingMode::VK_SHARING_MODE_EXCLUSIVE);
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
