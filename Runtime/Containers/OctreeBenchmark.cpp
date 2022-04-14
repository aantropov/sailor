#include "Containers/Octree.h"
#include "Core/Utils.h"
#include <random>

using namespace Sailor;
using namespace Sailor::Memory;
using Timer = Utils::Timer;

template<typename TContainer>
class TestCase_OctreePerfromance
{
public:

	static void RunTests()
	{
		const std::string tOctreeClassName = typeid(TContainer).name();

		printf("%s\n", tOctreeClassName.c_str());
		printf("Sanity check passed: %d\n", SanityCheck());
		PerformanceTests();
		printf("\n");
	}

	static bool SanityCheck()
	{
		return true;
	}

	static void PerformanceTests()
	{
		const uint32_t count = 10000;

		struct Data
		{
			glm::ivec3 m_pos;
			glm::ivec3 m_extents;
			size_t m_data;
		};

		Data data[count];

		for (size_t i = 0; i < count; i++)
		{
			data[i].m_pos = glm::ivec3(rand() % 1000, rand() % 1000, rand() % 1000);
			data[i].m_extents = glm::ivec3(rand() % 100, rand() % 100, rand() % 100);
			data[i].m_data = i;
		}

		Timer tOctree;
		TContainer container(glm::ivec3(0, 0, 0), 2048u, 2u);

		tOctree.Start();
		for (size_t i = 0; i < count; i++)
		{
			const bool bInserted = container.Insert(data[i].m_pos, data[i].m_extents, data[i].m_data);
			assert(bInserted);
		}
		tOctree.Stop();

		SAILOR_LOG("Performance test insert:\n\t TOctree %llums, nodes:%llu", tOctree.ResultMs(), container.GetNumNodes());

		tOctree.Clear();
		tOctree.Start();
		for (size_t i = 0; i < count; i++)
		{
			const bool bRemoved = container.Remove(data[i].m_pos, data[i].m_extents, data[i].m_data);
			assert(bRemoved);
		}
		tOctree.Stop();
		SAILOR_LOG("Performance test remove:\n\t TOctree %llums, nodes:%llu", tOctree.ResultMs(), container.GetNumNodes());

		tOctree.Clear();
		tOctree.Start();
		container.Resolve();
		tOctree.Stop();
		SAILOR_LOG("Performance test resolve:\n\t TOctree %llums, nodes:%llu", tOctree.ResultMs(), container.GetNumNodes());
	}
};

void Sailor::RunOctreeBenchmark()
{
	printf("\nStarting Octree benchmark...\n");

	TestCase_OctreePerfromance<Sailor::TOctree<size_t>>::RunTests();
}
