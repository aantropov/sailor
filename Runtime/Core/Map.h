#pragma once
#include <cassert>
#include <memory>
#include <functional>
#include <concepts>
#include <type_traits>
#include "Defines.h"
#include "Vector.h"
#include "Math/Math.h"
#include "Memory/Memory.h"

namespace Sailor
{
	template<typename TKeyType>
	class TSetElement
	{
	public:

		TSetElement() = default;
		TSetElement(TSetElement&&) = default;
		TSetElement(const TSetElement&) = default;
		TSetElement& operator=(TSetElement&&) = default;
		TSetElement& operator=(const TSetElement&) = default;

		__forceinline bool operator==(const TSetElement & Other) const
		{
			return this->Value == Other.Value;
		}
		__forceinline bool operator!=(const TSetElement & Other) const
		{
			return this->Value != Other.Value;
		}
	};

	template<typename TElementType,	typename TAllocator>
	class TSet
	{
	public:

		TSet() = default;
		TSet(TSet&&) = default;
		TSet(const TSet&) = default;
		TSet& operator=(TSet&&) = default;
		TSet& operator=(const TSet&) = default;

		Add(const InElementType& InElement, bool* bIsAlreadyInSetPtr = nullptr)
	protected:
		
		uint32_t HashNumBuckets;
		
		TVector<TElementType, TAllocator> m_buckets;
	};

	template<typename TKeyType, typename TValueType, typename TAllocator>
	class TMap
	{
	public:

		using TKeyConstPointerType = TKey const*;
		using TValueConstPointerType = TKey const*;

		TMap() = default;
		TMap(TMap&&) = default;
		TMap(const TMap&) = default;
		TMap& operator=(TMap&&) = default;
		TMap& operator=(const TMap&) = default;

		TValueType& operator[](TKeyConstPointerType key) { return FindChecked(key); }
		const TValueType& operator[](TKeyConstPointerType key) const { return FindChecked(key); }

	protected:

		TValueType& FindChecked(TKeyConstPointerType key)
		{
			return nullptr;
		}

		TAllocator m_allocator{};
	};

}