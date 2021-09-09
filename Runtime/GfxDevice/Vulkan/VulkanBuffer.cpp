#include "VulkanDevice.h"
#include "VulkanDeviceMemory.h"
#include "VulkanBuffer.h"
#include "Core/RefPtr.hpp"

using namespace Sailor::GfxDevice::Vulkan;

VulkanBuffer::VulkanBuffer(VkDeviceSize in_size, VkBufferUsageFlags in_usage, VkSharingMode in_sharingMode) :
	m_size(in_size),
	m_usage(in_usage),
	m_sharingMode(in_sharingMode)
{}

VulkanBuffer::VulkanBuffer(TRefPtr<VulkanDevice> device, VkDeviceSize in_size, VkBufferUsageFlags in_usage, VkSharingMode in_sharingMode) :
	VulkanBuffer(in_size, in_usage, in_sharingMode)
{
	Compile(device);
}

bool VulkanBuffer::Compile(TRefPtr<VulkanDevice> device)
{
	m_device = device;

	VkBufferCreateInfo bufferInfo = {};
	bufferInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
	bufferInfo.flags = m_flags;
	bufferInfo.size = m_size;
	bufferInfo.usage = m_usage;
	bufferInfo.sharingMode = m_sharingMode;

	const auto& queues = device->GetQueueFamilies();

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

	return true;
}

VulkanBuffer::~VulkanBuffer()
{
	if (m_buffer)
	{
		vkDestroyBuffer(*m_device, m_buffer, nullptr);
	}

	m_device.Clear();
	m_deviceMemory.Clear();
}

VkMemoryRequirements VulkanBuffer::GetMemoryRequirements() const
{
	VkMemoryRequirements memRequirements;
	vkGetBufferMemoryRequirements(*m_device, m_buffer, &memRequirements);
	return memRequirements;
}

VkResult VulkanBuffer::Bind(TRefPtr<VulkanDeviceMemory> deviceMemory, VkDeviceSize memoryOffset)
{
	VkResult result = vkBindBufferMemory(*m_device, m_buffer, *deviceMemory, memoryOffset);
	if (result == VK_SUCCESS)
	{
		m_deviceMemory = deviceMemory;
		m_memoryOffset = memoryOffset;
	}

	return result;
}
