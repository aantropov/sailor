#pragma once
#include <cassert>
#include <memory>
#include <functional>
#include <concepts>
#include <type_traits>
#include "Core/Defines.h"
#include "Containers/Vector.h"
#include "Containers/Set.h"

namespace Sailor
{
	template<typename TKeyType, typename TValueType>
	class TPair final
	{
	public:

		SAILOR_API TPair() : m_first(), m_second() {}
		SAILOR_API TPair(const TPair&) = default;
		SAILOR_API TPair(TPair&&) = default;
		SAILOR_API ~TPair() = default;

		SAILOR_API TPair(TKeyType&& first, const TValueType& second) : m_first(std::move(first)), m_second(second) {}
		SAILOR_API TPair(const TKeyType& first, TValueType&& second) : m_first(first), m_second(std::move(second)) {}
		SAILOR_API TPair(const TKeyType& first, const TValueType& second) : m_first(first), m_second(second) {}
		SAILOR_API TPair(TKeyType&& first, TValueType&& second) : m_first(std::move(first)), m_second(std::move(second)) {}

		SAILOR_API TPair& operator=(const TPair&) = default;
		SAILOR_API TPair& operator=(TPair&&) = default;

		SAILOR_API __forceinline const TKeyType& First() const { return m_first; }
		SAILOR_API __forceinline const TValueType& Second() const { return m_second; }

		TKeyType m_first;
		TValueType m_second;
	};

	template<typename TKeyType, typename TValueType, typename TAllocator = Memory::MallocAllocator>
	class SAILOR_API TMap final: public TSet<TPair<TKeyType, TValueType>, TAllocator>
	{
	public:

		using Super = TSet<TPair<TKeyType, TValueType>, TAllocator>;
		using TElementType = TPair<TKeyType, TValueType>;

		TMap(const uint32_t desiredNumBuckets = 16) : Super(desiredNumBuckets) {  }
		TMap(TMap&&) = default;
		TMap(const TMap&) = default;
		TMap& operator=(TMap&&) noexcept = default;
		TMap& operator=(const TMap&) = default;

		TMap(std::initializer_list<TElementType> initList)
		{
			for (const auto& el : initList)
			{
				Insert(el);
			}
		}

		void Insert(const TKeyType& key, const TValueType& value)
		{
			Super::Insert(TPair(key, value));
		}

		void Insert(TKeyType&& key, TValueType&& value)
		{
			Super::Insert(TPair(std::move(key), std::move(value)));
		}

		bool Remove(const TKeyType& key)
		{
			const auto& hash = Sailor::GetHash(key);
			const size_t index = hash % Super::m_buckets.Num();
			auto& element = Super::m_buckets[index];

			if (element)
			{
				auto& container = element->GetContainer();
				const auto& i = container.FindIf([&](const TElementType& el) {return el.First() == key; });
				if (i != -1)
				{
					container.RemoveAtSwap(i);

					if (container.Num() == 0)
					{
						if (element.GetRawPtr() == Super::m_last)
						{
							Super::m_last = element->m_prev;
						}

						if (element->m_next)
						{
							element->m_next->m_prev = element->m_prev;
						}

						if (element->m_prev)
						{
							element->m_prev->m_next = element->m_next;
						}

						if (Super::m_last == element.GetRawPtr())
						{
							Super::m_last = Super::m_last->m_prev;
						}

						if (Super::m_first == element.GetRawPtr())
						{
							Super::m_first = Super::m_first->m_next;
						}

						element.Clear();
					}

					Super::m_num--;
					return true;
				}
				return false;
			}
			return false;
		}

		TValueType& operator[] (const TKeyType& key)
		{
			return GetOrAdd(key).m_second;
		}

		const TValueType& operator[] (const TKeyType& key) const
		{
			return GetOrAdd(key).Second();
		}

		bool ContainsKey(const TKeyType& key) const
		{
			const auto& hash = Sailor::GetHash(key);
			const size_t index = hash % Super::m_buckets.Num();
			auto& element = Super::m_buckets[index];

			if (element && element->LikelyContains(hash))
			{
				auto& container = element->GetContainer();
				const size_t i = container.FindIf([&](const TElementType& el) { return el.First() == key; });
				return i != -1;
			}

			return false;
		}

		bool ContainsValue(const TValueType& value) const
		{
			for (const auto& bucket : Super::m_buckets)
			{
				if (bucket && bucket->GetContainer().FindIf([&](const TElementType& el) { return el.Second() == value; }) != -1)
				{
					return true;
				}
			}
			return false;
		}

	protected:

		TElementType& GetOrAdd(const TKeyType& key)
		{
			const auto& hash = Sailor::GetHash(key);
			{
				const size_t index = hash % Super::m_buckets.Num();
				auto& element = Super::m_buckets[index];

				if (element)
				{
					auto& container = element->GetContainer();

					const size_t i = container.FindIf([&](const TElementType& element) { return element.First() == key; });
					if (i != -1)
					{
						return container[i];
					}
				}
			}

			// TODO: rethink the approach when default constructor is missed
			Insert(key, TValueType());

			const size_t index = hash % Super::m_buckets.Num();
			auto& element = Super::m_buckets[index];

			return element->GetContainer()[element->GetContainer().Num() - 1];
		}
	};

	SAILOR_API void RunMapBenchmark();
}

namespace std
{
	template<typename TKeyType, typename TValueType>
	struct std::hash<Sailor::TPair<TKeyType, TValueType>>
	{
		SAILOR_API std::size_t operator()(const Sailor::TPair<TKeyType, TValueType>& p) const
		{
			return Sailor::GetHash(p.First());
		}
	};
}