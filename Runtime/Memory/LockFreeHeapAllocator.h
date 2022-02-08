#pragma once
#include <cassert>
#include "Core/Defines.h"
#include "Memory/UniquePtr.hpp"
#include "HeapAllocator.h"
#include "Containers/ConcurrentMap.h"

namespace Sailor::Memory
{
	// Global allocator
	class SAILOR_API LockFreeHeapAllocator
	{
	public:

		__forceinline void* Allocate(size_t size, size_t alignment = 8) { return LockFreeHeapAllocator::allocate(size, alignment); }
		__forceinline void* Reallocate(void* ptr, size_t size, size_t alignment = 8) { return LockFreeHeapAllocator::reallocate(ptr, size, alignment); }
		__forceinline void Free(void* ptr, size_t size = 0) { LockFreeHeapAllocator::free(ptr, size); }

		// Used for smart ptrs
		static void* allocate(size_t size, size_t alignment = 8);
		static void* reallocate(void* ptr, size_t size, size_t alignment = 8);
		static void free(void* ptr, size_t size = 0);

	protected:
	};
}