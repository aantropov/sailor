#include "Containers/Vector.h"
#include "VulkanApi.h"
#include "VulkanImage.h"
#include "VulkanDevice.h"
#include "VulkanDeviceMemory.h"
#if defined(__APPLE__)
#include <vulkan/vulkan_metal.h>
#endif

using namespace Sailor;
using namespace Sailor::GraphicsDriver::Vulkan;

void VulkanImage::Release()
{
	if (m_ptr)
	{
		const EVulkanMemoryClass memoryClass = m_tiling == VK_IMAGE_TILING_LINEAR ?
			EVulkanMemoryClass::Linear : EVulkanMemoryClass::OptimalImage;
		m_device->GetMemoryAllocator(m_deviceMemory->GetMemoryPropertyFlags(),
			m_deviceMemory->GetMemoryRequirements(), memoryClass).Free(m_ptr);
	}

	if (m_image)
	{
		vkDestroyImage(*m_device, m_image, nullptr);
		m_image = VK_NULL_HANDLE;
	}

	m_deviceMemory.Clear();
}

VkMemoryRequirements VulkanImage::GetMemoryRequirements() const
{
	VkMemoryRequirements memRequirements;
	vkGetImageMemoryRequirements(*m_device, m_image, &memRequirements);
	return memRequirements;
}

VulkanImage::~VulkanImage()
{
	VulkanImage::Release();
}

VulkanImage::VulkanImage(VulkanDevicePtr device) :
	m_device(device)
{
}

VulkanImage::VulkanImage(VkImage image, VulkanDevicePtr device)
{
	m_image = image;
	m_device = device;
}

VkResult VulkanImage::Bind(VulkanDeviceMemoryPtr deviceMemory, VkDeviceSize memoryOffset)
{
	VkResult result = vkBindImageMemory(*m_device, m_image, *deviceMemory, memoryOffset);
	if (result == VK_SUCCESS)
	{
		m_deviceMemory = deviceMemory;
		m_memoryOffset = memoryOffset;
	}
	return result;
}


VkResult VulkanImage::Bind(TMemoryPtr<VulkanMemoryPtr> ptr)
{
	m_ptr = ptr;
	return Bind((*ptr).m_deviceMemory, (*ptr).m_offset);
}

void VulkanImage::Compile()
{
	if (m_image != VK_NULL_HANDLE)
	{
		return;
	}

	VkImageCreateInfo info{ VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO };
	VkExternalMemoryImageCreateInfo externalInfo{ VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO };
	if (m_useExternalMemory)
	{
		externalInfo.handleTypes = m_externalHandleType;
		info.pNext = &externalInfo;
	}
#if defined(__APPLE__)
	VkExportMetalObjectCreateInfoEXT metalInfo{ VK_STRUCTURE_TYPE_EXPORT_METAL_OBJECT_CREATE_INFO_EXT };
	if (m_device->IsMetalObjectsSupported())
	{
		metalInfo.exportObjectType = VK_EXPORT_METAL_OBJECT_TYPE_METAL_TEXTURE_BIT_EXT;
		metalInfo.pNext = info.pNext;
		info.pNext = &metalInfo;
	}
#endif
	info.flags = m_flags;
	info.imageType = m_imageType;
	info.extent = m_extent;
	info.mipLevels = m_mipLevels;
	info.arrayLayers = m_arrayLayers;
	info.samples = m_samples;
	info.tiling = m_tiling;
	info.usage = m_usage;
	info.sharingMode = m_sharingMode;
	info.queueFamilyIndexCount = static_cast<uint32_t>(m_queueFamilyIndices.Num());
	info.pQueueFamilyIndices = m_queueFamilyIndices.GetData();
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
