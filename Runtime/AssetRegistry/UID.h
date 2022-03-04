#pragma once
#include <string>
#include "Core/JsonSerializable.h"
#include "Sailor.h"

using namespace nlohmann;
namespace Sailor { class UID; }

namespace Sailor
{
	class UID final : public IJsonSerializable
	{
	public:

		static const UID Invalid;

		static SAILOR_API UID CreateNewUID();
		SAILOR_API const std::string& ToString() const;

		SAILOR_API UID() = default;
		SAILOR_API UID(const UID& inUID) = default;
		SAILOR_API UID(UID&& inUID) = default;

		SAILOR_API UID& operator=(const UID& inUID) = default;
		SAILOR_API UID& operator=(UID&& inUID) = default;

		SAILOR_API __forceinline bool operator==(const UID& rhs) const;
		SAILOR_API __forceinline bool operator!=(const UID& rhs) const { return !(rhs == *this); }

		SAILOR_API operator bool() const { return m_UID != UID::Invalid.m_UID; }

		SAILOR_API ~UID() = default;

		virtual SAILOR_API void Serialize(nlohmann::json& outData) const override;
		virtual SAILOR_API void Deserialize(const nlohmann::json& inData) override;

	protected:

		std::string m_UID{};
	};
}

namespace std
{
	template<>
	struct hash<Sailor::UID>
	{
		SAILOR_API std::size_t operator()(const Sailor::UID& p) const
		{
			std::hash<std::string> h;
			return h(p.ToString());
		}
	};
}