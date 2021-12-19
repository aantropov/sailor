#pragma once
#include <cassert>
#include <memory>
#include <functional>
#include <concepts>
#include <type_traits>
#include "Core/Defines.h"
#include "Vector.h"
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

		using TContainer = TVector<TElementType>;// TVector<TElementType, Memory::TInlineAllocator<64u, TAllocator>>;

		TSetElement() = default;
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
		__forceinline void SetHash(size_t hashCode) { m_hashCode = hashCode; }

	protected:

		size_t m_hashCode;
		TContainer m_elements;
	};

	template<typename TElementType, typename TAllocator = Memory::MallocAllocator>
	class TSet
	{
	public:

		TSet(const uint32_t desiredNumBuckets = 16) { m_buckets.Resize(desiredNumBuckets); }
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
				return element.GetContainer().Find(inElement) != -1;
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
				element.SetHash(hash);
			}

			element.GetContainer().Emplace(inElement);

			m_num++;
		}

		bool Remove(const TElementType& inElement)
		{
			const auto& hash = Sailor::GetHash(inElement);
			const size_t index = hash % m_buckets.Num();
			auto& element = m_buckets[index];

			if (element)
			{
				const auto& i = element.GetContainer().Find(inElement);
				if (i != -1)
				{
					element.GetContainer().RemoveAtSwap(i);
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
				const auto& i = element.GetContainer().Find(element);
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
			// We assume that each bucket has up to 64 elements inside
			// TODO: Rethink the approach
			return (float)m_num > (float)m_buckets.Num() * 2;
		}

		void Rehash(size_t desiredBucketsNum)
		{
			if (desiredBucketsNum <= m_buckets.Num())
			{
				return;
			}

			TVector<TSetElement<TElementType, TAllocator>, TAllocator> buckets(desiredBucketsNum);

			for (auto& el : m_buckets)
			{
				if (el)
				{
					const size_t index = el.GetHash() % desiredBucketsNum;
					buckets[index] = std::move(el);
				}
			}
			m_buckets = std::move(buckets);
		}

		TVector<TSetElement<TElementType, TAllocator>, TAllocator> m_buckets{};
		size_t m_num = 0;
	};

	SAILOR_API void RunSetBenchmark();
}
