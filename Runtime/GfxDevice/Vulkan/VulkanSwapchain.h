#pragma once
#include "Memory/RefPtr.hpp"
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
		VulkanSwapchainImage(VkImage image, VulkanDevicePtr device);

	protected:
		virtual ~VulkanSwapchainImage();
	};

	class VulkanSwapchain final : public RHI::Resource
	{
	public:

		VulkanSwapchain(VulkanDevicePtr device, uint32_t width, uint32_t height, bool bIsVSync, VulkanSwapchainPtr oldSwapchain);

		operator VkSwapchainKHR() const { return m_swapchain; }

		VkFormat GetImageFormat() const { return m_surfaceFormat.format; }

		const VkExtent2D& GetExtent() const { return m_swapchainExtent; }

		VulkanImageViewPtr GetDepthBufferView() const { return m_depthBufferView; }
		VulkanImageViewPtr GetColorBufferView() const { return m_colorBufferView; }

		std::vector<VulkanImageViewPtr>& GetImageViews() { return m_swapchainImageViews; }
		const std::vector<VulkanImageViewPtr>& GetImageViews() const { return m_swapchainImageViews; }

		VkResult AcquireNextImage(uint64_t timeout, VulkanSemaphorePtr semaphore, VulkanFencePtr fence, uint32_t& imageIndex);

	protected:
		virtual ~VulkanSwapchain();

		VulkanDevicePtr m_device;
		VulkanSurfacePtr m_surface;
		VkSwapchainKHR m_swapchain = 0;

		VkExtent2D m_swapchainExtent;
		VkSurfaceFormatKHR m_surfaceFormat;
		VkPresentModeKHR m_presentMode;

		std::vector<VulkanSwapchainImagePtr> m_swapchainImages;
		std::vector<VulkanImageViewPtr> m_swapchainImageViews;

		VulkanImagePtr m_colorBuffer;
		VulkanImageViewPtr m_colorBufferView;

		VulkanImagePtr m_depthBuffer;
		VulkanImageViewPtr m_depthBufferView;
	};
}