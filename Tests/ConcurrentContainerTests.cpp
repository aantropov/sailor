#include <atomic>
#include <barrier>
#include <cstdint>
#include <iostream>
#include <iterator>
#include <stdexcept>
#include <thread>
#include <type_traits>
#include <vector>

#include "Containers/ConcurrentMap.h"
#include "Containers/ConcurrentSet.h"

using namespace Sailor;

namespace
{
	void Require(bool condition, const char* message)
	{
		if (!condition) throw std::runtime_error(message);
	}

	struct Key
	{
		uint32_t m_value = 0;
		size_t GetHash() const { return m_value; }
		bool operator==(const Key&) const = default;
	};

	class SetProbe : public TConcurrentSet<Key, 4, Memory::MallocAllocator>
	{
	public:
		using Super = TConcurrentSet<Key, 4, Memory::MallocAllocator>;
		using Super::Super;
		using Super::Lock;
		using Super::Unlock;
		using Super::TryLock;
		using Super::TryLockAll;
		using Super::UnlockAll;
		using Super::RehashForInsert;
		size_t BucketCount() const { return m_buckets.Num(); }
	};

	template<ERehashPolicy Policy>
	using TestMap = TConcurrentMap<Key, uint64_t, 4, Policy, Memory::MallocAllocator>;

	static_assert(std::bidirectional_iterator<SetProbe::TIterator>);
	static_assert(std::bidirectional_iterator<SetProbe::TConstIterator>);
	static_assert(std::is_convertible_v<SetProbe::TIterator, SetProbe::TConstIterator>);
	static_assert(std::is_const_v<std::remove_reference_t<decltype(*std::declval<SetProbe::TConstIterator>())>>);

	void TestBucketCountsAndBorrowedIteration()
	{
		for (uint32_t requested : { 0u, 3u, 6u, 7u, 24u })
		{
			SetProbe set(requested);
			Require(set.BucketCount() >= 4 && set.BucketCount() % 4 == 0,
				"bucket count must be a nonzero multiple of the stripe count");
			for (uint32_t i = 0; i < 32; ++i) set.Insert(Key{ i });
			for (uint32_t i = 0; i < 32; i += 3) set.Remove(Key{ i });
			set.LockAll();
			std::vector<uint32_t> forward;
			for (const auto& key : set) forward.push_back(key.m_value);
			std::vector<uint32_t> reverse;
			const auto& constSet = set;
			for (auto it = std::make_reverse_iterator(constSet.end());
				it != std::make_reverse_iterator(constSet.begin()); ++it)
			{
				reverse.push_back(it->m_value);
			}
			set.UnlockAll();
			Require(forward.size() == set.Num() && reverse.size() == forward.size(),
				"borrowed iteration must visit every live bucket entry exactly once");
			for (size_t i = 0; i < forward.size(); ++i)
			{
				Require(forward[i] == reverse[reverse.size() - i - 1], "reverse iteration must cross empty buckets correctly");
			}
			set.Clear(requested);
			set.Insert(Key{ 7 });
			Require(set.Num() == 1 && set.Contains(Key{ 7 }), "Clear must preserve the stripe/bucket mapping");
		}
		TConcurrentSet<Key, 4> initialized{ Key{ 1 }, Key{ 2 } };
		Require(initialized.Num() == 2 && initialized.Contains(Key{ 2 }), "initializer-list set must initialize buckets and policy");
		TestMap<ERehashPolicy::Always> initializedMap{ { Key{ 1 }, 10 }, { Key{ 2 }, 20 } };
		initializedMap.Insert(Key{ 1 }, 99);
		Require(initializedMap.Num() == 2 && initializedMap[Key{ 1 }] == 10,
			"map insertion must keep one value per key without replacing an existing value");
	}

	void TestBorrowedEquality()
	{
		SetProbe left(4);
		SetProbe right(24);
		for (uint32_t key : { 1u, 4u, 9u })
		{
			left.Insert(Key{ key });
			right.Insert(Key{ key });
		}
		left.LockAll();
		right.LockAll();
		const bool equal = left == right && right == left;
		const bool selfEqual = left == left && right == right;
		right.UnlockAll();
		left.UnlockAll();
		Require(equal && selfEqual, "borrowed equality and self-comparison must work with all stripes locked");

		right.Remove(Key{ 9 });
		right.Insert(Key{ 13 });
		left.LockAll();
		right.LockAll();
		const bool differentKeys = !(left == right) && !(right == left);
		right.UnlockAll();
		left.UnlockAll();
		Require(differentKeys, "borrowed equality must compare entries, not only counts");

		TestMap<ERehashPolicy::Always> leftMap{ { Key{ 1 }, 10 }, { Key{ 4 }, 40 } };
		TestMap<ERehashPolicy::Always> rightMap{ { Key{ 4 }, 40 }, { Key{ 1 }, 10 } };
		leftMap.LockAll();
		rightMap.LockAll();
		const bool mapsEqual = leftMap == rightMap && rightMap == leftMap;
		const bool mapSelfEqual = leftMap == leftMap && rightMap == rightMap;
		rightMap.UnlockAll();
		leftMap.UnlockAll();
		Require(mapsEqual && mapSelfEqual, "map equality must also retain the borrowed comparison contract");

		rightMap.At_Lock(Key{ 4 }) = 41;
		rightMap.Unlock(Key{ 4 });
		leftMap.LockAll();
		rightMap.LockAll();
		const bool differentValues = !(leftMap == rightMap) && !(rightMap == leftMap);
		rightMap.UnlockAll();
		leftMap.UnlockAll();
		Require(differentValues, "borrowed map equality must compare values as well as keys");
	}

	void TestTryLockOwnership()
	{
		SetProbe set(4);
		set.Lock(1);
		set.Lock(3);
		Require(!set.TryLockAll(1), "TryLockAll must fail when another stripe is owned");
		Require(!set.TryLock(1) && !set.TryLock(3), "failed TryLockAll must retain the excluded and contended locks");
		Require(set.TryLock(0), "failed TryLockAll must release acquired lower stripes");
		set.Unlock(0);
		Require(set.TryLock(2), "failed TryLockAll must release every stripe acquired by this attempt");
		set.Unlock(2);
		set.Unlock(3);
		Require(set.TryLockAll(1), "TryLockAll must acquire the remaining free stripes");
		set.UnlockAll(1);
		Require(!set.TryLock(1), "UnlockAll(except) must not unlock its caller's stripe");
		for (size_t stripe : { 0u, 2u, 3u })
		{
			Require(set.TryLock(stripe), "UnlockAll(except) must release the other stripes");
			set.Unlock(stripe);
		}
		set.Unlock(1);
	}

	void FillPastGrowthThreshold(SetProbe& set)
	{
		set.LockAll();
		for (uint32_t i = 0; i < 17; ++i) set.ForcelyInsert(Key{ i });
		set.UnlockAll();
	}

	void TestGrowthRetainsCallerLock()
	{
		for (const auto policy : { ERehashPolicy::Always, ERehashPolicy::IfNotWriting })
		{
			SetProbe set(4, policy);
			FillPastGrowthThreshold(set);
			set.Lock(2);
			set.RehashForInsert(2);
			Require(set.BucketCount() > 4 && set.Num() == 17, "growth must preserve entries and increase the table size");
			Require(!set.TryLock(2), "rehash must return with the caller's stripe still locked");
			Require(set.TryLock(1), "rehash must release the other stripes");
			set.Unlock(1);
			set.Unlock(2);
			for (uint32_t i = 0; i < 17; ++i) Require(set.Contains(Key{ i }), "rehash must retain each key");
		}
	}

	void TestConcurrentGrowthUpgrade()
	{
		SetProbe set(4, ERehashPolicy::Always);
		FillPastGrowthThreshold(set);
		std::barrier startGrowth(2);
		bool retained[2]{};
		std::vector<std::jthread> workers;
		for (size_t worker = 0; worker < 2; ++worker)
		{
			workers.emplace_back([&, worker]
				{
					const size_t stripe = worker + 2;
					set.Lock(stripe);
					startGrowth.arrive_and_wait();
					set.RehashForInsert(stripe);
					retained[worker] = !set.TryLock(stripe);
					set.Unlock(stripe);
				});
		}
		for (auto& worker : workers) worker.join();
		Require(retained[0] && retained[1], "contending growth must finish without losing either caller's stripe");
		Require(set.Num() == 17 && set.BucketCount() > 4, "contending growth must not duplicate or lose entries");
	}

	void TestIfNotWritingDefersGrowth()
	{
		SetProbe set(4, ERehashPolicy::IfNotWriting);
		FillPastGrowthThreshold(set);
		set.Lock(3);
		bool retained = false;
		std::jthread writer([&]
			{
				set.Lock(1);
				set.RehashForInsert(1);
				retained = !set.TryLock(1);
				set.ForcelyInsert(Key{ 101 });
				set.Unlock(1);
			});
		writer.join();
		Require(set.BucketCount() == 4 && retained, "IfNotWriting must skip growth while another stripe is held");
		set.Unlock(3);
		set.Insert(Key{ 102 });
		Require(set.BucketCount() > 4 && set.Num() == 19 && set.Contains(Key{ 101 }),
			"deferred growth must run on a later insertion without losing the deferred write");
	}

	void TestIndependentStripeProgress()
	{
		TConcurrentMap<Key, uint64_t, 4, ERehashPolicy::Never> map(6);
		auto& held = map.At_Lock(Key{ 0 });
		std::jthread otherStripe([&]
			{
				map.At_Lock(Key{ 1 }) = 17;
				map.Unlock(Key{ 1 });
			});
		otherStripe.join();
		held = 9;
		map.Unlock(Key{ 0 });
		Require(map[Key{ 0 }] == 9 && map[Key{ 1 }] == 17,
			"a different stripe must make progress while a caller holds At_Lock");
	}

	void TestIndependentStructuralWrites()
	{
		constexpr uint32_t numWorkers = 4;
		constexpr uint32_t writes = 128;
		SetProbe set(6, ERehashPolicy::Never);
		TestMap<ERehashPolicy::Never> map(6);
		std::barrier start(numWorkers);
		std::atomic<bool> valid = true;
		std::vector<std::jthread> workers;
		for (uint32_t worker = 0; worker < numWorkers; ++worker)
		{
			workers.emplace_back([&, worker]
				{
					start.arrive_and_wait();
					for (uint32_t i = 0; i < writes; ++i)
					{
						const Key key{ i * numWorkers + worker };
						set.Insert(key);
						map[key] = key.m_value + 100;
						if (!set.Contains(key) || !map.ContainsKey(key)) valid = false;
						if (!set.Remove(key) || !map.Remove(key)) valid = false;
						if (i % 2)
						{
							set.Insert(key);
							map[key] = key.m_value + 100;
						}
					}
				});
		}
		for (auto& worker : workers) worker.join();
		Require(valid, "independent stripe mutations must not corrupt bucket ownership");
		Require(set.Num() == numWorkers * writes / 2 && map.Num() == set.Num(), "structural insert/remove counts must match");
		for (uint32_t i = 0; i < numWorkers * writes; ++i)
		{
			const bool expected = (i / numWorkers) % 2 != 0;
			Require(set.Contains(Key{ i }) == expected && map.ContainsKey(Key{ i }) == expected,
				"independent stripe mutation must preserve exact membership");
		}
	}

	void TestSetWritersWithGrowth()
	{
		constexpr uint32_t numWorkers = 4;
		constexpr uint32_t writes = 128;
		const Key sharedKey{ numWorkers * writes + 1 };
		for (const auto policy : { ERehashPolicy::Always, ERehashPolicy::IfNotWriting })
		{
			SetProbe set(3, policy);
			std::barrier start(numWorkers);
			std::vector<std::jthread> workers;
			for (uint32_t worker = 0; worker < numWorkers; ++worker)
			{
				workers.emplace_back([&, worker]
					{
						start.arrive_and_wait();
						for (uint32_t i = 0; i < writes; ++i)
						{
							set.Insert(Key{ i * numWorkers + worker });
							set.Insert(sharedKey);
						}
					});
			}
			for (auto& worker : workers) worker.join();
			set.Insert(sharedKey);
			Require(set.BucketCount() > 4 && set.Num() == numWorkers * writes + 1,
				"concurrent set growth must retain one entry per key");
			for (uint32_t i = 0; i < numWorkers * writes; ++i)
			{
				Require(set.Contains(Key{ i }), "concurrent set growth must preserve every writer's keys");
			}
			Require(set.Contains(sharedKey), "duplicate insertion during growth must retain the shared key");
		}
	}

	template<ERehashPolicy Policy>
	void TestMapWritersAndSnapshots()
	{
		constexpr uint32_t numWorkers = 4;
		constexpr uint32_t writes = 256;
		const Key counterKey{ numWorkers * writes + 17 };
		TestMap<Policy> map(3);
		map.Insert(counterKey, 0);
		std::barrier start(numWorkers + 1);
		std::atomic<bool> valid = true;
		std::vector<std::jthread> workers;
		for (uint32_t worker = 0; worker < numWorkers; ++worker)
		{
			workers.emplace_back([&, worker]
				{
					start.arrive_and_wait();
					for (uint32_t i = 0; i < writes; ++i)
					{
						const Key key{ i * numWorkers + worker };
						auto& value = map.At_Lock(key);
						value = key.m_value + 1000;
						if (i % 8 == 0) std::this_thread::yield();
						map.Unlock(key);
						auto& counter = map.At_Lock(counterKey);
						++counter;
						map.Unlock(counterKey);
					}
				});
		}
		std::jthread reader([&]
			{
				start.arrive_and_wait();
				uint64_t previous = 0;
				const auto& constMap = map;
				for (uint32_t i = 0; i < writes; ++i)
				{
					const uint64_t count = constMap[counterKey];
					if (count < previous || count > numWorkers * writes) valid = false;
					previous = count;
				}
			});
		for (auto& worker : workers) worker.join();
		reader.join();
		Require(valid && map[counterKey] == numWorkers * writes, "At_Lock updates and copied reads must retain stripe ownership across growth");
		Require(map.Num() == numWorkers * writes + 1, "growth must retain one map entry per key");
		for (uint32_t i = 0; i < numWorkers * writes; ++i)
		{
			Require(map[Key{ i }] == i + 1000, "forced growth must preserve each writer's value");
		}
		map.LockAll();
		const auto keys = map.GetKeys();
		const auto values = map.GetValues();
		uint64_t* borrowed = nullptr;
		const bool found = map.Find(counterKey, borrowed);
		const bool borrowedValue = found && *borrowed == numWorkers * writes;
		const bool removed = map.ForcelyRemove(counterKey);
		map.UnlockAll();
		Require(keys.Num() == numWorkers * writes + 1 && values.Num() == keys.Num() && borrowedValue,
			"LockAll snapshots and borrowed Find must not acquire recursive hidden locks");
		Require(removed && !map.ContainsKey(counterKey) && map.Num() == numWorkers * writes,
			"caller-locked ForcelyRemove must remove and count the entry exactly once");
	}
}

int main()
{
	try
	{
		TestBucketCountsAndBorrowedIteration();
		TestBorrowedEquality();
		TestTryLockOwnership();
		TestGrowthRetainsCallerLock();
		TestConcurrentGrowthUpgrade();
		TestIfNotWritingDefersGrowth();
		TestIndependentStripeProgress();
		TestIndependentStructuralWrites();
		TestSetWritersWithGrowth();
		TestMapWritersAndSnapshots<ERehashPolicy::Always>();
		TestMapWritersAndSnapshots<ERehashPolicy::IfNotWriting>();
		std::cout << "ConcurrentContainerTests passed\n";
		return 0;
	}
	catch (const std::exception& error)
	{
		std::cerr << "ConcurrentContainerTests failed: " << error.what() << '\n';
		return 1;
	}
}
