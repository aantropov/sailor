#include <unordered_map>
#include "Containers/Map.h"
#include "Core/Utils.h"

using namespace Sailor;
using namespace Sailor::Memory;
using Timer = Utils::Timer;

class TestCase_MapPerfromance
{
public:

	static void RunTests()
	{
		printf("%s\n", "class Sailor::TMap");
		printf("Sanity check passed: %d\n", SanityCheck());
		PerformanceTests();
		printf("\n");
	}

	static void PerformanceTests()
	{
		const size_t count = 1000500;

		Timer stdMap;
		Timer tMap;

		TMap<size_t, std::string> container;
		std::unordered_map<size_t, std::string> ideal;

		stdMap.Start();
		for (size_t i = 0; i < count; i++)
		{
			ideal[i] = std::to_string(i * 3);
		}
		stdMap.Stop();

		tMap.Start();
		for (size_t i = 0; i < count; i++)
		{
			container[i] = std::to_string(i * 3);
		}
		tMap.Stop();

		SAILOR_LOG("Performance test operator[]:\n\tstd::Map %llums\n\tTMap %llums", stdMap.ResultMs(), tMap.ResultMs());

		/////////////////////////////////////////////
		stdMap.Clear();
		tMap.Clear();

		srand(0);
		stdMap.Start();
		for (size_t i = 0; i < count; i++)
		{
			const size_t value = rand();
			auto res = ideal.find(value);
		}
		stdMap.Stop();

		srand(0);
		tMap.Start();
		for (size_t i = 0; i < count; i++)
		{
			const int32_t value = rand();
			container.ContainsKey(value);
		}
		tMap.Stop();

		SAILOR_LOG("Performance test ContainsKey:\n\tstd::Map %llums\n\tTMap %llums", stdMap.ResultMs(), tMap.ResultMs());
		/////////////////////////////////////////////

		stdMap.Clear();
		tMap.Clear();

		tMap.Start();
		for (size_t i = 0; i < count; i++)
		{
			if (i % 2)
			{
				container.Remove(i);
			}
		}
		tMap.Stop();

		stdMap.Start();
		for (size_t i = 0; i < count; i++)
		{
			if (i % 2)
			{
				ideal.erase(i);
			}
		}
		stdMap.Stop();

		SAILOR_LOG("Performance test Remove:\n\tstd::Map %llums\n\tTMap %llums", stdMap.ResultMs(), tMap.ResultMs());
	}

	static bool SanityCheck()
	{
		const size_t count = 1800;

		TMap<size_t, size_t> container;
		std::unordered_map<size_t, size_t> ideal;

		for (size_t i = 0; i < count; i++)
		{
			ideal[i] = i * 3;
			container[i] = i * 3;
		}

		for (size_t i = 0; i < count / 2; i++)
		{
			ideal.erase(i * 2);
			container.Remove(i * 2);

			for (const auto& el : ideal)
			{
				if (container[el.first] != el.second)
				{
					return false;
				}
			}

			for (const auto& el : container)
			{
				if (ideal[el.m_first] != el.m_second)
				{
					return false;
				}
			}
		}
		return true;
	}
};

void Sailor::RunMapBenchmark()
{
	printf("\nStarting Map benchmark...\n");

	TestCase_MapPerfromance::RunTests();
}
