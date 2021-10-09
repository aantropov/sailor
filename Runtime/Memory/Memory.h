#pragma once
#include <array>
#include <limits>
#include <cassert>
#include <unordered_map>
#include <unordered_set>
#include <iostream>

#include "Defines.h"

#define InvalidIndex std::numeric_limits<uint32_t>::max()

namespace Sailor::Memory
{
	class HeapAllocator
	{
	public:

		SAILOR_API void* Allocate(size_t size);
		SAILOR_API void Free(void* pData, size_t size);
	};

	template<uint32_t stackSize = 1024>
	class StackAllocator
	{
	protected:
		uint8_t m_stack[stackSize];
		uint32_t m_index = 0;

	public:

		SAILOR_API void* Allocate(size_t size)
		{
			void* res = (void*)(&m_stack[m_index]);
			m_index += (uint32_t)(size);
			return res;
		}

		SAILOR_API void Free(void* pData, size_t size)
		{
			// we can only remove the objects that are placed on the top of the stack
			if (&((uint8_t*)pData)[(uint32_t)size] == &m_stack[m_index])
			{
				m_index -= (uint32_t)size;
			}
		}
	};

	template<typename TDataType, typename TPtrType>
	TPtrType GetAddress(TDataType& pData)
	{
		return reinterpret_cast<TPtrType>(&(((uint8_t*)(pData.m_ptr))[pData.m_offset]));
	}

	template<typename TPtrType>
	size_t Size(TPtrType from)
	{
		return sizeof(*from);
	}

	template<typename TDataType, typename TPtrType, typename TAllocator = HeapAllocator>
	TDataType Allocate(size_t size, TAllocator* allocator)
	{
		TDataType newObj{};
		newObj.m_ptr = static_cast<TPtrType>(allocator->Allocate(size));
		return newObj;
	}

	template<typename TDataType, typename TPtrType, typename TAllocator = HeapAllocator>
	void Free(TDataType& ptr, TAllocator* allocator)
	{
		allocator->Free(ptr.m_ptr, ptr.m_size);
		ptr.Clear();
	}

	void SAILOR_API TestPerformance();
}