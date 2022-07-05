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
			m_lock.Lock();
			void* res = m_globalAllocator.Allocate(size, alignment);
			m_lock.Unlock();
			
			return res;
		}

		SAILOR_API void* Reallocate(void* ptr, size_t size, size_t alignment = 8)
		{
			m_lock.Lock();
			void* res = m_globalAllocator.Reallocate(ptr, size, alignment);
			m_lock.Unlock();
			return res;
		}

		SAILOR_API void Free(void* ptr, size_t size = 0)
		{
			m_lock.Lock();
			m_globalAllocator.Free(ptr);
			m_lock.Unlock();
		}

		SAILOR_API ObjectAllocator() = default;

		SAILOR_API ObjectAllocator(const ObjectAllocator&) = delete;
		SAILOR_API ObjectAllocator(ObjectAllocator&&) = delete;

		SAILOR_API ObjectAllocator& operator=(const ObjectAllocator&) = delete;
		SAILOR_API ObjectAllocator& operator=(ObjectAllocator&&) = delete;

	protected:

		SpinLock m_lock;
		DefaultGlobalAllocator m_globalAllocator;
	};
}