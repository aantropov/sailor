#pragma once
#include <cstdint>
#include <optional>
#include <string>
#include "Containers/Vector.h"
#include <cassert>
#include <iostream>
#include <cstdint>
#include <vulkan/vulkan_core.h>
#include "Sailor.h"
#include "RHI/Types.h"
#include "AssetRegistry/Shader/ShaderCompiler.h"
#include "Memory/RefPtr.hpp"
#include "Core/Singleton.hpp"
#include "Containers/Vector.h"

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

namespace Sailor::GraphicsDriver::Vulkan
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
	typedef TRefPtr<class VulkanGraphicsPipeline> VulkanGraphicsPipelinePtr;
	typedef TRefPtr<class VulkanComputePipeline> VulkanComputePipelinePtr;
	typedef TRefPtr<class VulkanFramebuffer> VulkanFramebufferPtr;
	typedef TRefPtr<class VulkanSwapchain> VulkanSwapchainPtr;
	typedef TRefPtr<class VulkanDescriptorSet> VulkanDescriptorSetPtr;
	typedef TRefPtr<class VulkanSwapchain> VulkanSwapchainPtr;
	typedef TRefPtr<class VulkanDescriptorBuffer> VulkanDescriptorBufferPtr;
	typedef TRefPtr<class VulkanDescriptorCombinedImage> VulkanDescriptorCombinedImagePtr;
	typedef TRefPtr<class VulkanDescriptorStorageImage> VulkanDescriptorStorageImagePtr;
	typedef TRefPtr<class VulkanStateColorBlending> VulkanStateColorBlendingPtr;
	typedef TRefPtr<class VulkanStateViewport> VulkanStateViewportPtr;
	typedef TRefPtr<class VulkanStateRasterization> VulkanStateRasterizationPtr;
	typedef TRefPtr<class VulkanStateDynamicViewport> VulkanStateDynamicViewportPtr;
	typedef TRefPtr<class VulkanStateMultisample> VulkanStateMultisamplePtr;
	typedef TRefPtr<class VulkanStateDepthStencil> VulkanStateDepthStencilPtr;
	typedef TRefPtr<class VulkanStateInputAssembly> VulkanStateInputAssemblyPtr;
	typedef TRefPtr<class VulkanStateVertexDescription> VulkanStateVertexDescriptionPtr;
	typedef TRefPtr<class VulkanStateDynamicState> VulkanStateDynamicPtr;
	typedef TRefPtr<class VulkanStateDynamicRendering> VulkanStateDynamicRenderingPtr;

#define VK_CHECK(call) 	do { VkResult result_ = call; check(result_ == VK_SUCCESS); } while (0)

#define NUM_ELEMENTS(array) (sizeof(array) / sizeof(array[0]))

	struct VulkanQueueFamilyIndices
	{
		std::optional<uint32_t> m_graphicsFamily;
		std::optional<uint32_t> m_presentFamily;
		std::optional<uint32_t> m_transferFamily;
		std::optional<uint32_t> m_computeFamily;

		SAILOR_API bool IsComplete() const
		{
			return m_graphicsFamily.has_value() && m_presentFamily.has_value() && m_transferFamily.has_value() && m_computeFamily.has_value();
		}
	};

	struct SwapChainSupportDetails
	{
		VkSurfaceCapabilitiesKHR m_capabilities{};
		TVector<VkSurfaceFormatKHR> m_formats;
		TVector<VkPresentModeKHR> m_presentModes;
	};

	class VulkanApi : public TSingleton<VulkanApi>
	{
	public:

		static constexpr int MaxFramesInFlight = 2;

		// Reverse Z, 0.0f is the farest
		static constexpr VkClearDepthStencilValue DefaultClearDepthStencilValue{ 0.0f, 0 };
		static constexpr VkClearValue DefaultClearColor{ {0.0f,0.0f,0.0f,0.0f} };

		SAILOR_API static void Initialize(const Window* pViewport, RHI::EMsaaSamples msaaSamples, bool bIsDebug);
		SAILOR_API virtual ~VulkanApi() override;

		SAILOR_API static void WaitIdle();
		SAILOR_API VulkanDevicePtr GetMainDevice() const;

		SAILOR_API bool IsEnabledValidationLayers() const { return bIsEnabledValidationLayers; }
		SAILOR_API __forceinline static VkInstance& GetVkInstance() { return s_pInstance->m_vkInstance; }

		SAILOR_API static VulkanQueueFamilyIndices FindQueueFamilies(VkPhysicalDevice device, VulkanSurfacePtr surface);

		SAILOR_API static VkFormat SelectFormatByFeatures(VkPhysicalDevice physicalDevice, const TVector<VkFormat>& candidates, VkImageTiling tiling, VkFormatFeatureFlags features);
		SAILOR_API static bool HasStencilComponent(VkFormat format);

		SAILOR_API static SwapChainSupportDetails QuerySwapChainSupport(VkPhysicalDevice device, VulkanSurfacePtr surface);

		SAILOR_API static VkSurfaceFormatKHR ChooseSwapSurfaceFormat(const TVector<VkSurfaceFormatKHR>& availableFormats);
		SAILOR_API static VkPresentModeKHR ÑhooseSwapPresentMode(const TVector<VkPresentModeKHR>& availablePresentModes, bool bVSync);
		SAILOR_API static VkExtent2D ChooseSwapExtent(const VkSurfaceCapabilitiesKHR& capabilities, uint32_t width, uint32_t height);

		SAILOR_API static VkPhysicalDevice PickPhysicalDevice(VulkanSurfacePtr surface);
		SAILOR_API static void GetRequiredExtensions(TVector<const char*>& requiredDeviceExtensions, TVector<const char*>& requiredInstanceExtensions)
		{
			requiredDeviceExtensions =
			{
				VK_EXT_MEMORY_BUDGET_EXTENSION_NAME,
				VK_KHR_SWAPCHAIN_EXTENSION_NAME,
				"VK_KHR_dynamic_rendering",

				//Relax the interface matching rules to allow a larger output vector to match with a smaller input vector, with additional values being discarded.
				VK_KHR_MAINTENANCE_4_EXTENSION_NAME, 

				VK_EXT_SHADER_ATOMIC_FLOAT_EXTENSION_NAME

				// For some reasons on some video cards that is not supported for debug configuration (debug layer collisions?)
				//, VK_EXT_BLEND_OPERATION_ADVANCED_EXTENSION_NAME 
			};
		}

		SAILOR_API static VkAttachmentDescription GetDefaultColorAttachment(VkFormat imageFormat);
		SAILOR_API static VkAttachmentDescription GetDefaultDepthAttachment(VkFormat depthFormat);

		SAILOR_API static VulkanRenderPassPtr CreateDefaultRenderPass(VulkanDevicePtr device, VkFormat imageFormat, VkFormat depthFormat);
		SAILOR_API static VulkanRenderPassPtr CreateMSSRenderPass(VulkanDevicePtr device, VkFormat imageFormat, VkFormat depthFormat, VkSampleCountFlagBits samples);
		SAILOR_API static VulkanImageViewPtr CreateImageView(VulkanDevicePtr device, VulkanImagePtr image, VkImageAspectFlags aspectFlags);
		SAILOR_API static uint32_t FindMemoryByType(VkPhysicalDevice physicalDevice, uint32_t typeFilter, VkMemoryPropertyFlags properties);

		SAILOR_API static bool IsCompatible(const VulkanPipelineLayoutPtr& pipelineLayout, const VulkanDescriptorSetPtr& descriptorSet, uint32_t binding);
		SAILOR_API static TVector<bool> IsCompatible(const VulkanPipelineLayoutPtr& pipelineLayout, const TVector<VulkanDescriptorSetPtr>& descriptorSets);

		SAILOR_API static bool CreateDescriptorSetLayouts(VulkanDevicePtr device,
			const TVector<VulkanShaderStagePtr>& shaders,
			TVector<VulkanDescriptorSetLayoutPtr>& outVulkanLayouts,
			TVector<RHI::ShaderLayoutBinding>& outRhiLayout);

		SAILOR_API static VkDescriptorSetLayoutBinding CreateDescriptorSetLayoutBinding(
			uint32_t              binding = 0,
			VkDescriptorType      descriptorType = VkDescriptorType::VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
			uint32_t              descriptorCount = 1,
			VkShaderStageFlags    stageFlags = VK_SHADER_STAGE_COMPUTE_BIT | VK_SHADER_STAGE_FRAGMENT_BIT | VK_SHADER_STAGE_VERTEX_BIT,
			const VkSampler* pImmutableSamplers = nullptr);

		SAILOR_API static VkDescriptorPoolSize CreateDescriptorPoolSize(VkDescriptorType type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, uint32_t count = 1);

		SAILOR_API static VulkanBufferPtr CreateBuffer(VulkanDevicePtr device, VkDeviceSize size, VkBufferUsageFlags usage, VkMemoryPropertyFlags properties, VkSharingMode sharingMode = VkSharingMode::VK_SHARING_MODE_CONCURRENT);
		SAILOR_API static VulkanBufferPtr CreateBuffer(VulkanCommandBufferPtr& cmdBuffer, VulkanDevicePtr device, const void* pData, VkDeviceSize size, VkBufferUsageFlags usage, VkSharingMode sharingMode = VkSharingMode::VK_SHARING_MODE_CONCURRENT);

		SAILOR_API static VulkanImagePtr CreateImage(
			VulkanCommandBufferPtr& cmdBuffer,
			VulkanDevicePtr device,
			const void* pData,
			VkDeviceSize size,
			VkExtent3D extent,
			uint32_t mipLevels = 1,
			VkImageType type = VK_IMAGE_TYPE_2D,
			VkFormat format = VK_FORMAT_R8G8B8A8_SRGB,
			VkImageTiling tiling = VK_IMAGE_TILING_OPTIMAL,
			VkImageUsageFlags usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
			VkSharingMode sharingMode = VkSharingMode::VK_SHARING_MODE_EXCLUSIVE,
			VkImageLayout defaultLayout = VkImageLayout::VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
			VkImageCreateFlags flags = 0,
			uint32_t arrayLayers = 1);

		SAILOR_API static VulkanImagePtr CreateImage(
			VulkanDevicePtr device,
			VkExtent3D extent,
			uint32_t mipLevels = 1,
			VkImageType type = VK_IMAGE_TYPE_2D,
			VkFormat format = VK_FORMAT_R8G8B8A8_SRGB,
			VkImageTiling tiling = VK_IMAGE_TILING_OPTIMAL,
			VkImageUsageFlags usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
			VkSharingMode sharingMode = VkSharingMode::VK_SHARING_MODE_EXCLUSIVE,
			VkSampleCountFlagBits sampleCount = VkSampleCountFlagBits::VK_SAMPLE_COUNT_1_BIT,
			VkImageLayout defaultLayout = VkImageLayout::VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
			VkImageCreateFlags flags = 0,
			uint32_t arrayLayers = 1);

		SAILOR_API static VulkanCommandBufferPtr UpdateBuffer(VulkanDevicePtr device, const VulkanBufferMemoryPtr& dst, const void* pData, VkDeviceSize size);

		//Immediate context
		SAILOR_API static VulkanBufferPtr CreateBuffer_Immediate(VulkanDevicePtr device, const void* pData, VkDeviceSize size, VkBufferUsageFlags usage, VkSharingMode sharingMode = VkSharingMode::VK_SHARING_MODE_CONCURRENT);
		SAILOR_API static void CopyBuffer_Immediate(VulkanDevicePtr device, VulkanBufferMemoryPtr  src, VulkanBufferMemoryPtr dst, VkDeviceSize size, VkDeviceSize srcOffset = 0, VkDeviceSize dstOffset = 0);

		SAILOR_API static VulkanImagePtr CreateImage_Immediate(
			VulkanDevicePtr device,
			const void* pData,
			VkDeviceSize size,
			VkExtent3D extent,
			uint32_t mipLevels = 1,
			VkImageType type = VK_IMAGE_TYPE_2D,
			VkFormat format = VK_FORMAT_R8G8B8A8_SRGB,
			VkImageTiling tiling = VK_IMAGE_TILING_OPTIMAL,
			VkImageUsageFlags usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
			VkSharingMode sharingMode = VkSharingMode::VK_SHARING_MODE_EXCLUSIVE,
			VkImageLayout defaultLayout = VkImageLayout::VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);

		//Immediate context

		SAILOR_API static VkImageAspectFlags ComputeAspectFlagsForFormat(VkFormat format);
		SAILOR_API static VkAccessFlags ComputeAccessFlagsForFormat(VkFormat format);

		SAILOR_API static VkVertexInputBindingDescription GetBindingDescription(const RHI::RHIVertexDescriptionPtr& vertexDescription);
		SAILOR_API static TVector<VkVertexInputAttributeDescription> GetAttributeDescriptions(const RHI::RHIVertexDescriptionPtr& vertexDescription);

	private:

		SAILOR_API static uint32_t GetNumSupportedExtensions();
		SAILOR_API static void PrintSupportedExtensions();

		SAILOR_API static bool CheckDeviceExtensionSupport(VkPhysicalDevice device);
		SAILOR_API static bool CheckValidationLayerSupport(const TVector<const char*>& validationLayers);

		SAILOR_API static bool IsDeviceSuitable(VkPhysicalDevice device, VulkanSurfacePtr surface);
		SAILOR_API static int32_t GetDeviceScore(VkPhysicalDevice device);

		SAILOR_API static bool SetupDebugCallback();
		SAILOR_API static VkResult CreateDebugUtilsMessengerEXT(VkInstance instance, const VkDebugUtilsMessengerCreateInfoEXT* pCreateInfo, const	VkAllocationCallbacks* pAllocator, VkDebugUtilsMessengerEXT* pDebugMessenger);
		SAILOR_API static void DestroyDebugUtilsMessengerEXT(VkInstance instance, VkDebugUtilsMessengerEXT debugMessenger, const	VkAllocationCallbacks* pAllocator);

		VkDebugUtilsMessengerEXT m_debugMessenger = 0;
		bool bIsEnabledValidationLayers = false;

		VkInstance m_vkInstance = 0;
		VulkanDevicePtr m_device;
	};
}

bool operator==(const VkDescriptorSetLayoutBinding& lhs, const VkDescriptorSetLayoutBinding& rhs);
