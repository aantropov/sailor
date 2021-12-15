#pragma once
#include <cassert>
#include <memory>
#include <functional>
#include <concepts>
#include <type_traits>
#include "Defines.h"
#include "Math/Math.h"
#include "Memory/Memory.h"

namespace Sailor
{
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