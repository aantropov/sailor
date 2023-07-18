#pragma once
#include <string>
#include "Core/JsonSerializable.h"
#include "Core/YamlSerializable.h"
#include "Sailor.h"

using namespace nlohmann;
namespace Sailor { class InstanceId; }

namespace Sailor
{
	class InstanceId final : public IJsonSerializable, public IYamlSerializable
	{
	public:

		static const InstanceId Invalid;

		SAILOR_API static InstanceId CreateNewInstanceId();
		SAILOR_API const std::string& ToString() const;

		SAILOR_API InstanceId() = default;
		SAILOR_API InstanceId(const InstanceId& inInstanceId) = default;
		SAILOR_API InstanceId(InstanceId&& inInstanceId) = default;

		SAILOR_API InstanceId& operator=(const InstanceId& inInstanceId) = default;
		SAILOR_API InstanceId& operator=(InstanceId&& inInstanceId) = default;

		SAILOR_API __forceinline bool operator==(const InstanceId& rhs) const;
		SAILOR_API __forceinline bool operator!=(const InstanceId& rhs) const { return !(rhs == *this); }

		SAILOR_API explicit operator bool() const { return m_InstanceId != InstanceId::Invalid.m_InstanceId; }

		SAILOR_API ~InstanceId() = default;

		SAILOR_API virtual void Serialize(nlohmann::json& outData) const override;
		SAILOR_API virtual void Deserialize(const nlohmann::json& inData) override;

		SAILOR_API virtual YAML::Node Serialize() const override;
		SAILOR_API virtual void Deserialize(const YAML::Node& inData) override;

	protected:

		std::string m_InstanceId{};
	};
}

namespace std
{
	template<>
	struct hash<Sailor::InstanceId>
	{
		SAILOR_API std::size_t operator()(const Sailor::InstanceId& p) const
		{
			std::hash<std::string> h;
			return h(p.ToString());
		}
	};
}