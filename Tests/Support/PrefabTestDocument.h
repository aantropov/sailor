#pragma once

#include "EcsTestFixtures.h"
#include "AssetRegistry/Prefab/PrefabImporter.h"
#include "Components/Component.h"
#include <initializer_list>

namespace Sailor
{
	class PrefabReferenceContextTestComponent final : public Component
	{
	public:
		static const TypeInfo& GetStaticTypeInfo() { return TypeInfo::Get<PrefabReferenceContextTestComponent>(); }
		const TypeInfo& GetTypeInfo() const override { return GetStaticTypeInfo(); }
		ReflectedData GetReflectedData() const override { return Reflection::ReflectStatic(this); }
		void ApplyReflection(const ReflectedData& data) override { ApplyReflection_Impl(this, data); }
		bool ResolveRefs(const ReflectedData& data, const TMap<InstanceId, ObjectPtr>& context, bool bImmediate) override
		{
			s_maxContextSize = std::max(s_maxContextSize, context.Num());
			return ResolveRefs_Impl(this, data, context, bImmediate);
		}

		ComponentPtr m_local;
		ComponentPtr m_external;
		GameObjectPtr m_owner;
		inline static size_t s_maxContextSize = 0;

		SAILOR_REFLECTION_AUTO_REGISTRATION(PrefabReferenceContextTestComponent)
	};

	class PrefabRollbackTestComponent final : public Component
	{
		SAILOR_REFLECTABLE(PrefabRollbackTestComponent)

	public:

		PrefabRollbackTestComponent() = default;

		float m_value = 0.0f;
		ComponentPtr m_dependency;
	};

	class PrefabMixedDependencyTestComponent final : public Component
	{
		SAILOR_REFLECTABLE(PrefabMixedDependencyTestComponent)

	public:

		PrefabMixedDependencyTestComponent() = default;

		ComponentPtr m_sourceDependency;
		ComponentPtr m_liveDependency;
	};
}

REFL_AUTO(
	type(Sailor::PrefabReferenceContextTestComponent, bases<Sailor::Component>),
	field(m_local),
	field(m_external),
	field(m_owner)
)

REFL_AUTO(
	type(Sailor::PrefabRollbackTestComponent, bases<Sailor::Component>),
	field(m_value),
	field(m_dependency)
)

REFL_AUTO(
	type(Sailor::PrefabMixedDependencyTestComponent, bases<Sailor::Component>),
	field(m_sourceDependency),
	field(m_liveDependency)
)

namespace Sailor::Tests
{
	class PrefabDocumentTestAsset final : public Prefab
	{
	public:

		PrefabDocumentTestAsset(const FileId& fileId) :
			Prefab(fileId) {}

		static PrefabPtr Capture(
			PrefabTestWorld& world,
			GameObjectPtr root,
			const FileId& fileId = FileId::Invalid)
		{
			auto result =
				TObjectPtr<PrefabDocumentTestAsset>::Make(
					world.GetAllocator(),
					fileId);
			SerializeGameObject(
				root,
				static_cast<uint32_t>(-1),
				result->m_components,
				result->m_gameObjects,
				nullptr);

			std::string diagnostic;
			result->m_bIsReady.store(
				result->ValidateForInstantiation(
					diagnostic),
				std::memory_order_release);
			return result;
		}

		static bool MarkExpandedLinkedRecord(
			PrefabPtr prefab,
			const TMap<InstanceId, InstanceId>& mappings,
			std::string& outDiagnostic)
		{
			auto result =
				prefab.DynamicCast<
					PrefabDocumentTestAsset>();
			if (!result)
			{
				outDiagnostic =
					"the expanded fixture has an unexpected type";
				return false;
			}

			result->m_linkedInstanceIds = mappings;
			result->m_recordType = ERecordType::ExpandedLinkedInstance;
			result->m_detachedSupplementalInstanceIds.
				Clear();
			TSet<InstanceId> mappedLiveIds;
			for (const auto& mapping : mappings)
			{
				mappedLiveIds.Insert(
					*mapping.m_second);
			}
			for (const auto& gameObject :
				result->m_gameObjects)
			{
				if (!mappedLiveIds.Contains(
						gameObject.m_instanceId))
				{
					result->
						m_detachedSupplementalInstanceIds.
							Insert(
								gameObject.m_instanceId);
				}
			}

			return result->ValidateForInstantiation(
				outDiagnostic);
		}
	};

	inline YAML::Node MakePrefabNode(
		std::initializer_list<uint32_t> parentIndices,
		bool bReferenceMissingComponent = false,
		bool bIncludeParentIndex = true)
	{
		YAML::Node gameObjects(YAML::NodeType::Sequence);
		uint32_t index = 0;
		for (const uint32_t parentIndex : parentIndices)
		{
			Prefab::ReflectedGameObject gameObject{};
			gameObject.m_name = "GameObject" + std::to_string(index++);
			gameObject.m_position = glm::vec4(0.0f);
			gameObject.m_rotation = glm::identity<glm::quat>();
			gameObject.m_scale = glm::vec4(1.0f);
			gameObject.m_parentIndex = parentIndex;
			gameObject.m_instanceId = InstanceId::GenerateNewInstanceId();
			if (bReferenceMissingComponent)
			{
				gameObject.m_components.Add(0);
			}
			YAML::Node gameObjectNode = gameObject.Serialize();
			if (!bIncludeParentIndex)
			{
				gameObjectNode.remove("parentIndex");
			}
			gameObjects.push_back(std::move(gameObjectNode));
		}

		YAML::Node prefabNode;
		prefabNode["gameObjects"] = std::move(gameObjects);
		prefabNode["components"] = YAML::Node(YAML::NodeType::Sequence);
		return prefabNode;
	}

	inline YAML::Node MakeReflectedComponent(
		const std::string& componentInstanceId,
		const YAML::Node& overrideProperties,
		bool bIncludeInstanceId = true,
		const std::string& typeName =
			PrefabRollbackTestComponent::GetStaticTypeInfo().Name())
	{
		YAML::Node component;
		component["typename"] = typeName;
		component["overrideProperties"] = overrideProperties;
		if (bIncludeInstanceId)
		{
			component["overrideProperties"]["instanceId"] = componentInstanceId;
		}
		return component;
	}

	inline YAML::Node MakeComponentPrefabNode(const YAML::Node& components)
	{
		constexpr uint32_t noParent = static_cast<uint32_t>(-1);
		YAML::Node prefabNode = MakePrefabNode({ noParent });
		prefabNode["components"] = components;
		prefabNode["gameObjects"][0]["instanceId"] = "10010010010010010000";
		prefabNode["gameObjects"][0]["components"] = YAML::Node(YAML::NodeType::Sequence);
		for (uint32_t componentIndex = 0; componentIndex < components.size(); ++componentIndex)
		{
			prefabNode["gameObjects"][0]["components"].push_back(componentIndex);
		}
		return prefabNode;
	}

	inline PrefabPtr DeserializePrefab(PrefabTestWorld& world, const YAML::Node& node)
	{
		PrefabPtr prefab = PrefabPtr::Make(world.GetAllocator(), FileId());
		prefab->Deserialize(node);
		return prefab;
	}

	inline FileId DeserializeFileId(const char* value)
	{
		FileId fileId;
		fileId.Deserialize(YAML::Node(value));
		return fileId;
	}

	inline PrefabPtr DeserializePrefab(
		PrefabTestWorld& world,
		const FileId& fileId,
		const YAML::Node& node)
	{
		PrefabPtr prefab = PrefabPtr::Make(world.GetAllocator(), fileId);
		prefab->Deserialize(node);
		return prefab;
	}
}
