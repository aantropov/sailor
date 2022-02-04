#pragma once
#include <cassert>
#include <memory>
#include "Core/Defines.h"

namespace Sailor::Memory
{
	class SAILOR_API MallocAllocator
	{
	public:
		inline void* Allocate(size_t size, size_t alignment = 8)
		{
			return std::malloc(size);
		}

		inline void* Reallocate(void* ptr, size_t size, size_t alignment = 8)
		{
			return std::realloc(ptr, size);
		}

		inline void Free(void* ptr, size_t size = 0)
		{
			std::free(ptr);
		}
	};
}