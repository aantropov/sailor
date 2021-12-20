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
#include "Core/LogMacros.h"

namespace Sailor
{
	template<typename Type>
	size_t GetHash(const Type& instance)
	{
		static std::hash<Type> p;
		return p(instance);
	}

	template<typename TElementType, typename TAllocator = Memory::MallocAllocator>
	class SAILOR_API TSet
	{
	public:

		using TElementContainer = TVector<TElementType, Memory::TInlineAllocator<sizeof(TElementType) * 6, TAllocator>>;

		class SAILOR_API TSetElement
		{
		public:

			TSetElement(size_t hashCode) : m_hashCode(hashCode) {}
			TSetElement(TSetElement&&) = default;
			TSetElement(const TSetElement&) = default;
			TSetElement& operator=(TSetElement&&) = default;
			TSetElement& operator=(const TSetElement&) = default;

			__forceinline operator bool() const { return m_elements.Num() > 0; }
			virtual ~TSetElement() = default;

			__forceinline TElementContainer& GetContainer() { return m_elements; }
			__forceinline const TElementContainer& GetContainer() const { return m_elements; }

			__forceinline bool operator==(const TSetElement& Other) const
			{
				return this->Value == Other.Value;
			}
			__forceinline bool operator!=(const TSetElement& Other) const
			{
				return this->Value != Other.Value;
			}

			__forceinline size_t GetHash() const { return m_hashCode; }

		protected:

			size_t m_hashCode;
			TElementContainer m_elements;

			TSetElement* m_next = nullptr;
			TSetElement* m_prev = nullptr;

			friend class TSet;
		};

		template<typename TDataType = TElementType>
		class SAILOR_API TIterator
		{
		public:

			using iterator_category = std::bidirectional_iterator_tag;
			using value_type = TDataType;
			using difference_type = int64_t;
			using pointer = TDataType*;
			using reference = TDataType&;

			TIterator() : m_element(nullptr), m_currentBucket(nullptr) {}

			TIterator(const TIterator&) = default;
			TIterator(TIterator&&) = default;

			~TIterator() = default;

			TIterator(TSetElement* bucket, pointer element) : m_element(element), m_currentBucket(bucket) {}

			TIterator& operator=(const TIterator& rhs)
			{
				m_element = rhs.m_element;
				m_currentBucket = rhs.m_currentBucket;
				return *this;
			}

			TIterator& operator=(TIterator&& rhs)
			{
				m_element = rhs.m_element;
				m_currentBucket = rhs.m_currentBucket;
				rhs.m_element = nullptr;
				return *this;
			}

			bool operator==(const TIterator& rhs) const { return m_element == rhs.m_element; }
			bool operator!=(const TIterator& rhs) const { return m_element != rhs.m_element; }

			reference operator*() { return *m_element; }
			reference operator*() const { return *m_element; }

			reference operator->() { return m_element; }
			reference operator->() const { return m_element; }

			TIterator& operator++()
			{
				++m_element;

				if (m_element == &*m_currentBucket->GetContainer().end())
				{
					if (m_currentBucket->m_next)
					{
						m_currentBucket = m_currentBucket->m_next;
						m_element = &*m_currentBucket->GetContainer().begin();
					}
				}

				return *this;
			}

			TIterator& operator--()
			{
				if (m_element == &*m_currentBucket->begin())
				{
					if (m_currentBucket->m_prev)
					{
						m_currentBucket = m_currentBucket->m_prev;
						m_element = &*m_currentBucket->GetContainer().end();
					}
				}

				--m_element;

				return *this;
			}

		protected:

			TSetElement* m_currentBucket;
			pointer m_element;

			friend class TSetElement;
		};

		template<typename TDataType = TElementType>
		using TConstIterator = TSet::TIterator<const TDataType>;

		using TSetElementPtr = TUniquePtr<TSetElement>;

		using TBucketContainer = TVector<TSetElementPtr, TAllocator>;

		TSet(const uint32_t desiredNumBuckets = 8) { m_buckets.Resize(desiredNumBuckets); }
		TSet(TSet&&) = default;
		TSet(const TSet&) = default;
		TSet& operator=(TSet&&) = default;
		TSet& operator=(const TSet&) = default;

		size_t Num() const { return m_num; }

		bool Contains(const TElementType& inElement) const
		{
			const auto& hash = Sailor::GetHash(inElement);
			const size_t index = hash % m_buckets.Num();
			auto& element = m_buckets[index];

			if (element)
			{
				return element->GetContainer().Find(inElement) != -1;
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
				element = TSetElementPtr::Make(hash);

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

			element->GetContainer().Emplace(inElement);

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
				const auto& i = container.Find(inElement);
				if (i != -1)
				{
					container.RemoveAtSwap(i);

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

		const TElementType* Find(const TElementType& element) const
		{
			const auto& hash = Sailor::GetHash(element);
			const size_t index = hash % m_buckets.Num();
			auto& element = m_buckets[index];

			if (element)
			{
				const auto& i = element->GetContainer().Find(element);
				if (i != -1)
				{
					return &(*m_buckets[i]);
				}
				return nullptr;
			}

			return nullptr;
		}

		// Support ranged for
		TIterator<TElementType> begin() { return TIterator<TElementType>(m_first, m_first ? &*m_first->GetContainer().begin() : nullptr); }
		TIterator<TElementType> end() { return TIterator<TElementType>(m_last, m_last ? &*m_last->GetContainer().end() : nullptr); }

		TConstIterator<TElementType> begin() const { return TConstIterator<TElementType>(m_first, m_first ? &*m_first->GetContainer().cbegin() : nullptr); }
		TConstIterator<TElementType> end() const { return TConstIterator<TElementType>(m_last, m_last ? &*m_last->GetContainer().cend() : nullptr); }

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

			TVector<TSetElementPtr, TAllocator> buckets(desiredBucketsNum);
			TVector<TSetElementPtr, TAllocator>::Swap(buckets, m_buckets);

			TSetElement* current = m_first;
			
			m_num = 0;
			m_first = nullptr;
			m_last = nullptr;

			while (current)
			{
				const size_t oldIndex = current->GetHash() % m_buckets.Num();
				
				for (const auto& el : current->GetContainer())
				{
					Insert(el);
				}

				current = current->m_next;
			}

			buckets.Clear();
		}

		TBucketContainer m_buckets{};
		size_t m_num = 0;
		TSetElement* m_first = nullptr;
		TSetElement* m_last = nullptr;
	};

	SAILOR_API void RunSetBenchmark();
}
