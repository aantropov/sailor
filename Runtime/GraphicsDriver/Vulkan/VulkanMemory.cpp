#include "VulkanMemory.h"
#include "VulkanDevice.h"
#include "VulkanBuffer.h"

using namespace Sailor;
using namespace Sailor::Memory;
using namespace Sailor::GraphicsDriver::Vulkan;

std::atomic<size_t> GlobalVulkanMemoryAllocator::m_totalDeviceMemoryAllocated = 0u;
std::atomic<size_t> GlobalVulkanMemoryAllocator::m_totalHostMemoryAllocated = 0u;

namespace
{
	size_t SaturatingSubtract(std::atomic<size_t>& counter, size_t amount)
	{
		size_t current = counter.load(std::memory_order_relaxed);

		while (true)
		{
			const size_t next = amount >= current ? 0u : current - amount;
			if (counter.compare_exchange_weak(current, next, std::memory_order_relaxed, std::memory_order_relaxed))
			{
				return next;
			}
		}
	}
}

VulkanMemoryPtr::VulkanMemoryPtr(TRefPtr<Sailor::GraphicsDriver::Vulkan::VulkanDeviceMemory> deviceMemory) : m_deviceMemory(deviceMemory) {}
VulkanMemoryPtr::VulkanMemoryPtr(TRefPtr<Sailor::GraphicsDriver::Vulkan::VulkanDeviceMemory> deviceMemory, size_t offset, size_t size) :
	m_deviceMemory(deviceMemory), m_offset(offset), m_size(size) {}

VulkanMemoryPtr& VulkanMemoryPtr::operator=(const TRefPtr<Sailor::GraphicsDriver::Vulkan::VulkanDeviceMemory>& rhs)
{
	m_deviceMemory = rhs;
	return *this;
}

VulkanMemoryPtr::operator bool() const
{
	return m_deviceMemory.IsValid();
}

VulkanMemoryPtr GlobalVulkanMemoryAllocator::Allocate(size_t size)
{
	auto device = Sailor::GraphicsDriver::Vulkan::VulkanApi::GetInstance()->GetMainDevice();
	auto memoryRequirements = m_memoryRequirements;
	memoryRequirements.size = size;

	VulkanMemoryPtr memPtr(TRefPtr<Sailor::GraphicsDriver::Vulkan::VulkanDeviceMemory>::Make(device,
		memoryRequirements,
		m_memoryProperties),
		0,
		size);

	if (m_memoryProperties & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)
	{
		const size_t totalUsed = m_totalDeviceMemoryAllocated.fetch_add(size, std::memory_order_relaxed) + size;
		SAILOR_LOG("Allocate GPU device memory: %.2fmb, total used: %.2fmb", (double)size / (1024.0 * 1024.0), (double)totalUsed / (1024.0 * 1024.0));
	}
	else
	{
		m_totalHostMemoryAllocated += size;
	}

	return memPtr;
}

void GlobalVulkanMemoryAllocator::Free(VulkanMemoryPtr pData, size_t size)
{
	if (m_memoryProperties & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)
	{
		const size_t allocationSize = pData.m_size;
		const size_t totalUsed = SaturatingSubtract(m_totalDeviceMemoryAllocated, allocationSize);
		SAILOR_LOG("Free GPU device memory: %.2fmb, total used: %.2fmb", (double)allocationSize / (1024.0 * 1024.0), (double)totalUsed / (1024.0 * 1024.0));
	}
	else
	{
		SaturatingSubtract(m_totalHostMemoryAllocated, size);
	}

	pData.m_deviceMemory.Clear();
	pData.m_offset = pData.m_size = 0;
}
