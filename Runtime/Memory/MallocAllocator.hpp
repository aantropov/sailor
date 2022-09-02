#pragma once
#include <cassert>
#include <memory>
#include "Core/Defines.h"

namespace Sailor::Memory
{
	// Global allocator
	class SAILOR_API MallocAllocator
	{
	public:

		__forceinline void* Allocate(size_t size, size_t alignment = 8)
		{
			return MallocAllocator::allocate(size, alignment);
		}

		__forceinline bool Reallocate(void* ptr, size_t size, size_t alignment = 8)
		{
			return MallocAllocator::reallocate(ptr, size, alignment);
		}

		__forceinline void Free(void* ptr, size_t size = 0)
		{
			MallocAllocator::free(ptr, size);
		}

		// Used for smart ptrs
		static void* allocate(size_t size, size_t alignment = 8)
		{
			return std::malloc(size);
		}

		static bool reallocate(void* ptr, size_t size, size_t alignment = 8)
		{
			return false;
		}

		static void free(void* ptr, size_t size = 0)
		{
			std::free(ptr);
		}
	};
}