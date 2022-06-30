#pragma once
#include <cassert>
#include <memory>
#include <functional>
#include <concepts>
#include <type_traits>
#include "Core/Defines.h"
#include "Containers/Vector.h"
#include "Containers/Set.h"
#include "Containers/Pair.h"

namespace Sailor
{
	template<typename TKeyType, typename TValueType, typename TAllocator = Memory::DefaultGlobalAllocator>
	class TMap final: public TSet<TPair<TKeyType, TValueType>, TAllocator>
	{
	public:

		using Super = Sailor::TSet<TPair<TKeyType, TValueType>, TAllocator>;
		using TElementType = Sailor::TPair<TKeyType, TValueType>;

		TMap(const uint32_t desiredNumBuckets = 16) : Super(desiredNumBuckets) {  }
		TMap(std::initializer_list<TElementType> initList)
		{
			for (const auto& el : initList)
			{
				Insert(el);
			}
		}

		void Insert(const TKeyType& key, const TValueType& value) requires IsCopyConstructible<TValueType>
		{
			Super::Insert(TElementType(key, value));
		}

		void Insert(const TKeyType& key, TValueType&& value) requires IsMoveConstructible<TValueType>
		{
			Super::Insert(TElementType(key, std::move(value)));
		}

		bool Remove(const TKeyType& key)
		{
			const auto& hash = Sailor::GetHash(key);
			auto& element = Super::m_buckets[hash % Super::m_buckets.Num()];

			if (element)
			{
				auto& container = element->GetContainer();
				if (container.RemoveAll([&](const TElementType& el) { return el.First() == key; }))
				{
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

		// TODO: rethink the approach for const operator []
		const TValueType& operator[] (const TKeyType& key) const
		{
			TValueType const* out = nullptr;
			Find(key, out);
			return *out;
		}

		bool Find(const TKeyType& key, TValueType*& out)
		{
			auto it = Find(key);
			if (it != Super::end())
			{
				out = &it->m_second;
				return true;
			}
			return false;
		}

		bool Find(const TKeyType& key, TValueType const*& out) const
		{
			auto it = Find(key);
			out = &it->m_second;
			return it != Super::end();
		}

		Super::TIterator Find(const TKeyType& key)
		{
			const auto& hash = Sailor::GetHash(key);
			auto& element = Super::m_buckets[hash % Super::m_buckets.Num()];

			if (element && element->LikelyContains(hash))
			{
				auto& container = element->GetContainer();

				typename Super::TElementContainer::TIterator it = container.FindIf([&](const TElementType& el) { return el.First() == key; });
				if (it != container.end())
				{
					return Super::TIterator(element.GetRawPtr(), it);
				}
			}

			return Super::end();
		}

		Super::TConstIterator Find(const TKeyType& key) const
		{
			const auto& hash = Sailor::GetHash(key);
			auto& element = Super::m_buckets[hash % Super::m_buckets.Num()];

			if (element && element->LikelyContains(hash))
			{
				auto& container = element->GetContainer();
				typename Super::TElementContainer::TConstIterator it = container.FindIf([&](const TElementType& el) { return el.First() == key; });
				if (it != container.end())
				{
					return Super::TConstIterator(element.GetRawPtr(), it);
				}
			}

			return Super::end();
		}

		bool ContainsKey(const TKeyType& key) const
		{
			return Find(key) != Super::end();
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

					TElementType* out;
					if (container.FindIf(out, [&](const TElementType& element) { return element.First() == key; }))
					{
						return *out;
					}
				}
			}

			// TODO: rethink the approach when default constructor is missed
			Insert(key, TValueType());

			const size_t index = hash % Super::m_buckets.Num();
			auto& element = Super::m_buckets[index];

			return *element->GetContainer().Last();
		}
	};

	SAILOR_API void RunMapBenchmark();
}
