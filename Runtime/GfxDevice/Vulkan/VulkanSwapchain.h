#pragma once
#include "Core/RefPtr.hpp"
#include "VulkanApi.h"

namespace Sailor::GfxDevice::Vulkan
{
	class VulkanFence;
	class VulkanSemaphore;
	//class VulkanImageView;

	class VulkanSurface : public TRefBase
	{
	public:
		VulkanSurface(VkSurfaceKHR surface, VkInstance instance);

		operator VkSurfaceKHR() const { return m_surface; }

	protected:
		virtual ~VulkanSurface();

		VkSurfaceKHR m_surface;
		VkInstance m_instance;
	};

	class VulkanSwapchain final : public TRefBase
	{
	public:

		VulkanSwapchain(VkPhysicalDevice physicalDevice, VkDevice device, TRefPtr<VulkanSurface> surface, uint32_t width, uint32_t height, bool bIsVSync, TRefPtr<VulkanSwapchain> oldSwapchain = nullptr);

		operator VkSwapchainKHR() const { return m_swapchain; }

		VkFormat GetImageFormat() const { return m_surfaceFormat.format; }

		const VkExtent2D& GetExtent() const { return m_swapchainExtent; }

		std::vector<VkImageView>& GetImageViews() { return m_swapchainImageViews; }
		const std::vector<VkImageView>& GetImageViews() const { return m_swapchainImageViews; }

		VkResult AcquireNextImage(uint64_t timeout, TRefPtr<VulkanSemaphore> semaphore, TRefPtr<VulkanFence> fence, uint32_t& imageIndex);

	protected:
		virtual ~VulkanSwapchain();

		VkDevice m_device;
		TRefPtr<VulkanSurface> m_surface;
		VkSwapchainKHR m_swapchain = 0;

		VkExtent2D m_swapchainExtent;
		VkSurfaceFormatKHR m_surfaceFormat;
		VkPresentModeKHR m_presentMode;

		std::vector<VkImage> m_swapchainImages;
		std::vector<VkImageView> m_swapchainImageViews;

		//std::vector<TRefPtr<VulkanImageView>> m_imageViews;
	};
}