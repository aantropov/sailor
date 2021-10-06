#pragma once
#include <array>
#include <limits>
#include <cassert>
#include <unordered_map>
#include <unordered_set>
#include <iostream>

#include "Defines.h"

#define InvalidIndex std::numeric_limits<size_t>::max()

namespace Sailor::Memory
{
	class HeapAllocator
	{
	public:

		static SAILOR_API void* Allocate(size_t size);
		static SAILOR_API void Free(void* pData);
	};

	template<typename TDataType, typename TPtrType>
	TPtrType GetAddress(TDataType& pData)
	{
		return static_cast<TPtrType>((uint8_t*)(pData.m_ptr) + pData.m_offset);
	}

	template<typename TPtrType>
	size_t Size(TPtrType from)
	{
		return sizeof(*from);
	}

	template<typename TDataType, typename TPtrType>
	TDataType Allocate(size_t size)
	{
		TDataType newObj{};
		newObj.m_ptr = static_cast<TPtrType>(HeapAllocator::Allocate(size));
		return newObj;
	}

	template<typename TDataType, typename TPtrType>
	void Free(TDataType& ptr)
	{
		HeapAllocator::Free(ptr.m_ptr);
		ptr.Clear();
	}
}