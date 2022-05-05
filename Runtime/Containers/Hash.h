#pragma once
#include <functional>

namespace Sailor
{
	inline void HashCombine(std::size_t& seed) { }

	template <typename T, typename... Rest>
	inline void HashCombine(std::size_t& seed, const T& v, Rest... rest)
	{
		std::hash<T> hasher;
		seed ^= hasher(v) + 0x9e3779b9 + (seed << 6) + (seed >> 2);
		HashCombine(seed, rest...);
	}

	template<typename Type>
	size_t GetHash(const Type& instance)
	{
		static std::hash<Type> p;
		return p(instance);
	}
}
