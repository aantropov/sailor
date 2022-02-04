#pragma once
#include <cassert>
#include <memory>
#include <functional>
#include <concepts>
#include <type_traits>
#include "Core/Defines.h"
#include "Vector.h"
#include "Memory/UniquePtr.hpp"
#include "Memory/Memory.h"
#include "Containers/Pair.h"
#include "Containers/List.h"
#include "Core/LogMacros.h"

namespace Sailor
{
	template<typename Type>
	size_t GetHash(const Type& instance)
	{
		static std::hash<Type> p;
		return p(instance);
	}

	template<typename TElementType, typename TAllocator>
	class SAILOR_API TSet
	{
	public:

		using TElementContainer = TList<TElementType, Memory::TInlineAllocator<sizeof(TElementType) * 6, TAllocator>>;

		class SAILOR_API TEntry
		{
		public:

			TEntry(size_t hashCode) : m_hashCode(hashCode) {}
			TEntry(TEntry&&) noexcept = default;
			TEntry(const TEntry&) = default;
			TEntry& operator=(TEntry&&) noexcept = default;
			TEntry& operator=(const TEntry&) = default;

			__forceinline operator bool() const { return m_elements.Num() > 0; }
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
			TEntry* m_next = nullptr;
			TEntry* m_prev = nullptr;

			friend class TSet;
		};

		template<typename TDataType, typename TElementIterator>
		class SAILOR_API TBaseIterator
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

			TBaseIterator(TEntry* bucket, TElementIterator it) : m_it(std::move(it)), m_currentBucket(bucket) {}

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

				if (m_it == m_currentBucket->GetContainer().end())
				{
					if (m_currentBucket->m_next)
					{
						m_currentBucket = m_currentBucket->m_next;
						m_it = m_currentBucket->GetContainer().begin();
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
						m_it = m_currentBucket->GetContainer().Last();
					}
				}
				else
				{
					--m_it;
				}

				return *this;
			}

		protected:

			TEntry* m_currentBucket;
			TElementIterator m_it;

			friend class TEntry;
		};

		using TIterator = TBaseIterator<TElementType, typename TElementContainer::TIterator>;
		using TConstIterator = TBaseIterator<const TElementType, typename TElementContainer::TConstIterator>;
		using TEntryPtr = TUniquePtr<TEntry>;
		using TBucketContainer = TVector<TEntryPtr, TAllocator>;

		TSet(const uint32_t desiredNumBuckets = 8) { m_buckets.Resize(desiredNumBuckets); }
		TSet(TSet&&) = default;
		TSet(const TSet&) = default;
		TSet& operator=(TSet&&) noexcept = default;
		TSet& operator=(const TSet&) = default;

		TSet(std::initializer_list<TElementType> initList)
		{
			for (const auto& el : initList)
			{
				Insert(el);
			}
		}

		// TODO: Rethink the approach of base class for iterators
		TSet(const TVectorIterator<TElementType>& begin, const TVectorIterator<TElementType>& end)
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

		void Insert(TElementType inElement)
		{
			if (ShouldRehash())
			{
				Rehash(m_buckets.Capacity() * 4);
			}

			const auto& hash = Sailor::GetHash(inElement);
			const size_t index = hash % m_buckets.Num();
			auto& element = m_buckets[index];

			if (!element)
			{
				element = TEntryPtr::Make(hash);

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

		bool Remove(const TElementType& inElement)
		{
			const auto& hash = Sailor::GetHash(inElement);
			const size_t index = hash % m_buckets.Num();
			auto& element = m_buckets[index];

			if (element)
			{
				auto& container = element->GetContainer();
				if (container.RemoveFirst(inElement))
				{
					if (container.Num() == 0)
					{
						if (element.GetRawPtr() == m_last)
						{
							m_last = element->m_prev;
						}

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
					return true;
				}
				return false;
			}
			return false;
		}

		void Clear(uint32_t desiredBucketsNum = 8)
		{
			m_num = 0;
			m_first = m_last = nullptr;
			m_buckets.Clear();
			m_buckets.Resize(desiredBucketsNum);
		}

		// Support ranged for
		TIterator begin() { return TIterator(m_first, m_first ? m_first->GetContainer().begin() : nullptr); }
		TIterator end() { return TIterator(m_last, nullptr); }

		TConstIterator begin() const { return TConstIterator(m_first, m_first ? m_first->GetContainer().begin() : nullptr); }
		TConstIterator end() const { return TConstIterator(m_last, nullptr); }

	protected:

		__forceinline bool ShouldRehash() const
		{
			// We assume that each bucket has ~6 elements inside
			// TODO: Rethink the approach
			return (float)m_num > (float)m_buckets.Num() * 6;
		}

		void Rehash(size_t desiredBucketsNum)
		{
			if (desiredBucketsNum <= m_buckets.Num())
			{
				return;
			}

			TVector<TEntryPtr, TAllocator> buckets(desiredBucketsNum);
			TVector<TEntryPtr, TAllocator>::Swap(buckets, m_buckets);

			TEntry* current = m_first;

			m_num = 0;
			m_first = nullptr;
			m_last = nullptr;

			while (current)
			{
				const size_t oldIndex = current->GetHash() % m_buckets.Num();

				for (auto& el : current->GetContainer())
				{
					if constexpr (IsMoveConstructible<TElementType>)
					{
						Insert(std::move(el));
					}
					else
					{
						Insert(el);
					}
				}

				current = current->m_next;
			}

			buckets.Clear();
		}

		TBucketContainer m_buckets{};
		size_t m_num = 0;

		// That's unsafe but we handle that properly
		TEntry* m_first = nullptr;
		TEntry* m_last = nullptr;
	};

	SAILOR_API void RunSetBenchmark();
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