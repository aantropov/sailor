#pragma once
#include <string>
#include <string_view>
#include "Containers/Hash.h"

namespace Sailor
{
	struct SAILOR_API StringHash
	{
		uint64_t m_hash{ 0 };

		__forceinline constexpr StringHash() = default;

		__forceinline constexpr StringHash(const StringHash& OtherId) = default;

		__forceinline constexpr StringHash& operator=(const StringHash& OtherId) = default;

		[[nodiscard]] __forceinline constexpr explicit StringHash(std::string_view str);

		// constructor that forces runtime evaluation to put string into hashed strings table
		static StringHash Runtime(std::string_view str);

		[[nodiscard]] const std::string& ToString() const;

		[[nodiscard]] __forceinline constexpr bool IsEmpty() const
		{
			constexpr size_t s_emptyHash = Sailor::fnv1a("", 0);
			return m_hash == 0 || m_hash == s_emptyHash;
		}

		__forceinline constexpr bool operator==(StringHash Other) const { return m_hash == Other.m_hash; }

		__forceinline constexpr bool operator!=(StringHash Other) const { return m_hash != Other.m_hash; }

		[[nodiscard]] __forceinline constexpr uint64_t GetHash() const { return m_hash; }

		static void AddToHashedStringsTable(StringHash Hash, std::string_view str);
		static const std::string& GetStrFromHashedStringsTable(StringHash Hash);
	};

	SAILOR_API /*[[nodiscard]]*/ __forceinline constexpr StringHash operator""_h(const char* str, std::size_t size) { return StringHash{std::string_view(str, size)}; }
};