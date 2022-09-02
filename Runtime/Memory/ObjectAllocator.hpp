#pragma once
#include <cassert>
#include <memory>
#include "Core/SpinLock.h"
#include "Core/Defines.h"
#include "HeapAllocator.h"
#include "MallocAllocator.hpp"
#include "SharedPtr.hpp"

namespace Sailor::Memory
{
	using ObjectAllocatorPtr = TSharedPtr<class ObjectAllocator>;

	// Forcely mutexed heap allocator that is used to 
	// handle Objects
	class ObjectAllocator final
	{
	public:

		SAILOR_API void* Allocate(size_t size, size_t alignment = 8)
		{
			return m_globalAllocator.Allocate(size, alignment);
		}

		SAILOR_API bool Reallocate(void* ptr, size_t size, size_t alignment = 8)
		{
			return m_globalAllocator.Reallocate(ptr, size, alignment);
		}

		SAILOR_API void Free(void* ptr, size_t size = 0)
		{
			m_globalAllocator.Free(ptr);
		}

		SAILOR_API ObjectAllocator() = default;

		SAILOR_API ObjectAllocator(const ObjectAllocator&) = delete;
		SAILOR_API ObjectAllocator(ObjectAllocator&&) = delete;

		SAILOR_API ObjectAllocator& operator=(const ObjectAllocator&) = delete;
		SAILOR_API ObjectAllocator& operator=(ObjectAllocator&&) = delete;

	protected:

		DefaultGlobalAllocator m_globalAllocator;
	};
}