#include "VulkanImageView.h"
#include "VulkanImage.h"
#include "VulkanDevice.h"
#include "Memory/RefPtr.hpp"

using namespace Sailor;
using namespace Sailor::GraphicsDriver::Vulkan;

void VulkanImageView::Release()
{
	if (m_imageView)
	{
		vkDestroyImageView(*m_device, m_imageView, nullptr);
		m_imageView = VK_NULL_HANDLE;
	}
}

VulkanImageView::~VulkanImageView()
{
	Release();
}

VulkanImageView::VulkanImageView(VulkanDevicePtr device, VulkanImagePtr image) :
	m_image(image),
	m_device(device)
{
	const bool bIsArray = image->m_arrayLayers > 1;

	switch (image->m_imageType)
	{
	case VK_IMAGE_TYPE_3D:
		m_viewType = VK_IMAGE_VIEW_TYPE_3D;
		break;

	default:
		m_viewType = bIsArray ? VK_IMAGE_VIEW_TYPE_2D_ARRAY : VK_IMAGE_VIEW_TYPE_2D;
		break;
	}

	if (bIsArray && (image->m_flags & VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT))
	{
		m_viewType = VK_IMAGE_VIEW_TYPE_CUBE;
	}

	m_format = image->m_format;
	m_subresourceRange.aspectMask = VulkanApi::ComputeAspectFlagsForFormat(image->m_format);
	m_subresourceRange.baseMipLevel = 0;
	m_subresourceRange.levelCount = image->m_mipLevels;
	m_subresourceRange.baseArrayLayer = 0;
	m_subresourceRange.layerCount = image->m_arrayLayers;
}

VulkanImageView::VulkanImageView(VulkanDevicePtr device, VulkanImagePtr image, VkImageAspectFlags aspectFlags) :
	VulkanImageView(device, image)
{
	m_subresourceRange.aspectMask = aspectFlags;
}

void VulkanImageView::Compile()
{
	if (m_imageView != VK_NULL_HANDLE)
	{
		return;
	}

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
		m_image->Compile();
		info.image = *m_image;
	}*/

	VK_CHECK(vkCreateImageView(*m_device, &info, nullptr, &m_imageView));
}
