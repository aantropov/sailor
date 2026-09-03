#pragma once

#include <concepts>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <string_view>

#include "Concepts.h"

namespace Sailor
{
	inline constexpr uint64_t Fnv1aOffsetBasis = 14695981039346656037ull;
	inline constexpr uint64_t Fnv1aPrime = 1099511628211ull;
	inline constexpr uint64_t HashCombineConstant = 0x9e3779b97f4a7c15ull;

	[[nodiscard]] constexpr uint64_t MixHash(uint64_t value) noexcept
	{
		value += HashCombineConstant;
		value = (value ^ (value >> 30u)) * 0xbf58476d1ce4e5b9ull;
		value = (value ^ (value >> 27u)) * 0x94d049bb133111ebull;
		return value ^ (value >> 31u);
	}

	template<typename Type>
	size_t GetHash(const Type& instance)
	{
		constexpr bool bHasGetHash = requires(const Type & t) { t.GetHash(); };

		if constexpr (bHasGetHash)
		{
			return instance.GetHash();
		}
		else
		{
			static std::hash<Type> p;
			return p(instance);
		}
	}

	template<std::unsigned_integral Seed, typename... Types>
	inline void HashCombine(Seed& seed, const Types&... values)
	{
		((seed ^= static_cast<Seed>(GetHash(values)) +
			static_cast<Seed>(HashCombineConstant) +
			(seed << 6u) + (seed >> 2u)), ...);
	}

	constexpr uint64_t fnv1a(
		const char* data,
		size_t size,
		uint64_t hash = Fnv1aOffsetBasis) noexcept
	{
		for (size_t index = 0; index < size; ++index)
		{
			hash ^= static_cast<uint8_t>(data[index]);
			hash *= Fnv1aPrime;
		}
		return hash;
	}

	inline void HashBytes(
		uint64_t& hash,
		const void* data,
		size_t size) noexcept
	{
		const auto* bytes = static_cast<const uint8_t*>(data);
		for (size_t index = 0; index < size; ++index)
		{
			hash ^= bytes[index];
			hash *= Fnv1aPrime;
		}
	}

	[[nodiscard]] inline uint64_t HashBytes(
		const void* data,
		size_t size) noexcept
	{
		uint64_t hash = Fnv1aOffsetBasis;
		HashBytes(hash, data, size);
		return hash;
	}

	inline void HashString(
		uint64_t& hash,
		std::string_view value) noexcept
	{
		hash = fnv1a(value.data(), value.size(), hash);
	}

	[[nodiscard]] constexpr uint64_t HashString(
		std::string_view value) noexcept
	{
		return fnv1a(value.data(), value.size());
	}

	inline void HashValue(uint64_t& hash, std::string_view value) noexcept
	{
		const uint64_t size = value.size();
		HashBytes(hash, &size, sizeof(size));
		HashString(hash, value);
	}

	template<IsTriviallyCopyable Type>
	inline void HashValue(uint64_t& hash, const Type& value) noexcept
	{
		HashBytes(hash, &value, sizeof(Type));
	}

	template<IsTriviallyCopyable... Types>
	inline void HashValues(uint64_t& hash, const Types&... values) noexcept
	{
		(HashValue(hash, values), ...);
	}
}
