#pragma once
#include <unordered_map>
#include "VulkanApi.h"
#include "VulkanDescriptors.h"
#include "VulkanSamplers.h"
#include "Core/RefPtr.hpp"
#include "Core/UniquePtr.hpp"
#include "RHI/Types.h"
#include "VulkanMemory.h"

using namespace Sailor;
using namespace Sailor::Memory;
class Sailor::FrameState;
class Sailor::Win32::Window;

namespace Sailor::GfxDevice::Vulkan
{
	// Thread independent resources
	struct ThreadContext
	{
		VulkanCommandPoolPtr m_commandPool;
		VulkanCommandPoolPtr m_transferCommandPool;
		VulkanDescriptorPoolPtr m_descriptorPool;
	};

	class VulkanDevice final : public RHI::Resource
	{

	public:

		operator VkDevice() const { return m_device; }

		VulkanDevice(const Win32::Window* pViewport, RHI::EMsaaSamples requestMsaa);
		virtual ~VulkanDevice();
		void Shutdown();

		SAILOR_API VkPhysicalDevice GetPhysicalDevice() const { return m_physicalDevice; }

		void SAILOR_API WaitIdle();
		void SAILOR_API WaitIdlePresentQueue();
		bool SAILOR_API PresentFrame(const FrameState& state, const std::vector<VulkanCommandBufferPtr>* primaryCommandBuffers = nullptr,
			const std::vector<VulkanCommandBufferPtr>* secondaryCommandBuffers = nullptr,
			const std::vector<VulkanSemaphorePtr>* waitSemaphores = nullptr);

		bool SAILOR_API IsSwapChainOutdated() const { return m_bIsSwapChainOutdated; }

		SAILOR_API VulkanSurfacePtr GetSurface() const;
		SAILOR_API VulkanCommandBufferPtr CreateCommandBuffer(bool bOnlyTransferQueue = false);

		SAILOR_API void SubmitCommandBuffer(VulkanCommandBufferPtr commandBuffer,
			VulkanFencePtr fence = nullptr,
			std::vector<VulkanSemaphorePtr> signalSemaphores = {},
			std::vector<VulkanSemaphorePtr> waitSemaphores = {});

		SAILOR_API const VulkanQueueFamilyIndices& GetQueueFamilies() const { return m_queueFamilies; }

		SAILOR_API bool ShouldFixLostDevice(const Win32::Window* pViewport);
		SAILOR_API void FixLostDevice(const Win32::Window* pViewport);

		SAILOR_API float GetMaxAllowedAnisotropy() const { return m_maxAllowedAnisotropy; };
		SAILOR_API VkSampleCountFlagBits GetMaxAllowedMsaaSamples() const { return m_maxAllowedMsaaSamples; };
		SAILOR_API VkSampleCountFlagBits GetCurrentMsaaSamples() const { return m_currentMsaaSamples; };
		SAILOR_API const VkMemoryRequirements& GetMemoryRequirements_StagingBuffer() const { return m_memoryRequirements_StagingBuffer; }
		SAILOR_API const VkDeviceSize& GetMinUboOffsetAlignment() const { return m_minUboOffsetAlignment; }

		template<typename TData>
		VkDeviceSize GetUboOffsetAlignment() const
		{
			VkDeviceSize dynamicAlignment = sizeof(TData);
			if (m_minUboOffsetAlignment > 0)
			{
				dynamicAlignment = (dynamicAlignment + m_minUboOffsetAlignment - 1) & ~(m_minUboOffsetAlignment - 1);
			}

			return dynamicAlignment;
		}

		SAILOR_API VkFormat GetDepthFormat() const;
		SAILOR_API bool IsMipsSupported(VkFormat format) const;

		SAILOR_API ThreadContext& GetThreadContext();

		SAILOR_API void CreateGraphicsPipeline();

		SAILOR_API TBlockAllocator<class GlobalVulkanAllocator, class VulkanMemoryPtr>& GetMemoryAllocator(VkMemoryPropertyFlags properties, VkMemoryRequirements requirements);

	protected:

		SAILOR_API TUniquePtr<ThreadContext> CreateThreadContext();

		SAILOR_API void CreateLogicalDevice(VkPhysicalDevice physicalDevice);
		SAILOR_API void CreateWin32Surface(const Win32::Window* pViewport);
		SAILOR_API void CreateSwapchain(const Win32::Window* pViewport);
		SAILOR_API bool RecreateSwapchain(const Win32::Window* pViewport);
		SAILOR_API void CreateRenderPass();
		SAILOR_API void CreateFramebuffers();
		SAILOR_API void CreateCommandBuffers();
		SAILOR_API void CreateFrameSyncSemaphores();
		SAILOR_API void CleanupSwapChain();

		float m_maxAllowedAnisotropy = 0;
		VkSampleCountFlagBits m_maxAllowedMsaaSamples = VK_SAMPLE_COUNT_1_BIT;
		VkSampleCountFlagBits m_currentMsaaSamples = VK_SAMPLE_COUNT_1_BIT;
		VkDeviceSize m_minUboOffsetAlignment = 0;

		VkMemoryRequirements m_memoryRequirements_StagingBuffer;

		std::vector<VulkanCommandBufferPtr> m_commandBuffers;

		// Render Pass
		VulkanRenderPassPtr m_renderPass;

		// Queuees
		VulkanQueuePtr m_graphicsQueue;
		VulkanQueuePtr m_presentQueue;
		VulkanQueuePtr m_transferQueue;

		VulkanSurfacePtr m_surface;

		VkPhysicalDevice m_physicalDevice = 0;
		VkDevice m_device = 0;
		VulkanQueueFamilyIndices m_queueFamilies;

		// Swapchain
		TRefPtr<VulkanSwapchain> m_swapchain;
		std::vector<VulkanFramebufferPtr> m_swapChainFramebuffers;

		// Frame sync
		std::vector<VulkanSemaphorePtr> m_imageAvailableSemaphores;
		std::vector<VulkanSemaphorePtr> m_renderFinishedSemaphores;
		std::vector<VulkanFencePtr> m_syncFences;
		std::vector<VulkanFencePtr> m_syncImages;
		size_t m_currentFrame = 0;

		std::atomic<bool> m_bIsSwapChainOutdated = true;

		TUniquePtr<VulkanSamplers> m_samplers;

		// Custom testing code
		VulkanPipelineLayoutPtr m_pipelineLayout = nullptr;
		VulkanPipelinePtr m_graphicsPipeline = nullptr;

		VulkanBufferPtr m_uniformBuffer;
		VulkanDescriptorSetPtr m_descriptorSet;

		VulkanImagePtr m_image;
		VulkanImageViewPtr m_imageView;

		std::unordered_map<DWORD, TUniquePtr<ThreadContext>> m_threadContext;

		std::unordered_map<uint64_t, TBlockAllocator<GlobalVulkanAllocator, VulkanMemoryPtr>> m_memoryAllocators;
	};
}