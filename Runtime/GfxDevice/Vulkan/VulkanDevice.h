#pragma once
#include <unordered_map>
#include "VulkanApi.h"
#include "VulkanDescriptors.h"
#include "VulkanSamplers.h"
#include "Memory/RefPtr.hpp"
#include "Memory/UniquePtr.hpp"
#include "RHI/Types.h"
#include "VulkanMemory.h"
#include "VulkanBufferMemory.h"
#include "VulkanPipileneStates.h"

using namespace Sailor;
using namespace Sailor::Memory;
class Sailor::FrameState;
class Sailor::Win32::Window;

namespace Sailor::GfxDevice::Vulkan
{
	using VulkanDeviceMemoryAllocator = TBlockAllocator<Sailor::Memory::GlobalVulkanMemoryAllocator, VulkanMemoryPtr>;
	using VulkanBufferAllocator = TBlockAllocator<Sailor::Memory::GlobalVulkanBufferAllocator, VulkanBufferMemoryPtr>;

	// Thread independent resources
	struct ThreadContext
	{
		VulkanCommandPoolPtr m_commandPool;
		VulkanCommandPoolPtr m_transferCommandPool;
		VulkanDescriptorPoolPtr m_descriptorPool;
		TSharedPtr<VulkanBufferAllocator> m_stagingBufferAllocator;
	};

	class VulkanDevice final : public RHI::Resource
	{

	public:

		operator VkDevice() const { return m_device; }

		VulkanDevice(const Win32::Window* pViewport, RHI::EMsaaSamples requestMsaa);
		virtual ~VulkanDevice();
		void Shutdown();

		SAILOR_API VkPhysicalDevice GetPhysicalDevice() const { return m_physicalDevice; }
		SAILOR_API VulkanSurfacePtr GetSurface() const;
		SAILOR_API VulkanRenderPassPtr GetRenderPass() { return m_renderPass; }
		SAILOR_API const VulkanQueueFamilyIndices& GetQueueFamilies() const { return m_queueFamilies; }
		SAILOR_API const TUniquePtr<VulkanSamplerCache>& GetSamplers() const { return m_samplers; }
		SAILOR_API TUniquePtr<VulkanPipelineStateBuilder>& GetPipelineBuilder() { return m_pipelineBuilder; }

		void SAILOR_API WaitIdle();
		void SAILOR_API WaitIdlePresentQueue();
		bool SAILOR_API PresentFrame(const FrameState& state, std::vector<VulkanCommandBufferPtr> primaryCommandBuffers = {},
			std::vector<VulkanCommandBufferPtr> secondaryCommandBuffers = {},
			std::vector<VulkanSemaphorePtr> waitSemaphores = {});

		bool SAILOR_API IsSwapChainOutdated() const { return m_bIsSwapChainOutdated; }
		
		SAILOR_API VulkanCommandBufferPtr CreateCommandBuffer(bool bOnlyTransferQueue = false);

		SAILOR_API void SubmitCommandBuffer(VulkanCommandBufferPtr commandBuffer,
			VulkanFencePtr fence = nullptr,
			std::vector<VulkanSemaphorePtr> signalSemaphores = {},
			std::vector<VulkanSemaphorePtr> waitSemaphores = {});

		SAILOR_API bool ShouldFixLostDevice(const Win32::Window* pViewport);
		SAILOR_API void FixLostDevice(const Win32::Window* pViewport);

		SAILOR_API float GetMaxAllowedAnisotropy() const { return m_maxAllowedAnisotropy; };
		SAILOR_API VkSampleCountFlagBits GetMaxAllowedMsaaSamples() const { return m_maxAllowedMsaaSamples; };
		SAILOR_API VkSampleCountFlagBits GetCurrentMsaaSamples() const { return m_currentMsaaSamples; };
		SAILOR_API const VkMemoryRequirements& GetMemoryRequirements_StagingBuffer() const { return m_memoryRequirements_StagingBuffer; }
		SAILOR_API const VkDeviceSize& GetMinUboOffsetAlignment() const { return m_minUboOffsetAlignment; }
		SAILOR_API const VkDeviceSize& GetMinSsboOffsetAlignment() const { return m_minStorageBufferOffsetAlignment; }

		template<typename TData>
		VkDeviceSize GetUboOffsetAlignment() const
		{
			return GetUboOffsetAlignment(sizeof(TData));
		}

		VkDeviceSize GetUboOffsetAlignment(size_t size) const
		{
			VkDeviceSize dynamicAlignment = size;
			if (m_minUboOffsetAlignment > 0)
			{
				dynamicAlignment = (dynamicAlignment + m_minUboOffsetAlignment - 1) & ~(m_minUboOffsetAlignment - 1);
			}

			return dynamicAlignment;
		}

		VkDeviceSize GetSsboOffsetAlignment(size_t size) const
		{
			VkDeviceSize dynamicAlignment = size;
			if (m_minStorageBufferOffsetAlignment > 0)
			{
				dynamicAlignment = (dynamicAlignment + m_minStorageBufferOffsetAlignment - 1) & ~(m_minStorageBufferOffsetAlignment - 1);
			}

			return dynamicAlignment;
		}

		SAILOR_API VkFormat GetDepthFormat() const;
		SAILOR_API bool IsMipsSupported(VkFormat format) const;

		SAILOR_API ThreadContext& GetCurrentThreadContext();
		SAILOR_API ThreadContext& GetOrCreateThreadContext(DWORD threadId);
		SAILOR_API VulkanDeviceMemoryAllocator& GetMemoryAllocator(VkMemoryPropertyFlags properties, VkMemoryRequirements requirements);
		SAILOR_API TSharedPtr<VulkanBufferAllocator> GetStagingBufferAllocator() { return GetCurrentThreadContext().m_stagingBufferAllocator; }

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
		VkDeviceSize m_minUboOffsetAlignment = 1;
		VkDeviceSize m_minStorageBufferOffsetAlignment = 1;

		VkMemoryRequirements m_memoryRequirements_StagingBuffer;

		// Internal command buffers
		VulkanCommandPoolPtr m_commandPool;
		std::vector<VulkanCommandBufferPtr> m_commandBuffers;

		// Render Pass
		VulkanRenderPassPtr m_renderPass;

		// We have one global present queeueu
		VulkanQueuePtr m_presentQueue;
		VulkanQueuePtr m_graphicsQueue;
		VulkanQueuePtr m_transferQueue;

		VulkanSurfacePtr m_surface;

		VkPhysicalDevice m_physicalDevice = 0;
		VkDevice m_device = 0;
		VulkanQueueFamilyIndices m_queueFamilies;

		// Swapchain
		VulkanSwapchainPtr m_swapchain;
		std::vector<VulkanFramebufferPtr> m_swapChainFramebuffers;

		// Frame sync
		std::vector<VulkanSemaphorePtr> m_imageAvailableSemaphores;
		std::vector<VulkanSemaphorePtr> m_renderFinishedSemaphores;
		std::vector<VulkanFencePtr> m_syncFences;
		std::vector<VulkanFencePtr> m_syncImages;
		size_t m_currentFrame = 0;

		std::atomic<bool> m_bIsSwapChainOutdated = true;

		TUniquePtr<VulkanSamplerCache> m_samplers;
		TUniquePtr<VulkanPipelineStateBuilder> m_pipelineBuilder;

		std::unordered_map<DWORD, TUniquePtr<ThreadContext>> m_threadContext;

		// We're sharing the same device memory between the different buffers
		std::unordered_map<uint64_t, VulkanDeviceMemoryAllocator> m_memoryAllocators;

		// We're creating rendering context with sync
		std::mutex m_mutex;
	};
}