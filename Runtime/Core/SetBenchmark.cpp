#include "Core/Set.h"
#include "Core/Utils.h"

using namespace Sailor;
using namespace Sailor::Memory;
using Timer = Utils::Timer;

class TestCase_SetPerfromance
{
public:

	static void RunTests()
	{
		printf("\n\n%s\n", "class Sailor::TSet");
		printf("Sanity check passed: %d\n", SanityCheck());
		PerformanceTests();
		printf("\n\n");
	}

	static void PerformanceTests()
	{
		const size_t count = 10000600;

		Timer stdSet;
		Timer tSet;

		TSet<size_t> container;
		std::unordered_set<size_t> ideal;

		stdSet.Start();
		for (size_t i = 0; i < count; i++)
		{
			ideal.insert(i);
		}
		stdSet.Stop();

		tSet.Start();
		for (size_t i = 0; i < count; i++)
		{
			container.Insert(i);
		}
		tSet.Stop();

		SAILOR_LOG("Performance test insert:\n\tstd::set %llums\n\tTSet %llums", stdSet.ResultMs(), tSet.ResultMs());

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
			container.Contains(value);
		}
		tSet.Stop();

		SAILOR_LOG("Performance test contains:\n\tstd::set %llums\n\tTSet %llums", stdSet.ResultMs(), tSet.ResultMs());
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

		for (auto& el : container)
		{
			SAILOR_LOG("%llu", el);
		}

		SAILOR_LOG("Performance test remove:\n\tstd::set %llums\n\tTSet %llums", stdSet.ResultMs(), tSet.ResultMs());
	}

	static bool SanityCheck()
	{
		return false;
	}
};

void Sailor::RunSetBenchmark()
{
	printf("Starting set benchmark...\n");

	TestCase_SetPerfromance::RunTests();
}
