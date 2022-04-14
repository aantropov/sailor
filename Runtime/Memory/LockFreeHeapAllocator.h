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

		template<typename T>
		T* New()
		{
			void* ptr = allocate(sizeof(T));
			return new (ptr) T();
		}
		/*
		template<typename T, typename... TArgs>
		T* New(TArgs&& ... args)
		{
			void* ptr = allocate(sizeof(T));
			return new (ptr) T(std::forward(args));
		}*/

		template<typename T>
		void Delete(T* ptr)
		{
			if (ptr)
			{
				ptr.~T();
			}

			free(ptr);
		}

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