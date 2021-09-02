#pragma once
#include "VulkanApi.h"
#include "Core/RefPtr.hpp"

class Sailor::Window;
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

	class VulkanDevice final : public TRefBase
	{

	public:
		
		operator VkDevice() const { return m_device; }

		VulkanDevice(const Window* pViewport);
		virtual ~VulkanDevice();

		void SAILOR_API WaitIdle();
		void SAILOR_API WaitIdlePresentQueue();
		void SAILOR_API DrawFrame(Window* pViewport);
		
		TRefPtr<VulkanSurface> SAILOR_API GetSurface() const;

	protected:
		
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

		// Command pool
		TRefPtr<VulkanCommandPool> m_commandPool;
		std::vector<TRefPtr<VulkanCommandBuffer>> m_commandBuffers;

		// Render Pass
		TRefPtr<VulkanRenderPass> m_renderPass;

		// Pipeline
		VkPipelineLayout m_pipelineLayout = 0;
		VkPipeline m_graphicsPipeline = 0;

		// Queuees
		TRefPtr<VulkanQueue> m_graphicsQueue;
		TRefPtr<VulkanQueue> m_presentQueue;

		TRefPtr<VulkanSurface> m_surface;

		VkPhysicalDevice m_mainPhysicalDevice = 0;
		VkDevice m_device = 0;
		QueueFamilyIndices m_queueFamilies;

		// Swapchain
		TRefPtr<VulkanSwapchain> m_swapchain;
		std::vector<TRefPtr<VulkanFramebuffer>> m_swapChainFramebuffers;

		// Frame sync
		std::vector<TRefPtr<VulkanSemaphore>> m_imageAvailableSemaphores;
		std::vector<TRefPtr<VulkanSemaphore>> m_renderFinishedSemaphores;
		std::vector<TRefPtr<VulkanFence>> m_syncFences;
		std::vector<TRefPtr<VulkanFence>> m_syncImages;
		size_t m_currentFrame = 0;

		std::atomic<bool> bIsFramebufferResizedThisFrame = false;
	};
}