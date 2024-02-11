#include <unordered_map>
#include "Containers/Map.h"
#include "Containers/ConcurrentMap.h"
#include "Core/Utils.h"
#include <random>
#include "Tasks/Tasks.h"
#include "Tasks/Scheduler.h"

using namespace Sailor::Memory;
using Timer = Sailor::Utils::Timer;

#include <limits>
#include <type_traits>
#include <cstdlib>
#include <mutex>
#include <vector>
#include <atomic>
#include <thread>

namespace DaniilPavlenko {
	struct spinlock {
	private:
		std::atomic<bool> Flag{ 0 };

	public:
		void Lock() {
			while (Flag.exchange(1, std::memory_order_acquire)) {
				_mm_pause();
			}
		}

		void Unlock() {
			Flag.store(0, std::memory_order_release);
		}
	};

	template <typename key_type, typename value_type>
	struct ConcurrentMap {
	private:
		using hash_type = size_t;
		using index = size_t;

		constexpr static hash_type EmptyHash = std::numeric_limits<hash_type>::max();
		constexpr static hash_type DeletedHash = std::numeric_limits<hash_type>::max() - 1;
		constexpr static hash_type RelocatedHash = std::numeric_limits<hash_type>::max() - 2;
		constexpr static hash_type LastValidHash = std::numeric_limits<hash_type>::max() - 3;
		constexpr static index MinCapacity = 32;
		constexpr static float MaxLoadFactor = 0.7f;

		__declspec(align(64)) struct map_element {
			hash_type Hash;
			spinlock AccessLock;
			key_type Key;
			value_type Value;
		};

		map_element* Data = nullptr;
		map_element* OldData = nullptr;
		index Capacity = MinCapacity;
		std::atomic<index> Size = 0;
		std::atomic<index> Deleted = 0;
		index MaxSize = static_cast<index>(MinCapacity * MaxLoadFactor);
		std::mutex RelocateMutex;

	public:
		__forceinline ConcurrentMap() {
			Data = (map_element*)_aligned_malloc(MinCapacity * sizeof(map_element), 64);
			for (map_element* MapElem = Data; MapElem != Data + MinCapacity; ++MapElem) {
				MapElem->Hash = EmptyHash;
				MapElem->AccessLock.Unlock();
			}
		}

		__forceinline ~ConcurrentMap() {
			for (map_element* Elem = Data; Elem != Data + Capacity; ++Elem) {
				if (Elem->Hash <= LastValidHash) {
					Elem->Value.~value_type();
					Elem->Key.~key_type();
				}
			}
			_aligned_free(Data);
			Data = nullptr;
			Capacity = 0;
			MaxSize = 0;
			Size.store(0, std::memory_order::memory_order_relaxed);
			Deleted.store(0, std::memory_order::memory_order_relaxed);
		}

		value_type operator[](const key_type& Key) {
			value_type Result;
			hash_type Hash = GetHash(Key);
			while (true) {
				index HashMask = (1 << LogOfTwoCeil(Capacity)) - 1;
				index Iteration = 0;
				map_element* CurrentElem;
				bool Relocated = false;
				for (CurrentElem = Data + (Hash & HashMask);;
					CurrentElem = Data + ((Hash + TriangleNumber(++Iteration)) & HashMask)) {
					CurrentElem->AccessLock.Lock();
					if (CurrentElem->Hash == RelocatedHash) [[unlikely]] {
						CurrentElem->AccessLock.Unlock();
						Relocated = true;
						break;
						}
						if (CurrentElem->Hash == EmptyHash) {
							break;
						}
						if (CurrentElem->Hash == Hash && (Key == CurrentElem->Key)) {
							Result = CurrentElem->Value;
							CurrentElem->AccessLock.Unlock();
							return Result;
						}
						CurrentElem->AccessLock.Unlock();
				}
				if (Relocated) [[unlikely]] {
					continue;
					}
				index PreLockSize = Size.load(std::memory_order_relaxed);
				index PreLockDeleted = Deleted.load(std::memory_order_relaxed);
				const bool RelocationNeeded = (PreLockSize + PreLockDeleted + 1) > MaxSize;
				if (RelocationNeeded) [[unlikely]] {
					CurrentElem->AccessLock.Unlock();
					RelocateMutex.lock();
					index PostLockSize = Size.load(std::memory_order_relaxed);
					index PostLockDeleted = Deleted.load(std::memory_order_relaxed);
					const bool RelocationStillNeeded = (PostLockSize + PostLockDeleted + 1) > MaxSize;
					if (RelocationStillNeeded) {
						map_element* OldData = Data;
						index OldCapacity = Capacity;
						LockAll();
						constexpr index MAGIC_NUMBER = 100;
						Relocate(PostLockSize + MAGIC_NUMBER);
						OldDataUnlockAll(OldData, OldCapacity);
					}
					RelocateMutex.unlock();
					continue;
					}
				else {
					Size.fetch_add(1, std::memory_order::memory_order_relaxed);
					CurrentElem->Hash = Hash;
					new (&(CurrentElem->Key)) key_type(Key);
					new (&(CurrentElem->Value)) value_type();
					Result = CurrentElem->Value;
					CurrentElem->AccessLock.Unlock();
					return Result;
				}
			}
		}

		value_type& At_Lock(const key_type& Key) {
			hash_type Hash = GetHash(Key);
			while (true) {
				index HashMask = (1 << LogOfTwoCeil(Capacity)) - 1;
				index Iteration = 0;
				map_element* CurrentElem;
				bool Relocated = false;
				for (CurrentElem = Data + (Hash & HashMask);;
					CurrentElem = Data + ((Hash + TriangleNumber(++Iteration)) & HashMask)) {
					CurrentElem->AccessLock.Lock();
					if (CurrentElem->Hash == RelocatedHash) [[unlikely]] {
						CurrentElem->AccessLock.Unlock();
						Relocated = true;
						break;
						}
						if (CurrentElem->Hash == EmptyHash) {
							break;
						}
						if (CurrentElem->Hash == Hash && (Key == CurrentElem->Key)) {
							return CurrentElem->Value;
						}
						CurrentElem->AccessLock.Unlock();
				}
				if (Relocated) [[unlikely]] {
					continue;
					}
				index PreLockSize = Size.load(std::memory_order_relaxed);
				index PreLockDeleted = Deleted.load(std::memory_order_relaxed);
				const bool RelocationNeeded = (PreLockSize + PreLockDeleted + 1) > MaxSize;
				if (RelocationNeeded) [[unlikely]] {
					CurrentElem->AccessLock.Unlock();
					RelocateMutex.lock();
					index PostLockSize = Size.load(std::memory_order_relaxed);
					index PostLockDeleted = Deleted.load(std::memory_order_relaxed);
					const bool RelocationStillNeeded = (PostLockSize + PostLockDeleted + 1) > MaxSize;
					if (RelocationStillNeeded) {
						map_element* OldData = Data;
						index OldCapacity = Capacity;
						LockAll();
						constexpr index MAGIC_NUMBER = 100;
						Relocate(PostLockSize + MAGIC_NUMBER);
						OldDataUnlockAll(OldData, OldCapacity);
					}
					RelocateMutex.unlock();
					continue;
					}
				else {
					Size.fetch_add(1, std::memory_order::memory_order_relaxed);
					CurrentElem->Hash = Hash;
					new (&(CurrentElem->Key)) key_type(Key);
					new (&(CurrentElem->Value)) value_type();
					return CurrentElem->Value;
				}
			}
		}

		__forceinline void Unlock(const key_type& Key) {
			const hash_type Hash = GetHash(Key);
			const index HashMask = (1 << LogOfTwoCeil(Capacity)) - 1;
			index Iteration = 0;
			map_element* CurrentElem;
			for (CurrentElem = Data + (Hash & HashMask); CurrentElem->Hash != EmptyHash;
				CurrentElem = Data + ((Hash + TriangleNumber(++Iteration)) & HashMask)) {
				if (CurrentElem->Hash == Hash && (Key == CurrentElem->Key)) {
					CurrentElem->AccessLock.Unlock();
					return;
				}
			}
		}

		__forceinline void Remove(const key_type& Key) {
			const hash_type Hash = GetHash(Key);
			while (true) {
				bool Relocated = false;
				const index HashMask = (1 << LogOfTwoCeil(Capacity)) - 1;
				index Iteration = 0;
				map_element* CurrentElem;
				for (CurrentElem = Data + (Hash & HashMask);;
					CurrentElem = Data + ((Hash + TriangleNumber(++Iteration)) & HashMask)) {
					CurrentElem->AccessLock.Lock();
					if (CurrentElem->Hash == RelocatedHash) [[unlikely]] {
						CurrentElem->AccessLock.Unlock();
						Relocated = true;
						break;
						}
						if (CurrentElem->Hash == EmptyHash) {
							CurrentElem->AccessLock.Unlock();
							break;
						}
						if (CurrentElem->Hash == Hash && (Key == CurrentElem->Key)) {
							CurrentElem->Value.~value_type();
							CurrentElem->Key.~key_type();
							CurrentElem->Hash = DeletedHash;
							Deleted.fetch_add(1, std::memory_order_relaxed);
							Size.fetch_sub(1, std::memory_order_relaxed);
							CurrentElem->AccessLock.Unlock();
							return;
						}
						CurrentElem->AccessLock.Unlock();
				}
				if (Relocated) {
					continue;
				}
				return;
			}
		}

		[[nodiscard]] __forceinline index GetSize() const {
			return Size.load(std::memory_order_relaxed);
		}

	private:
		void Relocate(index DesiredSize) {
			const index NewCapacity = 1ull << index(LogOfTwoCeil(DesiredSize) + 1);
			auto* const NewData = (map_element*)_aligned_malloc(NewCapacity * sizeof(map_element), alignof(map_element));
			for (map_element* MapElem = NewData; MapElem != NewData + NewCapacity; ++MapElem) {
				MapElem->Hash = EmptyHash;
				MapElem->AccessLock.Unlock();
			}
			for (map_element* MapElem = Data; MapElem != Data + Capacity; ++MapElem) {
				if (MapElem->Hash <= LastValidHash) {
					const index HashMask = (1 << LogOfTwoCeil(NewCapacity)) - 1;
					index Iteration = 0;
					map_element* CurrentElem;
					const hash_type Hash = MapElem->Hash;
					for (CurrentElem = NewData + (Hash & HashMask); CurrentElem->Hash != EmptyHash;
						CurrentElem = NewData + ((Hash + TriangleNumber(++Iteration)) & HashMask)) {
					}
					new (&(CurrentElem->Value)) value_type(std::move(MapElem->Value));
					new (&(CurrentElem->Key)) key_type(std::move(MapElem->Key));
					CurrentElem->Hash = MapElem->Hash;
					MapElem->Value.~value_type();
					MapElem->Key.~key_type();
				}
			}
			for (map_element* MapElem = Data; MapElem != Data + Capacity; ++MapElem) {
				MapElem->Hash = RelocatedHash;
			}
			if (OldData) {
				_aligned_free(OldData);
			}
			OldData = Data;
			Data = NewData;
			Deleted.store(0, std::memory_order_relaxed);
			Capacity = NewCapacity;
			MaxSize = (index)(MaxLoadFactor * Capacity);
		}

		__forceinline static size_t GetHash(const key_type& Key) {
			size_t Hash = std::hash<key_type>{}(Key);
			return Hash - (Hash > LastValidHash) * 3;
		}

		__forceinline static index TriangleNumber(const index InIteration) {
			return (InIteration * (InIteration + 1)) >> 1;
		}

		__forceinline void LockAll() {
			for (map_element* Iter = Data; Iter != Data + Capacity; ++Iter) {
				Iter->AccessLock.Lock();
			}
		}

		__forceinline void OldDataUnlockAll(map_element* InData, index InCapacity) {
			for (map_element* Iter = InData; Iter != InData + InCapacity; ++Iter) {
				Iter->AccessLock.Unlock();
			}
		}

		__forceinline static index LogOfTwoCeil(index Value) {
			unsigned long Index;
			if (_BitScanReverse64(&Index, Value)) {
				index result = Index;
				if ((Value & ~((index)1 << Index)) > 0) {
					++result;
				}
				return result;
			}
			else {
				return 0ull;
			}
		}
	};
}	 // namespace DaniilPavlenko

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

			const size_t count = 150000;

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

			volatile float misses = 0;

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
			res.m_misses = misses / (float)count;

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

			const size_t count = 150000;

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

			volatile float misses = 0;

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
			res.m_misses = misses / (float)count;

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
	class TestCase_ConcurrrentMapPerfromance
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

			const size_t count = 300000;

			TContainer container;
			Timer tMap;

			tMap.Start();
			for (size_t i = 0; i < count; i++)
			{
				container.At_Lock(i) = i * 3;
				container.Unlock(i);
			}
			tMap.Stop();

			res.m_operator = tMap.ResultMs();

			tMap.Clear();
			std::random_device rd;
			std::mt19937 g(rd());

			volatile float misses = 0;

			g.seed(0);
			tMap.Start();
			/*for (size_t i = 0; i < count; i++)
			{
				const size_t value = i % 2 ? g() : g() % count;
				if (!container.ContainsKey(value))
				{
					misses++;
				}
			}*/
			tMap.Stop();

			res.m_containsKey = tMap.ResultMs();
			res.m_misses = misses / (float)count;

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

				container.At_Lock(i) = i * 3;
				container.Unlock(i);
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

	void Sailor::RunMapBenchmark()
	{
		using TDeepData = TDeepData<1024>;
		using TPlainData = TPlainData<1024>;

		printf("\nStarting Map benchmark...\n");

		TVector<Result> res;

		//res.Add(TestCase_MapPerfromance<TPlainData, std::unordered_map<size_t, TPlainData>>::RunTests());
		//res.Add(TestCase_MapPerfromance<TPlainData, Sailor::TMap<size_t, TPlainData>>::RunTests());

		res.Add(TestCase_ConcurrrentMapPerfromance<TPlainData, Sailor::TConcurrentMap<size_t, TPlainData>>::RunTests());
		res.Add(TestCase_ConcurrrentMapPerfromance<TDeepData, Sailor::TConcurrentMap<size_t, TDeepData>>::RunTests());

		res.Add(TestCase_ConcurrrentMapPerfromance<TPlainData, DaniilPavlenko::ConcurrentMap<size_t, TPlainData>>::RunTests());
		res.Add(TestCase_ConcurrrentMapPerfromance<TDeepData, DaniilPavlenko::ConcurrentMap<size_t, TDeepData>>::RunTests());


		//res.Add(TestCase_MapPerfromance<TDeepData, std::unordered_map<size_t, TDeepData>>::RunTests());
		//res.Add(TestCase_MapPerfromance<TDeepData, Sailor::TMap<size_t, TDeepData>>::RunTests());
		//res.Add(TestCase_ConcurrrentMapPerfromance<TDeepData, Sailor::TConcurrentMap<size_t, TDeepData>>::RunTests());

		for (auto& r : res)
		{
			r.PrintLog();
		}

		printf("\n\n");
	}
}
