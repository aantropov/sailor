#include <unordered_set>
#include "Containers/Set.h"
#include "Containers/ConcurrentSet.h"
#include "Core/Utils.h"
#include <random>

using namespace Sailor;
using namespace Sailor::Memory;
using Timer = Utils::Timer;

template<typename TContainer>
class TestCase_SetPerfromance
{
public:

	static void RunTests()
	{
		const std::string tSetClassName = typeid(TContainer).name();

		printf("%s\n", tSetClassName.c_str());
		printf("Sanity check passed: %d\n", SanityCheck());
		PerformanceTests();
		printf("\n");
	}

	static void PerformanceTests()
	{
		const size_t count = 10000600;

		Timer stdSet;
		Timer tSet;

		TContainer container;
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

		std::random_device rd;
		std::mt19937 g(rd());

		g.seed(0);
		stdSet.Start();
		for (size_t i = 0; i < count; i++)
		{
			const size_t value = count + g();
			auto res = ideal.find(value);
		}
		stdSet.Stop();

		g.seed(0);
		tSet.Start();
		for (size_t i = 0; i < count; i++)
		{
			const size_t value = count + g();
			container.Contains(value);
		}
		tSet.Stop();

		SAILOR_LOG("Performance test contains(only misses):\n\tstd::set %llums\n\tTSet %llums", stdSet.ResultMs(), tSet.ResultMs());
		/////////////////////////////////////////////
		stdSet.Clear();
		tSet.Clear();
		g.seed(0);
		stdSet.Start();
		for (size_t i = 0; i < count; i++)
		{
			const size_t value = g() % count;
			auto res = ideal.find(value);
		}
		stdSet.Stop();

		g.seed(0);
		tSet.Start();
		for (size_t i = 0; i < count; i++)
		{
			const size_t value = g() % count;
			container.Contains(value);
		}
		tSet.Stop();

		SAILOR_LOG("Performance test contains(never miss):\n\tstd::set %llums\n\tTSet %llums", stdSet.ResultMs(), tSet.ResultMs());
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

		SAILOR_LOG("Performance test remove:\n\tstd::set %llums\n\tTSet %llums", stdSet.ResultMs(), tSet.ResultMs());
	}

	static bool SanityCheck()
	{
		const size_t count = 18000;

		TContainer container;
		std::unordered_set<size_t> ideal;

		for (size_t i = 0; i < count; i++)
		{
			ideal.insert(i);
			container.Insert(i);
		}

		for (const auto& el : ideal)
		{
			if (!container.Contains(el))
			{
				return false;
			}
		}

		for (const auto& el : container)
		{
			if (!ideal.contains(el))
			{
				return false;
			}
		}

		for (size_t i = 0; i < count / 2; i++)
		{
			ideal.erase(i * 2);
			container.Remove(i * 2);
		}

		for (const auto& el : ideal)
		{
			if (!container.Contains(el))
			{
				return false;
			}
		}

		for (const auto& el : container)
		{
			if (!ideal.contains(el))
			{
				return false;
			}
		}

		return true;
	}
};

void Sailor::RunSetBenchmark()
{
	printf("\nStarting set benchmark...\n");
	TestCase_SetPerfromance<Sailor::TSet<size_t>>::RunTests();

	printf("\nStarting concurrent set benchmark...\n");
	TestCase_SetPerfromance<Sailor::TConcurrentSet<size_t>>::RunTests();
}
