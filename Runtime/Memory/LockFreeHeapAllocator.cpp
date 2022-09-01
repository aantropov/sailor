#pragma once
#include "LockFreeHeapAllocator.h"
#include <windows.h>
#include <mutex>

using namespace Sailor;
using namespace Sailor::Memory;

// For now TConcurrentMap doesn't have dll interface, so cannot 
// handle that in class
std::unique_ptr<TConcurrentMap<DWORD, TUniquePtr<HeapAllocator>, 8, Memory::MallocAllocator>>& GetAllocator()
{
	static std::unique_ptr<TConcurrentMap<DWORD, TUniquePtr<HeapAllocator>, 8, Memory::MallocAllocator>> g_lockFreeAllocators;
	static std::once_flag s_bIsInitedAllocator;
	std::call_once(s_bIsInitedAllocator, [&]()
	{
		g_lockFreeAllocators = std::make_unique<TConcurrentMap<DWORD, TUniquePtr<HeapAllocator>, 8, Memory::MallocAllocator>>();
	});

	return g_lockFreeAllocators;
}

void* LockFreeHeapAllocator::allocate(size_t size, size_t alignment)
{
	auto& allocator = GetAllocator();
	const DWORD currentThreadId = GetCurrentThreadId();
	void* res = nullptr;

	assert(currentThreadId < 100000000);

	auto& pAllocator = allocator->At_Lock(currentThreadId);

	if (!pAllocator)
	{
		pAllocator = TUniquePtr<HeapAllocator>::Make();
	}

	res = pAllocator->Allocate(size + sizeof(DWORD), alignment);
	allocator->Unlock(currentThreadId);

	if (!res)
	{
		return nullptr;
	}

	((DWORD*)res)[0] = currentThreadId;

	return &((DWORD*)res)[1];
}

void* LockFreeHeapAllocator::reallocate(void* ptr, size_t size, size_t alignment)
{
	auto& allocator = GetAllocator();
	void* pRaw = (((DWORD*)ptr) - 1);
	const DWORD allocatedThreadId = *(DWORD*)pRaw;

	void* res = allocator->At_Lock(allocatedThreadId)->Reallocate(pRaw, size + sizeof(DWORD), alignment);
	allocator->Unlock(allocatedThreadId);

	if (res == pRaw)
	{
		return ptr;
	}

	if (!res)
	{
		return nullptr;
	}

	return &((DWORD*)res)[1];
}

void LockFreeHeapAllocator::free(void* ptr, size_t size)
{
	if (ptr != nullptr)
	{
		auto& allocator = GetAllocator();
		void* pRaw = (((DWORD*)ptr) - 1);
		const DWORD allocatedThreadId = *((DWORD*)pRaw);

		assert(allocator->ContainsKey(allocatedThreadId));
		allocator->At_Lock(allocatedThreadId)->Free(pRaw);
		allocator->Unlock(allocatedThreadId);
	}
}
