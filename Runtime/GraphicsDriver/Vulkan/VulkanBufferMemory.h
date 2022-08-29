#pragma once
#include <map>
#include "Memory/RefPtr.hpp"
#include "VulkanBuffer.h"
#include "Memory/Memory.h"
#include "Memory/MemoryBlockAllocator.hpp"
#include "Memory/MemoryPoolAllocator.hpp"

namespace Sailor::Memory
{
	class VulkanBufferMemoryPtr
	{
	public:

		SAILOR_API VulkanBufferMemoryPtr() = default;
		SAILOR_API explicit VulkanBufferMemoryPtr(TRefPtr<class Sailor::GraphicsDriver::Vulkan::VulkanBuffer> buffer);
		SAILOR_API VulkanBufferMemoryPtr(TRefPtr<Sailor::GraphicsDriver::Vulkan::VulkanBuffer> buffer, size_t offset, size_t size);

		SAILOR_API VulkanBufferMemoryPtr& operator=(const TRefPtr<Sailor::GraphicsDriver::Vulkan::VulkanBuffer>& rhs);

		SAILOR_API explicit operator bool() const;
		SAILOR_API VulkanMemoryPtr operator*();

		TRefPtr<Sailor::GraphicsDriver::Vulkan::VulkanBuffer> m_buffer{};
		size_t m_offset{};
		size_t m_size{};
	};

	class GlobalVulkanBufferAllocator
	{
	public:

		SAILOR_API void SetMemoryProperties(VkMemoryPropertyFlags properties) { m_memoryProperties = properties; }
		SAILOR_API void SetUsage(VkBufferUsageFlags usage) { m_usage = usage; }
		SAILOR_API void SetSharingMode(VkSharingMode sharing) { m_sharing = sharing; }

		SAILOR_API VulkanBufferMemoryPtr Allocate(size_t size);
		SAILOR_API void Free(VulkanBufferMemoryPtr pData, size_t size);

	protected:

		VkMemoryPropertyFlags m_memoryProperties = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;
		VkBufferUsageFlags m_usage;
		VkSharingMode m_sharing = VkSharingMode::VK_SHARING_MODE_CONCURRENT;
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
