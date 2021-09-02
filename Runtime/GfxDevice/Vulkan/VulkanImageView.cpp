#include "VulkanImageView.h"
#include "VulkanImage.h"
#include "Core/RefPtr.hpp"

using namespace Sailor;
using namespace Sailor::GfxDevice::Vulkan;

VulkanImageView::VulkanData::~VulkanData()
{
	if (m_imageView)
	{
		vkDestroyImageView(m_device, m_imageView, nullptr);
		m_imageView = VK_NULL_HANDLE;
	}
}

VulkanImageView::VulkanImageView(TRefPtr<VulkanImage> image) :
	m_image(image)
{
	m_viewType = VK_IMAGE_VIEW_TYPE_2D;

	m_format = image->m_format;
	m_subresourceRange.aspectMask = VulkanApi::ComputeAspectFlagsForFormat(image->m_format);
	m_subresourceRange.baseMipLevel = 0;
	m_subresourceRange.levelCount = image->m_mipLevels;
	m_subresourceRange.baseArrayLayer = 0;
	m_subresourceRange.layerCount = image->m_arrayLayers;
}

VulkanImageView::VulkanImageView(TRefPtr<VulkanImage> image, VkImageAspectFlags aspectFlags) :
	m_image(image)
{
	m_viewType = VK_IMAGE_VIEW_TYPE_2D;
	m_format = image->m_format;
	m_subresourceRange.aspectMask = aspectFlags;
	m_subresourceRange.baseMipLevel = 0;
	m_subresourceRange.levelCount = image->m_mipLevels;
	m_subresourceRange.baseArrayLayer = 0;
	m_subresourceRange.layerCount = image->m_arrayLayers;
}

VulkanImageView::~VulkanImageView()
{
}

void VulkanImageView::Initialize(VkDevice device)
{
	if (m_vulkanData.m_imageView != VK_NULL_HANDLE)
	{
		return;
	}

	m_vulkanData.m_device = device;

	VkImageViewCreateInfo info = {};
	info.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
	info.pNext = nullptr;
	info.flags = m_flags;
	info.viewType = m_viewType;
	info.format = m_format;
	info.components = m_components;
	info.subresourceRange = m_subresourceRange;
	info.image = *m_image;

	/*if (m_image)
	{
		m_image->Initialize();
		info.image = *m_image;
	}*/

	VK_CHECK(vkCreateImageView(m_vulkanData.m_device, &info, nullptr, &m_vulkanData.m_imageView));
}
