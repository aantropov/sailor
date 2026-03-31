#include "VulkanMemory.h"
#include "VulkanDevice.h"
#include "VulkanBuffer.h"

using namespace Sailor;
using namespace Sailor::Memory;
using namespace Sailor::GraphicsDriver::Vulkan;

std::atomic<size_t> GlobalVulkanMemoryAllocator::m_totalDeviceMemoryAllocated = 0u;
std::atomic<size_t> GlobalVulkanMemoryAllocator::m_totalHostMemoryAllocated = 0u;

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
		const size_t prevTotal = m_totalDeviceMemoryAllocated.load(std::memory_order_relaxed);
		const size_t clampedSize = allocationSize > prevTotal ? prevTotal : allocationSize;
		const size_t totalUsed = m_totalDeviceMemoryAllocated.fetch_sub(clampedSize, std::memory_order_relaxed) - clampedSize;
		SAILOR_LOG("Free GPU device memory: %.2fmb, total used: %.2fmb", (double)allocationSize / (1024.0 * 1024.0), (double)totalUsed / (1024.0 * 1024.0));
	}
	else
	{
		m_totalHostMemoryAllocated -= size;
	}

	pData.m_deviceMemory.Clear();
	pData.m_offset = pData.m_size = 0;
}
