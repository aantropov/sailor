#pragma once
#include <map>
#include "Memory/RefPtr.hpp"
#include "VulkanDeviceMemory.h"
#include "Memory/Memory.h"
#include "Memory/MemoryBlockAllocator.hpp"
#include "Memory/MemoryPoolAllocator.hpp"
#include "Memory/MemoryPtr.hpp"

namespace Sailor::Memory
{
	class VulkanMemoryPtr
	{
	public:

		SAILOR_API VulkanMemoryPtr() = default;
		SAILOR_API VulkanMemoryPtr(TRefPtr<Sailor::GraphicsDriver::Vulkan::VulkanDeviceMemory> deviceMemory);
		SAILOR_API VulkanMemoryPtr(TRefPtr<Sailor::GraphicsDriver::Vulkan::VulkanDeviceMemory> deviceMemory, size_t offset, size_t size);

		SAILOR_API VulkanMemoryPtr& operator=(const TRefPtr<Sailor::GraphicsDriver::Vulkan::VulkanDeviceMemory>& rhs);

		SAILOR_API explicit operator bool() const;

		TRefPtr<Sailor::GraphicsDriver::Vulkan::VulkanDeviceMemory> m_deviceMemory{};
		size_t m_offset{};
		size_t m_size{};
	};

	class GlobalVulkanMemoryAllocator
	{
	protected:

		VkMemoryPropertyFlags m_memoryProperties = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;
		VkMemoryRequirements m_memoryRequirements{};

	public:

		SAILOR_API void SetMemoryProperties(VkMemoryPropertyFlags properties) { m_memoryProperties = properties; }
		SAILOR_API void SetMemoryRequirements(VkMemoryRequirements requirements) { m_memoryRequirements = requirements; }

		SAILOR_API VulkanMemoryPtr Allocate(size_t size);
		SAILOR_API void Free(VulkanMemoryPtr pData, size_t size);
	};

	// VulkanMemoryPtr & GlobalVulkanMemoryAllocator

	template<typename TDataType, typename TPtr = VulkanMemoryPtr>
	inline VulkanMemoryPtr GetPointer(TDataType& pData)
	{
		VulkanMemoryPtr ptr(pData.m_ptr);
		ptr.m_offset = pData.m_offset;
		ptr.m_size = pData.m_size;

		return ptr;
	}

	template<>
	inline VulkanMemoryPtr GetPointer<VulkanMemoryPtr>(const VulkanMemoryPtr& pStartBlock, size_t offset, size_t size)
	{
		VulkanMemoryPtr ptr(pStartBlock);
		ptr.m_offset = offset;
		ptr.m_size = size;
		return ptr;
	}

	template<>
	inline VulkanMemoryPtr Shift<VulkanMemoryPtr>(const VulkanMemoryPtr& ptr, size_t offset)
	{
		VulkanMemoryPtr memPtr(ptr);
		memPtr.m_offset += offset;
		return memPtr;
	}

	template<>
	inline uint32_t SizeOf<VulkanMemoryPtr>(const VulkanMemoryPtr& ptr)
	{
		return (uint32_t)ptr.m_size;
	}

	template<>
	inline uint32_t OffsetAlignment<VulkanMemoryPtr>(const VulkanMemoryPtr& from)
	{
		return (uint32_t)from.m_deviceMemory->GetMemoryRequirements().alignment;
	}

	template<>
	inline uint8_t* GetAddress<VulkanMemoryPtr>(VulkanMemoryPtr ptr)
	{
		return nullptr;
	}

	template<typename TMemoryPtr, typename TPtr = VulkanMemoryPtr, typename TGlobalAllocator = GlobalVulkanMemoryAllocator>
	TMemoryPtr Allocate(size_t size, GlobalVulkanMemoryAllocator* allocator)
	{
		TMemoryPtr newObj{};
		newObj.m_ptr = allocator->Allocate(size);
		return newObj;
	}

	template<typename TMemoryPtr, typename TPtr = VulkanMemoryPtr, typename TGlobalAllocator = GlobalVulkanMemoryAllocator>
	void Free(TMemoryPtr& ptr, GlobalVulkanMemoryAllocator* allocator)
	{
		allocator->Free(ptr.m_ptr, ptr.m_size);
		ptr.Clear();
	}

	template<>
	inline bool Align<VulkanMemoryPtr>(size_t sizeToEmplace, size_t alignment, const VulkanMemoryPtr& startPtr, size_t blockSize, uint32_t& alignmentOffset)
	{
		alignmentOffset = 0;
		size_t r = startPtr.m_offset % alignment;
		if (r > 0)
		{
			alignmentOffset = (uint32_t)(alignment - r); // number of bytes to skip
		}

		if (blockSize < alignmentOffset || blockSize - alignmentOffset < sizeToEmplace)
		{
			return false;
		}
		return true;
	}
}
