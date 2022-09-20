#pragma once
#include <array>
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
		void* ptr = allocator.Allocate(sizeof(T), 8);
		return new (ptr) T();
	}

	template<typename T, typename TAllocator, typename... TArgs>
	SAILOR_API __forceinline T* New(TAllocator& allocator, TArgs&& ... args)
	{
		void* ptr = allocator.Allocate(sizeof(T));
		return new (ptr) T(std::forward(args) ...);
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

		uint8_t m_stack[stackSize];
		uint16_t m_index = 0u;

		TAllocator m_allocator{};

		bool Contains(void* pData) const
		{
			return pData < &m_stack[stackSize] && &m_stack[sizeof(uint16_t) - 1] <= pData;
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
			// We store size of element before allocated memory
			size_t requiredSize = size + sizeof(uint16_t);

			if (stackSize - m_index - 1 < requiredSize)
			{
				return m_allocator.Allocate(size, alignment);
			}

			uint8_t* res = &m_stack[m_index];
			m_index += (uint32_t)(requiredSize);

			assert(size < 65536);

			*(uint16_t*)res = (uint16_t)size;

			return res + sizeof(uint16_t);
		}

		bool Reallocate(void* pData, size_t size, size_t alignment = 8)
		{
			if (Contains(pData) && size < 65536)
			{
				uint16_t usedSpace = *((uint8_t*)pData - sizeof(uint16_t)) + sizeof(uint16_t);

				// we can realloc only if we're at top of stack
				if (&((uint8_t*)pData - sizeof(uint16_t))[usedSpace] == &m_stack[m_index])
				{
					const uint16_t extraSpace = (uint16_t)(size - (usedSpace - sizeof(uint16_t)));

					// we can reallocate
					if (extraSpace <= (stackSize - m_index))
					{
						*(uint16_t*)((uint8_t*)pData - sizeof(uint16_t)) = (uint16_t)size;
						m_index += extraSpace;
						return true;
					}
				}
			}

			return false;
		}

		void Free(void* pData, size_t size = 0)
		{
			if (Contains(pData))
			{
				uint16_t usedSpace = *((uint8_t*)pData - sizeof(uint16_t)) + sizeof(uint16_t);

				// we can only remove the objects that are placed on the top of the stack
				if (&((uint8_t*)pData - sizeof(uint16_t))[usedSpace] == &m_stack[m_index])
				{
					m_index -= usedSpace;
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