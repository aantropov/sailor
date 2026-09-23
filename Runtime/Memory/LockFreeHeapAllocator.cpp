#include "LockFreeHeapAllocator.h"
#include <mutex>

#include "Memory/UniquePtr.hpp"
#include "Containers/ConcurrentMap.h"
#include "HeapAllocator.h"

using namespace Sailor;
using namespace Sailor::Memory;

namespace
{
	struct AllocationHeader
	{
		void* m_raw;
		DWORD m_threadId;
	};
}

// For now TConcurrentMap doesn't have dll interface, so cannot 
// handle that in class
TUniquePtr<TConcurrentMap<DWORD, TUniquePtr<HeapAllocator>, 8, ERehashPolicy::Never, Memory::MallocAllocator>>& GetAllocator()
{
	static TUniquePtr<TConcurrentMap<DWORD, TUniquePtr<HeapAllocator>, 8, ERehashPolicy::Never, Memory::MallocAllocator>> g_lockFreeAllocators =
		TUniquePtr<TConcurrentMap<DWORD, TUniquePtr<HeapAllocator>, 8, ERehashPolicy::Never, Memory::MallocAllocator>>::Make();

	return g_lockFreeAllocators;
}

void* LockFreeHeapAllocator::allocate(size_t size, size_t alignment)
{
	auto& allocator = GetAllocator();
	const DWORD currentThreadId = GetCurrentThreadId();
	check(alignment != 0 && (alignment & (alignment - 1)) == 0);
	alignment = (std::max)(alignment, alignof(AllocationHeader));
	const size_t allocationSize = size + sizeof(AllocationHeader) + alignment - 1;

	auto& pAllocator = allocator->At_Lock(currentThreadId);

	if (!pAllocator)
	{
		pAllocator = TUniquePtr<HeapAllocator>::Make();
	}

	void* raw = pAllocator->Allocate(allocationSize, alignof(AllocationHeader));
	allocator->Unlock(currentThreadId);

	if (!raw)
	{
		return nullptr;
	}

	void* res = static_cast<uint8_t*>(raw) + sizeof(AllocationHeader);
	size_t available = allocationSize - sizeof(AllocationHeader);
	std::align(alignment, size, res, available);
	new (static_cast<uint8_t*>(res) - sizeof(AllocationHeader)) AllocationHeader{ raw, currentThreadId };
	return res;
}

bool LockFreeHeapAllocator::reallocate(void* ptr, size_t size, size_t alignment)
{
	auto& allocator = GetAllocator();
	check(ptr);
	check(alignment != 0 && (alignment & (alignment - 1)) == 0);
	if (reinterpret_cast<uintptr_t>(ptr) % alignment != 0)
	{
		return false;
	}
	const auto* header = reinterpret_cast<const AllocationHeader*>(static_cast<uint8_t*>(ptr) - sizeof(AllocationHeader));
	void* pRaw = header->m_raw;
	const DWORD allocatedThreadId = header->m_threadId;
	const size_t prefixSize = static_cast<uint8_t*>(ptr) - static_cast<uint8_t*>(pRaw);

	bool res = allocator->At_Lock(allocatedThreadId)->Reallocate(pRaw, size + prefixSize, alignof(AllocationHeader));
	allocator->Unlock(allocatedThreadId);

	return res;
}

void LockFreeHeapAllocator::free(void* ptr, size_t size)
{
	if (ptr != nullptr)
	{
		auto& allocator = GetAllocator();
		const auto* header = reinterpret_cast<const AllocationHeader*>(static_cast<uint8_t*>(ptr) - sizeof(AllocationHeader));
		void* pRaw = header->m_raw;
		const DWORD allocatedThreadId = header->m_threadId;

		check(allocator->ContainsKey(allocatedThreadId));
		allocator->At_Lock(allocatedThreadId)->Free(pRaw);
		allocator->Unlock(allocatedThreadId);
	}
}
