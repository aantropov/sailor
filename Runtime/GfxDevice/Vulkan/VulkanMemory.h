#pragma once
#include <map>
#include "Core/RefPtr.hpp"
#include "VulkanDeviceMemory.h"
#include "Memory/Memory.h"
#include "Memory/MemoryBlockAllocator.hpp"
#include "Memory/MemoryPoolAllocator.hpp"

namespace Sailor::Memory
{
	class VulkanDeviceMemoryPtr
	{
	public:

		VulkanDeviceMemoryPtr() = default;
		VulkanDeviceMemoryPtr(TRefPtr<Sailor::GfxDevice::Vulkan::VulkanDeviceMemory> deviceMemory);
		VulkanDeviceMemoryPtr(TRefPtr<Sailor::GfxDevice::Vulkan::VulkanDeviceMemory> deviceMemory, size_t offset, size_t size);

		VulkanDeviceMemoryPtr& operator=(const TRefPtr<Sailor::GfxDevice::Vulkan::VulkanDeviceMemory>& rhs);

		operator bool();

		TRefPtr<Sailor::GfxDevice::Vulkan::VulkanDeviceMemory> m_deviceMemory{};
		size_t m_offset{};
		size_t m_size{};
	};

	class VulkanBufferPtr
	{
	public:

		VulkanBufferPtr() = default;
		VulkanBufferPtr(TRefPtr<class Sailor::GfxDevice::Vulkan::VulkanBuffer> buffer);
		VulkanBufferPtr(TRefPtr<Sailor::GfxDevice::Vulkan::VulkanBuffer> buffer, size_t offset, size_t size);

		VulkanBufferPtr& operator=(const TRefPtr<Sailor::GfxDevice::Vulkan::VulkanBuffer>& rhs);

		operator bool();

		TRefPtr<Sailor::GfxDevice::Vulkan::VulkanBuffer> m_buffer{};
		size_t m_offset{};
		size_t m_size{};
	};

	class GlobalVulkanAllocator
	{
	protected:

		VkMemoryPropertyFlags m_memoryProperties = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;
		VkMemoryRequirements m_memoryRequirements{};

	public:

		SAILOR_API void SetMemoryProperties(VkMemoryPropertyFlags properties) { m_memoryProperties = properties; }
		SAILOR_API void SetMemoryRequirements(VkMemoryRequirements requirements) { m_memoryRequirements = requirements; }

		SAILOR_API VulkanDeviceMemoryPtr Allocate(size_t size);
		SAILOR_API void Free(VulkanDeviceMemoryPtr pData, size_t size);
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

	template<typename TMemoryPtr, typename TPtr = VulkanDeviceMemoryPtr, typename TGlobalAllocator = GlobalVulkanAllocator>
	TMemoryPtr Allocate(size_t size, GlobalVulkanAllocator* allocator)
	{
		TMemoryPtr newObj{};
		newObj.m_ptr = allocator->Allocate(size);
		return newObj;
	}

	template<typename TMemoryPtr, typename TPtr = VulkanDeviceMemoryPtr, typename TGlobalAllocator = GlobalVulkanAllocator>
	void Free(TMemoryPtr& ptr, GlobalVulkanAllocator* allocator)
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
