#pragma once
#include <functional>
#include "Concepts.h"

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
}

// TODO: Should we implement the basic class?
/*
class IHashable
{
public:

	virtual ~IHashable() = default;
	virtual size_t GetHash() const = 0;
};

namespace std
{
	template<>
	struct std::hash<Sailor::IHashable>
	{
		SAILOR_API std::size_t operator()(const Sailor::IHashable& p) const
		{
			return p.GetHash();
		}
	};
}
*/