#include "Memory.h"
#include <cstdlib>
#include <malloc.h>
#include <random>
#include <fstream>
#include "Utils.h"
#include "MemoryBlockAllocator.hpp"
#include "MemoryPoolAllocator.hpp"
#include "MemoryMultiPoolAllocator.hpp"

using namespace Sailor;
using namespace Sailor::Memory;
using Timer = Utils::Timer;

HeapAllocator DefaultHeapAllocator::m_heapAllocator;

typedef std::unordered_map<std::string, std::unordered_map<size_t, size_t>> TestResult;

struct Result
{
	Result(std::string allocator) : m_allocator(allocator) {}

	std::string m_allocator;
	std::unordered_map<std::string, TestResult> m_testResults;
};

class DefaultMallocAllocator
{
public:
	inline void* Allocate(size_t size, size_t alignment)
	{
		m_size += size;
		return malloc(size);
	}

	inline void Free(void* ptr)
	{
		free(ptr);
	}

	size_t GetOccupiedSpace() const
	{
		return m_size;
	}

private:
	size_t m_size = 0;
};

template<typename TAllocator>
class TestCase_MemoryPerformance
{
public:
	static Result RunTests()
	{
		const std::string allocatorName = typeid(TAllocator).name();

		Result result(allocatorName);
		result.m_testResults["small"] = RunPerformanceTests(1, 32, 10, 1000000);
		result.m_testResults["medium"] = RunPerformanceTests(128, 40000, 10, 100000);
		result.m_testResults["large"] = RunPerformanceTests(4000000, 800000000, 10, 10000);
		result.m_testResults["random"] = RunPerformanceTests(1, 16000000000, 10, 30000);

		RunSanityTests();

		return result;
	}

	static void RunSanityTests()
	{
		const std::string allocatorName = typeid(TAllocator).name();

		printf("\n%s\n", allocatorName.c_str());
		printf("Can allocate huge block: %d\n", SanityCanAllocHugeBlock());
		printf("Is stack based: %d\n", !SanityCheckIsStack());
		printf("Sanity check passed: %d\n", SanityCheck());
	}

	static TestResult RunPerformanceTests(const size_t MinSize = 1, const size_t MaxSize = 32, const size_t IterationsCount = 10, const size_t AllocationsCount = 1000000)
	{
		TestResult result;

		const size_t step = AllocationsCount / 5;

		for (size_t allocCount = step; allocCount <= AllocationsCount; allocCount += step)
		{
			std::vector<size_t> sizesToAllocate;
			sizesToAllocate.resize(allocCount);

			srand(0);

			size_t totalAllocatedSize = 0;

			for (size_t i = 0; i < allocCount; ++i)
			{
				sizesToAllocate[i] = rand() % MaxSize + MinSize;
				totalAllocatedSize += sizesToAllocate[i];
			}

			Timer simpleTest;
			Timer shuffleTest;
			Timer randomTest;

			for (size_t i = 0; i < IterationsCount; i++)
			{
				{
					TAllocator allocator1;
					TestPerformanceSimple(allocator1, sizesToAllocate, simpleTest);
				}

				{
					TAllocator allocator2;
					TestPerformanceShuffle(allocator2, sizesToAllocate, shuffleTest);
				}

				{
					TAllocator allocator3;
					TestPerformanceRandom(allocator3, sizesToAllocate, randomTest);
				}
			}

			result["simple"][totalAllocatedSize] = simpleTest.ResultAccumulatedMs();
			result["shuffle"][totalAllocatedSize] = shuffleTest.ResultAccumulatedMs();
			result["random"][totalAllocatedSize] = randomTest.ResultAccumulatedMs();
		}

		return result;
	}

	static void TestPerformanceRandom(TAllocator& allocator, const std::vector<size_t>& sizesToAllocate, Timer& timer)
	{
		std::vector<void*> ptrs;
		ptrs.resize(sizesToAllocate.size());

		timer.Start();
		size_t border = sizesToAllocate.size() / 4;
		for (size_t i = 0; i < border * 2; ++i)
		{
			ptrs[i] = allocator.Allocate(sizesToAllocate[i], 1);
		}
		timer.Stop();

		std::random_device rd;
		std::mt19937 g(rd());
		std::shuffle(ptrs.begin(), ptrs.end() - (size_t)border * 3, g);

		timer.Start();

		for (size_t i = 0; i < border; ++i)
		{
			allocator.Free(ptrs[i]);
		}

		for (size_t i = border * 2; i < sizesToAllocate.size(); ++i)
		{
			ptrs[i] = allocator.Allocate(sizesToAllocate[i], 1);
		}

		for (size_t i = border; i < sizesToAllocate.size(); ++i)
		{
			allocator.Free(ptrs[i]);
		}
		timer.Stop();
	}

	static void TestPerformanceShuffle(TAllocator& allocator, const std::vector<size_t>& sizesToAllocate, Timer& timer)
	{
		std::vector<void*> ptrs;
		ptrs.resize(sizesToAllocate.size());

		timer.Start();
		for (size_t i = 0; i < sizesToAllocate.size(); ++i)
		{
			ptrs[i] = allocator.Allocate(sizesToAllocate[i], 4);
		}
		timer.Stop();

		std::random_device rd;
		std::mt19937 g(rd());
		std::shuffle(ptrs.begin(), ptrs.end(), g);

		timer.Start();
		for (size_t i = 0; i < sizesToAllocate.size(); ++i)
		{
			allocator.Free(ptrs[i]);
		}
		timer.Stop();
	}

	static void TestPerformanceSimple(TAllocator& allocator, const std::vector<size_t>& sizesToAllocate, Timer& timer)
	{
		std::vector<void*> ptrs;
		ptrs.resize(sizesToAllocate.size());

		timer.Start();
		for (size_t i = 0; i < sizesToAllocate.size(); ++i)
		{
			ptrs[i] = allocator.Allocate(sizesToAllocate[i], 1);
		}

		for (size_t i = 0; i < sizesToAllocate.size(); ++i)
		{
			allocator.Free(ptrs[i]);
		}
		timer.Stop();
	}

	static bool SanityCanAllocHugeBlock()
	{
		TAllocator allocator;

		const size_t maxSize = 8000000000ULL;
		void* ptr = nullptr;

		ptr = allocator.Allocate(maxSize, 1);
		bool bRes = memset(ptr, 123, maxSize);
		allocator.Free(ptr);

		return bRes;
	}

	static bool SanityCheck()
	{
		static const size_t HalfAllocationsCount = 10000;
		static const size_t MaxSize = 4096;

		std::vector<size_t> sizesToAllocate;
		sizesToAllocate.resize(HalfAllocationsCount * 2);

		srand(0);

		for (size_t i = 0; i < sizesToAllocate.size(); ++i)
		{
			sizesToAllocate[i] = rand() % MaxSize + 1;
		}

		TAllocator allocator;
		DefaultMallocAllocator ideal;

		std::vector<void*> allocatorPtrs;
		std::vector<void*> idealPtrs;
		allocatorPtrs.resize(sizesToAllocate.size());
		idealPtrs.resize(sizesToAllocate.size());

		for (size_t i = 0; i < HalfAllocationsCount; i++)
		{
			allocatorPtrs[i] = allocator.Allocate(sizesToAllocate[i], (size_t)pow(2, rand() % 4));
			idealPtrs[i] = ideal.Allocate(sizesToAllocate[i], 1);

			memset(allocatorPtrs[i], i % 128, sizesToAllocate[i]);
			memset(idealPtrs[i], i % 128, sizesToAllocate[i]);
		}

		for (size_t i = 0; i < HalfAllocationsCount; i += 2)
		{
			allocator.Free(allocatorPtrs[i]);
			ideal.Free(idealPtrs[i]);

			idealPtrs[i] = nullptr;
		}

		for (size_t i = HalfAllocationsCount; i < HalfAllocationsCount * 2; i++)
		{
			allocatorPtrs[i] = allocator.Allocate(sizesToAllocate[i], (size_t)pow(2, rand() % 4));
			idealPtrs[i] = ideal.Allocate(sizesToAllocate[i], 1);

			memset(allocatorPtrs[i], i % 128, sizesToAllocate[i]);
			memset(idealPtrs[i], i % 128, sizesToAllocate[i]);
		}

		for (size_t i = 0; i < HalfAllocationsCount * 2; i++)
		{
			if (idealPtrs[i])
			{
				if (!(memcmp(allocatorPtrs[i], idealPtrs[i], sizesToAllocate[i]) == 0))
				{
					return false;
				}
			}
		}

		return true;
	}

	static bool SanityCheckIsStack()
	{
		const size_t NumAllocations = 2560000;
		const size_t Step = 128;
		const size_t MaxSize = 4096;
		const size_t MinSize = 12;

		srand(0);

		for (size_t size = MinSize; size < MaxSize; size += Step)
		{
			TAllocator allocator;
			void* prev = allocator.Allocate(size, 8);

			for (size_t i = 0; i < NumAllocations; i++)
			{
				void* n = allocator.Allocate(size, 8);
				if (!n)
				{
					return false;
				}

				allocator.Free(prev);
				prev = n;
			}
		}

		return true;
	}

	struct PtrGuard
	{
		void* ptr;
		size_t size;
	};

	static void TestPerformanceStress(TAllocator& allocator, const std::vector<size_t>& sizesToAllocate, Timer& timer)
	{
		struct _util
		{
			inline static unsigned int random(unsigned int& seed)
			{
				seed = 1664525u * seed + 1013904223u;
				return seed >> 1;
			}

			inline static unsigned int constRandom(unsigned int seed)
			{
				return (1664525u * seed + 1013904223u) >> 1;
			}

			inline static std::string sizeToString(size_t size, uint32_t width = 2)
			{
				static const char METRICS[5][3] = { "bt", "Kb", "Mb", "Gb", "Tb" };
				uint32_t idx = 0;

				while (size >= (1 << 20))
				{
					size >>= 10;
					idx++;
				}

				char buf[64];
				if (size >= (1 << 10))
				{
					sprintf_s(buf, 64, "%.*f", 2, (double)size / (double)(1 << 10));
					return std::string(buf) + " " + METRICS[idx + 1];
				}
				else
				{
					sprintf_s(buf, 64, "%d", (int)size);
					return std::string(buf) + " " + METRICS[idx];
				}
			}
		};

		size_t ramSize;
		{
			ULONGLONG ramSizeKB;
			if (!GetPhysicallyInstalledSystemMemory(&ramSizeKB))
			{
				printf("FUCK!\n");
				exit(0);
			}
			ramSize = (size_t)ramSizeKB << 10;
		}

		timer.Start();
		{
			static const size_t POINTERS_COUNT = 1000000;
			size_t heapSize = ramSize >> 2; //strong allocator must handle 0
			size_t memoryLeft = heapSize;
			size_t memoryAllocated = 0;
			uint32_t seed = 13;
			size_t curMaxSize = 512 << 20;
			size_t curGeneration = 0;

			PtrGuard* grandHeap = (PtrGuard*)malloc(POINTERS_COUNT * sizeof(PtrGuard));
			for (size_t i = 0; i < POINTERS_COUNT; ++i)
			{
				grandHeap[i] = { nullptr, 0 };
			}

			while (curMaxSize > 0)
			{
				unsigned long long allocations = 0;
				unsigned long long deallocations = 0;
				unsigned long long minAlloc = 0llu - 1llu;
				unsigned long long maxAlloc = 0;
				unsigned long long sumAlloc = 0;
				unsigned long long countAlloc = 0;

				size_t before = memoryAllocated;
				size_t limit = min(curMaxSize * (POINTERS_COUNT >> 2), heapSize);
				while (memoryAllocated < limit)
				{
					auto& guard = grandHeap[_util::random(seed) % POINTERS_COUNT];
					if (guard.ptr)
					{
						unsigned int r = _util::constRandom((size_t)guard.ptr % 0xFFFFFFFF);
						if (((unsigned char*)guard.ptr)[r % guard.size] != r % 255)
						{
							printf("sanity is failed");
							exit(0);
						}

						memoryLeft += guard.size;
						memoryAllocated -= guard.size;
						allocator.Free(guard.ptr);
						++deallocations;
					}

					unsigned long long int s = _util::random(seed) % curMaxSize;
					guard.size = (size_t)(s * s / curMaxSize) + 1;
					guard.size = min(guard.size, memoryLeft);
					memoryLeft -= guard.size;
					memoryAllocated += guard.size;
					guard.ptr = allocator.Allocate(guard.size, 1);

					minAlloc = min(minAlloc, guard.size);
					maxAlloc = max(maxAlloc, guard.size);
					sumAlloc += guard.size;
					++countAlloc;
					++allocations;

					if (!guard.ptr)
					{
						printf("null pointer is returned\n");
						exit(0);
					}

					unsigned int r = _util::constRandom((size_t)guard.ptr % 0xFFFFFFFF);
					((unsigned char*)guard.ptr)[r % guard.size] = (unsigned char)(r % 255);
				}
				/*      printf("\n");
					  printf("generation: %d\n", (int)curGeneration);
					  printf("allocations: %llu\n", allocations);
					  printf("deallocations: %llu\n", deallocations);
					  printf("size: %s\n", _util::sizeToString(curMaxSize).c_str());
					  printf("min: %s\n", _util::sizeToString(minAlloc).c_str());
					  printf("max: %s\n", _util::sizeToString(maxAlloc).c_str());
					  printf("mean: %s\n", _util::sizeToString(sumAlloc / countAlloc).c_str());
					  printf("heap: %s -> %s\n", _util::sizeToString(before).c_str(), _util::sizeToString(memoryAllocated).c_str());
					  */
				++curGeneration;
				curMaxSize >>= 1;
				limit = memoryAllocated >> 2;
				while (1)
				{
					auto& guard = grandHeap[_util::random(seed) % POINTERS_COUNT];
					if (guard.ptr)
					{
						unsigned int r = _util::constRandom((size_t)guard.ptr % 0xFFFFFFFF);
						if (((unsigned char*)guard.ptr)[r % guard.size] != r % 255)
						{
							printf("sanity is failed");
							exit(0);
						}
						memoryLeft += guard.size;
						memoryAllocated -= guard.size;
						allocator.Free(guard.ptr);
						guard = { nullptr, 0 };
					}

					if (memoryAllocated <= limit)
						break;
				}
			}
			free(grandHeap);
		}

		timer.Stop();
	}

};

std::string GetJsData(std::string allocSize, std::string testName, std::vector<Result> results)
{
	std::string res;

	res += "[";
	res += "['Total allocated size'";
	for (auto& result : results)
	{
		res += ",'" + result.m_allocator + "'";
	}
	res += "]";

	for (auto& r : results[0].m_testResults[allocSize][testName])
	{
		char startString[1024];
		snprintf(startString, sizeof(startString), ",\n['%.2fmb'", (float)((double)(r.first / 1024) / 1024.0));
		res += std::string(startString);

		for (auto& result : results)
		{
			char buffer[1024];
			snprintf(buffer, sizeof(buffer), ",%llu", result.m_testResults[allocSize][testName][r.first]);
			res += std::string(buffer);
		}

		res += "]";
	}

	res += "]";

	return res;
}

void Sailor::Memory::RunMemoryBenchmark()
{
	printf("Starting memory benchmark...\n");

	std::vector<Result> results;

	results.push_back(TestCase_MemoryPerformance<HeapAllocator>::RunTests());
	results.push_back(TestCase_MemoryPerformance<DefaultMallocAllocator>::RunTests());

	std::string emplace;

	emplace += "var smallSimple = " + GetJsData("small", "simple", results) + ";\n";
	emplace += "var smallShuffle = " + GetJsData("small", "shuffle", results) + ";\n";
	emplace += "var smallRandom = " + GetJsData("small", "random", results) + ";\n";

	emplace += "var mediumSimple = " + GetJsData("medium", "simple", results) + ";\n";
	emplace += "var mediumShuffle = " + GetJsData("medium", "shuffle", results) + ";\n";
	emplace += "var mediumRandom = " + GetJsData("medium", "random", results) + ";\n";

	emplace += "var largeSimple = " + GetJsData("large", "simple", results) + ";\n";
	emplace += "var largeShuffle = " + GetJsData("large", "shuffle", results) + ";\n";
	emplace += "var largeRandom = " + GetJsData("large", "random", results) + ";\n";

	emplace += "var randomSimple = " + GetJsData("random", "simple", results) + ";\n";
	emplace += "var randomShuffle = " + GetJsData("random", "shuffle", results) + ";\n";
	emplace += "var randomRandom = " + GetJsData("random", "random", results) + ";\n";

	std::string html =
		"<!DOCTYPE html>\n"
		"<html>\n"
		"<head>\n"
		"<meta charset = \"utf-8\">\n"
		"<title>Results</title>\n"
		"<script src = \"https://www.google.com/jsapi\"></script>\n"
		"<script>\n";

	html += emplace;
	html +=
		"  google.load(\"visualization\", \"1\", {packages:[\"corechart\"]});"
		"  google.setOnLoadCallback(drawCharts);"
		"  function to_float(str_mb) {"
		"     return parseFloat(str_mb.substring(0, str_mb.length - 2));"
		"  }"
		"  function drawChart(d, div_id, title) {"
		"   d.sort(function(x, y) {"
		"     let a = to_float(x[0]);"
		"     let b = to_float(y[0]);"
		"     if (a < b) return -1;"
		"     if (a > b) return 1;"
		"     return 0;"
		"   });"
		"   var data = google.visualization.arrayToDataTable(d);"
		"   var options = {"
		"    title: title,"
		"    hAxis: {title: 'Total allocated'},"
		"    vAxis: {title: 'Ms'}"
		"   };"
		"   var chart = new google.visualization.AreaChart(document.getElementById(div_id));"
		"   chart.draw(data, options);"
		"  }"
		"  function drawCharts() {"
		"   drawChart(smallSimple,\"smallSimple\", \"Simple\");"
		"   drawChart(smallShuffle,\"smallShuffle\", \"Shuffle\");"
		"   drawChart(smallRandom, \"smallRandom\", \"Random\");"
		"   drawChart(mediumSimple,\"mediumSimple\", \"Simple\");"
		"   drawChart(mediumShuffle,\"mediumShuffle\", \"Shuffle\");"
		"   drawChart(mediumRandom,\"mediumRandom\", \"Random\");"
		"   drawChart(largeSimple, \"largeSimple\",  \"Simple\");"
		"   drawChart(largeShuffle,\"largeShuffle\", \"Shuffle\");"
		"   drawChart(largeRandom, \"largeRandom\",  \"Random\");"
		"   drawChart(randomSimple, \"randomSimple\", \"Simple\");"
		"   drawChart(randomShuffle,\"randomShuffle\",\"Shuffle\");"
		"   drawChart(randomRandom, \"randomRandom\", \"Random\");"
		"   }"
		" </script>\n"
		"</head>\n"
		"<body>\n"
		"<hr/>\n"
		"<h2>Small</h2>\n"
		"<div id=\"smallSimple\" style=\"width: 1024px; height: 200px; \"></div>\n"
		"<div id=\"smallShuffle\" style=\"width: 1024px; height: 200px; \"></div>\n"
		"<div id=\"smallRandom\" style=\"width: 1024px; height: 200px; \"></div>\n"
		"<hr/>\n"
		"<h2>Medium</h2>\n"
		"<div id=\"mediumSimple\" style=\"width: 1024px; height: 200px; \"></div>\n"
		"<div id=\"mediumShuffle\" style=\"width: 1024px; height: 200px; \"></div>\n"
		"<div id=\"mediumRandom\" style=\"width: 1024px; height: 200px; \"></div>\n"
		"<hr/>\n"
		"<h2>Large</h2>\n"
		"<div id=\"largeSimple\" style=\"width: 1024px; height: 200px; \"></div>\n"
		"<div id=\"largeShuffle\" style=\"width: 1024px; height: 200px; \"></div>\n"
		"<div id=\"largeRandom\" style=\"width: 1024px; height: 200px; \"></div>\n"
		"<hr/>\n"
		"<h2>Random</h2>\n"
		"<div id=\"randomSimple\" style=\"width: 1024px; height: 200px; \"></div>\n"
		"<div id=\"randomShuffle\" style=\"width: 1024px; height: 200px; \"></div>\n"
		"<div id=\"randomRandom\" style=\"width: 1024px; height: 200px; \"></div>\n"
		"<hr/>\n"
		"</body>\n"
		"</html>\n";

	std::ofstream htmlResult{ "MemoryBenchmark.html" };
	htmlResult << html;
	htmlResult.close();
}
