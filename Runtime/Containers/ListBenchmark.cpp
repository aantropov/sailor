#include "Memory.h"
#include <cstdlib>
#include <list>
#include <cassert>
#include <cctype>
#include "Core/Utils.h"
#include "List.h"
#include "Memory/Memory.h"

using namespace Sailor;
using namespace Sailor::Memory;
using Timer = Utils::Timer;

class TestCase_ListPerfromance
{
	struct TData
	{
		TData(uint32_t value)
		{
			m_value = value;
			m_data = new size_t[8];
			memset(m_data, value, 8 * sizeof(size_t));
			numInstances++;
		}

		TData(const TData& rhs)
		{
			m_data = new size_t[8];
			m_value = rhs.m_value;

			memcpy(m_data, rhs.m_data, 8 * sizeof(size_t));
			numInstances++;
		}

		~TData()
		{
			delete[] m_data;
			numInstances--;
		}

		bool operator==(const TData& rhs) const
		{
			return m_value == rhs.m_value && memcmp(m_data, rhs.m_data, 8 * sizeof(size_t)) == 0;
		}

		TData& operator=(const TData& rhs)
		{
			m_value = rhs.m_value;
			memcpy(m_data, rhs.m_data, 8 * sizeof(size_t));
			return *this;
		}

		size_t* m_data = nullptr;
		uint32_t m_value;
	};

public:

	static void RunTests()
	{
		RunSanityTests();
	}

	static void RunSanityTests()
	{
		printf("%s\n", "class Sailor::TList");
		printf("Sanity check passed: %d\n", SanityCheck());
		PerformanceTests();
		printf("\n");
	}

	static void PerformanceTests()
	{
	}

	static bool SanityCheck()
	{
		const size_t count = 16571;

		TList<TData> container;
		std::list<TData> ideal;

		srand(0);
		for (size_t i = 0; i < count; i++)
		{
			const int32_t value = rand();

			if (i % 2)
			{
				ideal.push_back(TData(value));
				container.PushBack(TData(value));
			}
			else
			{
				ideal.push_front(TData(value));
				container.PushFront(TData(value));
			}
		}

		bool bRes = Compare(ideal, container);

		uint32_t countToDelete = count / 2;
		for (size_t i = 0; i < countToDelete; i++)
		{
			const int32_t value = (int32_t)i;
			ideal.remove(value);
			container.RemoveAll(value);
		}

		bRes &= Compare(ideal, container);
		countToDelete = count / 5;
		for (size_t i = 0; i < countToDelete; i++)
		{
			if (i % 2)
			{
				ideal.pop_back();
				container.PopBack();
			}
			else
			{
				ideal.pop_front();
				container.PopFront();
			}
		}

		bRes &= Compare(ideal, container);

		container.Clear();
		ideal.clear();

		return bRes && numInstances == 0;
	}

	template<typename T>
	static bool Compare(const std::list<T>& lhs, const TList<T>& rhs)
	{
		if (lhs.size() != rhs.Num())
		{
			return false;
		}

		for (const auto& el : lhs)
		{
			if (!rhs.Contains(el))
			{
				return false;
			}
		}

		for (const auto& el : rhs)
		{
			if (lhs.end() == std::find(lhs.begin(), lhs.end(), el))
			{
				return false;
			}
		}

		return true;
	}

	static size_t numInstances;
};

size_t TestCase_ListPerfromance::numInstances = 0;

void Sailor::RunListBenchmark()
{
	printf("\nStarting List benchmark...\n");

	TestCase_ListPerfromance::RunTests();
}
