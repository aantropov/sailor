#include "VulkanDeviceMemory.h"
#include "VulkanDevice.h"

#include <atomic>
#include <cstring>
#include <iostream>

using namespace Sailor;
using namespace Sailor::GfxDevice::Vulkan;

VulkanDeviceMemory::VulkanDeviceMemory(VulkanDevicePtr device, const VkMemoryRequirements& memRequirements, VkMemoryPropertyFlags properties, void* pNextAllocInfo) :
	m_memoryRequirements(memRequirements),
	m_properties(properties),
	m_device(device)
{
	uint32_t typeFilter = memRequirements.memoryTypeBits;
	uint32_t memoryTypeIndex = VulkanApi::FindMemoryByType(m_device->GetPhysicalDevice(), typeFilter, properties);

	VkMemoryAllocateInfo allocateInfo = {};
	allocateInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
	allocateInfo.allocationSize = memRequirements.size;
	allocateInfo.memoryTypeIndex = memoryTypeIndex;
	allocateInfo.pNext = pNextAllocInfo;

	VK_CHECK(vkAllocateMemory(*m_device, &allocateInfo, nullptr, &m_deviceMemory));
}

VulkanDeviceMemory::~VulkanDeviceMemory()
{
	if (m_deviceMemory)
	{
		vkFreeMemory(*m_device, m_deviceMemory, nullptr);
	}
	m_device.Clear();
}

VkResult VulkanDeviceMemory::Map(VkDeviceSize offset, VkDeviceSize size, VkMemoryMapFlags flags, void** ppData)
{
	return vkMapMemory(*m_device, m_deviceMemory, offset, size, flags, ppData);
}

void VulkanDeviceMemory::Unmap()
{
	vkUnmapMemory(*m_device, m_deviceMemory);
}

void VulkanDeviceMemory::Copy(VkDeviceSize offset, VkDeviceSize size, const void* src_data)
{
	void* buffer_data;
	Map(offset, size, 0, &buffer_data);
	std::memcpy(buffer_data, src_data, (size_t)size);
	Unmap();
}
