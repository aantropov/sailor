#pragma once
#include <map>
#include "Core/RefPtr.hpp"
#include "VulkanDevice.h"
#include "VulkanDeviceMemory.h"
#include "Memory/Memory.h"

namespace Sailor::Memory
{
	using VulkanDeviceMemoryPtr = TRefPtr<Sailor::GfxDevice::Vulkan::VulkanDeviceMemory>;

	class VulkanStagingAllocator
	{
	protected:

		const VkMemoryPropertyFlags memoryProperties = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;

	public:

		SAILOR_API VulkanDeviceMemoryPtr Allocate(size_t size)
		{
			auto device = Sailor::GfxDevice::Vulkan::VulkanApi::GetInstance()->GetMainDevice();
			auto memoryRequirements = device->GetMemoryRequirements_StagingBuffer();
			memoryRequirements.size = size;

			VulkanDeviceMemoryPtr deviceMemory = (VulkanDeviceMemoryPtr)TRefPtr<Sailor::GfxDevice::Vulkan::VulkanDeviceMemory>::Make(device,
				memoryRequirements,
				memoryProperties);

			return deviceMemory;
		}

		SAILOR_API void Free(VulkanDeviceMemoryPtr pData, size_t size)
		{
			pData.Clear();
		}
	};

	template<>
	VulkanDeviceMemoryPtr Shift<VulkanDeviceMemoryPtr>(const VulkanDeviceMemoryPtr& ptr, size_t offset)
	{
		return nullptr;
	}

	template<>
	uint32_t SizeOf<VulkanDeviceMemoryPtr>()
	{
		return 16;
	}

	template<>
	uint32_t OffsetAlignment<VulkanDeviceMemoryPtr>(VulkanDeviceMemoryPtr from)
	{
		return (uint32_t)from->GetMemoryRequirements().alignment;
	}

	template<>
	uint8_t* GetAddress<VulkanDeviceMemoryPtr>(VulkanDeviceMemoryPtr ptr)
	{
		// todo store ptr to data
		return nullptr;
	}

	template<typename TPtrType = VulkanDeviceMemoryPtr>
	size_t Size(VulkanDeviceMemoryPtr from)
	{
		return from->GetMemoryRequirements().size;
	}

	template<typename TDataType, typename TPtrType = VulkanDeviceMemoryPtr, typename TBlockAllocator = VulkanStagingAllocator>
	TDataType Allocate(size_t size, VulkanStagingAllocator* allocator)
	{
		TDataType newObj{};
		newObj.m_ptr = allocator->Allocate(size);
		return newObj;
	}

	template<typename TDataType, typename TPtrType = VulkanDeviceMemoryPtr, typename TBlockAllocator = VulkanStagingAllocator>
	void Free(TDataType& ptr, VulkanStagingAllocator* allocator)
	{
		allocator->Free(ptr.m_ptr, ptr.m_size);
		ptr.Clear();
	}
}
