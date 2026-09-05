#pragma once
#include <initializer_list>
#include <string>
#include <string_view>
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
		static InstanceId GenerateDeterministic(
			std::initializer_list<std::string_view> values,
			uint32_t variant = 0);
		static InstanceId FromHash(uint64_t hash, uint16_t suffix);

		const std::string& ToString() const;

		InstanceId() = default;
		explicit InstanceId(std::string_view value);
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

		enum class EKind : uint8_t
		{
			Invalid,
			GameObject,
			Component
		};

		void Assign(std::string_view value);

		StringHash m_instanceId = "NullInstanceId"_h;
		StringHash m_gameObjectId{};
		StringHash m_componentId{};
		EKind m_kind = EKind::Invalid;
		bool m_bStandaloneComponentIsGameObjectId = false;
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
			return p.GetHash();
		}
	};
}
