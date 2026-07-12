#pragma once
#include <string>
#include "Core/YamlSerializable.h"
#include "Core/StringHash.h"
#include "Sailor.h"

namespace Sailor { class InstanceId; }

#if defined(_MSC_VER)
# pragma warning(push)
# pragma warning(disable: 4251)
#endif

namespace Sailor
{
	class SAILOR_SHARED_API InstanceId final : public IYamlSerializable
	{
	public:

		static const InstanceId Invalid;

		static InstanceId GenerateNewInstanceId();
		static InstanceId GenerateNewComponentId(const InstanceId& parentGameObjectId);

		const std::string& ToString() const;

		InstanceId() = default;
		InstanceId(const InstanceId& inInstanceId) = default;
		InstanceId(InstanceId&& inInstanceId) = default;
		InstanceId(const InstanceId& inComponentId, const InstanceId& inGameObjectId);

		InstanceId& operator=(const InstanceId& inInstanceId) = default;
		InstanceId& operator=(InstanceId&& inInstanceId) = default;

		__forceinline bool operator==(const InstanceId& rhs) const;
		__forceinline bool operator!=(const InstanceId& rhs) const { return !(rhs == *this); }

		explicit operator bool() const { return !m_instanceId.IsEmpty() && m_instanceId != InstanceId::Invalid.m_instanceId; }

		~InstanceId() = default;

		virtual YAML::Node Serialize() const override;
		virtual void Deserialize(const YAML::Node& inData) override;

		size_t GetHash() const { return m_instanceId.GetHash(); }

		bool IsGameObjectId() const;
		InstanceId GameObjectId() const;
		InstanceId ComponentId() const;

	protected:

		StringHash m_instanceId = "NullInstanceId"_h;
	};
}

#if defined(_MSC_VER)
# pragma warning(pop)
#endif

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
