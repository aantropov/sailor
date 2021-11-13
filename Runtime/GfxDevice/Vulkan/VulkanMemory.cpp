#include "VulkanMemory.h"
#include "VulkanDevice.h"
#include "VulkanBuffer.h"

using namespace Sailor;
using namespace Sailor::Memory;
using namespace Sailor::GfxDevice::Vulkan;

VulkanMemoryPtr::VulkanMemoryPtr(TRefPtr<Sailor::GfxDevice::Vulkan::VulkanDeviceMemory> deviceMemory) : m_deviceMemory(deviceMemory) {}
VulkanMemoryPtr::VulkanMemoryPtr(TRefPtr<Sailor::GfxDevice::Vulkan::VulkanDeviceMemory> deviceMemory, size_t offset, size_t size) :
	m_deviceMemory(deviceMemory), m_offset(offset), m_size(size) {}

VulkanMemoryPtr& VulkanMemoryPtr::operator=(const TRefPtr<Sailor::GfxDevice::Vulkan::VulkanDeviceMemory>& rhs)
{
	m_deviceMemory = rhs;
	return *this;
}

VulkanMemoryPtr::operator bool()
{
	return m_deviceMemory;
}

VulkanMemoryPtr GlobalVulkanMemoryAllocator::Allocate(size_t size)
{
	auto device = Sailor::GfxDevice::Vulkan::VulkanApi::GetInstance()->GetMainDevice();
	auto memoryRequirements = m_memoryRequirements;
	memoryRequirements.size = size;

	VulkanMemoryPtr memPtr(TRefPtr<Sailor::GfxDevice::Vulkan::VulkanDeviceMemory>::Make(device,
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
