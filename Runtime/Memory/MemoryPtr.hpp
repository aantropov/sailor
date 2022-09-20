#pragma once
#include "Memory.h"
#include "WeakPtr.hpp"

namespace Sailor::Memory
{
	// The core class to work with different pointers to data (for example pointer to DeviceMemory or pointer 'inside' VulkanBuffer)
	template<typename TPtr>
	class TMemoryPtr final
	{
	public:

		TMemoryPtr() = default;
		TMemoryPtr(const TMemoryPtr& rhs) = default;
		TMemoryPtr(TMemoryPtr&& rhs) = default;
		TMemoryPtr& operator=(const TMemoryPtr& rhs) = default;
		TMemoryPtr& operator=(TMemoryPtr&& rhs) = default;

		TMemoryPtr(size_t offset, size_t alignmentOffset, size_t size, TPtr ptr, uint32_t blockIndex) :
			m_offset(offset),
			m_size(size),
			m_alignmentOffset(alignmentOffset),
			m_blockIndex(blockIndex),
			m_ptr(ptr)
		{}

		operator bool() const { return (bool)m_ptr; }

		const TPtr operator*() const { return Memory::GetPointer(m_ptr, m_offset + m_alignmentOffset, m_size); }
		TPtr operator*() { return Memory::GetPointer(m_ptr, m_offset + m_alignmentOffset, m_size); }

		void Clear()
		{
			m_offset = 0;
			m_size = 0;
			m_blockIndex = 0;
			m_alignmentOffset = 0;
			m_ptr = nullptr;
		}

		void ShiftPointer(size_t offset)
		{
			m_offset += offset;
		}

		size_t Offset(const TMemoryPtr& from, const TMemoryPtr& to)
		{
			return (size_t)(to.m_offset - from.m_offset - from.m_size - from.m_alignmentOffset);
		}

		size_t m_offset{};
		size_t m_alignmentOffset{};
		size_t m_size{};
		uint32_t m_blockIndex{};
		TPtr m_ptr{};
	};

	template<typename T, typename TAllocator>
	class TManagedMemory final
	{
	public:

		TManagedMemory() = default;
		TManagedMemory(const TMemoryPtr<T>& pRaw, TWeakPtr<TAllocator> allocator)
		{
			m_pRawPtr = pRaw;
			m_pAllocator = allocator;
		}

		TManagedMemory(const TManagedMemory&) = default;
		TManagedMemory& operator=(const TManagedMemory&) = default;

		const TMemoryPtr<T>& Get() const noexcept { return m_pRawPtr; }
		TMemoryPtr<T>& Get() { return m_pRawPtr; }

		~TManagedMemory()
		{
			if (m_pAllocator)
			{
				auto allocator = m_pAllocator.Lock();
				allocator->Free(m_pRawPtr);
			}
		}

	protected:

		TWeakPtr<TAllocator> m_pAllocator{};
		TMemoryPtr<T> m_pRawPtr{};
	};

	template<typename T, typename TAllocator>
	using TManagedMemoryPtr = TSharedPtr<TManagedMemory<T, TAllocator>>;
}