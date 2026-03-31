#include "Components/Tests/BenchmarkTestCaseComponent.h"
#include "Containers/Vector.h"
#include "Containers/Set.h"
#include "Containers/Map.h"
#include "Containers/List.h"
#include "Containers/Octree.h"
#include "Sailor.h"
#include "Containers/Pair.h"
#include "Memory/Memory.h"
#include "Core/Utils.h"
#include <sstream>

using namespace Sailor;

void BenchmarkTestCaseComponent::BeginPlay()
{
	TestCaseComponent::BeginPlay();
	SAILOR_LOG("Benchmark config vector=%d set=%d map=%d list=%d octree=%d memory=%d",
		m_bRunVector, m_bRunSet, m_bRunMap, m_bRunList, m_bRunOctree, m_bRunMemory);
}

void BenchmarkTestCaseComponent::Tick(float)
{
	if (IsFinished())
	{
		return;
	}

	if (m_bStarted)
	{
		return;
	}

	m_bStarted = true;

	TVector<TPair<std::string, int64_t>> results;
	results.Reserve(8);

	const auto runBenchmark = [&](auto&& fn, const char* name)
	{
		const int64_t start = Utils::GetCurrentTimeMs();
		fn();
		const int64_t end = Utils::GetCurrentTimeMs();
		results.Emplace(name, end - start);
	};

	try
	{
		if (m_bRunVector) { runBenchmark(RunVectorBenchmark, "Vector"); }
		if (m_bRunSet) { runBenchmark(RunSetBenchmark, "Set"); }
		if (m_bRunMap) { runBenchmark(RunMapBenchmark, "Map"); }
		if (m_bRunList) { runBenchmark(RunListBenchmark, "List"); }
		if (m_bRunOctree) { runBenchmark(RunOctreeBenchmark, "Octree"); }
		if (m_bRunMemory) { runBenchmark(Sailor::Memory::RunMemoryBenchmark, "Memory"); }

		std::stringstream ss;
		for (const auto& entry : results)
		{
			ss << entry.m_first << ":" << entry.m_second << "ms" << ";";
		}
		AddJournalEvent("BenchmarkResults", ss.str(), Utils::GetCurrentTimeMs() - GetStartTimeMs());
		MarkPassed();
		App::Stop();
	}
	catch (const std::exception& ex)
	{
		MarkFailed(ex.what());
		App::Stop();
	}
	catch (...)
	{
		MarkFailed("Unknown exception during benchmarks");
		App::Stop();
	}
}
