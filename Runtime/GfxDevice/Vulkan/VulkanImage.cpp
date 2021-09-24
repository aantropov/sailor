#include <vector>
#include "VulkanApi.h"
#include "VulkanImage.h"
#include "VulkanDevice.h"
#include "VulkanDeviceMemory.h"

using namespace Sailor;
using namespace Sailor::GfxDevice::Vulkan;

void VulkanImage::Release()
{
	if (m_image)
	{
		vkDestroyImage(*m_device, m_image, nullptr);
		m_image = VK_NULL_HANDLE;
	}
}

VkMemoryRequirements VulkanImage::GetMemoryRequirements() const
{
	VkMemoryRequirements memRequirements;
	vkGetImageMemoryRequirements(*m_device, m_image, &memRequirements);
	return memRequirements;
}

VulkanImage::~VulkanImage()
{
	Release();
}

VulkanImage::VulkanImage(TRefPtr<VulkanDevice> device) :
	m_device(device)
{
	m_imageType = VK_IMAGE_TYPE_2D;
	m_format = VK_FORMAT_R8G8B8A8_SRGB;
}

VulkanImage::VulkanImage(VkImage image, TRefPtr<VulkanDevice> device)
{
	m_image = image;
	m_device = device;
}

VkResult VulkanImage::Bind(TRefPtr<VulkanDeviceMemory> deviceMemory, VkDeviceSize memoryOffset)
{
	VkResult result = vkBindImageMemory(*m_device, m_image, *deviceMemory, memoryOffset);
	if (result == VK_SUCCESS)
	{
		m_deviceMemory = deviceMemory;
		m_memoryOffset = memoryOffset;
	}
	return result;
}

void VulkanImage::Compile()
{
	if (m_image != VK_NULL_HANDLE)
	{
		return;
	}

	VkImageCreateInfo info = {};
	info.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
	info.pNext = nullptr;
	info.flags = m_flags;
	info.imageType = m_imageType;
	info.extent = m_extent;
	info.mipLevels = m_mipLevels;
	info.arrayLayers = m_arrayLayers;
	info.samples = m_samples;
	info.tiling = m_tiling;
	info.usage = m_usage;
	info.sharingMode = m_sharingMode;
	info.queueFamilyIndexCount = static_cast<uint32_t>(m_queueFamilyIndices.size());
	info.pQueueFamilyIndices = m_queueFamilyIndices.data();
	info.initialLayout = m_initialLayout;

	// remap RGB to RGBA
	if (m_format >= VK_FORMAT_R8G8B8_UNORM && m_format <= VK_FORMAT_B8G8R8_SRGB)
	{
		m_format = static_cast<VkFormat>(m_format + 14);
	}
	else if (m_format >= VK_FORMAT_R16G16B16_UNORM && m_format <= VK_FORMAT_R16G16B16_SFLOAT)
	{
		m_format = static_cast<VkFormat>(m_format + 7);
	}
	else if (m_format >= VK_FORMAT_R32G32B32_UINT && m_format <= VK_FORMAT_R32G32B32_SFLOAT)
	{
		m_format = static_cast<VkFormat>(m_format + 3);
	}

	info.format = m_format;

	VK_CHECK(vkCreateImage(*m_device, &info, nullptr, &m_image));
}
