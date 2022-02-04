#pragma once
#include "LockFreeHeapAllocator.h"
#include <windows.h>

using namespace Sailor;
using namespace Sailor::Memory;

void* LockFreeHeapAllocator::Allocate(size_t size, size_t alignment)
{
	const DWORD currentThreadId = GetCurrentThreadId();
	void* res = nullptr;

	auto& pAllocator = m_allocators.At_Lock(currentThreadId);

	if (!pAllocator)
	{
		pAllocator = TUniquePtr<HeapAllocator>::Make();
	}

	res = pAllocator->Allocate(size + sizeof(DWORD), alignment);
	m_allocators.Unlock(currentThreadId);

	((DWORD*)res)[0] = currentThreadId;

	return &((DWORD*)res)[1];
}

void* LockFreeHeapAllocator::Reallocate(void* ptr, size_t size, size_t alignment)
{
	const DWORD allocatedThreadId = *(((DWORD*)ptr) - 1);
	void* pRaw = (((DWORD*)ptr) - 1);

	void* res = nullptr;

	res = m_allocators.At_Lock(allocatedThreadId)->Reallocate(pRaw, size + sizeof(DWORD), alignment);
	m_allocators.Unlock(allocatedThreadId);

	if (res == pRaw)
	{
		return ptr;
	}

	((DWORD*)res)[0] = allocatedThreadId;
	return &((DWORD*)res)[1];
}

void LockFreeHeapAllocator::Free(void* ptr, size_t size)
{
	if (ptr != nullptr)
	{
		void* pRaw = (((DWORD*)ptr) - 1);
		const DWORD allocatedThreadId = *(((DWORD*)ptr) - 1);

		m_allocators.At_Lock(allocatedThreadId)->Free(pRaw);
		m_allocators.Unlock(allocatedThreadId);
	}
}

TConcurrentMap<DWORD, TUniquePtr<HeapAllocator>, 8, Memory::MallocAllocator> LockFreeHeapAllocator::m_allocators;
