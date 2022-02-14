#pragma once
#include <cassert>
#include <memory>
#include <mutex>
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
			std::scoped_lock<std::mutex> guard(m_mutex);
			return m_heapAllocator.Allocate(size, alignment);
		}

		void* Reallocate(void* ptr, size_t size, size_t alignment = 8)
		{
			std::scoped_lock<std::mutex> guard(m_mutex);
			return m_heapAllocator.Reallocate(ptr, size, alignment);
		}

		void Free(void* ptr, size_t size = 0)
		{
			std::scoped_lock<std::mutex> guard(m_mutex);
			return m_heapAllocator.Free(ptr);
		}

		ObjectAllocator() = default;

		ObjectAllocator(const ObjectAllocator&) = delete;
		ObjectAllocator(ObjectAllocator&&) = delete;

		ObjectAllocator& operator=(const ObjectAllocator&) = delete;
		ObjectAllocator& operator=(ObjectAllocator&&) = delete;

	protected:

		std::mutex m_mutex;
		HeapAllocator m_heapAllocator;
	};
}