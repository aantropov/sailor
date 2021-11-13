#pragma once
#include <map>
#include "Core/RefPtr.hpp"
#include "VulkanBuffer.h"
#include "Memory/Memory.h"
#include "Memory/MemoryBlockAllocator.hpp"
#include "Memory/MemoryPoolAllocator.hpp"

namespace Sailor::Memory
{
	class VulkanBufferMemoryPtr
	{
	public:

		VulkanBufferMemoryPtr() = default;
		VulkanBufferMemoryPtr(TRefPtr<class Sailor::GfxDevice::Vulkan::VulkanBuffer> buffer);
		VulkanBufferMemoryPtr(TRefPtr<Sailor::GfxDevice::Vulkan::VulkanBuffer> buffer, size_t offset, size_t size);

		VulkanBufferMemoryPtr& operator=(const TRefPtr<Sailor::GfxDevice::Vulkan::VulkanBuffer>& rhs);

		operator bool();

		TRefPtr<Sailor::GfxDevice::Vulkan::VulkanBuffer> m_buffer{};
		size_t m_offset{};
		size_t m_size{};
	};

	class GlobalVulkanBufferAllocator
	{
	public:

		SAILOR_API void SetUsage(VkBufferUsageFlags usage) { m_usage = usage; }

		SAILOR_API VulkanBufferMemoryPtr Allocate(size_t size);
		SAILOR_API void Free(VulkanBufferMemoryPtr pData, size_t size);

	protected:
		VkBufferUsageFlags m_usage;
	};

// VulkanBufferMemoryPtr & GlobalVulkanBufferAllocator

	template<typename TDataType, typename TPtr = VulkanBufferMemoryPtr>
	inline VulkanBufferMemoryPtr GetPointer(TDataType& pData)
	{
		VulkanBufferMemoryPtr ptr(pData.m_ptr);
		ptr.m_offset = pData.m_offset;
		ptr.m_size = pData.m_size;

		return ptr;
	}

	template<>
	inline VulkanBufferMemoryPtr GetPointer<VulkanBufferMemoryPtr>(const VulkanBufferMemoryPtr& pStartBlock, size_t offset, size_t size)
	{
		VulkanBufferMemoryPtr ptr(pStartBlock);
		ptr.m_offset = offset;
		ptr.m_size = size;
		return ptr;
	}

	template<>
	inline VulkanBufferMemoryPtr Shift<VulkanBufferMemoryPtr>(const VulkanBufferMemoryPtr& ptr, size_t offset)
	{
		VulkanBufferMemoryPtr memPtr(ptr);
		memPtr.m_offset += offset;
		return memPtr;
	}

	template<>
	inline uint32_t SizeOf<VulkanBufferMemoryPtr>(const VulkanBufferMemoryPtr& ptr)
	{
		return (uint32_t)ptr.m_size;
	}

	template<>
	inline uint32_t OffsetAlignment<VulkanBufferMemoryPtr>(const VulkanBufferMemoryPtr& from)
	{
		return (uint32_t)from.m_buffer->GetMemoryRequirements().alignment;
	}

	template<>
	inline uint8_t* GetAddress<VulkanBufferMemoryPtr>(VulkanBufferMemoryPtr ptr)
	{
		return nullptr;
	}

	template<typename TMemoryPtr, typename TPtr = VulkanBufferMemoryPtr, typename TGlobalAllocator = GlobalVulkanBufferAllocator>
	TMemoryPtr Allocate(size_t size, GlobalVulkanBufferAllocator* allocator)
	{
		TMemoryPtr newObj{};
		newObj.m_ptr = allocator->Allocate(size);
		return newObj;
	}

	template<typename TMemoryPtr, typename TPtr = VulkanBufferMemoryPtr, typename TGlobalAllocator = GlobalVulkanBufferAllocator>
	void Free(TMemoryPtr& ptr, GlobalVulkanBufferAllocator* allocator)
	{
		allocator->Free(ptr.m_ptr, ptr.m_size);
		ptr.Clear();
	}

	template<>
	inline bool Align<VulkanBufferMemoryPtr>(size_t sizeToEmplace, size_t alignment, const VulkanBufferMemoryPtr& startPtr, size_t blockSize, uint32_t& alignmentOffset)
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
