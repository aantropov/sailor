#pragma once
#include "Core/Defines.h"
#include "Containers/Concepts.h"

namespace Sailor
{
	template<typename TKeyType, typename TValueType>
	class TPair final
	{
	public:

		SAILOR_API TPair() noexcept requires IsDefaultConstructible<TKeyType> && IsDefaultConstructible<TValueType> : m_first(), m_second() {}
		TPair(const TPair&) noexcept requires Sailor::IsCopyConstructible<TKeyType>&& Sailor::IsCopyConstructible<TValueType> = default;

		SAILOR_API TPair(TPair&& rhs) noexcept requires 
			(Sailor::IsMoveConstructible<TKeyType> || Sailor::IsCopyConstructible<TKeyType>) &&
			(Sailor::IsCopyConstructible<TValueType> || Sailor::IsMoveConstructible<TValueType>) && 
			(Sailor::IsMoveConstructible<TKeyType> || Sailor::IsMoveConstructible<TValueType>)
		{
			if constexpr (Sailor::IsMoveConstructible<TKeyType>)
			{
				m_first = std::move(rhs.m_first);
			}
			else
			{
				m_first = rhs.m_first;
			}

			if constexpr (Sailor::IsMoveConstructible<TValueType>)
			{
				m_second = std::move(rhs.m_second);
			}
			else
			{
				m_second = rhs.m_second;
			}
		}

		SAILOR_API ~TPair() = default;

		SAILOR_API TPair(TKeyType&& first, const TValueType& second) noexcept requires Sailor::IsMoveConstructible<TKeyType> && Sailor::IsCopyConstructible<TValueType> :
		m_first(std::move(first)), m_second(second) {}

		SAILOR_API TPair(const TKeyType& first, TValueType&& second) noexcept requires Sailor::IsCopyConstructible<TKeyType> && Sailor::IsMoveConstructible<TValueType> :
			m_first(first), m_second(std::move(second)) {}

		SAILOR_API TPair(const TKeyType& first, const TValueType& second) noexcept requires Sailor::IsCopyConstructible<TKeyType>&& Sailor::IsCopyConstructible<TValueType> :
			m_first(first), m_second(second) {}

		SAILOR_API TPair(TKeyType&& first, TValueType&& second) noexcept requires Sailor::IsMoveConstructible<TKeyType>&& Sailor::IsMoveConstructible<TValueType> :
			m_first(std::move(first)), m_second(std::move(second)) {}

		TPair& operator=(const TPair&) requires Sailor::IsCopyConstructible<TKeyType>&& Sailor::IsCopyConstructible<TValueType> = default;

		SAILOR_API TPair& operator=(TPair&& rhs) noexcept requires Sailor::IsMoveConstructible<TKeyType> || Sailor::IsMoveConstructible<TValueType>
		{
			if constexpr (Sailor::IsMoveConstructible<TKeyType>)
			{
				m_first = std::move(rhs.m_first);
			}
			else
			{
				m_first = rhs.m_first;
			}

			if constexpr (Sailor::IsMoveConstructible<TValueType>)
			{
				m_second = std::move(rhs.m_second);
			}
			else
			{
				m_second = rhs.m_second;
			}

			return *this;
		}

		SAILOR_API bool operator==(const TPair& rhs) const
		{
			return Sailor::Equals(rhs.m_first, m_first) && Sailor::Equals(rhs.m_second, m_second);
		}

		SAILOR_API __forceinline const TKeyType& First() const { return m_first; }
		SAILOR_API __forceinline const TValueType& Second() const { return m_second; }

		TKeyType m_first{};
		TValueType m_second{};
	};
}
