#include "Defines.h"
#include "Set.h"
#include "Math/Math.h"
#include "Core/Utils.h"
#include "Memory/Memory.h"

using namespace Sailor;
using namespace Sailor::Memory;
using Timer = Utils::Timer;

class TestCase_SetPerfromance
{
	struct TData
	{
		TData(uint32_t value)
		{
			m_value = value;
		}

		bool operator==(const TData& rhs) const
		{
			return m_value == rhs.m_value;
		}

		uint32_t m_value;
	};

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
		Timer stdSet;
		Timer tSet;

		const size_t count = 13600;

		TSet<TData, Sailor::Memory::MallocAllocator> container;
		std::unordered_set<TData> ideal;

		srand(0);
		tSet.Start();
		for (size_t i = 0; i < count; i++)
		{
			const int32_t value = rand();
			container.Insert(value);
		}
		tSet.Stop();

		srand(0);
		stdSet.Start();
		for (size_t i = 0; i < count; i++)
		{
			const int32_t value = rand();
			ideal.insert(value);
		}
		stdSet.Stop();

		SAILOR_LOG("Performance test insert:\n\tstd::set %llums\n\tTSet %llums", stdSet.ResultMs(), tSet.ResultMs());
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
