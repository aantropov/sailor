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

namespace Sailor
{
	template<typename Type>
	size_t GetHash(const Type& instance)
	{
		static std::hash<Type> p;
		return p(instance);
	}

	template<typename TElementType, typename TAllocator = Memory::MallocAllocator>
	class TSet
	{
	public:

		class TSetElement
		{
		public:

			using TContainer = TVector<TElementType, Memory::TInlineAllocator<sizeof(TElementType) * 3, TAllocator>>;

			TSetElement(size_t hashCode) : m_hashCode(hashCode) {}
			TSetElement(TSetElement&&) = default;
			TSetElement(const TSetElement&) = default;
			TSetElement& operator=(TSetElement&&) = default;
			TSetElement& operator=(const TSetElement&) = default;

			__forceinline operator bool() const { return m_elements.Num() > 0; }
			virtual ~TSetElement() = default;

			__forceinline TContainer& GetContainer() { return m_elements; }
			__forceinline const TContainer& GetContainer() const { return m_elements; }

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
			TContainer m_elements;

			TSetElement* m_next = nullptr;
			TSetElement* m_prev = nullptr;

			friend class TSet;
		};

		using TSetElementPtr = TUniquePtr<TSetElement>;

		TSet(const uint32_t desiredNumBuckets = 1024) { m_buckets.Resize(desiredNumBuckets); }
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
				Rehash(m_buckets.Capacity() * 2);
			}

			const auto& hash = Sailor::GetHash(inElement);
			const size_t index = hash % m_buckets.Num();
			auto& element = m_buckets[index];

			if (!element)
			{
				element = TSetElementPtr::Make(hash);

				if (!m_first)
				{
					m_first = element.GetRawPtr();
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
						if (element->m_next)
						{
							element->m_next->m_prev = element->m_prev;
						}

						if (element->m_prev)
						{
							element->m_prev->m_next = element->m_next;
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

	protected:

		__forceinline bool ShouldRehash() const
		{
			// We assume that each bucket has up 16 elements inside
			// TODO: Rethink the approach
			return (float)m_num > (float)m_buckets.Num() * 16;
		}

		void Rehash(size_t desiredBucketsNum)
		{
			if (desiredBucketsNum <= m_buckets.Num())
			{
				return;
			}

			TVector<TSetElementPtr, TAllocator> buckets(desiredBucketsNum);

			TSetElement* current = m_first;
			while (current)
			{
				const size_t index = current->GetHash() % desiredBucketsNum;
				const size_t oldIndex = current->GetHash() % m_buckets.Num();

				buckets[index] = std::move(m_buckets[oldIndex]);

				current = current->m_next;
			}

			m_buckets = std::move(buckets);
		}

		TVector<TSetElementPtr, TAllocator> m_buckets{};
		size_t m_num = 0;
		TSetElement* m_first = nullptr;
	};

	SAILOR_API void RunSetBenchmark();
}
