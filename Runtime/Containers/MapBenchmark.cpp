#include <unordered_map>
#include "Containers/Map.h"
#include "Containers/ConcurrentMap.h"
#include "Core/Utils.h"
#include <random>
#include "Tasks/Tasks.h"
#include "Tasks/Scheduler.h"

using namespace Sailor::Memory;
using Timer = Sailor::Utils::Timer;

namespace Sailor
{
	template<uint32_t numBytes>
	struct TPlainData
	{
		TPlainData() : TPlainData(0) {}
		TPlainData(size_t value)
		{
			memset(m_data, (uint32_t)value, numBytes * sizeof(uint8_t));
		}

		TPlainData& operator++()
		{
			++(*((uint32_t*)m_data));
			return this;
		}

		bool operator==(const TPlainData& rhs) const
		{
			return memcmp(m_data, rhs.m_data, numBytes * sizeof(uint8_t)) == 0;
		}

		uint8_t m_data[numBytes];
	};

	template<uint32_t numBytes>
	struct TDeepData
	{
		TDeepData() : TDeepData(0) {}
		TDeepData(size_t value)
		{
			m_value = (uint32_t)value;
			m_data = new uint8_t[numBytes];
			memset(m_data, (uint32_t)value, numBytes * sizeof(uint8_t));
		}

		TDeepData(const TDeepData& rhs)
		{
			m_value = rhs.m_value;
			m_data = new uint8_t[numBytes];
			memcpy(m_data, rhs.m_data, numBytes * sizeof(uint8_t));
		}

		TDeepData(TDeepData&& rhs) noexcept
		{
			check(rhs.m_data != nullptr);

			m_value = rhs.m_value;

			if (m_data == rhs.m_data)
			{
				return;
			}

			std::swap(m_data, rhs.m_data);
		}

		TDeepData& operator++()
		{
			++(*((uint32_t*)m_data));
			return this;
		}

		TDeepData& operator=(const TDeepData& rhs)
		{
			m_value = rhs.m_value;

			if (m_data == nullptr)
			{
				m_data = new uint8_t[numBytes];
			}

			memcpy(m_data, rhs.m_data, numBytes * sizeof(uint8_t));
			return *this;
		}

		TDeepData& operator=(TDeepData&& rhs) noexcept
		{
			m_value = rhs.m_value;

			if (m_data == rhs.m_data)
			{
				return *this;
			}

			std::swap(m_data, rhs.m_data);
			return *this;
		}

		~TDeepData()
		{
			delete[] m_data;
		}

		bool operator==(const TDeepData& rhs) const
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

		size_t m_operator = 0;
		size_t m_containsKey = 0;
		size_t m_containsValue = 0;
		size_t m_removeFirst = 0;
		size_t m_remove = 0;
		float m_misses = 0.0f;

		bool m_bSanityPassed = false;

		void PrintLog()
		{
			SAILOR_LOG("\nClassName: %s", m_className.c_str());
			SAILOR_LOG("Sanity check passed: %d", m_bSanityPassed);
			SAILOR_LOG("Performance test operator[]: %llums", m_operator);
			SAILOR_LOG("Performance test Remove: %llums", m_remove);
			SAILOR_LOG("Performance test ContainsKey (misses %.2f): %llums", m_misses, m_containsKey);
			SAILOR_LOG("Performance test ContainsValue: %llums", m_containsValue);
		}
	};

	template<typename TValue, typename TContainer>
	class TestCase_MapPerfromance
	{
	public:

		static Result RunTests()
		{
			Result r = PerformanceTests();
			r.m_bSanityPassed = true;
			return r;
		}

		static Result PerformanceTests()
		{
			Result res(typeid(TContainer).name());

                        const size_t count = 1000;

			TContainer container;
			Timer tMap;

			tMap.Start();
			for (size_t i = 0; i < count; i++)
			{
				container[i] = i * 3;
			}
			tMap.Stop();

			res.m_operator = tMap.ResultMs();

			tMap.Clear();
			std::random_device rd;
			std::mt19937 g(rd());

			std::atomic<uint32_t> misses = 0;

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

			res.m_containsKey = tMap.ResultMs();
			res.m_misses = misses.load() / (float)count;

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

			res.m_remove = tMap.ResultMs();

			return res;
		}

		static bool SanityCheck()
		{
			const size_t count = 180;

			TContainer container;
			std::unordered_map<size_t, TValue> ideal;

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

	template<typename TValue>
	class TestCase_MapPerfromance<TValue, std::unordered_map<size_t, TValue>>
	{
	public:

		static Result RunTests()
		{
			Result r = PerformanceTests();
			r.m_bSanityPassed = true;
			return r;
		}

		static Result PerformanceTests()
		{
			Result res(typeid(std::unordered_map<size_t, TValue>).name());

                    const size_t count = 1000;

			Timer stdMap;
			std::unordered_map<size_t, TValue> ideal;

			stdMap.Start();
			for (size_t i = 0; i < count; i++)
			{
				ideal[i] = i * 3;
			}
			stdMap.Stop();

			res.m_operator = stdMap.ResultMs();

			stdMap.Clear();

			std::random_device rd;
			std::mt19937 g(rd());

			std::atomic<uint32_t> misses = 0;

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
			res.m_containsKey = stdMap.ResultMs();
			res.m_misses = misses.load() / (float)count;

			stdMap.Clear();
			stdMap.Start();
			for (size_t i = 0; i < count; i++)
			{
				if (i % 2)
				{
					ideal.erase(i);
				}
			}
			stdMap.Stop();

			res.m_remove = stdMap.ResultMs();
			return res;
		}
	};

	template<typename TValue, typename TContainer>
	class TestCase_ConcurrentMapPerfromance
	{
	public:

		static Result RunTests()
		{
                        const size_t numThreads = 4;
                    const size_t count = 1000;
			Result res(typeid(TContainer).name());

			TContainer container;
			Timer tMap;

			std::random_device rd;
			std::mt19937 g(rd());

			std::vector<uint32_t> order;

			for (uint32_t i = 0; i < count; i++)
			{
				order.push_back(i);
			}

			g.seed(0);
			uint32_t cShift = 127;
			std::shuffle(order.begin(), order.end(), g);

			TVector<Tasks::ITaskPtr> tasksToWait;

			for (uint32_t i = 0; i < numThreads; i++)
			{
				auto task = Tasks::CreateTask("Test Concurrent Map", [&container, i, cShift]() mutable
					{
						for (uint32_t k = 0; k < 1; k++)
						{
							uint32_t start = uint32_t(i * (count / numThreads) + k * cShift);

							for (uint32_t j = start; j < count + start; j++)
							{
								container.At_Lock(j % count);
								container.Unlock(j % count);
							}
						}
					});
				tasksToWait.Add(task);
			}

			tMap.Start();
			for (uint32_t i = 0; i < numThreads; i++)
			{
				tasksToWait[i]->Run();
			}

			for (uint32_t i = 0; i < numThreads; i++)
			{
				tasksToWait[i]->Wait();
			}
			tMap.Stop();

			res.m_operator = tMap.ResultMs();

			res.m_bSanityPassed = container.Num() == count;
			for (size_t i = 0; i < count; i++)
			{
				if (*((int*)container[0].m_data) != *((int*)container[i].m_data))
				{
					res.m_bSanityPassed = false;
					break;
				}
			}

			tMap.Clear();

			std::atomic<uint32_t> misses = 0;

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

			res.m_containsKey = tMap.ResultMs();
			res.m_misses = misses.load() / (float)count;

			tMap.Clear();

			tasksToWait.Clear();
			for (uint32_t i = 0; i < numThreads; i++)
			{
				auto task = Tasks::CreateTask("Test Concurrent Map", [&container, i]() mutable
					{
						uint32_t start = uint32_t(i * (count / numThreads));

						for (uint32_t j = start; j < count + start; j++)
						{
							if (j % 2)
							{
								container.Remove(j);
							}
						}

					});
				tasksToWait.Add(task);
			}

			tMap.Start();
			for (uint32_t i = 0; i < numThreads; i++)
			{
				tasksToWait[i]->Run();
			}

			for (uint32_t i = 0; i < numThreads; i++)
			{
				tasksToWait[i]->Wait();
			}
			tMap.Stop();
			res.m_remove = tMap.ResultMs();

			return res;
		}
	};

	void RunMapBenchmark()
	{
		using TDeepData = TDeepData<1024>;
		using TPlainData = TPlainData<1024>;

		printf("\nStarting Map benchmark...\n");

		TVector<Result> res;

		res.Add(TestCase_MapPerfromance<TPlainData, std::unordered_map<size_t, TPlainData>>::RunTests());
		res.Add(TestCase_MapPerfromance<TDeepData, std::unordered_map<size_t, TDeepData>>::RunTests());

		res.Add(TestCase_MapPerfromance<TPlainData, Sailor::TMap<size_t, TPlainData>>::RunTests());
		res.Add(TestCase_MapPerfromance<TDeepData, Sailor::TMap<size_t, TDeepData>>::RunTests());

		res.Add(TestCase_ConcurrentMapPerfromance<TPlainData, Sailor::TConcurrentMap<size_t, TPlainData, 32u, ERehashPolicy::Always>>::RunTests());
		res.Add(TestCase_ConcurrentMapPerfromance<TDeepData, Sailor::TConcurrentMap<size_t, TDeepData, 32u, ERehashPolicy::Always>>::RunTests());

		for (auto& r : res)
		{
			r.PrintLog();
		}

		printf("\n\n");
	}
}
