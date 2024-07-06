#pragma once
#include <string>
#include "Core/YamlSerializable.h"
#include "Core/StringHash.h"
#include "Sailor.h"

namespace Sailor { class InstanceId; }

namespace Sailor
{
	class InstanceId final : public IYamlSerializable
	{
	public:

		static const InstanceId Invalid;

		SAILOR_API static InstanceId GenerateNewInstanceId();
		SAILOR_API static InstanceId GenerateNewComponentId(const InstanceId& parentGameObjectId);

		SAILOR_API const std::string& ToString() const;

		SAILOR_API InstanceId() = default;
		SAILOR_API InstanceId(const InstanceId& inInstanceId) = default;
		SAILOR_API InstanceId(InstanceId&& inInstanceId) = default;
		SAILOR_API InstanceId(const InstanceId& inComponentId, const InstanceId& inGameObjectId);

		SAILOR_API InstanceId& operator=(const InstanceId& inInstanceId) = default;
		SAILOR_API InstanceId& operator=(InstanceId&& inInstanceId) = default;

		SAILOR_API __forceinline bool operator==(const InstanceId& rhs) const;
		SAILOR_API __forceinline bool operator!=(const InstanceId& rhs) const { return !(rhs == *this); }

		SAILOR_API explicit operator bool() const { return !m_instanceId.IsEmpty() && m_instanceId != InstanceId::Invalid.m_instanceId; }

		SAILOR_API ~InstanceId() = default;

		SAILOR_API virtual YAML::Node Serialize() const override;
		SAILOR_API virtual void Deserialize(const YAML::Node& inData) override;

		SAILOR_API size_t GetHash() const { return m_instanceId.GetHash(); }

		SAILOR_API InstanceId GameObjectId() const;
		SAILOR_API InstanceId ComponentId() const;

	protected:

		StringHash m_instanceId = "NullInstanceId"_h;
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