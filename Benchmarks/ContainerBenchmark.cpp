#include "Containers/ConcurrentMap.h"
#include "Containers/ConcurrentSet.h"
#include "Containers/List.h"
#include "Containers/Map.h"
#include "Containers/Octree.h"
#include "Containers/Set.h"
#include "Containers/Vector.h"
#include "Memory/UniquePtr.hpp"

#include <algorithm>
#include <array>
#include <barrier>
#include <charconv>
#include <chrono>
#include <deque>
#include <iostream>
#include <list>
#include <numeric>
#include <random>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <type_traits>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace
{
	using namespace Sailor;
	using Clock = std::chrono::steady_clock;

	struct PlainValue
	{
		std::array<uint64_t, 64> m_words;
		explicit PlainValue(uint64_t value = 0)
		{
			for (size_t i = 0; i < m_words.size(); ++i) m_words[i] = value * 131 + i;
		}
		bool operator==(const PlainValue&) const = default;
		uint64_t Checksum() const { return std::accumulate(m_words.begin(), m_words.end(), uint64_t(0)); }
	};

	struct OwnedValue
	{
		TUniquePtr<PlainValue> m_value;
		explicit OwnedValue(uint64_t value = 0) : m_value(TUniquePtr<PlainValue>::Make(value)) {}
		OwnedValue(const OwnedValue& other) : m_value(TUniquePtr<PlainValue>::Make(*other.m_value)) {}
		OwnedValue& operator=(const OwnedValue& other) { m_value = TUniquePtr<PlainValue>::Make(*other.m_value); return *this; }
		OwnedValue(OwnedValue&&) noexcept = default;
		OwnedValue& operator=(OwnedValue&&) noexcept = default;
		bool operator==(const OwnedValue& other) const { return *m_value == *other.m_value; }
		uint64_t Checksum() const { return m_value->Checksum(); }
	};

	void Require(bool condition, std::string_view message)
	{
		if (!condition) throw std::runtime_error(std::string(message));
	}

	struct Sample
	{
		std::string_view m_container, m_payload;
		size_t m_iteration, m_threads = 1;
	};

	template<typename Operation, typename Inspect>
	void Measure(const Sample& sample, std::string_view operation, size_t count, Operation work, Inspect inspect)
	{
		const auto start = Clock::now();
		work();
		const auto elapsed = Clock::now() - start;
		const uint64_t checksum = inspect();
		std::cout << sample.m_container << ',' << sample.m_payload << ',' << sample.m_iteration << ','
			<< sample.m_threads << ',' << operation << ',' << count << ','
			<< std::chrono::duration<double, std::micro>(elapsed).count() << ',' << checksum << ",measured\n";
	}

	template<typename Container, typename Expected>
	uint64_t CheckSequence(const Container& actual, const Expected& expected)
	{
		Require(std::equal(actual.begin(), actual.end(), expected.begin(), expected.end()), "Sequence contents or order differ");
		uint64_t checksum = 0;
		for (const auto& value : actual) checksum = checksum * 31 + value.Checksum();
		return checksum;
	}

	template<typename Value, bool SailorContainer>
	void MeasureVector(size_t count, size_t iteration, std::string_view payload)
	{
		using Container = std::conditional_t<SailorContainer, TVector<Value>, std::vector<Value>>;
		Container values;
		std::vector<Value> expected;
		std::mt19937 random(0);
		const Sample sample{ SailorContainer ? "TVector" : "std::vector", payload, iteration };
		auto inspect = [&] { return CheckSequence(values, expected); };
		for (size_t i = 0; i < count; ++i) expected.emplace_back(i);
		Measure(sample, "append", count, [&]
		{
			for (size_t i = 0; i < count; ++i)
			{
				if constexpr (SailorContainer)
				{
					if (i % 2) values.Add(Value(i)); else values.Emplace(i);
				}
				else
				{
					if (i % 2) values.push_back(Value(i)); else values.emplace_back(i);
				}
			}
		}, inspect);

		std::vector<size_t> positions;
		for (size_t i = 0; i < count / 2; ++i)
		{
			positions.push_back(random() % expected.size());
			if (positions.back() + 1 != expected.size()) expected[positions.back()] = std::move(expected.back());
			expected.pop_back();
		}
		Measure(sample, "remove_swap", positions.size(), [&]
		{
			for (size_t pos : positions)
			{
				if constexpr (SailorContainer) values.RemoveAtSwap(pos);
				else { if (pos + 1 != values.size()) values[pos] = std::move(values.back()); values.pop_back(); }
			}
		}, inspect);
		positions.clear();
		for (size_t i = 0; i < count / 128; ++i)
		{
			positions.push_back(random() % expected.size());
			expected.erase(expected.begin() + positions.back());
		}
		Measure(sample, "erase", positions.size(), [&]
		{
			for (size_t pos : positions)
			{
				if constexpr (SailorContainer) values.RemoveAt(pos);
				else values.erase(values.begin() + pos);
			}
		}, inspect);
		std::vector<Value> removed;
		for (size_t i = 0; i < count / 128; ++i)
		{
			const size_t pos = random() % expected.size();
			removed.push_back(expected[pos]);
			expected.erase(expected.begin() + pos);
		}
		Measure(sample, "remove_first", removed.size(), [&]
		{
			for (const auto& value : removed)
			{
				if constexpr (SailorContainer) values.RemoveFirst(value);
				else values.erase(std::find(values.begin(), values.end(), value));
			}
		}, inspect);
		positions.clear();
		for (size_t i = 0; i < count / 256; ++i)
		{
			positions.push_back(random() % (expected.size() + 1));
			expected.insert(expected.begin() + positions.back(), Value(count + i));
		}
		Measure(sample, "insert", positions.size(), [&]
		{
			for (size_t i = 0; i < positions.size(); ++i)
			{
				if constexpr (SailorContainer) values.Insert(Value(count + i), positions[i]);
				else values.insert(values.begin() + positions[i], Value(count + i));
			}
		}, inspect);
	}

	template<typename Value, bool SailorContainer>
	void MeasureList(size_t count, size_t iteration, std::string_view payload)
	{
		using Container = std::conditional_t<SailorContainer, TList<Value>, std::list<Value>>;
		Container values;
		std::deque<Value> expected;
		const Sample sample{ SailorContainer ? "TList" : "std::list", payload, iteration };
		auto inspect = [&] { return CheckSequence(values, expected); };
		for (size_t i = 0; i < count; ++i)
		{
			Value value(i % std::max(size_t(1), count / 4));
			if (i % 2) expected.push_back(value); else expected.push_front(value);
		}
		Measure(sample, "push_front_back", count, [&]
		{
			for (size_t i = 0; i < count; ++i)
			{
				Value value(i % std::max(size_t(1), count / 4));
				if constexpr (SailorContainer)
				{
					if (i % 2) values.PushBack(std::move(value)); else values.PushFront(std::move(value));
				}
				else
				{
					if (i % 2) values.push_back(std::move(value)); else values.push_front(std::move(value));
				}
			}
		}, inspect);
		for (size_t i = 0; i < count / 2; ++i)
		{
			if (i % 2) expected.pop_back(); else expected.pop_front();
		}
		Measure(sample, "pop_front_back", count / 2, [&]
		{
			for (size_t i = 0; i < count / 2; ++i)
			{
				if constexpr (SailorContainer) { if (i % 2) values.PopBack(); else values.PopFront(); }
				else { if (i % 2) values.pop_back(); else values.pop_front(); }
			}
		}, inspect);
		std::vector<Value> removed;
		std::mt19937 random(0);
		for (size_t i = 0; i < count / 256 && !expected.empty(); ++i)
		{
			removed.push_back(expected[random() % expected.size()]);
			std::erase(expected, removed.back());
		}
		Measure(sample, "remove_all", removed.size(), [&]
		{
			for (const auto& value : removed)
			{
				if constexpr (SailorContainer) values.RemoveAll(value); else values.remove(value);
			}
		}, inspect);
	}

	template<typename Container>
	void MeasureSet(size_t count, size_t iteration, std::string_view name)
	{
		Container values;
		const Sample sample{ name, "uint32", iteration };
		auto contains = [&](uint32_t key)
		{
			if constexpr (requires { values.Contains(key); }) return values.Contains(key);
			else return values.contains(key);
		};
		auto inspect = [&](bool removed)
		{
			size_t size;
			if constexpr (requires { values.Num(); }) size = values.Num(); else size = values.size();
			Require(size == (removed ? (count + 1) / 2 : count), "Set size differs");
			uint64_t checksum = 0;
			for (uint32_t key = 0; key < count * 2; ++key)
			{
				const bool expected = key < count && (!removed || key % 2 == 0);
				Require(contains(key) == expected, "Set membership differs");
				if (expected) checksum += key + 1;
			}
			return checksum;
		};
		Measure(sample, "insert", count, [&]
		{
			for (uint32_t key = 0; key < count; ++key)
			{
				if constexpr (requires { values.Insert(key); }) values.Insert(key); else values.insert(key);
			}
		}, [&] { return inspect(false); });
		std::vector<uint32_t> keys(count);
		std::iota(keys.begin(), keys.end(), 0u);
		std::mt19937 random(0);
		std::shuffle(keys.begin(), keys.end(), random);
		for (bool misses : { false, true })
		{
			size_t hits = 0;
			Measure(sample, misses ? "find_miss" : "find_hit", count,
				[&] { for (uint32_t key : keys) hits += contains(key + (misses ? static_cast<uint32_t>(count) : 0)); },
				[&] { Require(hits == (misses ? 0 : count), "Set hit count differs"); return hits; });
		}
		Measure(sample, "remove_odd", count / 2, [&]
		{
			for (uint32_t key = 1; key < count; key += 2)
			{
				if constexpr (requires { values.Remove(key); }) values.Remove(key); else values.erase(key);
			}
		}, [&] { return inspect(true); });
	}

	template<bool Concurrent, typename Operation>
	void ForKeys(size_t count, uint32_t first, uint32_t step, Operation operation)
	{
		if constexpr (!Concurrent)
		{
			for (uint32_t key = first; key < count; key += step) operation(key);
		}
		else
		{
			std::barrier start(4);
			std::vector<std::jthread> workers;
			for (uint32_t worker = 0; worker < 4; ++worker)
			{
				workers.emplace_back([&, worker]
				{
					start.arrive_and_wait();
					for (uint32_t key = first + worker * step; key < count; key += 4 * step) operation(key);
				});
			}
			for (auto& worker : workers) worker.join();
		}
	}

	template<typename Value, typename Container, bool Concurrent = false>
	void MeasureMap(size_t count, size_t iteration, std::string_view payload, std::string_view name)
	{
		Container values;
		const Sample sample{ name, payload, iteration, Concurrent ? 4u : 1u };
		auto expected = [](uint32_t key, bool updated) { return Value(updated && key % 2 == 0 ? key * 7ull + 3 : key * 3ull + 1); };
		auto inspect = [&](bool updated, bool removed)
		{
			size_t size;
			if constexpr (requires { values.Num(); }) size = values.Num(); else size = values.size();
			Require(size == (removed ? (count + 1) / 2 : count), "Map size differs");
			uint64_t checksum = 0;
			for (uint32_t key = 0; key < count; ++key)
			{
				Value value;
				bool found;
				if constexpr (Concurrent) found = values.TryGet(key, value);
				else if constexpr (requires { values.Find(key); })
				{
					const auto it = values.Find(key);
					found = it != values.end();
					if (found) value = it.Value();
				}
				else
				{
					const auto it = values.find(key);
					found = it != values.end();
					if (found) value = it->second;
				}
				Require(found == (!removed || key % 2 == 0), "Map membership differs");
				if (found) { Require(value == expected(key, updated), "Map value differs"); checksum += value.Checksum(); }
			}
			return checksum;
		};
		for (bool updated : { false, true })
		{
			Measure(sample, updated ? "update_even" : "insert", updated ? (count + 1) / 2 : count, [&]
			{
				ForKeys<Concurrent>(count, 0, updated ? 2 : 1, [&](uint32_t key)
				{
					if constexpr (Concurrent) { values.At_Lock(key) = expected(key, updated); values.Unlock(key); }
					else values[key] = expected(key, updated);
				});
			}, [&] { return inspect(updated, false); });
		}
		std::vector<uint8_t> hits(count);
		std::vector<uint32_t> keys(count);
		std::mt19937 random(0);
		const auto numKeys = static_cast<uint32_t>(count);
		for (size_t i = 0; i < count; ++i) keys[i] = random() % numKeys + (i % 2 ? numKeys : 0u);
		Measure(sample, "find_mixed", count, [&]
		{
			ForKeys<Concurrent>(count, 0, 1, [&](uint32_t i)
			{
				if constexpr (requires { values.ContainsKey(keys[i]); }) hits[i] = values.ContainsKey(keys[i]);
				else hits[i] = values.contains(keys[i]);
			});
		}, [&]
		{
			for (size_t i = 0; i < count; ++i) Require(hits[i] == (i % 2 == 0), "Map lookup differs");
			return std::accumulate(hits.begin(), hits.end(), uint64_t(0));
		});
		Measure(sample, "remove_odd", count / 2, [&]
		{
			ForKeys<Concurrent>(count, 1, 2, [&](uint32_t key)
			{
				if constexpr (requires { values.Remove(key); }) values.Remove(key); else values.erase(key);
			});
		}, [&] { return inspect(true, true); });
	}

	void MeasureOctree(size_t count, size_t iteration)
	{
		TOctree<size_t> tree(glm::ivec3(0), 2048, 2);
		std::vector<glm::ivec3> positions(count), extents(count);
		std::mt19937 random(0);
		for (size_t i = 0; i < count; ++i)
		{
			for (int axis = 0; axis < 3; ++axis)
			{
				positions[i][axis] = static_cast<int>(random() % 960) - 480;
				extents[i][axis] = 1 + random() % 32;
			}
		}
		const Sample sample{ "TOctree", "bounds", iteration };
		auto inspect = [&](bool removed)
		{
			Require(tree.Num() == (removed ? (count + 1) / 2 : count), "Octree size differs");
			for (size_t i = 0; i < count; ++i) Require(tree.Contains(i) == (!removed || i % 2 == 0), "Octree membership differs");
			return tree.Num();
		};
		size_t completed = 0;
		Measure(sample, "insert", count, [&]
		{
			for (size_t i = 0; i < count; ++i) completed += tree.Insert(positions[i], extents[i], i);
		}, [&] { Require(completed == count, "Octree insertion failed"); return inspect(false); });
		for (auto& position : positions) position += glm::ivec3(3, -2, 1);
		completed = 0;
		Measure(sample, "update", count, [&]
		{
			for (size_t i = 0; i < count; ++i) completed += tree.Update(positions[i], extents[i], i);
		}, [&] { Require(completed == count, "Octree update failed"); return inspect(false); });
		Measure(sample, "remove_odd", count / 2, [&]
		{
			for (size_t i = 1; i < count; i += 2) tree.Remove(i);
		}, [&] { return inspect(true); });
		Measure(sample, "resolve_populated", 1, [&] { tree.Resolve(); }, [&] { return inspect(true); });

		std::array<TVector<size_t>, 32> hits;
		std::array<std::vector<size_t>, 32> expected;
		std::vector<Math::Ray> rays;
		for (size_t query = 0; query < hits.size(); ++query)
		{
			const auto& target = positions[(query * 2) % count];
			rays.emplace_back(glm::vec3(-900, target.y, target.z), glm::vec3(1, 0, 0));
			for (size_t i = 0; i < count; i += 2)
			{
				if (std::abs(target.y - positions[i].y) <= extents[i].y && std::abs(target.z - positions[i].z) <= extents[i].z)
					expected[query].push_back(i);
			}
		}
		Measure(sample, "trace_ray", rays.size(), [&]
		{
			for (size_t query = 0; query < rays.size(); ++query) tree.TraceRay(rays[query], hits[query], 1800);
		}, [&]
		{
			uint64_t checksum = 0;
			for (size_t query = 0; query < rays.size(); ++query)
			{
				std::sort(hits[query].begin(), hits[query].end());
				Require(std::equal(hits[query].begin(), hits[query].end(), expected[query].begin(), expected[query].end()), "Octree spatial query differs");
				for (size_t key : hits[query]) checksum = checksum * 31 + key + 1;
			}
			return checksum;
		});
	}

	template<typename Value>
	void MeasurePayload(size_t count, size_t iteration, std::string_view payload)
	{
		MeasureVector<Value, false>(count, iteration, payload);
		MeasureVector<Value, true>(count, iteration, payload);
		MeasureList<Value, false>(count, iteration, payload);
		MeasureList<Value, true>(count, iteration, payload);
		MeasureMap<Value, std::unordered_map<uint32_t, Value>>(count, iteration, payload, "std::unordered_map");
		MeasureMap<Value, TMap<uint32_t, Value>>(count, iteration, payload, "TMap");
		MeasureMap<Value, TConcurrentMap<uint32_t, Value>, true>(count, iteration, payload, "TConcurrentMap");
	}
}

int main(int argc, char** argv)
{
	constexpr std::string_view Usage = "SailorContainerBenchmark [--count N] [--iterations N]\n";
	if (argc == 2 && std::string_view(argv[1]) == "--help") { std::cout << Usage; return 0; }
	size_t count = 4096, iterations = 3;
	for (int i = 1; i < argc; i += 2)
	{
		if (i + 1 == argc) { std::cerr << Usage; return 2; }
		const std::string_view option(argv[i]), value(argv[i + 1]);
		size_t number = 0;
		const auto result = std::from_chars(value.data(), value.data() + value.size(), number);
		if (result.ec != std::errc{} || result.ptr != value.data() + value.size() || number == 0) { std::cerr << Usage; return 2; }
		if (option == "--count" && number <= UINT32_MAX / 2) count = number;
		else if (option == "--iterations") iterations = number;
		else { std::cerr << Usage; return 2; }
	}
	try
	{
		std::cout << "container,payload,iteration,threads,operation,operations,elapsed_us,checksum,result\n";
		for (size_t iteration = 0; iteration < iterations; ++iteration)
		{
			MeasurePayload<PlainValue>(count, iteration, "inline512");
			MeasurePayload<OwnedValue>(count, iteration, "owned512");
			MeasureSet<std::unordered_set<uint32_t>>(count, iteration, "std::unordered_set");
			MeasureSet<TSet<uint32_t>>(count, iteration, "TSet");
			MeasureSet<TConcurrentSet<uint32_t>>(count, iteration, "TConcurrentSet");
			MeasureOctree(count, iteration);
		}
		return 0;
	}
	catch (const std::exception& error)
	{
		std::cerr << "Container benchmark failed: " << error.what() << '\n';
		return 1;
	}
}
