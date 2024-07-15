#pragma once
#include "Containers/Map.h"
#include "Containers/ConcurrentMap.h"
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
class FrameState;

namespace Win32
{
	class Window;
}

using namespace Sailor::Memory;

namespace Sailor::GraphicsDriver::Vulkan
{
	using VulkanDeviceMemoryAllocator = TBlockAllocator<Sailor::Memory::GlobalVulkanMemoryAllocator, VulkanMemoryPtr>;
	using VulkanBufferAllocator = TBlockAllocator<Sailor::Memory::GlobalVulkanBufferAllocator, VulkanBufferMemoryPtr>;

	// Thread independent resources
	struct ThreadContext
	{
		VulkanCommandPoolPtr m_commandPool;
		VulkanCommandPoolPtr m_transferCommandPool;
		VulkanCommandPoolPtr m_computeCommandPool;
		VulkanDescriptorPoolPtr m_descriptorPool;
		TSharedPtr<VulkanBufferAllocator> m_stagingBufferAllocator;
	};

	class VulkanDevice final : public RHI::RHIResource
	{

	public:

		SAILOR_API operator VkDevice() const { return m_device; }

		SAILOR_API VulkanDevice(Win32::Window* pViewport, RHI::EMsaaSamples requestMsaa);
		SAILOR_API virtual ~VulkanDevice();

		SAILOR_API void BeginConditionalDestroy();
		SAILOR_API void Shutdown();

		SAILOR_API VkPhysicalDevice GetPhysicalDevice() const { return m_physicalDevice; }
		SAILOR_API VulkanSurfacePtr GetSurface() const;
		SAILOR_API VulkanRenderPassPtr GetRenderPass() { return m_renderPass; }
		SAILOR_API const VulkanQueueFamilyIndices& GetQueueFamilies() const { return m_queueFamilies; }
		SAILOR_API const TUniquePtr<VulkanSamplerCache>& GetSamplers() const { return m_samplers; }
		SAILOR_API TUniquePtr<VulkanPipelineStateBuilder>& GetPipelineBuilder() { return m_pipelineBuilder; }

		SAILOR_API void WaitIdle();
		SAILOR_API void WaitIdlePresentQueue();

		SAILOR_API bool AcquireNextImage();

		SAILOR_API VulkanImageViewPtr GetBackBuffer() const;
		SAILOR_API VulkanImageViewPtr GetDepthBuffer() const;

		SAILOR_API bool PresentFrame(const FrameState& state, const TVector<VulkanCommandBufferPtr>& primaryCommandBuffers, const TVector<VulkanSemaphorePtr>& waitSemaphores);

		SAILOR_API bool IsSwapChainOutdated() const { return m_bIsSwapChainOutdated; }
		SAILOR_API VulkanCommandBufferPtr CreateCommandBuffer(RHI::ECommandListQueue queue = RHI::ECommandListQueue::Graphics);

		SAILOR_API bool SubmitCommandBuffer(VulkanCommandBufferPtr commandBuffer,
			VulkanFencePtr fence = nullptr,
			TVector<VulkanSemaphorePtr> signalSemaphores = {},
			TVector<VulkanSemaphorePtr> waitSemaphores = {});

		SAILOR_API bool ShouldFixLostDevice(const Win32::Window* pViewport);
		SAILOR_API void FixLostDevice(Win32::Window* pViewport);

		SAILOR_API bool IsMultiDrawIndirectSupported() const { return m_bSupportsMultiDrawIndirect; };
		SAILOR_API float GetMaxAllowedAnisotropy() const { return m_physicalDeviceProperties.limits.maxSamplerAnisotropy; };
		SAILOR_API VkSampleCountFlagBits GetMaxAllowedMsaaSamples() const { return m_maxAllowedMsaaSamples; };
		SAILOR_API VkSampleCountFlagBits GetCurrentMsaaSamples() const { return m_currentMsaaSamples; };
		SAILOR_API const VkMemoryRequirements& GetMemoryRequirements_StagingBuffer() const { return m_memoryRequirements_StagingBuffer; }
		SAILOR_API const VkDeviceSize& GetMinUboOffsetAlignment() const { return m_physicalDeviceProperties.limits.minUniformBufferOffsetAlignment; }
		SAILOR_API const VkDeviceSize& GetMinSsboOffsetAlignment() const { return m_physicalDeviceProperties.limits.minStorageBufferOffsetAlignment; }
		SAILOR_API const VkDeviceSize& GetBufferImageGranuality() const { return m_physicalDeviceProperties.limits.bufferImageGranularity; }

		template<typename TData>
		VkDeviceSize GetUboOffsetAlignment() const
		{
			return GetUboOffsetAlignment(sizeof(TData));
		}

		VkDeviceSize GetUboOffsetAlignment(size_t size) const
		{
			VkDeviceSize dynamicAlignment = size;
			const VkDeviceSize& uboOffsetAlignment = GetMinUboOffsetAlignment();
			if (uboOffsetAlignment > 0)
			{
				dynamicAlignment = (dynamicAlignment + uboOffsetAlignment - 1) & ~(uboOffsetAlignment - 1);
			}

			return dynamicAlignment;
		}

		VkDeviceSize GetSsboOffsetAlignment(size_t size) const
		{
			VkDeviceSize dynamicAlignment = size;
			const VkDeviceSize& ssbboOffsetAlignment = GetMinSsboOffsetAlignment();
			if (ssbboOffsetAlignment > 0)
			{
				dynamicAlignment = (dynamicAlignment + ssbboOffsetAlignment - 1) & ~(ssbboOffsetAlignment - 1);
			}

			return dynamicAlignment;
		}

		SAILOR_API VkFormat GetColorFormat() const;
		SAILOR_API VkFormat GetDepthFormat() const;
		SAILOR_API bool IsMipsSupported(VkFormat format) const;

		SAILOR_API ThreadContext& GetCurrentThreadContext();
		SAILOR_API ThreadContext& GetOrAddThreadContext(DWORD threadId);
		SAILOR_API VulkanDeviceMemoryAllocator& GetMemoryAllocator(VkMemoryPropertyFlags properties, VkMemoryRequirements requirements);
		SAILOR_API TSharedPtr<VulkanBufferAllocator> GetStagingBufferAllocator() { return GetCurrentThreadContext().m_stagingBufferAllocator; }
		SAILOR_API VulkanStateViewportPtr GetCurrentFrameViewport() const { return m_pCurrentFrameViewport; }
		SAILOR_API const auto& GetMemoryAllocators() const { return m_memoryAllocators; }

		SAILOR_API void GetOccupiedVideoMemory(VkMemoryHeapFlags memFlags, size_t& outHeapBudget, size_t& outHeapUsage);

		// Device specific extensions
		SAILOR_API void vkCmdBeginRenderingKHR(VkCommandBuffer commandBuffer, const VkRenderingInfo* pRenderingInfo);
		SAILOR_API void vkCmdEndRenderingKHR(VkCommandBuffer commandBuffer);

		SAILOR_API VulkanSwapchainPtr GetSwapchain() { return m_swapchain; }

		SAILOR_API uint32_t GetNumSubmittedCommandBufers() const { return m_numSubmittedCommandBuffers; }

		SAILOR_API VulkanQueuePtr GetGraphicsQueue() { return m_graphicsQueue; }

		// Debug specific extensions
		SAILOR_API void vkCmdDebugMarkerBegin(VulkanCommandBufferPtr cmdBuffer, const VkDebugMarkerMarkerInfoEXT* markerInfo);
		SAILOR_API void vkCmdDebugMarkerEnd(VulkanCommandBufferPtr cmdBuffer);
		SAILOR_API void SetDebugName(VkObjectType type, uint64_t objectHandle, const std::string& name);

	protected:

		SAILOR_API TUniquePtr<ThreadContext> CreateThreadContext();

		SAILOR_API void CreateLogicalDevice(VkPhysicalDevice physicalDevice);
		SAILOR_API void CreateWin32Surface(const Win32::Window* pViewport);
		SAILOR_API void CreateSwapchain(Win32::Window* pViewport);
		SAILOR_API bool RecreateSwapchain(Win32::Window* pViewport);
		SAILOR_API void CreateDefaultRenderPass();
		SAILOR_API void CreateFrameDependencies();
		SAILOR_API void CreateFrameSyncSemaphores();
		SAILOR_API void CleanupSwapChain();

		VkPhysicalDeviceProperties m_physicalDeviceProperties{};

		VkSampleCountFlagBits m_maxAllowedMsaaSamples = VK_SAMPLE_COUNT_1_BIT;
		VkSampleCountFlagBits m_currentMsaaSamples = VK_SAMPLE_COUNT_1_BIT;
		bool m_bSupportsMultiDrawIndirect = false;

		VkMemoryRequirements m_memoryRequirements_StagingBuffer;

		// Internal command buffers
		VulkanCommandPoolPtr m_commandPool;
		TVector<TVector<RHI::RHIResourcePtr>> m_frameDeps;

		// Render Pass
		VulkanRenderPassPtr m_renderPass;

		// We have one global present queeueu
		VulkanQueuePtr m_presentQueue;
		VulkanQueuePtr m_graphicsQueue;
		VulkanQueuePtr m_computeQueue;
		VulkanQueuePtr m_transferQueue;

		VulkanSurfacePtr m_surface;

		VkPhysicalDevice m_physicalDevice = 0;
		VkDevice m_device = 0;
		VulkanQueueFamilyIndices m_queueFamilies;

		// Swapchain
		VulkanSwapchainPtr m_swapchain;
		VulkanSwapchainPtr m_oldSwapchain;

		// TODO: Use VK_NV_inherited_viewport_scissor extension if possible
		// TODO: Move current viewport settings to rendering pipeline
		VulkanStateViewportPtr m_pCurrentFrameViewport;

		// Frame sync
		TVector<VulkanSemaphorePtr> m_imageAvailableSemaphores;
		TVector<VulkanSemaphorePtr> m_renderFinishedSemaphores;
		TVector<VulkanFencePtr> m_syncFences;
		TVector<VulkanFencePtr> m_syncImages;
		size_t m_currentFrame = 0;
		uint32_t m_currentSwapchainImageIndex = 0;
		bool m_bNeedToTransitSwapchainToPresent = true;

		std::atomic<bool> m_bIsSwapChainOutdated = true;

		TUniquePtr<VulkanSamplerCache> m_samplers;
		TUniquePtr<VulkanPipelineStateBuilder> m_pipelineBuilder;

		TConcurrentMap<DWORD, TUniquePtr<ThreadContext>> m_threadContext;

		// We're sharing the same device memory between the different buffers
		TConcurrentMap<uint64_t, TUniquePtr<VulkanDeviceMemoryAllocator>, 16u, ERehashPolicy::Never> m_memoryAllocators;

		// Dynamic rendering extension
		PFN_vkCmdBeginRenderingKHR pVkCmdBeginRenderingKHR;
		PFN_vkCmdEndRenderingKHR pVkCmdEndRenderingKHR;

		PFN_vkSetDebugUtilsObjectNameEXT m_pSetDebugUtilsObjectNameEXT{};
		PFN_vkCmdDebugMarkerBeginEXT m_pCmdDebugMarkerBegin{};
		PFN_vkCmdDebugMarkerEndEXT m_pCmdDebugMarkerEnd{};
		PFN_vkCmdDebugMarkerInsertEXT m_pCmdDebugMarkerInsert{};

		TSet<string> supportedDeviceExtensions{};

		// Stats
		uint32_t m_numSubmittedCommandBuffersAcc = 0;
		uint32_t m_numSubmittedCommandBuffers = 0;

		bool m_bIsDeviceLost = false;
	};
}