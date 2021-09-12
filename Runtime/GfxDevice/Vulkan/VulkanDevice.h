#pragma once
#include "VulkanApi.h"
#include "Core/RefPtr.hpp"

class Sailor::Win32::Window;

namespace Sailor::GfxDevice::Vulkan
{
	class VulkanCommandBuffer;
	class VulkanCommandPool;
	class VulkanQueue;
	class VulkanDevice;
	class VulkanFence;
	class VulkanSemaphore;
	class VulkanRenderPass;
	class VulkanSwapchain;
	class VulkanSurface;
	class VulkanFramebuffer;
	class VulkanDeviceMemory;
	class VulkanBuffer;

	class VulkanDevice final : public TRefBase
	{

	public:

		operator VkDevice() const { return m_device; }

		VulkanDevice(const Win32::Window* pViewport);
		void Shutdown();
		virtual ~VulkanDevice();

		SAILOR_API VkPhysicalDevice GetPhysicalDevice() const { return m_physicalDevice; }

		void SAILOR_API WaitIdle();
		void SAILOR_API WaitIdlePresentQueue();
		void SAILOR_API DrawFrame(Win32::Window* pViewport);

		SAILOR_API TRefPtr<VulkanSurface> GetSurface() const;
		SAILOR_API TRefPtr<VulkanCommandBuffer> CreateCommandBuffer(bool bOnlyTransferQueue = false);

		SAILOR_API void SubmitCommandBuffer(TRefPtr<VulkanCommandBuffer> commandBuffer, TRefPtr<VulkanFence> fence = nullptr) const;

		SAILOR_API const VulkanQueueFamilyIndices& GetQueueFamilies() const { return m_queueFamilies; }

	protected:

		SAILOR_API void CreateLogicalDevice(VkPhysicalDevice physicalDevice);
		SAILOR_API void CreateWin32Surface(const Win32::Window* pViewport);
		SAILOR_API void CreateSwapchain(const Win32::Window* pViewport);
		SAILOR_API bool RecreateSwapchain(Win32::Window* pViewport);
		SAILOR_API void CreateGraphicsPipeline();
		SAILOR_API void CreateRenderPass();
		SAILOR_API void CreateFramebuffers();
		SAILOR_API void CreateCommandPool();
		SAILOR_API void CreateCommandBuffers();
		SAILOR_API void CreateFrameSyncSemaphores();
		SAILOR_API void CleanupSwapChain();
		SAILOR_API void CreateVertexBuffer();

		// Command pool
		TRefPtr<VulkanCommandPool> m_commandPool;
		TRefPtr<VulkanCommandPool> m_transferCommandPool;

		std::vector<TRefPtr<VulkanCommandBuffer>> m_commandBuffers;

		// Render Pass
		TRefPtr<VulkanRenderPass> m_renderPass;

		// Pipeline
		VkPipelineLayout m_pipelineLayout = 0;
		VkPipeline m_graphicsPipeline = 0;

		// Queuees
		TRefPtr<VulkanQueue> m_graphicsQueue;
		TRefPtr<VulkanQueue> m_presentQueue;
		TRefPtr<VulkanQueue> m_transferQueue;

		TRefPtr<VulkanSurface> m_surface;

		VkPhysicalDevice m_physicalDevice = 0;
		VkDevice m_device = 0;
		VulkanQueueFamilyIndices m_queueFamilies;

		// Swapchain
		TRefPtr<VulkanSwapchain> m_swapchain;
		std::vector<TRefPtr<VulkanFramebuffer>> m_swapChainFramebuffers;

		// Frame sync
		std::vector<TRefPtr<VulkanSemaphore>> m_imageAvailableSemaphores;
		std::vector<TRefPtr<VulkanSemaphore>> m_renderFinishedSemaphores;
		std::vector<TRefPtr<VulkanFence>> m_syncFences;
		std::vector<TRefPtr<VulkanFence>> m_syncImages;
		size_t m_currentFrame = 0;

		TRefPtr<VulkanBuffer> m_vertexBuffer;
		TRefPtr<VulkanBuffer> m_indexBuffer;

		std::atomic<bool> bIsFramebufferResizedThisFrame = false;
	};
}