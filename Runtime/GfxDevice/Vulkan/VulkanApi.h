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
#include "RHI/Types.h"
#include "AssetRegistry/Shader/ShaderCompiler.h"
#include "Memory/RefPtr.hpp"
#include "Core/Singleton.hpp"

namespace Sailor
{
	class Win32::Window;
	class FrameState;
	namespace Memory 
	{
		class VulkanBufferMemoryPtr;
	}
}

using namespace Sailor::Win32;
using namespace Sailor;
using namespace Sailor::Memory;

namespace Sailor::GfxDevice::Vulkan
{
	typedef TRefPtr<class VulkanSampler> VulkanSamplerPtr;
	typedef TRefPtr<class VulkanDescriptor> VulkanDescriptorPtr;
	typedef TRefPtr<class VulkanDeviceMemory> VulkanDeviceMemoryPtr;
	typedef TRefPtr<class VulkanDescriptorSetLayout> VulkanDescriptorSetLayoutPtr;
	typedef TRefPtr<class VulkanPipelineState> VulkanPipelineStatePtr;
	typedef TRefPtr<class VulkanPipelineLayout> VulkanPipelineLayoutPtr;
	typedef TRefPtr<class VulkanShaderModule> VulkanShaderModulePtr;
	typedef TRefPtr<class VulkanSwapchainImage> VulkanSwapchainImagePtr;
	typedef TRefPtr<class VulkanSemaphore> VulkanSemaphorePtr;
	typedef TRefPtr<class VulkanQueue> VulkanQueuePtr;
	typedef TRefPtr<class VulkanFence> VulkanFencePtr;
	typedef TRefPtr<class VulkanBuffer> VulkanBufferPtr;
	typedef TRefPtr<class VulkanCommandBuffer> VulkanCommandBufferPtr;
	typedef TRefPtr<class VulkanCommandPool> VulkanCommandPoolPtr;
	typedef TRefPtr<class VulkanDescriptorPool> VulkanDescriptorPoolPtr;
	typedef TRefPtr<class VulkanDevice> VulkanDevicePtr;
	typedef TRefPtr<class VulkanSurface> VulkanSurfacePtr;
	typedef TRefPtr<class VulkanShaderStage> VulkanShaderStagePtr;
	typedef TRefPtr<class VulkanRenderPass> VulkanRenderPassPtr;
	typedef TRefPtr<class VulkanImage> VulkanImagePtr;
	typedef TRefPtr<class VulkanImageView> VulkanImageViewPtr;
	typedef TRefPtr<class VulkanPipeline> VulkanPipelinePtr;
	typedef TRefPtr<class VulkanFramebuffer> VulkanFramebufferPtr;
	typedef TRefPtr<class VulkanSwapchain> VulkanSwapchainPtr;
	typedef TRefPtr<class VulkanDescriptorSet> VulkanDescriptorSetPtr;
	typedef TRefPtr<class VulkanSwapchain> VulkanSwapchainPtr;
	typedef TRefPtr<class VulkanDescriptorBuffer> VulkanDescriptorBufferPtr;
	typedef TRefPtr<class VulkanDescriptorImage> VulkanDescriptorImagePtr;
	typedef TRefPtr<class VulkanStateColorBlending> VulkanStateColorBlendingPtr;
	typedef TRefPtr<class VulkanStateViewport> VulkanStateViewportPtr;
	typedef TRefPtr<class VulkanStateRasterization> VulkanStateRasterizationPtr;
	typedef TRefPtr<class VulkanStateDynamicViewport> VulkanStateDynamicViewportPtr;
	typedef TRefPtr<class VulkanStateMultisample> VulkanStateMultisamplePtr;
	typedef TRefPtr<class VulkanStateDepthStencil> VulkanStateDepthStencilPtr;
	typedef TRefPtr<class VulkanStateInputAssembly> VulkanStateInputAssemblyPtr;
	typedef TRefPtr<class VulkanStateVertexDescription> VulkanStateVertexDescriptionPtr;
	typedef TRefPtr<class VulkanStateDynamic> VulkanStateDynamicPtr;
	
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

		static void SAILOR_API WaitIdle();
		SAILOR_API VulkanDevicePtr GetMainDevice() const;

		bool SAILOR_API IsEnabledValidationLayers() const { return bIsEnabledValidationLayers; }
		__forceinline static SAILOR_API VkInstance& GetVkInstance() { return s_pInstance->m_vkInstance; }

		static SAILOR_API VulkanQueueFamilyIndices FindQueueFamilies(VkPhysicalDevice device, VulkanSurfacePtr surface);

		static SAILOR_API VkFormat SelectFormatByFeatures(VkPhysicalDevice physicalDevice, const std::vector<VkFormat>& candidates, VkImageTiling tiling, VkFormatFeatureFlags features);
		static SAILOR_API bool HasStencilComponent(VkFormat format);

		static SAILOR_API SwapChainSupportDetails QuerySwapChainSupport(VkPhysicalDevice device, VulkanSurfacePtr surface);

		static SAILOR_API VkSurfaceFormatKHR ChooseSwapSurfaceFormat(const std::vector<VkSurfaceFormatKHR>& availableFormats);
		static SAILOR_API VkPresentModeKHR ÑhooseSwapPresentMode(const std::vector<VkPresentModeKHR>& availablePresentModes, bool bVSync);
		static SAILOR_API VkExtent2D ChooseSwapExtent(const VkSurfaceCapabilitiesKHR& capabilities, uint32_t width, uint32_t height);

		static SAILOR_API VkPhysicalDevice PickPhysicalDevice(VulkanSurfacePtr surface);
		static SAILOR_API void GetRequiredExtensions(std::vector<const char*>& requiredDeviceExtensions, std::vector<const char*>& requiredInstanceExtensions) { requiredDeviceExtensions = { VK_KHR_SWAPCHAIN_EXTENSION_NAME }; }

		static SAILOR_API VkAttachmentDescription GetDefaultColorAttachment(VkFormat imageFormat);
		static SAILOR_API VkAttachmentDescription GetDefaultDepthAttachment(VkFormat depthFormat);

		static SAILOR_API VulkanRenderPassPtr CreateRenderPass(VulkanDevicePtr device, VkFormat imageFormat, VkFormat depthFormat);
		static SAILOR_API VulkanRenderPassPtr CreateMSSRenderPass(VulkanDevicePtr device, VkFormat imageFormat, VkFormat depthFormat, VkSampleCountFlagBits samples);
		static SAILOR_API VulkanImageViewPtr CreateImageView(VulkanDevicePtr device, VulkanImagePtr image, VkImageAspectFlags aspectFlags);
		static SAILOR_API uint32_t FindMemoryByType(VkPhysicalDevice physicalDevice, uint32_t typeFilter, VkMemoryPropertyFlags properties);

		static SAILOR_API bool CreateDescriptorSetLayouts(VulkanDevicePtr device,
			const std::vector<VulkanShaderStagePtr>& shaders,
			std::vector<VulkanDescriptorSetLayoutPtr>& outVulkanLayouts,
			std::vector<RHI::ShaderLayoutBinding>& outRhiLayout);

		static SAILOR_API VkDescriptorSetLayoutBinding CreateDescriptorSetLayoutBinding(
			uint32_t              binding = 0,
			VkDescriptorType      descriptorType = VkDescriptorType::VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
			uint32_t              descriptorCount = 1,
			VkShaderStageFlags    stageFlags = VK_SHADER_STAGE_ALL,
			const VkSampler* pImmutableSamplers = nullptr);

		static SAILOR_API VkDescriptorPoolSize CreateDescriptorPoolSize(VkDescriptorType type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, uint32_t count = 1);

		static SAILOR_API VulkanBufferPtr CreateBuffer(VulkanDevicePtr device, VkDeviceSize size, VkBufferUsageFlags usage, VkMemoryPropertyFlags properties, VkSharingMode sharingMode = VkSharingMode::VK_SHARING_MODE_CONCURRENT);
		static SAILOR_API VulkanCommandBufferPtr CreateBuffer(VulkanBufferPtr& outbuffer, VulkanDevicePtr device, const void* pData, VkDeviceSize size, VkBufferUsageFlags usage, VkSharingMode sharingMode = VkSharingMode::VK_SHARING_MODE_CONCURRENT);

		static SAILOR_API VulkanCommandBufferPtr CreateImage(
			VulkanImagePtr& outImage,
			VulkanDevicePtr device,
			const void* pData,
			VkDeviceSize size,
			VkExtent3D extent,
			uint32_t mipLevels = 1,
			VkImageType type = VK_IMAGE_TYPE_2D,
			VkFormat format = VK_FORMAT_R8G8B8A8_SRGB,
			VkImageTiling tiling = VK_IMAGE_TILING_OPTIMAL,
			VkImageUsageFlags usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
			VkSharingMode sharingMode = VkSharingMode::VK_SHARING_MODE_EXCLUSIVE);

		static SAILOR_API VulkanCommandBufferPtr UpdateBuffer(VulkanDevicePtr device, const VulkanBufferMemoryPtr& dst, const void* pData, VkDeviceSize size);

		//Immediate context
		static SAILOR_API VulkanBufferPtr CreateBuffer_Immediate(VulkanDevicePtr device, const void* pData, VkDeviceSize size, VkBufferUsageFlags usage, VkSharingMode sharingMode = VkSharingMode::VK_SHARING_MODE_CONCURRENT);
		static SAILOR_API void CopyBuffer_Immediate(VulkanDevicePtr device, VulkanBufferPtr  src, VulkanBufferPtr dst, VkDeviceSize size, VkDeviceSize srcOffset = 0, VkDeviceSize dstOffset = 0);

		static SAILOR_API VulkanImagePtr CreateImage_Immediate(
			VulkanDevicePtr device,
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

		static SAILOR_API bool IsDeviceSuitable(VkPhysicalDevice device, VulkanSurfacePtr surface);
		static SAILOR_API int32_t GetDeviceScore(VkPhysicalDevice device);

		static SAILOR_API bool SetupDebugCallback();
		static SAILOR_API VkResult CreateDebugUtilsMessengerEXT(VkInstance instance, const VkDebugUtilsMessengerCreateInfoEXT* pCreateInfo, const	VkAllocationCallbacks* pAllocator, VkDebugUtilsMessengerEXT* pDebugMessenger);
		static SAILOR_API void DestroyDebugUtilsMessengerEXT(VkInstance instance, VkDebugUtilsMessengerEXT debugMessenger, const	VkAllocationCallbacks* pAllocator);

		VkDebugUtilsMessengerEXT m_debugMessenger = 0;
		bool bIsEnabledValidationLayers = false;

		VkInstance m_vkInstance = 0;
		VulkanDevicePtr m_device;

		VulkanStateColorBlendingPtr m_additiveBlendMode;
		VulkanStateColorBlendingPtr m_alphaBlendingBlendMode;
		VulkanStateColorBlendingPtr m_multiplyBlendMode;

	};
}
