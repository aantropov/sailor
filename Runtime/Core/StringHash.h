#pragma once
#include <string>
#include <string_view>
#include <type_traits>
#include "Containers/Hash.h"

namespace Sailor
{
	struct SAILOR_API StringHash
	{
		uint64_t m_hash{ 0 };

		__forceinline constexpr StringHash() = default;

		__forceinline constexpr StringHash(const StringHash& OtherId) = default;

		__forceinline constexpr StringHash& operator=(const StringHash& OtherId) = default;

		[[nodiscard]] __forceinline constexpr explicit StringHash(std::string_view str)
		{
			m_hash = Sailor::fnv1a(str.data(), str.length());
			if (!std::is_constant_evaluated())
			{
				AddToHashedStringsTable(*this, str);
			}
		}

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

	namespace Internal
	{
		template<size_t N>
		struct TStringLiteral
		{
			char m_chars[N];

			constexpr TStringLiteral(const char (&value)[N])
			{
				for (size_t i = 0; i < N; ++i) m_chars[i] = value[i];
			}
		};

		template<TStringLiteral Literal>
		void RegisterStringLiteral()
		{
			static const bool bRegistered = []
			{
				constexpr std::string_view text(Literal.m_chars, sizeof(Literal.m_chars) - 1);
				constexpr StringHash hash(text);
				StringHash::AddToHashedStringsTable(hash, text);
				return true;
			}();
			(void)bRegistered;
		}
	}

	template<Internal::TStringLiteral Literal>
	[[nodiscard]] constexpr StringHash operator""_h()
	{
		constexpr StringHash hash(std::string_view(Literal.m_chars, sizeof(Literal.m_chars) - 1));
		if (!std::is_constant_evaluated()) Internal::RegisterStringLiteral<Literal>();
		return hash;
	}
};
