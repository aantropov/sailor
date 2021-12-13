#include "Memory.h"
#include <cstdlib>
#include <malloc.h>
#include <vector>
#include <fstream>
#include <cassert>
#include <functional> 
#include <cctype>
#include "Core/Utils.h"
#include "Vector.h"

using namespace Sailor;
using namespace Sailor::Memory;
using Timer = Utils::Timer;

class TestCase_VectorPerfromance
{
	struct TData
	{
		TData(uint32_t value)
		{
			m_value = value;
			//memset(m_data, value, 128 * sizeof(size_t));
		}

		bool operator==(const TData& rhs) const
		{
			return m_value == rhs.m_value;// && (memcmp(rhs.m_data, m_data, 128 * sizeof(size_t)) == 0);
		}

		//size_t m_data[128];
		uint32_t m_value;
	};

public:

	static void RunTests()
	{

		RunSanityTests();
	}

	static void RunSanityTests()
	{
		printf("\n\n%s\n", "class Sailor::TVector");
		printf("Sanity check passed: %d\n", SanityCheck());
		printf("\n\n");
	}

	static bool SanityCheck()
	{
		const size_t count = 16536;

		TVector<TData> container;
		std::vector<TData> ideal;

		srand(0);
		for (size_t i = 0; i < count; i++)
		{
			const int32_t value = rand();

			if (i % 2 == 0)
			{
				ideal.push_back(value);
				container.Add(value);
			}
			else
			{
				ideal.emplace_back(value);
				container.Emplace(value);
			}
		}

		bool bRes = Compare(ideal, container);

		uint32_t countToDelete = count / 2;
		for (size_t i = 0; i < countToDelete; i++)
		{
			const int32_t value = rand() % ideal.size();
			ideal.erase(ideal.begin() + value);
			container.RemoveAt(value);
		}

		bRes &= Compare(ideal, container);

		uint32_t countToAdd = count / 4;
		for (size_t i = 0; i < countToAdd; i++)
		{
			const size_t num = rand() % 16;
			const size_t pos = rand() % ideal.size();

			ideal.insert(ideal.begin() + pos, 1u, TData((uint32_t)i));
			container.Insert(TData((uint32_t)i), pos);
		}

		bRes &= Compare(ideal, container);

		uint32_t countToRemove = count / 16;
		for (size_t i = 0; i < countToRemove; i++)
		{
			const size_t pos = rand() % ideal.size();
			const auto valueToRemove = ideal[pos];

			ideal.erase(std::find(ideal.begin(), ideal.end(), valueToRemove));
			container.RemoveFirst(valueToRemove);
		}

		bRes &= Compare(ideal, container);

		uint32_t countToRemoveSwap = count / 16;
		for (size_t i = 0; i < countToRemoveSwap; i++)
		{
			const size_t pos = rand() % ideal.size();
			const auto valueToRemove = ideal[pos];

			std::iter_swap(ideal.begin() + pos, ideal.end() - 1);
			ideal.pop_back();

			container.RemoveAtSwap(pos);
		}

		bRes &= Compare(ideal, container);

		return bRes;
	}

	template<typename T>
	static bool Compare(const std::vector<T>& lhs, const TVector<T>& rhs)
	{
		if (lhs.size() != rhs.Num())
		{
			return false;
		}

		for (size_t i = 0; i < lhs.size(); i++)
		{
			if (rhs[i] != lhs[i])
			{
				return false;
			}
		}
		return true;
	}

};

void Sailor::RunVectorBenchmark()
{
	printf("Starting vector benchmark...\n");

	TestCase_VectorPerfromance::RunTests();
}
