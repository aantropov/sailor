#include "Memory.h"
#include <cstdlib>
#include <malloc.h>
#include "Utils.h"
#include "MemoryBlockAllocator.hpp"

using namespace Sailor;
using namespace Sailor::Memory;

void* HeapAllocator::Allocate(size_t size, HeapAllocator*)
{
	return malloc(size);
}

void HeapAllocator::Free(void* pData, HeapAllocator*)
{
	free(pData);
}

void Sailor::Memory::TestPerformance()
{
	static const uint32 ITERATIONS_COUNT = 10;
	static const uint32 ALLOCATIONS_COUNT = 1000000;
	static const uint32 ALLOCATOR_SIZE = 1024 * 1024 * 32;
	static const uint32 MAX_SIZE = 32;

	static const uint32 STACK_ITERATIONS_COUNT = 10000;
	static const uint32 STACK_ALLOCATIONS_COUNT = 10;

	std::vector<std::pair<void*, size_t> > objs;
	objs.resize(ALLOCATIONS_COUNT);

	srand(0);

	size_t sumSize = 0;

	for (uint32 i = 0; i < objs.size(); ++i)
	{
		objs[i].second = rand() % MAX_SIZE + 1;
		sumSize += objs[i].second;
	}

	printf("Total alloc memory size: %.2f Mb\n\n", (float)((double)(sumSize / 1024) / 1024.0));

	Utils::AccurateTimer stackTimer;
	Utils::AccurateTimer heapTimer;
	Utils::AccurateTimer hybridTimer;

	Utils::AccurateTimer mallocTimer;
	Utils::AccurateTimer allocaTimer;

	{
		{
			std::vector<Memory::TMemoryBlockAllocator<uint32_t*, Memory::HeapAllocator, 2048>::TData> currentObjs;
			currentObjs.resize(ALLOCATIONS_COUNT);

			heapTimer.Start();
			for (int n = 0; n <= ITERATIONS_COUNT; ++n)
			{
				Memory::TMemoryBlockAllocator<uint32_t*, Memory::HeapAllocator, 2048> heapAllocator;

				for (uint32 i = 0; i < objs.size(); ++i)
					currentObjs[i] = heapAllocator.Allocate(objs[i].second);

				for (uint32 i = 0; i < objs.size(); ++i)
					heapAllocator.Free(currentObjs[i]);
			}
			heapTimer.Stop();
		}

		mallocTimer.Start();
		for (int n = 0; n <= ITERATIONS_COUNT; ++n)
		{
			for (uint32 i = 0; i < objs.size(); ++i)
				objs[i].first = malloc(objs[i].second);
			for (uint32 i = 0; i < objs.size(); ++i)
				free(objs[i].first);
		}
		mallocTimer.Stop();
			

		/*
		{
			std::vector<std::pair<Memory::TMemoryBlockAllocator<uint32_t*, Memory::HeapAllocator, 524288, Memory::StackAllocator<4096>>::TData, size_t> > currentObjs;
			currentObjs.resize(ALLOCATIONS_COUNT);

			hybridTimer.Start();
			for (int n = 0; n <= ITERATIONS_COUNT; ++n)
			{
				Memory::TMemoryBlockAllocator<uint32_t*, Memory::HeapAllocator, 524288, Memory::StackAllocator<4096>> hybridAllocator;

				for (uint32 i = 0; i < objs.size(); ++i)
					currentObjs[i].first = hybridAllocator.Allocate(objs[i].second);
				for (uint32 i = 0; i < objs.size(); ++i)
					hybridAllocator.Free(currentObjs[i].first);
			}
			hybridTimer.Stop();
		}

		/*
		allocaTimer.Start();
		{
			objs.resize(STACK_ALLOCATIONS_COUNT);
			for (int n = 0; n <= STACK_ITERATIONS_COUNT; ++n)
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
			std::vector<std::pair<Memory::TMemoryBlockAllocator<uint32_t*, Memory::StackAllocator<4096>, 256>::TData, size_t> > objs;
			objs.resize(STACK_ALLOCATIONS_COUNT);

			Memory::TMemoryBlockAllocator<uint32_t*, Memory::StackAllocator<4096>, 256> stackAllocator;
			for (int n = 0; n <= ITERATIONS_COUNT; ++n)
			{
				for (uint32 i = 0; i < objs.size(); ++i)
					objs[i].first = stackAllocator.Allocate(objs[i].second);
				for (uint32 i = 0; i < objs.size(); ++i)
					stackAllocator.Free(objs[i].first);
			}
		}
		stackTimer.Stop();
		*/

		//printf("allocAllocator %lld ms\n", allocaTimer.ResultAccumulatedMs());
		//printf("stackAllocator %lld ms\n", stackTimer.ResultAccumulatedMs());

		printf("mallocAllocator %lld ms\n", mallocTimer.ResultAccumulatedMs());
		printf("heapAllocator %lld ms\n", heapTimer.ResultAccumulatedMs());
		printf("hybridTimer %lld ms\n", hybridTimer.ResultAccumulatedMs());
	}
}
