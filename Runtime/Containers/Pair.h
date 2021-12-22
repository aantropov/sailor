#pragma once
#include "Core/Defines.h"

namespace Sailor
{
	template<typename TKeyType, typename TValueType>
	class TPair final
	{
	public:

		SAILOR_API TPair() : m_first(), m_second() {}
		SAILOR_API TPair(const TPair&) = default;
		SAILOR_API TPair(TPair&&) = default;
		SAILOR_API ~TPair() = default;

		SAILOR_API TPair(TKeyType&& first, const TValueType& second) : m_first(std::move(first)), m_second(second) {}
		SAILOR_API TPair(const TKeyType& first, TValueType&& second) : m_first(first), m_second(std::move(second)) {}
		SAILOR_API TPair(const TKeyType& first, const TValueType& second) : m_first(first), m_second(second) {}
		SAILOR_API TPair(TKeyType&& first, TValueType&& second) : m_first(std::move(first)), m_second(std::move(second)) {}

		SAILOR_API TPair& operator=(const TPair&) = default;
		SAILOR_API TPair& operator=(TPair&&) = default;

		SAILOR_API __forceinline const TKeyType& First() const { return m_first; }
		SAILOR_API __forceinline const TValueType& Second() const { return m_second; }

		TKeyType m_first;
		TValueType m_second;
	};
}
