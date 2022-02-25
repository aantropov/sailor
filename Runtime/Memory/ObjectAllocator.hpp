#pragma once
#include <cassert>
#include <memory>
#include "Core/SpinLock.h"
#include "Core/Defines.h"
#include "HeapAllocator.h"
#include "SharedPtr.hpp"

namespace Sailor::Memory
{
	using ObjectAllocatorPtr = TSharedPtr<class ObjectAllocator>;

	// Forcely mutexed heap allocator that is used to 
	// handle Objects
	class ObjectAllocator final
	{
	public:

		void* Allocate(size_t size, size_t alignment = 8)
		{
			m_lock.Lock();
			void* res = m_heapAllocator.Allocate(size, alignment);
			m_lock.Unlock();
			
			return res;
		}

		void* Reallocate(void* ptr, size_t size, size_t alignment = 8)
		{
			m_lock.Lock();
			void* res = m_heapAllocator.Reallocate(ptr, size, alignment);
			m_lock.Unlock();
			return res;
		}

		void Free(void* ptr, size_t size = 0)
		{
			m_lock.Lock();
			m_heapAllocator.Free(ptr);
			m_lock.Unlock();
		}

		ObjectAllocator() = default;

		ObjectAllocator(const ObjectAllocator&) = delete;
		ObjectAllocator(ObjectAllocator&&) = delete;

		ObjectAllocator& operator=(const ObjectAllocator&) = delete;
		ObjectAllocator& operator=(ObjectAllocator&&) = delete;

	protected:

		SpinLock m_lock;
		HeapAllocator m_heapAllocator;
	};
}