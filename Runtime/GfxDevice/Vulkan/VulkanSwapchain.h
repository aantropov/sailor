#pragma once
#include "Core/RefPtr.hpp"
#include "VulkanImage.h"
#include "VulkanApi.h"

namespace Sailor::GfxDevice::Vulkan
{
	class VulkanFence;
	class VulkanSemaphore;
	class VulkanImageView;

	class VulkanSurface : public RHI::Resource
	{
	public:
		VulkanSurface(VkSurfaceKHR surface, VkInstance instance);

		operator VkSurfaceKHR() const { return m_surface; }

	protected:
		virtual ~VulkanSurface();

		VkSurfaceKHR m_surface;
		VkInstance m_instance;
	};

	class VulkanSwapchainImage : public VulkanImage
	{
	public:
		VulkanSwapchainImage(VkImage image, TRefPtr<VulkanDevice> device);

	protected:
		virtual ~VulkanSwapchainImage();
	};

	class VulkanSwapchain final : public RHI::Resource
	{
	public:

		VulkanSwapchain(TRefPtr<VulkanDevice> device, uint32_t width, uint32_t height, bool bIsVSync, TRefPtr<VulkanSwapchain> oldSwapchain);

		operator VkSwapchainKHR() const { return m_swapchain; }

		VkFormat GetImageFormat() const { return m_surfaceFormat.format; }

		const VkExtent2D& GetExtent() const { return m_swapchainExtent; }

		TRefPtr<VulkanImageView> GetDepthBufferView() const { return m_depthBufferView; }
		TRefPtr<VulkanImageView> GetColorBufferView() const { return m_colorBufferView; }

		std::vector<TRefPtr<VulkanImageView>>& GetImageViews() { return m_swapchainImageViews; }
		const std::vector<TRefPtr<VulkanImageView>>& GetImageViews() const { return m_swapchainImageViews; }

		VkResult AcquireNextImage(uint64_t timeout, TRefPtr<VulkanSemaphore> semaphore, TRefPtr<VulkanFence> fence, uint32_t& imageIndex);

	protected:
		virtual ~VulkanSwapchain();

		TRefPtr<VulkanDevice> m_device;
		TRefPtr<VulkanSurface> m_surface;
		VkSwapchainKHR m_swapchain = 0;

		VkExtent2D m_swapchainExtent;
		VkSurfaceFormatKHR m_surfaceFormat;
		VkPresentModeKHR m_presentMode;

		std::vector<TRefPtr<VulkanSwapchainImage>> m_swapchainImages;
		std::vector<TRefPtr<VulkanImageView>> m_swapchainImageViews;

		TRefPtr<VulkanImage> m_colorBuffer;
		TRefPtr<VulkanImageView> m_colorBufferView;

		TRefPtr<VulkanImage> m_depthBuffer;
		TRefPtr<VulkanImageView> m_depthBufferView;
	};
}