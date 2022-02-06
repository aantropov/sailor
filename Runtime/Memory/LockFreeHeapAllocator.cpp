#pragma once
#include "LockFreeHeapAllocator.h"
#include <windows.h>

using namespace Sailor;
using namespace Sailor::Memory;

TConcurrentMap<DWORD, TUniquePtr<HeapAllocator>, 8, Memory::MallocAllocator> g_lockFreeAllocators;

void* LockFreeHeapAllocator::Allocate(size_t size, size_t alignment)
{
	const DWORD currentThreadId = GetCurrentThreadId();
	void* res = nullptr;

	auto& pAllocator = g_lockFreeAllocators.At_Lock(currentThreadId);

	if (!pAllocator)
	{
		pAllocator = TUniquePtr<HeapAllocator>::Make();
	}

	res = pAllocator->Allocate(size + sizeof(DWORD), alignment);
	g_lockFreeAllocators.Unlock(currentThreadId);

	if (!res)
	{
		return nullptr;
	}

	((DWORD*)res)[0] = currentThreadId;

	return &((DWORD*)res)[1];
}

void* LockFreeHeapAllocator::Reallocate(void* ptr, size_t size, size_t alignment)
{
	const DWORD allocatedThreadId = *(((DWORD*)ptr) - 1);
	void* pRaw = (((DWORD*)ptr) - 1);

	void* res = g_lockFreeAllocators.At_Lock(allocatedThreadId)->Reallocate(pRaw, size + sizeof(DWORD), alignment);
	g_lockFreeAllocators.Unlock(allocatedThreadId);

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

void LockFreeHeapAllocator::Free(void* ptr, size_t size)
{
	if (ptr != nullptr)
	{
		void* pRaw = (((DWORD*)ptr) - 1);
		const DWORD allocatedThreadId = *(((DWORD*)ptr) - 1);

		g_lockFreeAllocators.At_Lock(allocatedThreadId)->Free(pRaw);
		g_lockFreeAllocators.Unlock(allocatedThreadId);
	}
}
