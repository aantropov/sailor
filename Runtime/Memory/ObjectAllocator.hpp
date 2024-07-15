#pragma once
#include <cassert>
#include <memory>
#include "Core/SpinLock.h"
#include "Core/Defines.h"
#include "HeapAllocator.h"
#include "MallocAllocator.hpp"
#include "SharedPtr.hpp"
#include "BaseAllocator.hpp"

namespace Sailor::Memory
{
	using ObjectAllocatorPtr = TSharedPtr<class ObjectAllocator>;

	enum class EAllocationPolicy : uint8_t
	{
		SharedMemory_MultiThreaded = 0,
		LocalMemory_SingleThread
	};

	// Allocator that is used only for managed memory (Objects)
	// A bit clumsy, but functional
	class ObjectAllocator final : public IBaseAllocator
	{
	public:

		ObjectAllocator(EAllocationPolicy policy) : m_policy(policy), m_globalAllocator(nullptr)
		{
			switch (m_policy)
			{
			case EAllocationPolicy::SharedMemory_MultiThreaded:
				m_globalAllocator = new DefaultGlobalAllocator();
				break;

			case EAllocationPolicy::LocalMemory_SingleThread:
				m_globalAllocator = new HeapAllocator();
				break;
			}
		}

		SAILOR_API void* Allocate(size_t size, size_t alignment = 8)
		{
			switch (m_policy)
			{
			case EAllocationPolicy::SharedMemory_MultiThreaded:
				return GetAllocator<DefaultGlobalAllocator>()->Allocate(size, alignment);
				break;

			case EAllocationPolicy::LocalMemory_SingleThread:
				return GetAllocator<HeapAllocator>()->Allocate(size, alignment);
				break;
			}

			return nullptr;
		}

		SAILOR_API bool Reallocate(void* ptr, size_t size, size_t alignment = 8)
		{
			switch (m_policy)
			{
			case EAllocationPolicy::SharedMemory_MultiThreaded:
				return GetAllocator<DefaultGlobalAllocator>()->Reallocate(ptr, size, alignment);
				break;

			case EAllocationPolicy::LocalMemory_SingleThread:
				return GetAllocator<HeapAllocator>()->Reallocate(ptr, size, alignment);
				break;
			}

			return false;
		}

		SAILOR_API void Free(void* ptr, size_t size = 0)
		{
			switch (m_policy)
			{
			case EAllocationPolicy::SharedMemory_MultiThreaded:
				return GetAllocator<DefaultGlobalAllocator>()->Free(ptr);
				break;

			case EAllocationPolicy::LocalMemory_SingleThread:
				return GetAllocator<HeapAllocator>()->Free(ptr);
				break;
			}
		}

		SAILOR_API ObjectAllocator() = default;

		ObjectAllocator(const ObjectAllocator&) = delete;
		ObjectAllocator(ObjectAllocator&&) = delete;

		ObjectAllocator& operator=(const ObjectAllocator&) = delete;
		ObjectAllocator& operator=(ObjectAllocator&&) = delete;

		~ObjectAllocator()
		{
			delete m_globalAllocator;
		}

	protected:

		template<typename TAllocator>
		__forceinline TAllocator* GetAllocator() { return reinterpret_cast<TAllocator*>(m_globalAllocator); }

		EAllocationPolicy m_policy;
		IBaseAllocator* m_globalAllocator;
	};
}