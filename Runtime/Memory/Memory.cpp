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

class TestMallocAllocator
{
public:
	inline void* Allocate(size_t size, uint32_t alignment)
	{
		return malloc(size);
	}

	inline void Free(void* ptr)
	{
		free(ptr);
	}
};

template<typename TAllocator>
void TestPerformanceRandom(TAllocator& allocator, const std::vector<size_t>& sizesToAllocate, Utils::AccurateTimer& timer)
{
	auto firstTestElement = allocator.Allocate(1, 1);

	std::vector<decltype(firstTestElement)> ptrs;
	ptrs.resize(sizesToAllocate.size());

	timer.Start();
	uint32_t border = (uint32_t)sizesToAllocate.size() / 4;
	for (uint32 i = 0; i < border * 2; ++i)
	{
		ptrs[i] = allocator.Allocate(sizesToAllocate[i], 1);
	}
	timer.Stop();

	std::random_device rd;
	std::mt19937 g(rd());
	std::shuffle(ptrs.begin(), ptrs.end() - (size_t)border * 3, g);

	timer.Start();

	for (uint32 i = 0; i < border; ++i)
	{
		allocator.Free(ptrs[i]);
	}

	for (uint32 i = border * 2; i < sizesToAllocate.size(); ++i)
	{
		ptrs[i] = allocator.Allocate(sizesToAllocate[i], 1);
	}

	for (uint32 i = border; i < sizesToAllocate.size(); ++i)
	{
		allocator.Free(ptrs[i]);
	}
	timer.Stop();
}

template<typename TAllocator>
void TestPerformanceShuffled(TAllocator& allocator, const std::vector<size_t>& sizesToAllocate, Utils::AccurateTimer& timer)
{
	auto firstTestElement = allocator.Allocate(1, 1);

	std::vector<decltype(firstTestElement)> ptrs;
	ptrs.resize(sizesToAllocate.size());

	timer.Start();
	for (uint32 i = 0; i < sizesToAllocate.size(); ++i)
	{
		ptrs[i] = allocator.Allocate(sizesToAllocate[i], 1);
	}
	timer.Stop();

	std::random_device rd;
	std::mt19937 g(rd());
	std::shuffle(ptrs.begin(), ptrs.end(), g);

	timer.Start();
	for (uint32 i = 0; i < sizesToAllocate.size(); ++i)
	{
		allocator.Free(ptrs[i]);
	}
	timer.Stop();
}

template<typename TAllocator>
void TestPerformanceSimple(TAllocator& allocator, const std::vector<size_t>& sizesToAllocate, Utils::AccurateTimer& timer)
{
	auto firstTestElement = allocator.Allocate(1, 1);

	std::vector<decltype(firstTestElement)> ptrs;
	ptrs.resize(sizesToAllocate.size());

	timer.Start();
	for (uint32 i = 0; i < sizesToAllocate.size(); ++i)
	{
		ptrs[i] = allocator.Allocate(sizesToAllocate[i], 1);
	}
	
	for (uint32 i = 0; i < sizesToAllocate.size(); ++i)
	{
		allocator.Free(ptrs[i]);
	}
	timer.Stop();
}

void Sailor::Memory::TestPerformance()
{
	static const uint32 IterationsCount = 10;
	static const uint32 AllocationsCount = 1000000;
	static const uint32 MaxSize = 32;

	std::vector<size_t> sizesToAllocate;
	sizesToAllocate.resize(AllocationsCount);

	srand(0);

	size_t totalAllocatedSize = 0;

	for (uint32 i = 0; i < sizesToAllocate.size(); ++i)
	{
		sizesToAllocate[i] = rand() % MaxSize + 1;
		totalAllocatedSize += sizesToAllocate[i];
	}

	printf("Total alloc memory size: %.2f Mb\n\n", (float)((double)(totalAllocatedSize / 1024) / 1024.0));

	Utils::AccurateTimer poolAllocatorTimer;
	Utils::AccurateTimer multiPoolAllocatorTimer;
	Utils::AccurateTimer blockAllocatorTimer;
	Utils::AccurateTimer mallocTimer;

	for (uint32_t i = 0; i < IterationsCount; i++)
	{
		TestMallocAllocator mallocAllocator;
		TestPerformanceSimple(mallocAllocator, sizesToAllocate, mallocTimer);

		Memory::TMultiPoolAllocator multiPoolAllocator;
		TestPerformanceSimple(multiPoolAllocator, sizesToAllocate, multiPoolAllocatorTimer);

		Memory::TPoolAllocator poolAllocator(128, 32);
		TestPerformanceSimple(poolAllocator, sizesToAllocate, poolAllocatorTimer);

		Memory::TBlockAllocator blockAllocator(2048, 32);
		TestPerformanceSimple(blockAllocator, sizesToAllocate, blockAllocatorTimer);
	}

	printf("mallocAllocator %lld ms\n", mallocTimer.ResultAccumulatedMs());
	printf("blockAllocator %lld ms\n", blockAllocatorTimer.ResultAccumulatedMs());
	printf("poolAllocator %lld ms\n", poolAllocatorTimer.ResultAccumulatedMs());
	printf("multiPoolAllocator %lld ms\n", multiPoolAllocatorTimer.ResultAccumulatedMs());
}
