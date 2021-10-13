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
void TestPerformanceRandom(TAllocator& allocator, const std::vector<size_t>& sizesToAllocate, Utils::Timer& timer)
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
void TestPerformanceShuffle(TAllocator& allocator, const std::vector<size_t>& sizesToAllocate, Utils::Timer& timer)
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
void TestPerformanceSimple(TAllocator& allocator, const std::vector<size_t>& sizesToAllocate, Utils::Timer& timer)
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

template<typename TAllocator>
struct TestCase_MemoryPerformance
{
	static void RunTests()
	{
		Utils::Timer simpleTest;
		Utils::Timer shuffleTest;
		Utils::Timer randomTest;

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

		for (uint32_t i = 0; i < IterationsCount; i++)
		{
			TAllocator allocator1;
			TestPerformanceSimple(allocator1, sizesToAllocate, simpleTest);

			TAllocator allocator2;
			TestPerformanceShuffle(allocator2, sizesToAllocate, shuffleTest);

			TAllocator allocator3;
			TestPerformanceRandom(allocator3, sizesToAllocate, randomTest);
		}

		const std::string allocatorName = typeid(TAllocator).name();

		printf("\n%s, Total alloc memory size: %.2f Mb:\n", allocatorName.c_str(), (float)((double)(totalAllocatedSize / 1024) / 1024.0));
		printf("simpleTest %lld ms\n", simpleTest.ResultAccumulatedMs());
		printf("shuffleTest %lld ms\n", shuffleTest.ResultAccumulatedMs());
		printf("randomTest %lld ms\n", randomTest.ResultAccumulatedMs());

	}
};

void Sailor::Memory::TestPerformance()
{
	TestCase_MemoryPerformance<TestMallocAllocator>::RunTests();
	TestCase_MemoryPerformance<TBlockAllocator<GlobalHeapAllocator, void*>>::RunTests();
	TestCase_MemoryPerformance<TPoolAllocator<GlobalHeapAllocator, void*>>::RunTests();
	TestCase_MemoryPerformance<TMultiPoolAllocator<GlobalHeapAllocator, void*>>::RunTests();
}
