#include "VulkanApi.h"
#include "VulkanSwapchain.h"
#include "VulkanFence.h"
#include "VulkanSemaphore.h"
#include "VulkanImage.h"
#include "VulkanImageView.h"
#include "VulkanDevice.h"
#include "VulkanDeviceMemory.h"
#include "Core/RefPtr.hpp"

using namespace Sailor;
using namespace Sailor::GfxDevice::Vulkan;

VulkanSwapchainImage::VulkanSwapchainImage(VkImage image, TRefPtr<VulkanDevice> device) : VulkanImage(image, device)
{
}

VulkanSwapchainImage::~VulkanSwapchainImage()
{
	m_deviceMemory = 0;
	m_image = VK_NULL_HANDLE;
}

VulkanSurface::VulkanSurface(VkSurfaceKHR surface, VkInstance instance) :
	m_surface(surface),
	m_instance(instance)
{
}

VulkanSurface::~VulkanSurface()
{
	if (m_surface)
	{
		vkDestroySurfaceKHR(m_instance, m_surface, nullptr);
	}
}

VulkanSwapchain::VulkanSwapchain(TRefPtr<VulkanDevice> device, uint32_t width, uint32_t height, bool bIsVSync, TRefPtr<VulkanSwapchain> oldSwapchain) :
	m_device(device),
	m_surface(device->GetSurface())
{
	SwapChainSupportDetails swapChainSupport = VulkanApi::QuerySwapChainSupport(device->GetPhysicalDevice(), m_surface);

	m_surfaceFormat = VulkanApi::ChooseSwapSurfaceFormat(swapChainSupport.m_formats);
	m_presentMode = VulkanApi::ÑhooseSwapPresentMode(swapChainSupport.m_presentModes, bIsVSync);
	m_swapchainExtent = VulkanApi::ChooseSwapExtent(swapChainSupport.m_capabilities, width, height);

	uint32_t imageCount = swapChainSupport.m_capabilities.minImageCount + 1;

	if (swapChainSupport.m_capabilities.maxImageCount > 0 && imageCount > swapChainSupport.m_capabilities.maxImageCount)
	{
		imageCount = swapChainSupport.m_capabilities.maxImageCount;
	}

	VkSwapchainCreateInfoKHR createSwapChainInfo{ VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR };
	createSwapChainInfo.surface = *m_surface;
	createSwapChainInfo.minImageCount = imageCount;
	createSwapChainInfo.imageFormat = m_surfaceFormat.format;
	createSwapChainInfo.imageColorSpace = m_surfaceFormat.colorSpace;
	createSwapChainInfo.imageExtent = m_swapchainExtent;
	createSwapChainInfo.imageArrayLayers = 1;
	createSwapChainInfo.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT; //TODO: Use VK_IMAGE_USAGE_TRANSFER_DST_BIT for Post Processing

	VulkanQueueFamilyIndices indices = VulkanApi::FindQueueFamilies(device->GetPhysicalDevice(), m_surface);
	uint32_t queueFamilyIndices[] = { indices.m_graphicsFamily.value(), indices.m_presentFamily.value() };

	if (indices.m_graphicsFamily != indices.m_presentFamily)
	{
		createSwapChainInfo.imageSharingMode = VK_SHARING_MODE_CONCURRENT;
		createSwapChainInfo.queueFamilyIndexCount = 2;
		createSwapChainInfo.pQueueFamilyIndices = queueFamilyIndices;
	}
	else
	{
		createSwapChainInfo.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
		createSwapChainInfo.queueFamilyIndexCount = 0; // Optional
		createSwapChainInfo.pQueueFamilyIndices = nullptr; // Optional
	}

	createSwapChainInfo.preTransform = swapChainSupport.m_capabilities.currentTransform;
	createSwapChainInfo.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;

	createSwapChainInfo.presentMode = m_presentMode;
	createSwapChainInfo.clipped = VK_TRUE;

	if (oldSwapchain)
	{
		createSwapChainInfo.oldSwapchain = *oldSwapchain;
	}

	VK_CHECK(vkCreateSwapchainKHR(*m_device, &createSwapChainInfo, nullptr, &m_swapchain));

	oldSwapchain.Clear();

	std::vector<VkImage> vkSwapchainImages;

	// Create Swapchain images & image views
	VK_CHECK(vkGetSwapchainImagesKHR(*m_device, m_swapchain, &imageCount, nullptr));
	vkSwapchainImages.resize(imageCount);
	VK_CHECK(vkGetSwapchainImagesKHR(*m_device, m_swapchain, &imageCount, vkSwapchainImages.data()));

	for (size_t i = 0; i < vkSwapchainImages.size(); i++)
	{
		m_swapchainImages.push_back(TRefPtr<VulkanSwapchainImage>::Make(vkSwapchainImages[i], m_device));
		m_swapchainImageViews.push_back(TRefPtr<VulkanImageView>::Make(m_device, m_swapchainImages[i]));

		m_swapchainImageViews[i]->m_format = m_surfaceFormat.format;

		m_swapchainImageViews[i]->m_components.r = VK_COMPONENT_SWIZZLE_IDENTITY;
		m_swapchainImageViews[i]->m_components.g = VK_COMPONENT_SWIZZLE_IDENTITY;
		m_swapchainImageViews[i]->m_components.b = VK_COMPONENT_SWIZZLE_IDENTITY;
		m_swapchainImageViews[i]->m_components.a = VK_COMPONENT_SWIZZLE_IDENTITY;

		m_swapchainImageViews[i]->m_subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
		m_swapchainImageViews[i]->m_subresourceRange.baseMipLevel = 0;
		m_swapchainImageViews[i]->m_subresourceRange.levelCount = 1;
		m_swapchainImageViews[i]->m_subresourceRange.baseArrayLayer = 0;
		m_swapchainImageViews[i]->m_subresourceRange.layerCount = 1;

		m_swapchainImageViews[i]->Compile();
	}

	m_colorBuffer = TRefPtr<VulkanImage>::Make(m_device);
	m_colorBuffer->m_extent = VkExtent3D{ m_swapchainExtent.width, m_swapchainExtent.height, 1 };
	m_colorBuffer->m_format = m_surfaceFormat.format;
	m_colorBuffer->m_imageType = VkImageType::VK_IMAGE_TYPE_2D;
	m_colorBuffer->m_tiling = VkImageTiling::VK_IMAGE_TILING_OPTIMAL;
	m_colorBuffer->m_sharingMode = VK_SHARING_MODE_EXCLUSIVE;
	m_colorBuffer->m_mipLevels = 1;
	m_colorBuffer->m_usage = VK_IMAGE_USAGE_TRANSIENT_ATTACHMENT_BIT | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
	m_colorBuffer->m_arrayLayers = 1;
	m_colorBuffer->m_samples = device->GetMaxAllowedMSAASamples();
	m_colorBuffer->Compile();

	m_colorBuffer->Bind(TRefPtr<VulkanDeviceMemory>::Make(m_device, m_colorBuffer->GetMemoryRequirements(), VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT), 0);
	m_colorBufferView = TRefPtr<VulkanImageView>::Make(m_device, m_colorBuffer, VulkanApi::ComputeAspectFlagsForFormat(m_surfaceFormat.format));
	m_colorBufferView->Compile();

	VkFormat depthFormat = device->GetDepthFormat();

	m_depthBuffer = TRefPtr<VulkanImage>::Make(m_device);
	m_depthBuffer->m_extent = VkExtent3D{ m_swapchainExtent.width, m_swapchainExtent.height, 1 };
	m_depthBuffer->m_format = depthFormat;
	m_depthBuffer->m_imageType = VkImageType::VK_IMAGE_TYPE_2D;
	m_depthBuffer->m_tiling = VkImageTiling::VK_IMAGE_TILING_OPTIMAL;
	m_depthBuffer->m_sharingMode = VK_SHARING_MODE_EXCLUSIVE;
	m_depthBuffer->m_mipLevels = 1;
	m_depthBuffer->m_usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
	m_depthBuffer->m_arrayLayers = 1;
	m_depthBuffer->m_samples = device->GetMaxAllowedMSAASamples();

	m_depthBuffer->Compile();

	m_depthBuffer->Bind(TRefPtr<VulkanDeviceMemory>::Make(m_device, m_depthBuffer->GetMemoryRequirements(), VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT), 0);
	m_depthBufferView = TRefPtr<VulkanImageView>::Make(m_device, m_depthBuffer, VulkanApi::ComputeAspectFlagsForFormat(depthFormat));
	m_depthBufferView->Compile();
}

VulkanSwapchain::~VulkanSwapchain()
{
	m_swapchainImages.clear();
	m_swapchainImageViews.clear();

	if (m_swapchain)
	{
		vkDestroySwapchainKHR(*m_device, m_swapchain, nullptr);
	}
	m_device.Clear();
}

VkResult VulkanSwapchain::AcquireNextImage(uint64_t timeout, TRefPtr<VulkanSemaphore> semaphore, TRefPtr<VulkanFence> fence, uint32_t& imageIndex)
{
	return vkAcquireNextImageKHR(*m_device,
		m_swapchain,
		timeout,
		semaphore ? (VkSemaphore)*semaphore : VK_NULL_HANDLE,
		fence ? (VkFence)*fence : VK_NULL_HANDLE,
		&imageIndex);
}
