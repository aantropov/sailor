#include "VulkanBuffer.h"
#include "Memory/RefPtr.hpp"
#include <vulkan/vulkan.h>
#include "VulkanDevice.h"
#include "VulkanDeviceMemory.h"

using namespace Sailor;
using namespace Sailor::Memory;
using namespace Sailor::GraphicsDriver::Vulkan;

VulkanBuffer::VulkanBuffer(VulkanDevicePtr device, VkDeviceSize size, VkBufferUsageFlags usage, VkSharingMode sharingMode) :
	m_size(size),
	m_usage(usage),
	m_sharingMode(sharingMode),
	m_device(device)
{
}

void VulkanBuffer::Release()
{
	if (m_ptr)
	{
		m_device->GetMemoryAllocator(m_deviceMemory->GetMemoryPropertyFlags(), m_deviceMemory->GetMemoryRequirements()).Free(m_ptr);
	}
	
	if (m_buffer)
	{
		vkDestroyBuffer(*m_device, m_buffer, nullptr);
	}

	m_deviceMemory.Clear();
}

void VulkanBuffer::Compile()
{
	VkBufferCreateInfo bufferInfo = {};
	bufferInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
	bufferInfo.flags = m_flags;
	bufferInfo.size = m_size;
	bufferInfo.usage = m_usage;
	bufferInfo.sharingMode = m_sharingMode;

	const auto& queues = m_device->GetQueueFamilies();

	uint32_t queueFamily[] = { queues.m_graphicsFamily.value() };
	uint32_t queueFamilies[] = { queues.m_graphicsFamily.value(), queues.m_transferFamily.value() };

	if (m_sharingMode == VK_SHARING_MODE_CONCURRENT)
	{
		bufferInfo.queueFamilyIndexCount = 2;
		bufferInfo.pQueueFamilyIndices = queueFamilies;
	}
	else
	{
		bufferInfo.queueFamilyIndexCount = 1;
		bufferInfo.pQueueFamilyIndices = queueFamily;
	}

	VK_CHECK(vkCreateBuffer(*m_device, &bufferInfo, nullptr, &m_buffer));
}

VulkanBuffer::~VulkanBuffer() 
{
	Release();
}

VkMemoryRequirements VulkanBuffer::GetMemoryRequirements() const
{
	VkMemoryRequirements memRequirements;
	vkGetBufferMemoryRequirements(*m_device, m_buffer, &memRequirements);
	return memRequirements;
}

VulkanBufferMemoryPtr VulkanBuffer::GetBufferMemoryPtr()
{
	auto ptr = VulkanBufferMemoryPtr();
	ptr.m_buffer = this->ToRefPtr<VulkanBuffer>();
	ptr.m_offset = 0;
	ptr.m_size = m_size;

	return ptr;
}

VkResult VulkanBuffer::Bind(TMemoryPtr<VulkanMemoryPtr> ptr)
{
	m_ptr = ptr;
	return Bind((*ptr).m_deviceMemory, (*ptr).m_offset);
}

VkResult VulkanBuffer::Bind(VulkanDeviceMemoryPtr deviceMemory, VkDeviceSize memoryOffset)
{
	VkResult result = vkBindBufferMemory(*m_device, m_buffer, *deviceMemory, memoryOffset);
	if (result == VK_SUCCESS)
	{
		m_deviceMemory = deviceMemory;
		m_memoryOffset = memoryOffset;
	}

	return result;
}
