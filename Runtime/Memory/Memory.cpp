#include "Memory.h"
#include <cstdlib>
#include <malloc.h>
#include <random>
#include <fstream>
#include <cassert>
#include <functional> 
#include <cctype>
#include "Core/Utils.h"
#include "Containers/Pair.h"
#include "MallocAllocator.hpp"
#include "MemoryBlockAllocator.hpp"
#include "MemoryPoolAllocator.hpp"
#include "MemoryMultiPoolAllocator.hpp"
#include "psapi.h"

using namespace Sailor;
using namespace Sailor::Memory;
using Timer = Utils::Timer;

size_t GetTotalUsedVirtualMemory()
{
	PROCESS_MEMORY_COUNTERS_EX pmc;
	GetProcessMemoryInfo(GetCurrentProcess(), (PROCESS_MEMORY_COUNTERS*)&pmc, sizeof(pmc));
	SIZE_T virtualMemUsedByMe = pmc.PrivateUsage;

	MEMORYSTATUSEX statex;

	statex.dwLength = sizeof(statex);

	GlobalMemoryStatusEx(&statex);

	MEMORYSTATUSEX memInfo;
	memInfo.dwLength = sizeof(MEMORYSTATUSEX);
	GlobalMemoryStatusEx(&memInfo);
	//DWORDLONG virtualMemUsed = memInfo.ullTotalVirtual - memInfo.ullAvailVirtual;

	return (size_t)virtualMemUsedByMe;
}

typedef std::unordered_map<std::string, std::unordered_map<size_t, std::pair<size_t, float>>> TestResult;

struct Result
{
	Result(std::string allocator) : m_allocator(allocator) {}

	std::string m_allocator;
	std::unordered_map<std::string, TestResult> m_results;

	size_t m_scoreTiming = 0;
	size_t m_scoreMemory = 0;
	size_t m_globalScore = 0;

	void CalculateScore()
	{
		/*Was used for contest*/
	}
};

template<typename TAllocator>
class TestCase_MemoryPerformance
{
	static float m_globalScore;

public:
	static Result RunTests()
	{
		const std::string allocatorName = typeid(TAllocator).name();

		Result result(allocatorName);
		result.m_results["small"] = RunPerformanceTests(1, 32, 10, 1000000);
		result.m_results["medium"] = RunPerformanceTests(128, 40000, 10, 100000);
		result.m_results["large"] = RunPerformanceTests(4000000, 800000000, 10, 10000);
		result.m_results["random"] = RunPerformanceTests(1, 16000000000, 10, 100000);

		RunSanityTests();

		printf("Score: %.2f\n", m_globalScore);

		return result;
	}

	static void RunSanityTests()
	{
		const std::string allocatorName = typeid(TAllocator).name();

		printf("\n\n%s\n", allocatorName.c_str());
		printf("Can allocate huge block: %d\n", SanityCanAllocHugeBlock());
		printf("Is stack based: %d\n", !SanityCheckIsStack());
		printf("Sanity check passed: %d\n", SanityCheck());
	}

	static TestResult RunPerformanceTests(const size_t MinSize = 1, const size_t MaxSize = 32, const size_t IterationsCount = 10, const size_t AllocationsCount = 1000000)
	{
		TestResult result;

		const size_t step = AllocationsCount / 5;
		uint32_t index = 0;

		for (size_t allocCount = step; allocCount <= AllocationsCount; allocCount += step)
		{
			index++;

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
			size_t memoryOverhead1 = 0;

			Timer shuffleTest;
			size_t memoryOverhead2 = 0;

			Timer randomTest;
			size_t memoryOverhead3 = 0;

			bool bTrackSimple = true;
			bool bTrackShuffle = true;
			bool bTrackRandom = true;

			for (size_t i = 0; i < IterationsCount; i++)
			{
				bTrackSimple &= TestPerformanceSimple(sizesToAllocate, simpleTest, memoryOverhead1);
				bTrackShuffle &= TestPerformanceShuffle(sizesToAllocate, shuffleTest, memoryOverhead2);
				bTrackRandom &= TestPerformanceRandom(sizesToAllocate, randomTest, memoryOverhead3);
			}

			result["simple"][totalAllocatedSize] = { simpleTest.ResultAccumulatedMs(), (float)((double)memoryOverhead1 / 1048576.0) };
			result["shuffle"][totalAllocatedSize] = { shuffleTest.ResultAccumulatedMs(), (float)((double)memoryOverhead2 / 1048576.0) };
			result["random"][totalAllocatedSize] = { randomTest.ResultAccumulatedMs(), (float)((double)memoryOverhead3 / 1048576.0) };

			if (bTrackSimple)
			{
				m_globalScore += CalculateScore((float)result["simple"][totalAllocatedSize].first,
					result["simple"][totalAllocatedSize].second, (float)index, (float)allocCount);
			}

			if (bTrackShuffle)
			{
				m_globalScore += CalculateScore((float)result["simple"][totalAllocatedSize].first,
					result["shuffle"][totalAllocatedSize].second, (float)index, (float)allocCount);
			}

			if (bTrackRandom)
			{
				m_globalScore += CalculateScore((float)result["simple"][totalAllocatedSize].first,
					result["random"][totalAllocatedSize].second, (float)index, (float)allocCount);
			}
		}

		return result;
	}

	static float CalculateScore(float ms, float memoryOverhead, float step, float allocationSize)
	{
		return (float)(((1500.0 * (step / 5.0)) / ms) * (allocationSize / (allocationSize + memoryOverhead)));
	}

	static bool TestPerformanceRandom(const std::vector<size_t>& sizesToAllocate, Timer& timer, size_t& memoryOverhead)
	{
		size_t totalAllocated = 0;
		std::vector<void*> ptrs;
		ptrs.resize(sizesToAllocate.size());
		size_t beforeTest = GetTotalUsedVirtualMemory();

		TAllocator allocator;

		timer.Start();
		size_t border = sizesToAllocate.size() / 4;
		for (size_t i = 0; i < border * 2; ++i)
		{
			ptrs[i] = allocator.Allocate(sizesToAllocate[i], 1);
			totalAllocated += sizesToAllocate[i];

			if (!ptrs[i])
			{
				return false;
			}
		}
		timer.Stop();

		std::random_device rd;
		std::mt19937 g(rd());
		std::shuffle(ptrs.begin(), ptrs.end() - (size_t)border * 3, g);

		timer.Start();

		for (size_t i = 0; i < border; ++i)
		{
			allocator.Free(ptrs[i]);
			totalAllocated -= sizesToAllocate[i];
		}

		for (size_t i = border * 2; i < sizesToAllocate.size(); ++i)
		{
			ptrs[i] = allocator.Allocate(sizesToAllocate[i], 1);
			totalAllocated += sizesToAllocate[i];

			if (!ptrs[i])
			{
				return false;
			}
		}

		memoryOverhead = 0;
		if (GetTotalUsedVirtualMemory() > beforeTest + totalAllocated)
		{
			memoryOverhead = GetTotalUsedVirtualMemory() - beforeTest - totalAllocated;
		}

		for (size_t i = border; i < sizesToAllocate.size(); ++i)
		{
			allocator.Free(ptrs[i]);
		}
		timer.Stop();

		return true;
	}

	static bool TestPerformanceShuffle(const std::vector<size_t>& sizesToAllocate, Timer& timer, size_t& memoryOverhead)
	{
		size_t totalAllocated = 0;
		std::vector<void*> ptrs;
		ptrs.resize(sizesToAllocate.size());

		size_t beforeTest = GetTotalUsedVirtualMemory();
		TAllocator allocator;

		timer.Start();
		for (size_t i = 0; i < sizesToAllocate.size(); ++i)
		{
			ptrs[i] = allocator.Allocate(sizesToAllocate[i], 4);
			totalAllocated += sizesToAllocate[i];

			if (!ptrs[i])
			{
				return false;
			}
		}
		timer.Stop();

		memoryOverhead = 0;
		if (GetTotalUsedVirtualMemory() > beforeTest + totalAllocated)
		{
			memoryOverhead = GetTotalUsedVirtualMemory() - beforeTest - totalAllocated;
		}

		std::random_device rd;
		std::mt19937 g(rd());
		std::shuffle(ptrs.begin(), ptrs.end(), g);

		timer.Start();
		for (size_t i = 0; i < sizesToAllocate.size(); ++i)
		{
			allocator.Free(ptrs[i]);
		}
		timer.Stop();

		return true;
	}

	static bool TestPerformanceSimple(const std::vector<size_t>& sizesToAllocate, Timer& timer, size_t& memoryOverhead)
	{
		size_t totalAllocated = 0;

		std::vector<void*> ptrs;
		ptrs.resize(sizesToAllocate.size());
		size_t beforeTest = GetTotalUsedVirtualMemory();
		TAllocator allocator;

		timer.Start();
		for (size_t i = 0; i < sizesToAllocate.size(); ++i)
		{
			ptrs[i] = allocator.Allocate(sizesToAllocate[i], 1);

			if (!ptrs[i])
			{
				return false;
			}

			totalAllocated += sizesToAllocate[i];
		}
		timer.Stop();

		memoryOverhead = 0;
		if (GetTotalUsedVirtualMemory() > beforeTest + totalAllocated)
		{
			memoryOverhead = GetTotalUsedVirtualMemory() - beforeTest - totalAllocated;
		}

		timer.Start();
		for (size_t i = 0; i < sizesToAllocate.size(); ++i)
		{
			allocator.Free(ptrs[i]);
		}
		timer.Stop();

		return true;
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
		MallocAllocator ideal;

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

	static void TestPerformanceStress(const std::vector<size_t>& sizesToAllocate, Timer& timer)
	{
		TAllocator allocator;

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

			PtrGuard* grandHeap = (PtrGuard*)malloc(POINTERS_COUNT * sizeof(PtrGuard));
			for (size_t i = 0; i < POINTERS_COUNT; ++i)
			{
				grandHeap[i] = { nullptr, 0 };
			}

			while (curMaxSize > 0)
			{
				unsigned long long minAlloc = 0llu - 1llu;
				unsigned long long maxAlloc = 0;
				unsigned long long sumAlloc = 0;

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

template<typename TAllocator>
float TestCase_MemoryPerformance<TAllocator>::m_globalScore = 0.0f;

std::string GetJsData(std::string allocSize, std::string testName, std::vector<Result> results, bool bTime)
{
	std::string res;

	res += "[";
	res += "['Total allocated size'";
	for (auto& result : results)
	{
		res += ",'" + result.m_allocator + "'";
	}
	res += "]";

	for (auto& r : results[0].m_results[allocSize][testName])
	{
		char startString[1024];
		snprintf(startString, sizeof(startString), ",\n['%.2fmb'", (float)((double)(r.first / 1024) / 1024.0));
		res += std::string(startString);

		for (auto& result : results)
		{
			char buffer[1024];
			if (bTime)
			{
				snprintf(buffer, sizeof(buffer), ",%llu", result.m_results[allocSize][testName][r.first].first);
			}
			else
			{
				snprintf(buffer, sizeof(buffer), ",%.2f", result.m_results[allocSize][testName][r.first].second);
			}
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

	results.reserve(3);
	results.push_back(TestCase_MemoryPerformance<HeapAllocator>::RunTests());
	results.push_back(TestCase_MemoryPerformance<LockFreeHeapAllocator>::RunTests());
	results.push_back(TestCase_MemoryPerformance<MallocAllocator>::RunTests());

	std::string emplace;

	emplace += "var smallSimple = " + GetJsData("small", "simple", results, true) + ";\n";
	emplace += "var smallShuffle = " + GetJsData("small", "shuffle", results, true) + ";\n";
	emplace += "var smallRandom = " + GetJsData("small", "random", results, true) + ";\n";

	emplace += "var mediumSimple = " + GetJsData("medium", "simple", results, true) + ";\n";
	emplace += "var mediumShuffle = " + GetJsData("medium", "shuffle", results, true) + ";\n";
	emplace += "var mediumRandom = " + GetJsData("medium", "random", results, true) + ";\n";

	emplace += "var largeSimple = " + GetJsData("large", "simple", results, true) + ";\n";
	emplace += "var largeShuffle = " + GetJsData("large", "shuffle", results, true) + ";\n";
	emplace += "var largeRandom = " + GetJsData("large", "random", results, true) + ";\n";

	emplace += "var randomSimple = " + GetJsData("random", "simple", results, true) + ";\n";
	emplace += "var randomShuffle = " + GetJsData("random", "shuffle", results, true) + ";\n";
	emplace += "var randomRandom = " + GetJsData("random", "random", results, true) + ";\n";

	emplace += "var smallSimpleMemory = " + GetJsData("small", "simple", results, false) + ";\n";
	emplace += "var smallShuffleMemory = " + GetJsData("small", "shuffle", results, false) + ";\n";
	emplace += "var smallRandomMemory = " + GetJsData("small", "random", results, false) + ";\n";

	emplace += "var mediumSimpleMemory = " + GetJsData("medium", "simple", results, false) + ";\n";
	emplace += "var mediumShuffleMemory = " + GetJsData("medium", "shuffle", results, false) + ";\n";
	emplace += "var mediumRandomMemory = " + GetJsData("medium", "random", results, false) + ";\n";

	emplace += "var largeSimpleMemory = " + GetJsData("large", "simple", results, false) + ";\n";
	emplace += "var largeShuffleMemory = " + GetJsData("large", "shuffle", results, false) + ";\n";
	emplace += "var largeRandomMemory = " + GetJsData("large", "random", results, false) + ";\n";

	emplace += "var randomSimpleMemory = " + GetJsData("random", "simple", results, false) + ";\n";
	emplace += "var randomShuffleMemory = " + GetJsData("random", "shuffle", results, false) + ";\n";
	emplace += "var randomRandomMemory = " + GetJsData("random", "random", results, false) + ";\n";

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
		"	function d_sort(d) {"
		"  d.sort(function(x, y) {"
		"      let a = to_float(x[0]);"
		"      let b = to_float(y[0]);"
		"      if (a < b) return -1;"
		"      if (a > b) return 1;"
		"      return 0;"
		"  });"
		"  return d;"
		"}"
		"function drawChart(d, div_id, title) {"
		"  var data = google.visualization.arrayToDataTable(d_sort(d));"
		"  var options = {"
		"      title: title,"
		"      hAxis: {title: 'Total allocated'},"
		"      vAxis: {title: 'Ms'}"
		"  };"
		"  var chart = new google.visualization.AreaChart(document.getElementById(div_id));"
		"  chart.draw(data, options);"
		"}"
		"function drawBars(d, div_id, title) {"
		"var data = google.visualization.arrayToDataTable(d_sort(d));"
		"var options = {"
		"	title: title,"
		"	hAxis : {title: 'Total allocated'},"
		"	vAxis : {title: 'Mb'}"
		"};"
		"var chart = new google.visualization.ColumnChart(document.getElementById(div_id));"
		"chart.draw(data, options);"
		"}"
		"  function drawCharts() {"
		"   drawChart(smallSimple,\"smallSimple\", \"Simple consumed time\");"
		"   drawChart(smallShuffle,\"smallShuffle\", \"Shuffle consumed time\");"
		"   drawChart(smallRandom, \"smallRandom\", \"Random consumed time\");"
		"   drawChart(mediumSimple,\"mediumSimple\", \"Simple consumed time\");"
		"   drawChart(mediumShuffle,\"mediumShuffle\", \"Shuffle consumed time\");"
		"   drawChart(mediumRandom,\"mediumRandom\", \"Random consumed time\");"
		"   drawChart(largeSimple, \"largeSimple\",  \"Simple consumed time\");"
		"   drawChart(largeShuffle,\"largeShuffle\", \"Shuffle consumed time\");"
		"   drawChart(largeRandom, \"largeRandom\",  \"Random consumed time\");"
		"   drawChart(randomSimple, \"randomSimple\", \"Simple consumed time\");"
		"   drawChart(randomShuffle,\"randomShuffle\",\"Shuffle consumed time\");"
		"   drawChart(randomRandom, \"randomRandom\", \"Random consumed time\");"
		"	drawBars(smallSimpleMemory, \"smallSimpleMemory\", \"Simple memory overhead\");"
		"	drawBars(smallShuffleMemory, \"smallShuffleMemory\", \"Shuffle memory overhead\");"
		"	drawBars(smallRandomMemory, \"smallRandomMemory\", \"Random memory overhead\");"
		"	drawBars(mediumSimpleMemory, \"mediumSimpleMemory\", \"Simple memory overhead\");"
		"	drawBars(mediumShuffleMemory, \"mediumShuffleMemory\", \"Shuffle memory overhead\");"
		"	drawBars(mediumRandomMemory, \"mediumRandomMemory\", \"Random memory overhead\");"
		"	drawBars(largeSimpleMemory, \"largeSimpleMemory\", \"Simple memory overhead\");"
		"	drawBars(largeShuffleMemory, \"largeShuffleMemory\", \"Shuffle memory overhead\");"
		"	drawBars(largeRandomMemory, \"largeRandomMemory\", \"Random memory overhead\");"
		"	drawBars(randomSimpleMemory, \"randomSimpleMemory\", \"Simple memory overhead\");"
		"	drawBars(randomShuffleMemory, \"randomShuffleMemory\", \"Shuffle memory overhead\");"
		"	drawBars(randomRandomMemory, \"randomRandomMemory\", \"Random memory overhead\");"
		"   }"
		" </script>\n"
		"<style>"
		".container {"
		"    display: flex;"
		"}"
		".timegraph {"
		"    flex: 1;"
		"    width: 50%;"
		"    padding: 5px;"
		"    margin: 5px;"
		"    height: 200px;"
		"}"
		".memgraph {"
		"    flex: 1;"
		"    width: 40%;"
		"    padding: 5px;"
		"    margin: 5px;"
		"    height: 200px;"
		"}"
		"}"
		"</style>"
		"</head>\n"
		"<body>\n"
		"<hr/>\n"
		"<h2>Small allocations [1,32] bytes</h2>\n"
		"<div class=\"container\">"
		"<div id=\"smallSimple\" class=\"timegraph\"></div>"
		"<div id=\"smallSimpleMemory\" class=\"memgraph\"></div>"
		"</div>"
		"<div class=\"container\">"
		"<div id=\"smallShuffle\" class=\"timegraph\"></div>"
		"<div id=\"smallShuffleMemory\" class=\"memgraph\"></div>"
		"</div>"
		"<div class=\"container\">"
		"<div id=\"smallRandom\" class=\"timegraph\"></div>"
		"<div id=\"smallRandomMemory\" class=\"memgraph\"></div>"
		"</div>"
		"<hr/>"
		"<h2>Medium allocations [128,40000] bytes</h2>"
		"<div class=\"container\">"
		"<div id=\"mediumSimple\" class=\"timegraph\"></div>"
		"<div id=\"mediumSimpleMemory\" class=\"memgraph\"></div>"
		"</div>"
		"<div class=\"container\">"
		"<div id=\"mediumShuffle\" class=\"timegraph\"></div>"
		"<div id=\"mediumShuffleMemory\" class=\"memgraph\"></div>"
		"</div>"
		"<div class=\"container\">"
		"<div id=\"mediumRandom\" class=\"timegraph\"></div>"
		"<div id=\"mediumRandomMemory\" class=\"memgraph\"></div>"
		"</div>"
		"<hr/>"
		"<h2>Large allocations [4000000,800000000] bytes</h2>"
		"<div class=\"container\">"
		"<div id=\"largeSimple\" class=\"timegraph\"></div>"
		"<div id=\"largeSimpleMemory\" class=\"memgraph\"></div>"
		"</div>"
		"<div class=\"container\">"
		"<div id=\"largeShuffle\" class=\"timegraph\"></div>"
		"<div id=\"largeShuffleMemory\" class=\"memgraph\"></div>"
		"</div>"
		"<div class=\"container\">"
		"<div id=\"largeRandom\" class=\"timegraph\"></div>"
		"<div id=\"largeRandomMemory\" class=\"memgraph\"></div>"
		"</div>"
		"<hr/>"
		"<h2>Random allocations</h2>"
		"<div class=\"container\">"
		"<div id=\"randomSimple\" class=\"timegraph\"></div>"
		"<div id=\"randomSimpleMemory\" class=\"memgraph\"></div>"
		"</div>"
		"<div class=\"container\">"
		"<div id=\"randomShuffle\" class=\"timegraph\"></div>"
		"<div id=\"randomShuffleMemory\" class=\"memgraph\"></div>"
		"</div>"
		"<div class=\"container\">"
		"<div id=\"randomRandom\" class=\"timegraph\"></div>"
		"<div id=\"randomRandomMemory\" class=\"memgraph\"></div>"
		"</div>"
		"<hr/>\n"
		"</body>\n"
		"</html>\n";

	std::ofstream htmlResult{ "results.html" };
	htmlResult << html;
	htmlResult.close();
}
