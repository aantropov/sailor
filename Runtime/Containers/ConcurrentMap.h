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
	template<typename TKeyType, typename TValueType, const uint32_t concurrencyLevel = 32, const ERehashPolicy policy = ERehashPolicy::Always, typename TAllocator = Memory::DefaultGlobalAllocator>
	class TConcurrentMap final : public TConcurrentSet<TPair<TKeyType, TValueType>, concurrencyLevel, TAllocator>
	{
	public:

		using Super = Sailor::TConcurrentSet<TPair<TKeyType, TValueType>, concurrencyLevel, TAllocator>;
		using TElementType = Sailor::TPair<TKeyType, TValueType>;

		SAILOR_API TConcurrentMap(const uint32_t desiredNumBuckets = 24) : Super(desiredNumBuckets, policy) {  }
		TConcurrentMap(const TConcurrentMap&) = default;
		TConcurrentMap& operator=(const TConcurrentMap& rhs) = default;

		TConcurrentMap(TConcurrentMap&&) noexcept = default;
		TConcurrentMap& operator=(TConcurrentMap&&) noexcept = default;

		SAILOR_API TConcurrentMap(std::initializer_list<TElementType> initList) : TConcurrentMap()
		{
			for (const auto& el : initList)
			{
				Insert(el.First(), el.Second());
			}
		}

		SAILOR_API void Insert(const TKeyType& key, const TValueType& value) requires IsCopyConstructible<TValueType>
		{
			const size_t hash = Sailor::GetHash(key);
			Super::Lock(hash);
			GetOrAdd(key, value);
			Super::Unlock(hash);
		}

		SAILOR_API void Insert(const TKeyType& key, TValueType&& value) requires IsMoveConstructible<TValueType>
		{
			const size_t hash = Sailor::GetHash(key);
			Super::Lock(hash);
			GetOrAdd(key, std::move(value));
			Super::Unlock(hash);
		}

		// Caller owns the key's stripe or LockAll; descriptor-cache collection uses this path.
		SAILOR_API bool ForcelyRemove(const TKeyType& key)
		{
			const size_t hash = Sailor::GetHash(key);
			auto& element = Super::m_buckets[hash % Super::m_buckets.Num()];

			if (element)
			{
				auto& container = element->GetContainer();
				const size_t removed = container.RemoveAll([&](const TElementType& el) { return el.First() == key; });
				if (removed)
				{
					if (container.Num() == 0)
					{
						element.Clear();
					}

					Super::m_num -= static_cast<uint32_t>(removed);
					return true;
				}

				return false;
			}
			return false;
		}

		SAILOR_API bool Remove(const TKeyType& key)
		{
			const size_t hash = Sailor::GetHash(key);
			Super::Lock(hash);
			const bool removed = ForcelyRemove(key);
			Super::Unlock(hash);
			return removed;
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

		// Lookup/insertion is locked; the returned reference is borrowed. Never prevents
		// rehash, not erase or concurrent value writes: callers still synchronize its use.
		template<ERehashPolicy P = policy>
		typename std::enable_if<P == ERehashPolicy::Never, TValueType&>::type operator[] (const TKeyType& key)
		{
			const size_t hash = Sailor::GetHash(key);
			Super::Lock(hash);
			auto& value = GetOrAdd(key).m_second;
			Super::Unlock(hash);
			return value;
		}

		template<ERehashPolicy P = policy>
		typename std::enable_if<P == ERehashPolicy::Never, const TValueType&>::type operator[] (const TKeyType& key) const
		{
			const size_t hash = Sailor::GetHash(key);
			Super::Lock(hash);
			TValueType const* out = nullptr;
			Find(key, out);
			Super::Unlock(hash);
			return *out;
		}

		// Copy under the stripe lock so rehash cannot invalidate the source mid-copy.
		template<ERehashPolicy P = policy>
		typename std::enable_if<P != ERehashPolicy::Never, const TValueType>::type operator[] (const TKeyType& key) const
		{
			const size_t hash = Sailor::GetHash(key);
			Super::Lock(hash);
			TValueType const* out = nullptr;
			Find(key, out);
			TValueType value = *out;
			Super::Unlock(hash);
			return value;
		}

		// Copy under the stripe, then replace the caller's value after unlocking.
		// A missing key leaves out unchanged.
		SAILOR_API bool TryGet(const TKeyType& key, TValueType& out) const
			requires IsCopyConstructible<TValueType> && (IsMoveAssignable<TValueType> || IsCopyAssignable<TValueType>)
		{
			const size_t hash = Sailor::GetHash(key);
			Super::Lock(hash);
			const auto it = Find(key);
			if (it == Super::end())
			{
				Super::Unlock(hash);
				return false;
			}
			TValueType value = it->m_second;
			Super::Unlock(hash);
			if constexpr (IsMoveAssignable<TValueType>)
			{
				out = std::move(value);
			}
			else
			{
				out = value;
			}
			return true;
		}

		// Borrowed lookup: caller excludes structural writers for the whole use of the result.
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
			if (it != Super::end())
			{
				out = &it->m_second;
				return true;
			}
			return false;
		}

		SAILOR_API Super::TIterator Find(const TKeyType& key)
		{
			const auto& hash = Sailor::GetHash(key);
			const size_t index = hash % Super::m_buckets.Num();
			auto& element = Super::m_buckets[index];

			if (element && element->LikelyContains(hash))
			{
				auto& container = element->GetContainer();
				typename Super::TElementContainer::TIterator it = container.FindIf([&](const TElementType& el) { return el.First() == key; });
				if (it != container.end())
				{
					return typename Super::TIterator(this, index, it);
				}
			}

			return Super::end();
		}

		SAILOR_API Super::TConstIterator Find(const TKeyType& key) const
		{
			const auto& hash = Sailor::GetHash(key);
			const size_t index = hash % Super::m_buckets.Num();
			auto& element = Super::m_buckets[index];

			if (element && element->LikelyContains(hash))
			{
				auto& container = element->GetContainer();
				typename Super::TElementContainer::TConstIterator it = container.FindIf([&](const TElementType& el) { return el.First() == key; });
				if (it != container.end())
				{
					return typename Super::TConstIterator(this, index, it);
				}
			}

			return Super::end();
		}

		SAILOR_API bool ContainsKey(const TKeyType& key) const
		{
			const size_t hash = Sailor::GetHash(key);
			Super::Lock(hash);
			const bool found = Find(key) != Super::end();
			Super::Unlock(hash);
			return found;
		}

		// Like iteration/GetKeys/GetValues, this whole-table query needs external exclusion.
		SAILOR_API bool ContainsValue(const TValueType& value) const
		{
			for (const auto& bucket : Super::m_buckets)
			{
				if (bucket && bucket->GetContainer().ContainsIf([&](const TElementType& el) { return el.Second() == value; }))
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
			return GetOrAdd(key, TValueType());
		}

		SAILOR_API TElementType& GetOrAdd(const TKeyType& key, TValueType defaultValue)
		{
			const auto& hash = Sailor::GetHash(key);
			auto existing = Find(key);
			if (existing != Super::end())
			{
				return *existing;
			}

			Super::RehashForInsert(hash);
			// Always may drop the stripe while acquiring all locks; another writer may insert this key.
			existing = Find(key);
			if (existing != Super::end())
			{
				return *existing;
			}

			Super::Insert_Internal(TElementType(key, std::move(defaultValue)), hash);

			const size_t index = hash % Super::m_buckets.Num();
			auto& element = Super::m_buckets[index];

			return *element->GetContainer().Last();
		}

	};

	SAILOR_API void RunMapBenchmark();
}
