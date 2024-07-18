#pragma once
#include <cassert>
#include <memory>
#include "Core/Defines.h"
#include "BaseAllocator.hpp"

namespace Sailor::Memory
{
	// Global allocator
	class SAILOR_API MallocAllocator : public IBaseAllocator
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
			auto ptr = std::malloc(size);
			SAILOR_PROFILE_ALLOC(ptr, size);
			return ptr;
		}

		static bool reallocate(void* ptr, size_t size, size_t alignment = 8)
		{
			return false;
		}

		static void free(void* ptr, size_t size = 0)
		{
			SAILOR_PROFILE_FREE(ptr);
			std::free(ptr);
		}
	};
}