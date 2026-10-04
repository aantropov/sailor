#pragma once
#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <cassert>
#include <unordered_map>
#include <memory>
#include "Core/Defines.h"
#include "HeapAllocator.h"
#include "MallocAllocator.hpp"

namespace Sailor::Memory
{
	template<typename T, typename TAllocator = DefaultGlobalAllocator>
	SAILOR_API __forceinline T* New(TAllocator& allocator)
	{
		void* ptr = allocator.Allocate(sizeof(T), alignof(T));
		return new (ptr) T();
	}

	template<typename T, typename TAllocator, typename... TArgs>
	SAILOR_API __forceinline T* New(TAllocator& allocator, TArgs&& ... args)
	{
		void* ptr = allocator.Allocate(sizeof(T), alignof(T));
		return new (ptr) T(std::forward<TArgs>(args)...);
	}

	template<typename T, typename TAllocator = DefaultGlobalAllocator>
	SAILOR_API __forceinline void Delete(TAllocator& allocator, T* ptr)
	{
		if (ptr)
		{
			ptr->~T();
		}

		allocator.Free(ptr);
	}

	template<uint16_t stackSize = 1024, typename TAllocator = DefaultGlobalAllocator>
	class SAILOR_API TInlineAllocator final
	{
	protected:

		struct Header
		{
			uint16_t m_size;
			uint16_t m_previousIndex;
		};

		alignas(std::max_align_t) uint8_t m_stack[stackSize];
		uint16_t m_index = 0u;

		TAllocator m_allocator{};

		bool Contains(void* pData) const
		{
			const auto address = reinterpret_cast<uintptr_t>(pData);
			return address >= reinterpret_cast<uintptr_t>(m_stack) && address < reinterpret_cast<uintptr_t>(m_stack + stackSize);
		}

	public:

		TInlineAllocator() = default;
		TInlineAllocator(const TInlineAllocator&) = delete;
		TInlineAllocator(TInlineAllocator&&) = delete;
		TInlineAllocator& operator=(TInlineAllocator&&) = delete;
		TInlineAllocator& operator=(const TInlineAllocator&) = delete;
		~TInlineAllocator() = default;

		void* Allocate(size_t size, size_t alignment = 8)
		{
			check(alignment != 0 && (alignment & (alignment - 1)) == 0);
			if (stackSize - m_index >= sizeof(Header))
			{
				void* data = m_stack + m_index + sizeof(Header);
				size_t available = stackSize - m_index - sizeof(Header);
				if (std::align((std::max)(alignment, alignof(Header)), size, data, available))
				{
					auto* header = reinterpret_cast<Header*>(static_cast<uint8_t*>(data) - sizeof(Header));
					new (header) Header{ static_cast<uint16_t>(size), m_index };
					m_index = static_cast<uint16_t>(static_cast<uint8_t*>(data) - m_stack + size);
					return data;
				}
			}
			return m_allocator.Allocate(size, alignment);
		}

		bool Reallocate(void* pData, size_t size, size_t alignment = 8)
		{
			if (!Contains(pData))
			{
				return m_allocator.Reallocate(pData, size, alignment);
			}
			check(alignment != 0 && (alignment & (alignment - 1)) == 0);
			auto* header = reinterpret_cast<Header*>(static_cast<uint8_t*>(pData) - sizeof(Header));
			const size_t offset = static_cast<uint8_t*>(pData) - m_stack;
			if (offset + header->m_size == m_index && size <= stackSize - offset &&
				reinterpret_cast<uintptr_t>(pData) % alignment == 0)
			{
				header->m_size = static_cast<uint16_t>(size);
				m_index = static_cast<uint16_t>(offset + size);
				return true;
			}
			return false;
		}

		void Free(void* pData, size_t size = 0)
		{
			if (Contains(pData))
			{
				auto* header = reinterpret_cast<Header*>(static_cast<uint8_t*>(pData) - sizeof(Header));
				if (static_cast<uint8_t*>(pData) + header->m_size == &m_stack[m_index])
				{
					m_index = header->m_previousIndex;
				}
			}
			else
			{
				m_allocator.Free(pData);
			}
		}
	};

	template<typename TGlobalAllocator = Sailor::Memory::DefaultGlobalAllocator, typename TPtr = void*>
	class TBlockAllocator;

	template<typename TGlobalAllocator = Sailor::Memory::DefaultGlobalAllocator, typename TPtr = void*>
	class TPoolAllocator;

	template<typename TGlobalAllocator = Sailor::Memory::DefaultGlobalAllocator, typename TPtr = void*>
	class TMultiPoolAllocator;

	template<typename TPtr = void*>
	class TMemoryPtr;

	template<typename TPtr>
	__forceinline uint8_t* GetAddress(TPtr ptr)
	{
		return reinterpret_cast<uint8_t*>(ptr);
	}

	template<typename TPtr>
	__forceinline TPtr Shift(const TPtr& ptr, size_t offset)
	{
		return reinterpret_cast<TPtr>(&((GetAddress(ptr)[offset])));
	}

	template<typename TPtr>
	__forceinline uint32_t SizeOf(const TPtr& ptr)
	{
		return sizeof(typename std::remove_pointer<TPtr>::type);
	}

	template<typename TPtr>
	__forceinline uint32_t OffsetAlignment(const TPtr& from)
	{
		return alignof(typename std::remove_pointer<TPtr>::type);
	}

	template<typename TPtr>
	__forceinline TPtr GetPointer(const TPtr& pStartBlock, size_t offset, size_t size)
	{
		return Shift(pStartBlock, offset);
	}

	template<typename TDataType, typename TPtr, typename TGlobalAllocator = Sailor::Memory::DefaultGlobalAllocator>
	TDataType Allocate(size_t size, TGlobalAllocator* allocator)
	{
		TDataType newObj{};
		newObj.m_ptr = static_cast<TPtr>(allocator->Allocate(size));
		return newObj;
	}

	template<typename TDataType, typename TPtr, typename TGlobalAllocator = Sailor::Memory::DefaultGlobalAllocator>
	void Free(TDataType& ptr, TGlobalAllocator* allocator)
	{
		allocator->Free(ptr.m_ptr, ptr.m_size);
		ptr.Clear();
	}

	template<typename TPtr>
	__forceinline bool Align(size_t sizeToEmplace, size_t alignment, const TPtr& startPtr, size_t blockSize, uint32_t& alignmentOffset)
	{
		uint8_t* ptr = GetAddress(startPtr);
		void* alignedPtr = ptr;

		if (std::align(alignment, sizeToEmplace, alignedPtr, blockSize))
		{
			alignmentOffset = (uint32_t)(reinterpret_cast<uint8_t*>(alignedPtr) - ptr);
			return true;
		}
		return false;
	}

	void SAILOR_API RunMemoryBenchmark();
}
