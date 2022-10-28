#include "VulkanMemory.h"
#include "VulkanDevice.h"
#include "VulkanBuffer.h"

using namespace Sailor;
using namespace Sailor::Memory;
using namespace Sailor::GraphicsDriver::Vulkan;

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

	return memPtr;
}

void GlobalVulkanMemoryAllocator::Free(VulkanMemoryPtr pData, size_t size)
{
	pData.m_deviceMemory.Clear();
	pData.m_offset = pData.m_size = 0;
}
