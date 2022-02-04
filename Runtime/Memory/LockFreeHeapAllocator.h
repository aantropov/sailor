#pragma once
#include <cassert>
#include "Core/Defines.h"
#include "Memory/UniquePtr.hpp"
#include "HeapAllocator.h"
#include "Containers/ConcurrentMap.h"

namespace Sailor::Memory
{
	class LockFreeHeapAllocator
	{
	public:

		inline SAILOR_API void* Allocate(size_t size, size_t alignment = 8);
		inline SAILOR_API void* Reallocate(void* ptr, size_t size, size_t alignment = 8);
		inline SAILOR_API void Free(void* ptr, size_t size = 0);

	protected:

		static TConcurrentMap<DWORD, TUniquePtr<HeapAllocator>, 8, Memory::MallocAllocator> m_allocators;
	};
}