#pragma once
#include "LockFreeHeapAllocator.h"
#include <windows.h>
#include <mutex>

#include "Memory/UniquePtr.hpp"
#include "Containers/ConcurrentMap.h"
#include "HeapAllocator.h"

using namespace Sailor;
using namespace Sailor::Memory;

// For now TConcurrentMap doesn't have dll interface, so cannot 
// handle that in class
std::unique_ptr<TConcurrentMap<DWORD, TUniquePtr<HeapAllocator>, 8, ERehashPolicy::Never, Memory::MallocAllocator>>& GetAllocator()
{
	static std::unique_ptr<TConcurrentMap<DWORD, TUniquePtr<HeapAllocator>, 8, ERehashPolicy::Never, Memory::MallocAllocator>> g_lockFreeAllocators =
		std::make_unique<TConcurrentMap<DWORD, TUniquePtr<HeapAllocator>, 8, ERehashPolicy::Never, Memory::MallocAllocator>>();

	return g_lockFreeAllocators;
}

void* LockFreeHeapAllocator::allocate(size_t size, size_t alignment)
{
	auto& allocator = GetAllocator();
	const DWORD currentThreadId = GetCurrentThreadId();
	void* res = nullptr;

	check(currentThreadId < 100000000);

	auto& pAllocator = allocator->At_Lock(currentThreadId);

	if (!pAllocator)
	{
		pAllocator = TUniquePtr<HeapAllocator>::Make();
	}

	res = pAllocator->Allocate(size + sizeof(DWORD), alignment);
	((DWORD*)res)[0] = currentThreadId;
	allocator->Unlock(currentThreadId);

	if (!res)
	{
		return nullptr;
	}

	return &((DWORD*)res)[1];
}

bool LockFreeHeapAllocator::reallocate(void* ptr, size_t size, size_t alignment)
{
	auto& allocator = GetAllocator();
	void* pRaw = (((DWORD*)ptr) - 1);
	const DWORD allocatedThreadId = *(DWORD*)pRaw;

	bool res = allocator->At_Lock(allocatedThreadId)->Reallocate(pRaw, size + sizeof(DWORD), alignment);
	allocator->Unlock(allocatedThreadId);

	check(((DWORD*)pRaw)[0] == allocatedThreadId);

	return res;
}

void LockFreeHeapAllocator::free(void* ptr, size_t size)
{
	if (ptr != nullptr)
	{
		auto& allocator = GetAllocator();
		void* pRaw = (((DWORD*)ptr) - 1);
		const DWORD allocatedThreadId = *((DWORD*)pRaw);

		check(allocator->ContainsKey(allocatedThreadId));
		allocator->At_Lock(allocatedThreadId)->Free(pRaw);
		allocator->Unlock(allocatedThreadId);
	}
}
