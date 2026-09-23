#pragma once
#include <cassert>
#include <memory>
#include <functional>
#include <concepts>
#include <type_traits>
#include "Core/Defines.h"
#include "Memory/LockFreeHeapAllocator.h"
#include "Memory/UniquePtr.hpp"
#include "Memory/Memory.h"
#include "Containers/Pair.h"
#include "Containers/List.h"
#include "Containers/Vector.h"
#include "Containers/Hash.h"
#include "Core/LogMacros.h"
#include "Core/SpinLock.h"

namespace Sailor
{
	enum class ERehashPolicy
	{
		Never = 0,
		IfNotWriting,
		Always
	};

	template<typename TElementType, const uint32_t concurrencyLevel = 8, typename TAllocator = Memory::DefaultGlobalAllocator>
	class TConcurrentSet
	{
		static_assert(concurrencyLevel > 0);

	public:

		using TElementContainer = TList<TElementType, TAllocator>;

		class TEntry
		{
		public:

			TEntry(size_t hashCode) : m_hashCode(hashCode) {}
			TEntry(TEntry&&) = default;
			TEntry(const TEntry&) = default;

			TEntry& operator=(TEntry&&) = default;
			TEntry& operator=(const TEntry&) = default;

			__forceinline explicit operator bool() const { return m_elements.Num() > 0; }
			virtual ~TEntry() = default;

			__forceinline TElementContainer& GetContainer() { return m_elements; }
			__forceinline const TElementContainer& GetContainer() const { return m_elements; }

			__forceinline bool operator==(const TEntry& Other) const
			{
				return this->m_hashCode == Other.m_hashCode;
			}
			__forceinline bool operator!=(const TEntry& Other) const
			{
				return !(*this == Other);
			}

			__forceinline size_t GetHash() const { return m_hashCode; }
			__forceinline size_t LikelyContains(size_t hashCode) const { return (m_bloom & hashCode) == hashCode; }

			size_t m_bloom = 0;
			size_t m_hashCode = 0;
			TElementContainer m_elements;

			friend class TConcurrentSet;
		};

		template<typename TDataType, typename TElementIterator>
		class TBaseIterator
		{
		public:

			using TSetType = std::conditional_t<std::is_const_v<TDataType>, const TConcurrentSet, TConcurrentSet>;
			using iterator_category = std::bidirectional_iterator_tag;
			using value_type = std::remove_const_t<TDataType>;
			using difference_type = int64_t;
			using pointer = TDataType*;
			using reference = TDataType&;

			TBaseIterator() = default;

			TBaseIterator(const TBaseIterator&) = default;
			TBaseIterator(TBaseIterator&&) = default;

			~TBaseIterator() = default;

			TBaseIterator(TSetType* owner, size_t bucketIndex, TElementIterator it = {}) :
				m_owner(owner), m_bucketIndex(bucketIndex), m_it(std::move(it))
			{
				if (m_owner && m_it == TElementIterator{})
				{
					AdvanceToBucket();
				}
			}

			operator TBaseIterator<const TElementType, typename TElementContainer::TConstIterator>() const
				requires (!std::is_const_v<TDataType>)
			{
				return { m_owner, m_bucketIndex, m_it };
			}

			TBaseIterator& operator=(const TBaseIterator& rhs) = default;
			TBaseIterator& operator=(TBaseIterator&& rhs) = default;

			bool operator==(const TBaseIterator& rhs) const { return m_owner == rhs.m_owner && m_bucketIndex == rhs.m_bucketIndex && m_it == rhs.m_it; }
			bool operator!=(const TBaseIterator& rhs) const { return !(*this == rhs); }

			pointer operator->() { return &*m_it; }
			pointer operator->() const { return &*m_it; }

			reference operator*() { return *m_it; }
			reference operator*() const { return *m_it; }

			TBaseIterator& operator++()
			{
				++m_it;

				if (m_it == TElementIterator{})
				{
					++m_bucketIndex;
					AdvanceToBucket();
				}

				return *this;
			}

			TBaseIterator& operator--()
			{
				if (m_bucketIndex == m_owner->m_buckets.Num() ||
					m_it == m_owner->m_buckets[m_bucketIndex]->GetContainer().begin())
				{
					while (m_bucketIndex > 0)
					{
						auto& bucket = m_owner->m_buckets[--m_bucketIndex];
						if (bucket)
						{
							m_it = bucket->GetContainer().Last();
							break;
						}
					}
				}
				else
				{
					--m_it;
				}

				return *this;
			}

			TBaseIterator operator++(int)
			{
				auto previous = *this;
				++(*this);
				return previous;
			}

			TBaseIterator operator--(int)
			{
				auto previous = *this;
				--(*this);
				return previous;
			}

		protected:

			void AdvanceToBucket()
			{
				while (m_bucketIndex < m_owner->m_buckets.Num())
				{
					auto& bucket = m_owner->m_buckets[m_bucketIndex];
					if (bucket)
					{
						m_it = bucket->GetContainer().begin();
						return;
					}
					++m_bucketIndex;
				}
				m_it = {};
			}

			TSetType* m_owner = nullptr;
			size_t m_bucketIndex = 0;
			TElementIterator m_it{};

			friend class TEntry;
		};

		using TIterator = TBaseIterator<TElementType, typename TElementContainer::TIterator>;
		using TConstIterator = TBaseIterator<const TElementType, typename TElementContainer::TConstIterator>;
		using TConcurrentEntryPtr = TUniquePtr<TEntry>;
		using TBucketContainer = TVector<TConcurrentEntryPtr, TAllocator>;

		SAILOR_API TConcurrentSet(const uint32_t desiredNumBuckets = 16, ERehashPolicy policy = ERehashPolicy::Never) : m_rehashPolicy(policy)
		{
			m_buckets.Resize(RoundBucketCount(desiredNumBuckets));
		}

		TConcurrentSet(TConcurrentSet&&) = default;
		
		SAILOR_API TConcurrentSet(const TConcurrentSet& rhs) requires IsCopyConstructible<TElementType> : TConcurrentSet((uint32_t)rhs.m_buckets.Num(), rhs.m_rehashPolicy)
		{
			for (const auto& el : rhs)
			{
				Insert(el);
			}
		}

		TConcurrentSet& operator=(TConcurrentSet&&) = default;

		SAILOR_API TConcurrentSet& operator=(const TConcurrentSet& rhs) requires IsCopyConstructible<TElementType>
		{
			if (this == &rhs)
			{
				return *this;
			}
			m_rehashPolicy = rhs.m_rehashPolicy;

			Clear((uint32_t)rhs.m_buckets.Num());
			for (const auto& el : rhs)
			{
				Insert(el);
			}

			return *this;
		}

		SAILOR_API TConcurrentSet(std::initializer_list<TElementType> initList) : TConcurrentSet()
		{
			for (const auto& el : initList)
			{
				Insert(el);
			}
		}

		// TODO: Rethink the approach of base class for iterators
		SAILOR_API TConcurrentSet(const TVectorIterator<TElementType>& begin, const TVectorIterator<TElementType>& end) : TConcurrentSet()
		{
			TVectorIterator<TElementType> it = begin;
			while (it != end)
			{
				Insert(*it);
				it++;
			}
		}

		__forceinline bool IsEmpty() const { return m_num == 0; }
		__forceinline size_t Num() const { return m_num.load(); }

		bool Contains(const TElementType& inElement) const
		{
			const auto& hash = Sailor::GetHash(inElement);
			Lock(hash);
			const bool found = Contains_Internal(inElement, hash);
			Unlock(hash);
			return found;
		}

		// Caller owns the stripe, or all stripes. This entry point does not rehash.
		void ForcelyInsert(TElementType inElement)
		{
			const auto& hash = Sailor::GetHash(inElement);
			Insert_Internal(std::move(inElement), hash);
		}

		void Insert(TElementType inElement)
		{
			const auto& hash = Sailor::GetHash(inElement);
			Lock(hash);
			RehashForInsert(hash);
			Insert_Internal(std::move(inElement), hash);
			Unlock(hash);
		}

		bool Remove(const TElementType& inElement)
		{
			const auto& hash = Sailor::GetHash(inElement);
			Lock(hash);
			const size_t index = hash % m_buckets.Num();
			auto& element = m_buckets[index];
			const bool removed = element && element->GetContainer().RemoveFirst(inElement);
			if (removed)
			{
				if (element->GetContainer().IsEmpty())
				{
					element.Clear();
				}
				m_num--;
			}
			Unlock(hash);
			return removed;
		}

		void Clear(uint32_t desiredBucketsNum = 8)
		{
			LockAll();

			m_num = 0;
			m_buckets.Clear();

			m_buckets.Resize(RoundBucketCount(desiredBucketsNum));

			UnlockAll();
		}

		// Borrowed iterators: keep writers excluded (for example with LockAll) during traversal.
		TIterator begin() { return TIterator(this, 0); }
		TIterator end() { return TIterator(this, m_buckets.Num()); }

		TConstIterator begin() const { return TConstIterator(this, 0); }
		TConstIterator end() const { return TConstIterator(this, m_buckets.Num()); }

		// Like iteration, comparison borrows both containers; the caller excludes their writers.
		bool operator==(const TConcurrentSet& rhs) const
		{
			if (rhs.Num() != this->Num())
			{
				return false;
			}

			for (auto& el : rhs)
			{
				if (!Contains_Internal(el, Sailor::GetHash(el)))
				{
					return false;
				}
			}

			for (auto& el : *this)
			{
				if (!rhs.Contains_Internal(el, Sailor::GetHash(el)))
				{
					return false;
				}
			}

			return true;
		}

		__forceinline void LockAll() const
		{
			for (size_t i = 0; i < concurrencyLevel; i++)
			{
				m_locks[i].Lock();
			}
		}

		__forceinline void UnlockAll() const
		{
			for (size_t i = 0; i < concurrencyLevel; i++)
			{
				m_locks[i].Unlock();
			}
		}

	protected:

		static size_t RoundBucketCount(size_t requested)
		{
			const size_t count = std::max(requested, static_cast<size_t>(concurrencyLevel));
			return ((count + concurrencyLevel - 1) / concurrencyLevel) * concurrencyLevel;
		}

		bool Contains_Internal(const TElementType& inElement, size_t hash) const
		{
			const auto& element = m_buckets[hash % m_buckets.Num()];
			return element && element->LikelyContains(hash) && element->GetContainer().Contains(inElement);
		}

		__forceinline void Insert_Internal(TElementType inElement, const size_t& hash)
		{
			const size_t index = hash % m_buckets.Num();
			auto& element = m_buckets[index];

			if (!element)
			{
				element = TConcurrentEntryPtr::Make(hash);
			}

			if (element->GetContainer().Contains(inElement))
			{
				return;
			}

			element->GetContainer().EmplaceBack(std::move(inElement));
			element->m_bloom |= hash;

			m_num++;
		}

		__forceinline bool TryLock(size_t hash) const { return m_locks[hash % concurrencyLevel].TryLock(); }
		__forceinline void Lock(size_t hash) const { m_locks[hash % concurrencyLevel].Lock(); }
		__forceinline void Unlock(size_t hash) const { m_locks[hash % concurrencyLevel].Unlock(); }

		__forceinline void UnlockAll(uint32_t exceptConcurrencyLevel) const
		{
			for (size_t i = 0; i < concurrencyLevel; i++)
			{
				if (exceptConcurrencyLevel != (uint32_t)i)
				{
					m_locks[i].Unlock();
				}
			}
		}

		__forceinline bool TryLockAll(uint32_t exceptConcurrencyLevel) const
		{
			for (uint32_t i = 0; i < concurrencyLevel; i++)
			{
				if (exceptConcurrencyLevel != i && !m_locks[i].TryLock())
				{
					for (uint32_t j = 0; j < i; j++)
					{
						if (exceptConcurrencyLevel != j)
						{
							m_locks[j].Unlock();
						}
					}
					return false;
				}
			}
			return true;
		}

		__forceinline bool ShouldRehash() const
		{
			return m_rehashPolicy != ERehashPolicy::Never && (size_t)m_num > m_buckets.Num() * 4;
		}

		// Enter and return with the caller's stripe locked. A blocking upgrade must
		// first drop that stripe so two writers cannot wait on each other's locks.
		void RehashForInsert(size_t hash)
		{
			if (!ShouldRehash())
			{
				return;
			}
			const uint32_t stripe = static_cast<uint32_t>(hash % concurrencyLevel);
			if (!TryLockAll(stripe))
			{
				if (m_rehashPolicy != ERehashPolicy::Always)
				{
					return;
				}
				Unlock(hash);
				LockAll();
			}
			if (ShouldRehash())
			{
				Rehash(m_buckets.Num() * 4);
			}
			UnlockAll(stripe);
		}

		__forceinline void Rehash(size_t desiredBucketsNum)
		{
			desiredBucketsNum = RoundBucketCount(desiredBucketsNum);
			if (desiredBucketsNum <= m_buckets.Num())
			{
				return;
			}

			TVector<TConcurrentEntryPtr, TAllocator> buckets(desiredBucketsNum);
			TVector<TConcurrentEntryPtr, TAllocator>::Swap(buckets, m_buckets);

			m_num = 0;

			for (auto& bucket : buckets)
			{
				if (!bucket)
				{
					continue;
				}
				for (auto& el : bucket->GetContainer())
				{
					const auto& hash = Sailor::GetHash(el);

					if constexpr (IsMoveConstructible<TElementType>)
					{
						Insert_Internal(std::move(el), hash);
					}
					else
					{
						Insert_Internal(el, hash);
					}
				}
			}

			buckets.Clear();
		}

		TBucketContainer m_buckets{};
		mutable SpinLock m_locks[concurrencyLevel];

		std::atomic<uint32_t> m_num = 0;

		ERehashPolicy m_rehashPolicy;
	};

	SAILOR_API void RunSetBenchmark();
}
