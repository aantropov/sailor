#include "Memory.h"
#include <cstdlib>
#include <vector>
#include <cassert>
#include <cctype>
#include "Core/Utils.h"
#include "Vector.h"
#include "Memory/Memory.h"
#include "Tasks/Tasks.h"
#include "Tasks/Scheduler.h"

using namespace Sailor;
using namespace Sailor::Memory;
using Timer = Utils::Timer;

template<uint32_t numBytes>
struct TData
{
	TData(uint32_t value)
	{
		m_value = value;
		m_data = new uint8_t[numBytes];
		memset(m_data, value, numBytes * sizeof(uint8_t));
	}

	TData(const TData& rhs)
	{
		m_value = rhs.m_value;
		m_data = new uint8_t[numBytes];
		memcpy(m_data, rhs.m_data, numBytes * sizeof(uint8_t));
	}

	TData& operator=(const TData& rhs)
	{
		m_value = rhs.m_value;
		memcpy(m_data, rhs.m_data, numBytes * sizeof(uint8_t));
		return *this;
	}

	~TData()
	{
		delete[] m_data;
	}

	bool operator==(const TData& rhs) const
	{
		return m_value == rhs.m_value && memcmp(m_data, rhs.m_data, numBytes * sizeof(uint8_t)) == 0;
	}

	uint8_t* m_data = nullptr;
	uint32_t m_value;
};

struct Result
{
	std::string m_className;

	Result() = default;
	Result(std::string className) : m_className(className) {}

	size_t m_addEmplace = 0;
	size_t m_removeAtSwap = 0;
	size_t m_removeAt = 0;
	size_t m_removeFirst = 0;
	size_t m_insert = 0;

	bool m_bSanityPassed = false;

	void PrintLog()
	{
		SAILOR_LOG("\nClassName: %s", m_className.c_str());
		SAILOR_LOG("Sanity check passed: %d", m_bSanityPassed);
		SAILOR_LOG("Performance test Add/Emplace: %llums", m_addEmplace);
		SAILOR_LOG("Performance test Insert: %llums", m_insert);
		SAILOR_LOG("Performance test RemoveAtSwap: %llums", m_removeAtSwap);
		SAILOR_LOG("Performance test RemoveAt: %llums", m_removeAt);
		SAILOR_LOG("Performance test RemoveFirst: %llums\n", m_removeFirst);
	}
};

template<typename TElement, typename TContainer>
class TestCase_VectorPerfromance
{
public:

	static Result RunTests()
	{
		Result r = PerformanceTests();
		r.m_bSanityPassed = SanityCheck();
		return r;
	}

	static Result PerformanceTests()
	{
		Timer tVector;
		Timer tVectorRemoveSwap;
		Timer tVectorRemoveAt;
		Timer tVectorRemoveFirst;
		Timer tVectorInsert;

		const size_t count = 163600;

		TContainer container;

		srand(0);
		tVector.Start();
		for (size_t i = 0; i < count; i++)
		{
			const int32_t value = rand();

			if (i % 2 == 0)
			{
				container.Add(value);
			}
			else
			{
				container.Emplace(value);
			}
		}
		tVector.Stop();

		uint32_t countToRemoveSwap = count / 2;
		srand(0);
		tVectorRemoveSwap.Start();
		for (size_t i = 0; i < countToRemoveSwap; i++)
		{
			const size_t pos = rand() % container.Num();
			const auto valueToRemove = container[pos];

			container.RemoveAtSwap(pos);
		}
		tVectorRemoveSwap.Stop();

		srand(0);
		uint32_t countToDelete = count / 128;
		tVectorRemoveAt.Start();
		for (size_t i = 0; i < countToDelete; i++)
		{
			const int32_t value = rand() % container.Num();
			container.RemoveAt(value);
		}
		tVectorRemoveAt.Stop();

		uint32_t countToRemove = count / 128;
		srand(0);
		tVectorRemoveFirst.Start();
		for (size_t i = 0; i < countToRemove; i++)
		{
			const size_t pos = rand() % container.Num();
			const auto valueToRemove = container[pos];

			container.RemoveFirst(valueToRemove);
		}
		tVectorRemoveFirst.Stop();

		uint32_t countToAdd = count / 256;
		srand(0);
		tVectorInsert.Start();
		for (size_t i = 0; i < countToAdd; i++)
		{
			const size_t num = rand() % 16;
			const size_t pos = rand() % container.Num();
			container.Insert(TElement((uint32_t)i), pos);
		}
		tVectorInsert.Stop();

		Result r(typeid(TContainer).name());
		r.m_addEmplace = tVector.ResultMs();
		r.m_removeAtSwap = tVectorRemoveSwap.ResultMs();
		r.m_removeAt = tVectorRemoveAt.ResultMs();
		r.m_removeFirst = tVectorRemoveFirst.ResultMs();
		r.m_insert = tVectorInsert.ResultMs();

		return r;
	}

	static bool SanityCheck()
	{
		const size_t count = 16571;

		TContainer container;
		std::vector<TElement> ideal;

		srand(0);
		for (size_t i = 0; i < count; i++)
		{
			const int32_t value = rand();

			if (i % 2 == 0)
			{
				ideal.push_back(TElement(value));
				container.Add(TElement(value));
			}
			else
			{
				ideal.emplace_back(TElement(value));
				container.Emplace(TElement(value));
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

			ideal.insert(ideal.begin() + pos, 1u, TElement((uint32_t)i));
			container.Insert(TElement((uint32_t)i), pos);
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

	static bool Compare(const std::vector<TElement>& lhs, const TContainer& rhs)
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

template<typename T>
class TestCase_VectorPerfromance<T, std::vector<T>>
{
public:

	static Result RunTests()
	{
		Result r = PerformanceTests();
		r.m_bSanityPassed = SanityCheck();
		return r;
	}

	static Result PerformanceTests()
	{
		Timer stdVector;
		Timer stdVectorRemoveSwap;
		Timer stdVectorRemoveAt;
		Timer stdVectorRemoveFirst;
		Timer stdVectorInsert;

		const size_t count = 163600;
		std::vector<T> ideal;
		srand(0);
		stdVector.Start();
		for (size_t i = 0; i < count; i++)
		{
			const int32_t value = rand();

			if (i % 2 == 0)
			{
				ideal.push_back(value);
			}
			else
			{
				ideal.emplace_back(value);
			}
		}
		stdVector.Stop();

		uint32_t countToRemoveSwap = count / 2;
		srand(0);
		stdVectorRemoveSwap.Start();
		for (size_t i = 0; i < countToRemoveSwap; i++)
		{
			const size_t pos = rand() % ideal.size();
			const auto valueToRemove = ideal[pos];

			std::iter_swap(ideal.begin() + pos, ideal.end() - 1);
			ideal.pop_back();
		}
		stdVectorRemoveSwap.Stop();

		srand(0);
		stdVectorRemoveAt.Start();
		uint32_t countToDelete = count / 128;
		for (size_t i = 0; i < countToDelete; i++)
		{
			const int32_t value = rand() % ideal.size();
			ideal.erase(ideal.begin() + value);
		}
		stdVectorRemoveAt.Stop();

		uint32_t countToRemove = count / 128;
		srand(0);
		stdVectorRemoveFirst.Start();
		for (size_t i = 0; i < countToRemove; i++)
		{
			const size_t pos = rand() % ideal.size();
			const auto valueToRemove = ideal[pos];

			ideal.erase(std::find(ideal.begin(), ideal.end(), valueToRemove));
		}
		stdVectorRemoveFirst.Stop();

		uint32_t countToAdd = count / 256;
		stdVectorInsert.Start();
		for (size_t i = 0; i < countToAdd; i++)
		{
			const size_t num = rand() % 16;
			const size_t pos = rand() % ideal.size();

			ideal.insert(ideal.begin() + pos, 1u, T((uint32_t)i));
		}
		stdVectorInsert.Stop();

		Result r(typeid(std::vector<T>).name());
		r.m_addEmplace = stdVector.ResultMs();
		r.m_removeAtSwap = stdVectorRemoveSwap.ResultMs();
		r.m_removeAt = stdVectorRemoveAt.ResultMs();
		r.m_removeFirst = stdVectorRemoveFirst.ResultMs();
		r.m_insert = stdVectorInsert.ResultMs();

		return r;
	}
	static bool SanityCheck() { return true; }
};

void Sailor::RunVectorBenchmark()
{
	using TDataSmall = TData<5>;
	using TDataMedium = TData<125>;
	using TDataHuge = TData<1023>;

	printf("\nStarting Vector benchmark...\n");
	
	TVector<Result> res;

	res.Add(TestCase_VectorPerfromance<TDataSmall, TVector<TDataSmall>>::RunTests());
	res.Add(TestCase_VectorPerfromance<TDataMedium, TVector<TDataMedium>>::RunTests());
	res.Add(TestCase_VectorPerfromance<TDataHuge, TVector<TDataHuge>>::RunTests());
	
	res.Add(TestCase_VectorPerfromance<TDataSmall, std::vector<TDataSmall>>::RunTests());
	res.Add(TestCase_VectorPerfromance<TDataMedium, std::vector<TDataMedium>>::RunTests());
	res.Add(TestCase_VectorPerfromance<TDataHuge, std::vector<TDataHuge>>::RunTests());

	for (auto& r : res)
	{
		r.PrintLog();
	}

	printf("\n\n");

	//TestCase_VectorPerfromance<TData, std::vector<TData>>::RunTests();
}
