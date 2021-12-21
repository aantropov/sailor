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
		const size_t count = 10000600;

		Timer stdSet;
		Timer tSet;

		TMap<size_t, size_t> container;
		std::unordered_map<size_t, size_t> ideal;

		stdSet.Start();
		for (size_t i = 0; i < count; i++)
		{
			ideal[i] = i * 3;
		}
		stdSet.Stop();

		tSet.Start();
		for (size_t i = 0; i < count; i++)
		{
			container[i] = i * 3;
		}
		tSet.Stop();

		SAILOR_LOG("Performance test operator[]:\n\tstd::set %llums\n\tTSet %llums", stdSet.ResultMs(), tSet.ResultMs());

		/////////////////////////////////////////////
		stdSet.Clear();
		tSet.Clear();

		srand(0);
		stdSet.Start();
		for (size_t i = 0; i < count; i++)
		{
			const int32_t value = rand();
			auto res = ideal.find(value);
		}
		stdSet.Stop();

		srand(0);
		tSet.Start();
		for (size_t i = 0; i < count; i++)
		{
			const int32_t value = rand();
			container.ContainsKey(value);
		}
		tSet.Stop();

		SAILOR_LOG("Performance test ContainsKey:\n\tstd::set %llums\n\tTSet %llums", stdSet.ResultMs(), tSet.ResultMs());
		/////////////////////////////////////////////

		stdSet.Clear();
		tSet.Clear();

		tSet.Start();
		for (size_t i = 0; i < count; i++)
		{
			if (i % 2)
			{
				container.Remove(i);
			}
		}
		tSet.Stop();

		stdSet.Start();
		for (size_t i = 0; i < count; i++)
		{
			if (i % 2)
			{
				ideal.erase(i);
			}
		}
		stdSet.Stop();

		SAILOR_LOG("Performance test Remove:\n\tstd::set %llums\n\tTSet %llums", stdSet.ResultMs(), tSet.ResultMs());
	}

	static bool SanityCheck()
	{
		return true;
	}
};

void Sailor::RunMapBenchmark()
{
	printf("\nStarting set benchmark...\n");

	TestCase_MapPerfromance::RunTests();
}
