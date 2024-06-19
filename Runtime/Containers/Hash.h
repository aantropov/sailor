#pragma once
#include <functional>
#include "Concepts.h"

namespace Sailor
{
	inline void HashCombine(std::size_t& seed) { }

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

	template <typename T, typename... Rest>
	inline void HashCombine(std::size_t& seed, const T& v, Rest... rest)
	{
		size_t hashV = GetHash(v);

		seed ^= hashV + 0x9e3779b9 + (seed << 6) + (seed >> 2);
		HashCombine(seed, rest...);
	}

	static constexpr std::uint64_t fnv1a(const char* str, size_t num)
	{
		std::uint64_t hash = 0xcbf29ce484222325; // FNV offset basis
		constexpr std::uint64_t prime = 0x100000001b3; // FNV prime

		for (size_t i = 0; i < num; ++i) {
			hash ^= static_cast<std::uint8_t>(str[i]);
			hash *= prime;
		}
		return hash;
	}
}