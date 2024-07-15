#include "VulkanDeviceMemory.h"
#include "VulkanDevice.h"

#include <atomic>
#include <cstring>
#include <iostream>

using namespace Sailor;
using namespace Sailor::GraphicsDriver::Vulkan;

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

	if (m_properties & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT)
	{
		Map(0, VK_WHOLE_SIZE, 0, &pData);
	}
}

VulkanDeviceMemory::~VulkanDeviceMemory()
{
	if (m_properties & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT)
	{
		Unmap();
	}

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
	void* pBufferData = ((uint8_t*)pData) + offset;

	//Map(offset, size, 0, &buffer_data);

	const uint32_t mbToUseThreads = 4 * 1024 * 1024;

	// That's not tested
	if (false && size > mbToUseThreads)
	{
		const int numThreads = 4;
		TVector<Tasks::ITaskPtr> tasks;
		for (uint32_t i = 0; i < numThreads; i++)
		{
			auto pTask = Tasks::CreateTask("VulkanDeviceMemory, CPU side memory copy", [pBufferData, i, src_data, size]()
			{
				std::memcpy((char*)pBufferData + i * size / numThreads, ((char*)src_data + i * size / numThreads), (size_t)(size / numThreads));
			})->Run();

			tasks.Add(pTask);
		}

		for (uint32_t i = 0; i < numThreads; i++)
		{
			tasks[i]->Wait();
		}
	}
	else
	{
		std::memcpy(pBufferData, src_data, (size_t)size);
	}

	//Unmap();
}
