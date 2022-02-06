#pragma once
#include <cassert>
#include "Core/Defines.h"
#include "Memory/UniquePtr.hpp"
#include "HeapAllocator.h"
#include "Containers/ConcurrentMap.h"

namespace Sailor::Memory
{
	class SAILOR_API LockFreeHeapAllocator
	{
	public:

		inline void* Allocate(size_t size, size_t alignment = 8);
		inline void* Reallocate(void* ptr, size_t size, size_t alignment = 8);
		inline void Free(void* ptr, size_t size = 0);

	protected:
	};
}