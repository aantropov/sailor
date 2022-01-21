#pragma once
#include <map>
#include "Core/RefPtr.hpp"
#include "VulkanDevice.h"
#include "VulkanDeviceMemory.h"
#include "Memory/Memory.h"
#include "Memory/MemoryBlockAllocator.hpp"

namespace Sailor::Memory
{
	class VulkanDeviceMemoryPtr
	{
	public:

		VulkanDeviceMemoryPtr() = default;
		VulkanDeviceMemoryPtr(TRefPtr<Sailor::GraphicsDriver::Vulkan::VulkanDeviceMemory> deviceMemory) : m_deviceMemory(deviceMemory) {}
		VulkanDeviceMemoryPtr(TRefPtr<Sailor::GraphicsDriver::Vulkan::VulkanDeviceMemory> deviceMemory, size_t offset, size_t size) :
			m_deviceMemory(deviceMemory), m_offset(offset), m_size(size) {}

		VulkanDeviceMemoryPtr& operator=(const TRefPtr<Sailor::GraphicsDriver::Vulkan::VulkanDeviceMemory>& rhs)
		{
			m_deviceMemory = rhs;
			return *this;
		}

		operator bool()
		{
			return m_deviceMemory;
		}

		TRefPtr<Sailor::GraphicsDriver::Vulkan::VulkanDeviceMemory> m_deviceMemory{};
		size_t m_offset{};
		size_t m_size{};
	};

	class GlobalVulkanHostAllocator
	{
	protected:

		const VkMemoryPropertyFlags memoryProperties = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;

	public:

		SAILOR_API VulkanDeviceMemoryPtr Allocate(size_t size)
		{
			auto device = Sailor::GraphicsDriver::Vulkan::VulkanApi::GetInstance()->GetMainDevice();
			auto memoryRequirements = device->GetMemoryRequirements_StagingBuffer();
			memoryRequirements.size = size;

			VulkanDeviceMemoryPtr memPtr(TRefPtr<Sailor::GraphicsDriver::Vulkan::VulkanDeviceMemory>::Make(device,
				memoryRequirements,
				memoryProperties),
				0,
				size);

			return memPtr;
		}

		SAILOR_API void Free(VulkanDeviceMemoryPtr pData, size_t size)
		{
			pData.m_deviceMemory.Clear();
			pData.m_offset = pData.m_size = 0;
		}
	};

	template<typename TDataType, typename TPtr = VulkanDeviceMemoryPtr>
	inline VulkanDeviceMemoryPtr GetPointer(TDataType& pData)
	{
		VulkanDeviceMemoryPtr ptr(pData.m_ptr);
		ptr.m_offset = pData.m_offset;
		ptr.m_size = pData.m_size;

		return ptr;
	}

	template<>
	inline VulkanDeviceMemoryPtr GetPointer<VulkanDeviceMemoryPtr>(const VulkanDeviceMemoryPtr& pStartBlock, size_t offset, size_t size)
	{
		VulkanDeviceMemoryPtr ptr(pStartBlock);
		ptr.m_offset = offset;
		ptr.m_size = size;
		return ptr;
	}

	template<>
	inline VulkanDeviceMemoryPtr Shift<VulkanDeviceMemoryPtr>(const VulkanDeviceMemoryPtr& ptr, size_t offset)
	{
		VulkanDeviceMemoryPtr memPtr(ptr);
		memPtr.m_offset += offset;
		return memPtr;
	}

	template<>
	inline uint32_t SizeOf<VulkanDeviceMemoryPtr>(const VulkanDeviceMemoryPtr& ptr)
	{
		return (uint32_t)ptr.m_size;
	}

	template<>
	inline uint32_t OffsetAlignment<VulkanDeviceMemoryPtr>(const VulkanDeviceMemoryPtr& from)
	{
		return (uint32_t)from.m_deviceMemory->GetMemoryRequirements().alignment;
	}

	template<>
	inline uint8_t* GetAddress<VulkanDeviceMemoryPtr>(VulkanDeviceMemoryPtr ptr)
	{
		return nullptr;
	}

	template<typename TMemoryPtr, typename TPtr = VulkanDeviceMemoryPtr, typename TGlobalAllocator = GlobalVulkanHostAllocator>
	TMemoryPtr Allocate(size_t size, GlobalVulkanHostAllocator* allocator)
	{
		TMemoryPtr newObj{};
		newObj.m_ptr = allocator->Allocate(size);
		return newObj;
	}

	template<typename TMemoryPtr, typename TPtr = VulkanDeviceMemoryPtr, typename TGlobalAllocator = GlobalVulkanHostAllocator>
	void Free(TMemoryPtr& ptr, GlobalVulkanHostAllocator* allocator)
	{
		allocator->Free(ptr.m_ptr, ptr.m_size);
		ptr.Clear();
	}

	template<>
	inline bool Align<VulkanDeviceMemoryPtr>(size_t sizeToEmplace, size_t alignment, const VulkanDeviceMemoryPtr& startPtr, size_t blockSize, uint32_t& alignmentOffset)
	{
		// try to carve out _Size bytes on boundary _Bound
		alignmentOffset = static_cast<uint32_t>(((uint32_t)startPtr.m_offset) & (alignment - 1));
		if (alignmentOffset != 0)
		{
			alignmentOffset = (uint32_t)(alignment - alignmentOffset); // number of bytes to skip
		}

		if (blockSize < alignmentOffset || blockSize - alignmentOffset < sizeToEmplace)
		{
			return false;
		}

		return true;
	}
}
