#include "Memory.h"
#include <cstdlib>
#include <malloc.h>
#include <random>
#include "Utils.h"
#include "MemoryBlockAllocator.hpp"
#include "MemoryPoolAllocator.hpp"
#include "MemoryMultiPoolAllocator.hpp"

using namespace Sailor;
using namespace Sailor::Memory;

void* GlobalHeapAllocator::Allocate(size_t size)
{
	return malloc(size);
}

void GlobalHeapAllocator::Free(void* pData, size_t size)
{
	free(pData);
}

void Sailor::Memory::TestPerformance()
{
	static const uint32 IterationsCount = 10;
	static const uint32 AllocationsCount = 1000000;
	static const uint32 AllocatorSize = 1024 * 1024 * 32;
	static const uint32 MaxSize = 32;

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

	Utils::AccurateTimer poolAllocatorTimer;
	Utils::AccurateTimer multiPoolAllocatorTimer;
	Utils::AccurateTimer blockAllocatorTimer;
	Utils::AccurateTimer mallocTimer;

	mallocTimer.Start();
	for (int n = 0; n <= IterationsCount; ++n)
	{
		for (uint32 i = 0; i < objs.size(); ++i)
			objs[i].first = malloc(objs[i].second);

		mallocTimer.Stop();

		std::random_device rd;
		std::mt19937 g(rd());
		//std::shuffle(objs.begin(), objs.end(), g);

		mallocTimer.Start();
		for (uint32 i = 0; i < objs.size(); ++i)
			free(objs[i].first);
	}
	mallocTimer.Stop();

	size_t maxSizeBlock = 0;
	{
		std::vector<Memory::TMemoryPtr<void*>> currentObjs;
		currentObjs.resize(AllocationsCount);

		blockAllocatorTimer.Start();

		for (int n = 0; n <= IterationsCount; ++n)
		{
			Memory::TBlockAllocator<Memory::GlobalHeapAllocator> heapAllocator(1024, 32);

			for (uint32 i = 0; i < objs.size(); ++i)
				currentObjs[i] = heapAllocator.Allocate(objs[i].second, 1);

			blockAllocatorTimer.Stop();

			maxSizeBlock = heapAllocator.GetOccupiedSpace();

			std::random_device rd;
			std::mt19937 g(rd());
			//std::shuffle(currentObjs.begin(), currentObjs.end(), g);

			blockAllocatorTimer.Start();

			for (uint32 i = 0; i < objs.size(); ++i)
				heapAllocator.Free(currentObjs[i]);
		}

		blockAllocatorTimer.Stop();
	}

	{
		std::vector<Memory::TMemoryPtr<void*>> currentObjs;
		currentObjs.resize(AllocationsCount);

		multiPoolAllocatorTimer.Start();

		for (int n = 0; n <= IterationsCount; ++n)
		{
			Memory::TMultiPoolAllocator heapAllocator;

			for (uint32 i = 0; i < objs.size(); ++i)
				currentObjs[i] = heapAllocator.Allocate(objs[i].second, 1);

			multiPoolAllocatorTimer.Stop();

			std::random_device rd;
			std::mt19937 g(rd());
			//std::shuffle(currentObjs.begin(), currentObjs.end(), g);

			multiPoolAllocatorTimer.Start();

			for (uint32 i = 0; i < objs.size(); ++i)
				heapAllocator.Free(currentObjs[i]);
		}

		multiPoolAllocatorTimer.Stop();
	}

	size_t maxSizePool = 0;
	{
		std::vector<Memory::TMemoryPtr<void*>> currentObjs;
		currentObjs.resize(AllocationsCount);

		for (int n = 0; n <= IterationsCount; ++n)
		{
			poolAllocatorTimer.Start();

			Memory::TPoolAllocator heapAllocator(128, 32);

			for (uint32 i = 0; i < objs.size(); ++i)
				currentObjs[i] = heapAllocator.Allocate(objs[i].second, 1);

			maxSizePool = std::max(maxSizePool, heapAllocator.GetOccupiedSpace());

			poolAllocatorTimer.Stop();

			std::random_device rd;
			std::mt19937 g(rd());
			//std::shuffle(currentObjs.begin(), currentObjs.end(), g);

			poolAllocatorTimer.Start();

			for (uint32 i = 0; i < objs.size(); ++i)
				heapAllocator.Free(currentObjs[i]);

			poolAllocatorTimer.Stop();
		}
	}

	printf("mallocAllocator %lld ms\n", mallocTimer.ResultAccumulatedMs());
	printf("blockAllocator %lld ms, total space %lld kb \n", blockAllocatorTimer.ResultAccumulatedMs(), maxSizeBlock / 1024);
	printf("poolAllocator %lld ms, total space %lld kb \n", poolAllocatorTimer.ResultAccumulatedMs(), maxSizePool / 1024);
	printf("multiPoolAllocator %lld ms\n", multiPoolAllocatorTimer.ResultAccumulatedMs());
}
