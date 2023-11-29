#pragma once
#include <cassert>
#include <memory>
#include <functional>
#include <concepts>
#include <type_traits>
#include "Core/Defines.h"
#include "Containers/Vector.h"
#include "Containers/Set.h"
#include "Containers/ConcurrentSet.h"
#include "Containers/Pair.h"

namespace Sailor
{
	template<typename TKeyType, typename TValueType, const uint32_t concurrencyLevel = 8, typename TAllocator = Memory::DefaultGlobalAllocator>
	class TConcurrentMap final : public TConcurrentSet<TPair<TKeyType, TValueType>, concurrencyLevel, TAllocator>
	{
	public:

		using Super = Sailor::TConcurrentSet<TPair<TKeyType, TValueType>, concurrencyLevel, TAllocator>;
		using TElementType = Sailor::TPair<TKeyType, TValueType>;

		SAILOR_API TConcurrentMap(const uint32_t desiredNumBuckets = 16) : Super(desiredNumBuckets) {  }
		SAILOR_API TConcurrentMap(TConcurrentMap&&) = default;
		SAILOR_API TConcurrentMap(const TConcurrentMap&) = default;
		SAILOR_API TConcurrentMap& operator=(TConcurrentMap&&) noexcept = default;
		SAILOR_API TConcurrentMap& operator=(const TConcurrentMap&) = default;

		SAILOR_API TConcurrentMap(std::initializer_list<TElementType> initList)
		{
			for (const auto& el : initList)
			{
				Insert(el);
			}
		}

		SAILOR_API void Insert(const TKeyType& key, const TValueType& value) requires IsCopyConstructible<TValueType>
		{
			Super::Insert(TElementType(key, value));
		}

		SAILOR_API void Insert(const TKeyType& key, TValueType&& value) requires IsMoveConstructible<TValueType>
		{
			Super::Insert(TElementType(key, std::move(value)));
		}

		SAILOR_API bool ForcelyRemove(const TKeyType& key)
		{
			const size_t hash = Sailor::GetHash(key);
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

		SAILOR_API bool Remove(const TKeyType& key)
		{
			const size_t hash = Sailor::GetHash(key);
			auto& element = Super::m_buckets[hash % Super::m_buckets.Num()];

			if (element)
			{
				Super::Lock(hash);
				bool bRes = ForcelyRemove(key);
				Super::Unlock(hash);

				return bRes;
			}
			return false;
		}

		SAILOR_API TValueType& At_Lock(const TKeyType& key)
		{
			const auto& hash = Sailor::GetHash(key);
			Super::Lock(hash);

			auto& res = GetOrAdd(key).m_second;
			return res;
		}

		SAILOR_API TValueType& At_Lock(const TKeyType& key, TValueType defaultValue)
		{
			const auto& hash = Sailor::GetHash(key);
			Super::Lock(hash);

			auto& res = GetOrAdd(key, std::move(defaultValue)).m_second;
			return res;
		}

		SAILOR_API void Unlock(const TKeyType& key)
		{
			const auto& hash = Sailor::GetHash(key);
			Super::Unlock(hash);
		}

		SAILOR_API TValueType& operator[] (const TKeyType& key)
		{
			return GetOrAdd(key).m_second;
		}

		// TODO: rethink the approach for const operator []
		SAILOR_API const TValueType& operator[] (const TKeyType& key) const
		{
			TValueType const* out = nullptr;
			Find(key, out);
			return *out;
		}

		SAILOR_API bool Find(const TKeyType& key, TValueType*& out)
		{
			auto it = Find(key);
			if (it != Super::end())
			{
				out = &it->m_second;
				return true;
			}
			return false;
		}

		SAILOR_API bool Find(const TKeyType& key, TValueType const*& out) const
		{
			auto it = Find(key);
			out = &it->m_second;
			return it != Super::end();
		}

		SAILOR_API Super::TIterator Find(const TKeyType& key)
		{
			const auto& hash = Sailor::GetHash(key);
			auto& element = Super::m_buckets[hash % Super::m_buckets.Num()];

			if (element && element->LikelyContains(hash))
			{
				auto& container = element->GetContainer();
				size_t index = container.FindIf([&](const TElementType& el) { return el.First() == key; });
				if (index != -1)
				{
					typename Super::TElementContainer::TIterator it = Super::TElementContainer::TIterator(container.GetData() + index);
					return Super::TIterator(element.GetRawPtr(), it);
				}
			}

			return Super::end();
		}

		SAILOR_API Super::TConstIterator Find(const TKeyType& key) const
		{
			const auto& hash = Sailor::GetHash(key);
			auto& element = Super::m_buckets[hash % Super::m_buckets.Num()];

			if (element && element->LikelyContains(hash))
			{
				auto& container = element->GetContainer();
				size_t index = container.FindIf([&](const TElementType& el) { return el.First() == key; });
				if (index != -1)
				{
					typename Super::TElementContainer::TConstIterator it = Super::TElementContainer::TConstIterator(container.GetData() + index);
					return Super::TConstIterator(element.GetRawPtr(), it);
				}
			}

			return Super::end();
		}

		SAILOR_API bool ContainsKey(const TKeyType& key) const
		{
			return Find(key) != Super::end();
		}

		SAILOR_API bool ContainsValue(const TValueType& value) const
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

		SAILOR_API TVector<TKeyType> GetKeys() const
		{
			TVector<TKeyType> res;
			res.Reserve(Super::Num());

			for (const auto& pair : *this)
			{
				res.Add(pair.m_first);
			}

			return res;
		}

		SAILOR_API TVector<TValueType> GetValues() const
		{
			TVector<TValueType> res;
			res.Reserve(Super::Num());

			for (const auto& pair : *this)
			{
				res.Add(pair.m_second);
			}

			return res;
		}

	protected:

		SAILOR_API TElementType& GetOrAdd(const TKeyType& key)
		{
			const auto& hash = Sailor::GetHash(key);
			{
				const size_t index = hash % Super::m_buckets.Num();
				auto& element = Super::m_buckets[index];

				if (element)
				{
					auto& container = element->GetContainer();

					size_t index = container.FindIf([&](const TElementType& element) { return element.First() == key; });

					if (index != -1)
					{
						return *(container.GetData() + index);
					}
				}
			}

			// TODO: rethink the approach when default constructor is missed
			Super::Insert_Internal(TElementType(key, TValueType()), hash);

			const size_t index = hash % Super::m_buckets.Num();
			auto& element = Super::m_buckets[index];

			return *element->GetContainer().Last();
		}

		SAILOR_API TElementType& GetOrAdd(const TKeyType& key, TValueType defaultValue)
		{
			const auto& hash = Sailor::GetHash(key);
			{
				const size_t index = hash % Super::m_buckets.Num();
				auto& element = Super::m_buckets[index];

				if (element)
				{
					auto& container = element->GetContainer();

					size_t index = container.FindIf([&](const TElementType& element) { return element.First() == key; });

					if (index != -1)
					{
						return *(container.GetData() + index);
					}
				}
			}

			Super::Insert_Internal(TElementType(key, std::move(defaultValue)), hash);

			const size_t index = hash % Super::m_buckets.Num();
			auto& element = Super::m_buckets[index];

			return *element->GetContainer().Last();
		}
	};

	SAILOR_API void RunMapBenchmark();
}
