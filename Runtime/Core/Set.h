#pragma once
#include <cassert>
#include <memory>
#include <functional>
#include <concepts>
#include <type_traits>
#include "Defines.h"
#include "Vector.h"
#include "Math/Math.h"
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

	template<typename TElementType, typename TAllocator>
	class TSetElement
	{
	public:

		TSetElement() = default;
		TSetElement(TSetElement&&) = default;
		TSetElement(const TSetElement&) = default;
		TSetElement& operator=(TSetElement&&) = default;
		TSetElement& operator=(const TSetElement&) = default;

		virtual ~TSetElement() = default;

		TVector<TElementType, TAllocator>& GetContainer() { return m_elements; }

		__forceinline bool operator==(const TSetElement& Other) const
		{
			return this->Value == Other.Value;
		}
		__forceinline bool operator!=(const TSetElement& Other) const
		{
			return this->Value != Other.Value;
		}

	protected:

		TVector<TElementType, TAllocator> m_elements;
	};

	template<typename TElementType, typename TAllocator = Memory::MallocAllocator>
	class TSet
	{
	public:

		TSet(const uint32_t desiredNumBuckets = 16)
		{
			m_buckets.Reserve(desiredNumBuckets);
		}

		TSet(TSet&&) = default;
		TSet(const TSet&) = default;
		TSet& operator=(TSet&&) = default;
		TSet& operator=(const TSet&) = default;

		bool Contains(const TElementType& element) const
		{
			const auto& hash = Sailor::GetHash(element);
			const size_t index = hash % m_buckets.Num();

			if (m_buckets[index].Num() > 0)
			{
				return m_buckets[index].Find(element) != -1;
			}

			return false;
		}

		void Insert(TElementType element)
		{
			if (ShouldRehash())
			{
				Rehash(m_buckets.Capacity() * 2);
			}

			const auto& hash = Sailor::GetHash(element);
			const size_t index = hash % m_buckets.Num();
			m_buckets[index].Emplace(element);
		}

		bool Remove(const TElementType& element)
		{
			const auto& hash = Sailor::GetHash(element);
			const size_t index = hash % m_buckets.Num();

			if (m_buckets[index].Num() > 0)
			{
				const auto& i = m_buckets[index].Find(element);
				if (i != -1)
				{
					m_buckets[index].RemoveAtSwap(i);
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

			if (m_buckets[index].Num() > 0)
			{
				const auto& i = m_buckets[index].Find(element);
				if (i != -1)
				{
					return &m_buckets[index];
				}
				return nullptr;
			}

			return nullptr;
		}

	protected:

		TVector<TSetElement<TElementType, TAllocator>, TAllocator> m_buckets;

		bool ShouldRehash() const
		{
			// TODO: Rethink the approach
			return (float)m_buckets.Num() > m_buckets.Capacity() * 0.8f;
		}

		void Rehash(size_t desiredBucketsNum)
		{
			if (desiredBucketsNum <= m_buckets.Capacity())
			{
				return;
			}

			TVector<TSetElement<TElementType>, TAllocator> buckets(desiredBucketsNum);

			for (auto& el : m_buckets)
			{
				buckets.Add(std::move(el));
			}
			m_buckets = std::move(buckets);
		}
	};

	SAILOR_API void RunSetBenchmark();
}
