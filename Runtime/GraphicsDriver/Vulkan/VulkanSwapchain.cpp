#include "VulkanApi.h"
#include "VulkanSwapchain.h"
#include "VulkanFence.h"
#include "VulkanSemaphore.h"
#include "VulkanImage.h"
#include "VulkanImageView.h"
#include "VulkanDevice.h"
#include "VulkanDeviceMemory.h"
#include "Memory/RefPtr.hpp"

using namespace Sailor;
using namespace Sailor::GraphicsDriver::Vulkan;

VulkanSwapchainImage::VulkanSwapchainImage(VkImage image, VulkanDevicePtr device) : VulkanImage(image, device)
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

VulkanSwapchain::VulkanSwapchain(VulkanDevicePtr device, uint32_t width, uint32_t height, bool bIsVSync, VulkanSwapchainPtr oldSwapchain) :
	m_device(device),
	m_surface(device->GetSurface())
{
	const bool bIsRecreating = oldSwapchain.IsValid();

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
	createSwapChainInfo.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;

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
	
	TVector<VkImage> vkSwapchainImages;

	// Create Swapchain images & image views
	VK_CHECK(vkGetSwapchainImagesKHR(*m_device, m_swapchain, &imageCount, nullptr));
	vkSwapchainImages.Resize(imageCount);
	VK_CHECK(vkGetSwapchainImagesKHR(*m_device, m_swapchain, &imageCount, vkSwapchainImages.GetData()));

	for (size_t i = 0; i < vkSwapchainImages.Num(); i++)
	{
		m_swapchainImages.Add(VulkanSwapchainImagePtr::Make(vkSwapchainImages[i], m_device));
		m_swapchainImageViews.Add(VulkanImageViewPtr::Make(m_device, m_swapchainImages[i]));

		m_swapchainImages[i]->m_defaultLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
		m_swapchainImages[i]->m_mipLevels = 1;
		m_swapchainImages[i]->m_arrayLayers = 1;
		m_swapchainImages[i]->m_extent = VkExtent3D(m_swapchainExtent.width, m_swapchainExtent.height, 1);
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

		m_device->SetDebugName(VK_OBJECT_TYPE_IMAGE, (uint64_t)((VkImage)*m_swapchainImages[i]), "Swapchain Image");
	}

	VkFormat depthFormat = device->GetDepthFormat();

	m_depthBuffer = VulkanImagePtr::Make(m_device);
	m_depthBuffer->m_extent = VkExtent3D{ m_swapchainExtent.width, m_swapchainExtent.height, 1 };
	m_depthBuffer->m_format = depthFormat;
	m_depthBuffer->m_imageType = VkImageType::VK_IMAGE_TYPE_2D;
	m_depthBuffer->m_tiling = VkImageTiling::VK_IMAGE_TILING_OPTIMAL;
	m_depthBuffer->m_sharingMode = VK_SHARING_MODE_EXCLUSIVE;
	m_depthBuffer->m_mipLevels = 1;
	m_depthBuffer->m_usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
	m_depthBuffer->m_arrayLayers = 1;
	m_depthBuffer->m_samples = VK_SAMPLE_COUNT_1_BIT;
	m_depthBuffer->m_defaultLayout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL;

	m_depthBuffer->Compile();

	if (bIsRecreating)
	{
		// We are using device memory allocator to fastly recreate the swapchain
		auto requirements = m_depthBuffer->GetMemoryRequirements();
		
		//requirements.size += device->GetBufferImageGranuality();
		//requirements.alignment = std::max(requirements.alignment, device->GetBufferImageGranuality());

		auto memoryPtr = device->GetMemoryAllocator(VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, requirements).Allocate(requirements.size, requirements.alignment);
		m_depthBuffer->Bind(memoryPtr);
	}
	else
	{
		m_depthBuffer->Bind(VulkanDeviceMemoryPtr::Make(m_device, m_depthBuffer->GetMemoryRequirements(), VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT), 0);
	}

	m_depthBufferView = VulkanImageViewPtr::Make(m_device, m_depthBuffer, VK_IMAGE_ASPECT_DEPTH_BIT);
	m_depthBufferView->Compile();

	if (const bool bHasStencil = VulkanApi::ComputeAspectFlagsForFormat(depthFormat) & VK_IMAGE_ASPECT_STENCIL_BIT)
	{
		m_stencilBufferView = VulkanImageViewPtr::Make(m_device, m_depthBuffer, VK_IMAGE_ASPECT_DEPTH_BIT);
		m_stencilBufferView->Compile();
	}

	m_device->SetDebugName(VK_OBJECT_TYPE_IMAGE, (uint64_t)((VkImage)*m_depthBuffer), "Depth Buffer");
}

VulkanSwapchain::~VulkanSwapchain()
{
	m_swapchainImages.Clear();
	m_swapchainImageViews.Clear();

	if (m_swapchain)
	{
		vkDestroySwapchainKHR(*m_device, m_swapchain, nullptr);
	}
	m_device.Clear();
}

VkResult VulkanSwapchain::AcquireNextImage(uint64_t timeout, VulkanSemaphorePtr semaphore, VulkanFencePtr fence, uint32_t& imageIndex)
{
	return vkAcquireNextImageKHR(*m_device,
		m_swapchain,
		timeout,
		semaphore ? (VkSemaphore)*semaphore : VK_NULL_HANDLE,
		fence ? (VkFence)*fence : VK_NULL_HANDLE,
		&imageIndex);
}
