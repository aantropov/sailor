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

		static SAILOR_API void* Allocate(size_t size, HeapAllocator* context = nullptr);
		static SAILOR_API void Free(void* pData, HeapAllocator* context = nullptr);
	};

	template<uint32_t stackSize = 1024>
	class StackAllocator
	{
	protected:
		uint8_t m_stack[stackSize];
		uint32_t m_index = 0;

	public:

		static void* Allocate(size_t size, StackAllocator* context = nullptr)
		{
			void* res = (void*)(&context->m_stack[context->m_index + sizeof(uint32_t)]);
			*((uint32_t*)&context->m_stack[context->m_index]) = (uint32_t)size;
			context->m_index += (uint32_t)(size + sizeof(uint32_t));
			return res;
		}

		static void Free(void* pData, StackAllocator* context = nullptr)
		{
			uint32_t size = *(uint32_t*)(((uint8_t*)pData - sizeof(uint32_t)));

			// we can only remove the objects that are placed on the top of the stack
			if (&((uint8_t*)pData)[size] == &context->m_stack[context->m_index])
			{
				context->m_index -= size;
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
	TDataType Allocate(size_t size, TAllocator* context = nullptr)
	{
		TDataType newObj{};
		newObj.m_ptr = static_cast<TPtrType>(TAllocator::Allocate(size, context));
		return newObj;
	}

	template<typename TDataType, typename TPtrType, typename TAllocator = HeapAllocator>
	void Free(TDataType& ptr, TAllocator* context = nullptr)
	{
		TAllocator::Free(ptr.m_ptr, context);
		ptr.Clear();
	}

	void SAILOR_API TestPerformance();
}