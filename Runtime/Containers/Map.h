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
	class SAILOR_API TPair final
	{
	public:
		TPair() = default;
		TPair(const TPair&) = default;
		TPair(TPair&&) = default;
		~TPair() = default;

		TPair(const TKeyType& key, const TValueType& value) : m_key(key), m_value(value) {}
		TPair(TKeyType&& key, TValueType&& value) : m_key(std::move(key)), m_value(std::move(value)) {}

		TKeyType& First() { return m_key; }
		const TKeyType& First() const { return m_key; }

		TValueType& Second() { return m_key; }
		const TValueType& Second() const { return m_value; }

	private:

		TKeyType m_key;
		TValueType m_value;
	};

	template<typename TKeyType, typename TValueType, typename TAllocator = Memory::MallocAllocator>
	class SAILOR_API TMap final: public TSet<TPair<TKeyType, TValueType>, TAllocator>
	{
	public:

		using Super = TSet<TPair<TKeyType, TValueType>, TAllocator>;
		using TElementType = TPair<TKeyType, TValueType>;

		TMap(const uint32_t desiredNumBuckets = 8) : Super(desiredNumBuckets) {  }
		TMap(TMap&&) = default;
		TMap(const TMap&) = default;
		TMap& operator=(TMap&&) = default;
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
			return GetOrAdd(key).Second();
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

			if (element)
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

			// TODO: rethink the approach with missed default constructor
			Insert(key, TValueType());
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