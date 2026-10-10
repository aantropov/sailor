#pragma once
#include <algorithm>
#include <cassert>
#include <cstdlib>
#include <memory>
#if defined(_WIN32)
#include <malloc.h>
#endif
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
			check(alignment != 0 && (alignment & (alignment - 1)) == 0);
			alignment = (std::max)(alignment, alignof(void*));
			void* ptr = nullptr;
#if defined(_WIN32)
			ptr = _aligned_malloc(size ? size : 1, alignment);
#else
			if (posix_memalign(&ptr, alignment, size ? size : 1) != 0)
			{
				return nullptr;
			}
#endif
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
#if defined(_WIN32)
			_aligned_free(ptr);
#else
			std::free(ptr);
#endif
		}
	};
}
