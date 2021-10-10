#include "Memory.h"
#include <cstdlib>
#include <malloc.h>
#include "Utils.h"
#include "MemoryBlockAllocator.hpp"

using namespace Sailor;
using namespace Sailor::Memory;

void* HeapAllocator::Allocate(size_t size)
{
	return malloc(size);
}

void HeapAllocator::Free(void* pData, size_t size)
{
	free(pData);
}

void Sailor::Memory::TestPerformance()
{
//	std::vector<Memory::BlockAllocator::TBlockAllocator<uint32_t*, Memory::HeapAllocator, 512>::TData> currentObjs;
	
//	return;

	static const uint32 IterationsCount = 10;
	static const uint32 AllocationsCount = 1000000;
	static const uint32 AllocatorSize = 1024 * 1024 * 32;
	static const uint32 MaxSize = 32;

	static const uint32 StackIterationsCount = 1000;
	static const uint32 StackAllocationsCount = 30;

	std::vector<std::pair<void*, size_t> > objs;
	objs.resize(AllocationsCount);

	srand(0);

	size_t sumSize = 0;

	for (uint32 i = 0; i < objs.size(); ++i)
	{
		objs[i].second = rand() % MaxSize + 1;
		sumSize += objs[i].second;
	}

	printf("Total alloc memory size: %.2f Mb\n\n", (float)((double)(sumSize / 1024) / 1024.0));

	Utils::AccurateTimer stackTimer;
	Utils::AccurateTimer heapTimer;
	Utils::AccurateTimer hybridTimer;

	Utils::AccurateTimer mallocTimer;
	Utils::AccurateTimer allocaTimer;

	{
		std::vector<Memory::TBlockAllocator<uint32_t*, Memory::HeapAllocator, 4096>::TData> currentObjs;
		currentObjs.resize(AllocationsCount);

		heapTimer.Start();

		for (int n = 0; n <= IterationsCount; ++n)
		{
			Memory::TBlockAllocator<uint32_t*, Memory::HeapAllocator, 4096> heapAllocator;

			for (uint32 i = 0; i < objs.size(); ++i)
				currentObjs[i] = heapAllocator.Allocate(objs[i].second);

			for (uint32 i = 0; i < objs.size(); ++i)
				heapAllocator.Free(currentObjs[i]);
		}

		heapTimer.Stop();
	}

	mallocTimer.Start();
	for (int n = 0; n <= IterationsCount; ++n)
	{
		for (uint32 i = 0; i < objs.size(); ++i)
			objs[i].first = malloc(objs[i].second);
		for (uint32 i = 0; i < objs.size(); ++i)
			free(objs[i].first);
	}
	mallocTimer.Stop();

	allocaTimer.Start();
	{
		objs.resize(StackAllocationsCount);
		for (int n = 0; n <= StackIterationsCount; ++n)
		{
			for (uint32 i = 0; i < objs.size(); ++i)
				objs[i].first = _alloca(objs[i].second);
			for (uint32 i = 0; i < objs.size(); ++i)
				_freea(objs[i].first);
		}
	}
	allocaTimer.Stop();

	stackTimer.Start();
	{
		std::vector<Memory::TBlockAllocator<uint32_t*, Memory::StackAllocator<4096>>::TData> currentObjs;
		currentObjs.resize(StackAllocationsCount);

		for (int n = 0; n <= StackIterationsCount; ++n)
		{
			Memory::TBlockAllocator<uint32_t*, Memory::StackAllocator<4096>> stackAllocator;

			for (uint32 i = 0; i < objs.size(); ++i)
				currentObjs[i] = stackAllocator.Allocate(objs[i].second);

			for (uint32 i = 0; i < objs.size(); ++i)
				stackAllocator.Free(currentObjs[i]);
		}
	}
	stackTimer.Stop();

	printf("allocAllocator %lld ms\n", allocaTimer.ResultAccumulatedMs());
	printf("stackAllocator %lld ms\n", stackTimer.ResultAccumulatedMs());

	printf("mallocAllocator %lld ms\n", mallocTimer.ResultAccumulatedMs());
	printf("heapAllocator %lld ms\n", heapTimer.ResultAccumulatedMs());
}
