#include <unordered_map>
#include "Containers/Map.h"
#include "Containers/ConcurrentMap.h"
#include "Core/Utils.h"
#include <random>

using namespace Sailor;
using namespace Sailor::Memory;
using Timer = Utils::Timer;

template<typename TContainer>
class TestCase_MapPerfromance
{
public:

	static void RunTests()
	{
		const std::string tMapClassName = typeid(TContainer).name();

		printf("%s\n", tMapClassName.c_str());
		printf("Sanity check passed: %d\n", SanityCheck());
		PerformanceTests();
		printf("\n");
	}

	static void PerformanceTests()
	{
		const size_t count = 10000500;

		Timer stdMap;
		Timer tMap;

		TContainer container;
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

		std::random_device rd;
		std::mt19937 g(rd());

		volatile float misses = 0;

		g.seed(0);
		tMap.Start();
		for (size_t i = 0; i < count; i++)
		{
			const size_t value = i % 2 ? g() : g() % count;
			if (!container.ContainsKey(value))
			{
				misses++;
			}
		}
		tMap.Stop();

		misses = 0;
		g.seed(0);
		stdMap.Start();
		for (size_t i = 0; i < count; i++)
		{
			const size_t value = i % 2 ? g() : g() % count;
			if (ideal.find(value) == ideal.end())
			{
				misses++;
			}
		}
		stdMap.Stop();

		SAILOR_LOG("Performance test ContainsKey(percent of misses %.2f):\n\tstd::map %llums\n\tTMap %llums", misses / (float)count, stdMap.ResultMs(), tMap.ResultMs());
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
		const size_t count = 18000;

		TConcurrentMap<size_t, size_t> container;
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

	TestCase_MapPerfromance<Sailor::TMap<size_t, std::string>>::RunTests();
	TestCase_MapPerfromance<TConcurrentMap<size_t, std::string>>::RunTests();
}
