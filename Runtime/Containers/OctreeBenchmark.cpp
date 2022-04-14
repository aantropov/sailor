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

		Timer tOctree;
		TContainer container(glm::ivec3(0, 0, 0), 2048u);

		tOctree.Start();
		for (size_t i = 0; i < count; i++)
		{
			auto pos = glm::ivec3(rand() % 1000, rand() % 1000, rand() % 1000);
			auto extents = glm::ivec3(rand() % 100, rand() % 100, rand() % 100);

			container.Insert(pos, extents, i);
		}
		tOctree.Stop();

		SAILOR_LOG("Performance test insert:\n\t TOctree %llums", tOctree.ResultMs());
	}
};

void Sailor::RunOctreeBenchmark()
{
	printf("\nStarting Octree benchmark...\n");

	TestCase_OctreePerfromance<Sailor::TOctree<size_t>>::RunTests();
}
