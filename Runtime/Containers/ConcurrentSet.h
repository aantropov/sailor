#pragma once
#include <cassert>
#include <memory>
#include <functional>
#include <concepts>
#include <type_traits>
#include "Core/Defines.h"
#include "Memory/UniquePtr.hpp"
#include "Memory/Memory.h"
#include "Containers/Pair.h"
#include "Containers/List.h"
#include "Containers/Vector.h"
#include "Core/LogMacros.h"
#include "Core/SpinLock.h"

namespace Sailor
{
	template<typename TElementType, const uint32_t concurrencyLevel = 8, typename TAllocator = Memory::DefaultGlobalAllocator>
	class TConcurrentSet
	{
	public:

		using TElementContainer = TList<TElementType, Memory::TInlineAllocator<sizeof(TElementType) * 6, TAllocator>>;

		class TEntry
		{
		public:

			TEntry(size_t hashCode) : m_hashCode(hashCode) {}
			TEntry(TEntry&&) = default;
			TEntry(const volatile TEntry& rhs)
			{
				m_bloom = rhs.m_bloom;
				m_hashCode = rhs.m_hashCode;
				m_elements = rhs.m_elements;

				m_next = rhs.m_next;
				m_prev = rhs.m_prev;
			}

			TEntry& operator=(TEntry&&) = default;
			TEntry& operator=(const volatile TEntry& rhs)
			{
				m_bloom = rhs.m_bloom;
				m_hashCode = rhs.m_hashCode;
				m_elements = rhs.m_elements;

				m_next = rhs.m_next;
				m_prev = rhs.m_prev;
				return *this;
			}

			__forceinline explicit operator bool() const { return m_elements.Num() > 0; }
			virtual ~TEntry() = default;

			__forceinline TElementContainer& GetContainer() { return m_elements; }
			__forceinline const TElementContainer& GetContainer() const { return m_elements; }

			__forceinline bool operator==(const TEntry& Other) const
			{
				return this->Value == Other.Value;
			}
			__forceinline bool operator!=(const TEntry& Other) const
			{
				return this->Value != Other.Value;
			}

			__forceinline size_t GetHash() const { return m_hashCode; }
			__forceinline size_t LikelyContains(size_t hashCode) const { return (m_bloom & hashCode) == hashCode; }

			// Should we hide the data in internal class 
			// that programmer has no access but it could be used by derived classes?
			//protected:

			size_t m_bloom = 0;
			size_t m_hashCode = 0;
			TElementContainer m_elements;

			// That's unsafe but we handle that properly
			volatile TEntry* m_next = nullptr;
			volatile TEntry* m_prev = nullptr;

			friend class TConcurrentSet;
		};

		template<typename TDataType, typename TElementIterator>
		class TBaseIterator
		{
		public:

			using iterator_category = std::bidirectional_iterator_tag;
			using value_type = TDataType;
			using difference_type = int64_t;
			using pointer = TDataType*;
			using reference = TDataType&;

			TBaseIterator() : m_it(nullptr), m_currentBucket(nullptr) {}

			TBaseIterator(const TBaseIterator&) = default;
			TBaseIterator(TBaseIterator&&) = default;

			~TBaseIterator() = default;

			TBaseIterator(volatile TEntry* bucket, TElementIterator it) : m_it(std::move(it)), m_currentBucket(bucket) {}

			operator TBaseIterator<const TDataType, TElementIterator>() { return TBaseIterator<const TDataType, TElementIterator>(m_currentBucket, m_it); }

			TBaseIterator& operator=(const TBaseIterator& rhs) = default;
			TBaseIterator& operator=(TBaseIterator&& rhs) = default;

			bool operator==(const TBaseIterator& rhs) const { return m_it == rhs.m_it; }
			bool operator!=(const TBaseIterator& rhs) const { return m_it != rhs.m_it; }

			pointer operator->() { return &*m_it; }
			pointer operator->() const { return &*m_it; }

			reference operator*() { return *m_it; }
			reference operator*() const { return *m_it; }

			TBaseIterator& operator++()
			{
				++m_it;

				if (m_it == ((TEntry*)m_currentBucket)->GetContainer().end())
				{
					if (m_currentBucket->m_next)
					{
						m_currentBucket = m_currentBucket->m_next;
						m_it = ((TEntry*)m_currentBucket)->GetContainer().begin();
					}
				}

				return *this;
			}

			TBaseIterator& operator--()
			{
				if (m_it == m_currentBucket->GetContainer().begin())
				{
					if (m_currentBucket->m_prev)
					{
						m_currentBucket = m_currentBucket->m_prev;
						m_it = ((TEntry*)m_currentBucket)->GetContainer().Last();
					}
				}
				else
				{
					--m_it;
				}

				return *this;
			}

		protected:

			volatile TEntry* m_currentBucket;
			TElementIterator m_it;

			friend class TEntry;
		};

		using TIterator = TBaseIterator<TElementType, typename TElementContainer::TIterator>;
		using TConstIterator = TBaseIterator<const TElementType, typename TElementContainer::TConstIterator>;
		using TConcurrentEntryPtr = TUniquePtr<TEntry>;
		using TBucketContainer = TVector<TConcurrentEntryPtr, TAllocator>;

		SAILOR_API TConcurrentSet(const uint32_t desiredNumBuckets = 8) { m_buckets.Resize(desiredNumBuckets); }
		SAILOR_API TConcurrentSet(TConcurrentSet&&) = default;
		SAILOR_API TConcurrentSet(const TConcurrentSet& rhs) requires IsCopyConstructible<TElementType> : TConcurrentSet((uint32_t)rhs.m_buckets.Num())
		{
			for (const auto& el : rhs)
			{
				Insert(el);
			}
		}

		SAILOR_API TConcurrentSet& operator=(TConcurrentSet&&) = default;
		SAILOR_API TConcurrentSet& operator=(const TConcurrentSet& rhs) requires IsCopyConstructible<TElementType>
		{
			Clear((uint32_t)rhs.m_buckets.Num());
			for (const auto& el : rhs)
			{
				Insert(el);
			}

			return *this;
		}

		SAILOR_API TConcurrentSet(std::initializer_list<TElementType> initList)
		{
			for (const auto& el : initList)
			{
				Insert(el);
			}
		}

		// TODO: Rethink the approach of base class for iterators
		SAILOR_API TConcurrentSet(const TVectorIterator<TElementType>& begin, const TVectorIterator<TElementType>& end)
		{
			TVectorIterator<TElementType> it = begin;
			while (it != end)
			{
				Insert(*it);
				it++;
			}
		}

		__forceinline bool IsEmpty() const { return m_num == 0; }
		__forceinline size_t Num() const { return m_num; }

		bool Contains(const TElementType& inElement) const
		{
			const auto& hash = Sailor::GetHash(inElement);
			const size_t index = hash % m_buckets.Num();
			auto& element = m_buckets[index];

			if (element && element->LikelyContains(hash))
			{
				return element->GetContainer().Contains(inElement);
			}

			return false;
		}

		void ForcelyInsert(TElementType inElement)
		{
			const auto& hash = Sailor::GetHash(inElement);
			Insert_Internal(std::move(inElement), hash);
		}

		void Insert(TElementType inElement)
		{
			if (ShouldRehash() && TryLockAll())
			{
				Rehash(m_buckets.Capacity() * 4);
				UnlockAll();
			}

			const auto& hash = Sailor::GetHash(inElement);

			Lock(hash);

			Insert_Internal(std::move(inElement), hash);

			Unlock(hash);
		}

		bool Remove(const TElementType& inElement)
		{
			const auto& hash = Sailor::GetHash(inElement);
			const size_t index = hash % m_buckets.Num();
			auto& element = m_buckets[index];

			if (element)
			{
				Lock(hash);

				auto& container = element->GetContainer();
				if (container.RemoveFirst(inElement))
				{
					if (container.Num() == 0)
					{
						if (element->m_next)
						{
							element->m_next->m_prev = element->m_prev;
						}

						if (element->m_prev)
						{
							element->m_prev->m_next = element->m_next;
						}

						if (m_last == element.GetRawPtr())
						{
							m_last = m_last->m_prev;
						}

						if (m_first == element.GetRawPtr())
						{
							m_first = m_first->m_next;
						}

						element.Clear();
					}

					m_num--;
					Unlock(hash);
					return true;
				}
				Unlock(hash);
				return false;
			}
			return false;
		}

		void Clear(uint32_t desiredBucketsNum = 8)
		{
			LockAll();

			m_num = 0;
			m_first = m_last = nullptr;
			m_buckets.Clear();

			m_buckets.Resize(desiredBucketsNum);

			UnlockAll();
		}

		// Support ranged for
		TIterator begin() { return TIterator(m_first, m_first ? ((TEntry*)m_first)->GetContainer().begin() : nullptr); }
		TIterator end() { return TIterator(m_last, m_last ? ((TEntry*)m_last)->GetContainer().end() : nullptr); }

		TConstIterator begin() const { return TConstIterator(m_first, m_first ? ((TEntry*)m_first)->GetContainer().begin() : nullptr); }
		TConstIterator end() const { return TConstIterator(m_last, m_last ? ((TEntry*)m_last)->GetContainer().end() : nullptr); }

		bool operator==(const TConcurrentSet& rhs) const
		{
			if (rhs.Num() != this->Num())
			{
				return false;
			}

			for (auto& el : rhs)
			{
				if (!this->Contains(el))
				{
					return false;
				}
			}

			for (auto& el : *this)
			{
				if (!rhs.Contains(el))
				{
					return false;
				}
			}

			return true;
		}

		__forceinline void LockAll()
		{
			for (size_t i = 0; i < concurrencyLevel; i++)
			{
				m_locks[i].Lock();
			}
		}

		__forceinline void UnlockAll()
		{
			for (size_t i = 0; i < concurrencyLevel; i++)
			{
				m_locks[i].Unlock();
			}
		}

	protected:

		__forceinline void Insert_Internal(TElementType inElement, const size_t& hash)
		{
			const size_t index = hash % m_buckets.Num();
			auto& element = m_buckets[index];

			if (!element)
			{
				element = TConcurrentEntryPtr::Make(hash);

				if (!m_first)
				{
					m_last = m_first = element.GetRawPtr();
				}
				else
				{
					m_first->m_prev = element.GetRawPtr();
					element->m_next = m_first;
					m_first = element.GetRawPtr();
				}
			}

			if (element->GetContainer().Contains(inElement))
			{
				return;
			}

			element->GetContainer().EmplaceBack(std::move(inElement));
			element->m_bloom |= hash;

			m_num++;
		}

		__forceinline bool TryLock(size_t hash) { return m_locks[hash % concurrencyLevel].TryLock(); }
		__forceinline void Lock(size_t hash) { m_locks[hash % concurrencyLevel].Lock(); }
		__forceinline void Unlock(size_t hash) { m_locks[hash % concurrencyLevel].Unlock(); }

		__forceinline bool TryLockAll()
		{
			for (uint32_t i = 0; i < concurrencyLevel; i++)
			{
				if (!m_locks[i].TryLock())
				{
					for (uint32_t j = i; j >= 0; j--)
					{
						m_locks[j].Unlock();
					}
					return false;
				}
			}
			return true;
		}

		__forceinline bool ShouldRehash() const { return (float)m_num > (float)m_buckets.Num() * 6; }
		__forceinline void Rehash(size_t desiredBucketsNum)
		{
			if (desiredBucketsNum <= m_buckets.Num())
			{
				return;
			}

			TVector<TConcurrentEntryPtr, TAllocator> buckets(desiredBucketsNum);
			TVector<TConcurrentEntryPtr, TAllocator>::Swap(buckets, m_buckets);

			volatile TEntry* current = (TEntry*)m_first;

			m_num = 0;
			m_first = nullptr;
			m_last = nullptr;

			while (current)
			{
				const size_t oldIndex = ((TEntry*)current)->GetHash() % m_buckets.Num();

				for (auto& el : ((TEntry*)current)->GetContainer())
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

				current = current->m_next;
			}

			buckets.Clear();
		}

		TBucketContainer m_buckets{};
		SpinLock m_locks[concurrencyLevel];

		size_t m_num = 0;

		// That's unsafe but we handle that properly
		volatile TEntry* m_first = nullptr;
		volatile TEntry* m_last = nullptr;
	};

	SAILOR_API void RunSetBenchmark();
}
