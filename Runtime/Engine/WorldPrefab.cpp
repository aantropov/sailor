#include "Engine/World.h"
#include "Engine/GameObject.h"
#include "AssetRegistry/Prefab/PrefabImporter.h"
#include "Core/LogMacros.h"
#include "Core/Reflection.h"
#include "ECS/TransformECS.h"
#include "YamlExceptionBoundary.h"

using namespace Sailor;

namespace
{
	void AddReferencedWorldObjects(const ReflectedData& reflection, const World& world,
		TMap<InstanceId, ObjectPtr>& references)
	{
		const auto& types = reflection.GetTypeInfo().Properties();
		for (const auto& property : reflection.GetProperties())
		{
			const YAML::Node& value = *property.m_second;
			if (!types.ContainsKey(property.m_first) || !types[property.m_first].starts_with("TObjectPtr<") || !value.IsMap())
				continue;
			const auto idNode = value["instanceId"];
			if (!idNode.IsScalar()) continue;
			const auto id = idNode.as<InstanceId>();
			if (!id || references.ContainsKey(id)) continue;
			const auto ownerId = id.GameObjectId();
			// Internal aliases always win, including component lookup through a local owner.
			if (references.ContainsKey(ownerId)) continue;
			if (auto owner = world.GetObjectByInstanceId(ownerId)) references[ownerId] = std::move(owner);
		}
	}

	ReflectedData RemapPendingReferences(
		const ReflectedData& reflection,
		const TMap<InstanceId, ObjectPtr>& internalDependencies)
	{
		YAML::Node properties(YAML::NodeType::Map);
		const auto& propertyTypes = reflection.GetTypeInfo().Properties();
		for (const auto& property : reflection.GetProperties())
		{
			YAML::Node value = YAML::Clone(*property.m_second);
			if (propertyTypes.ContainsKey(property.m_first) &&
				propertyTypes[property.m_first].starts_with("TObjectPtr<") && value.IsMap())
			{
				const YAML::Node instanceIdNode = static_cast<const YAML::Node&>(value)["instanceId"];
				if (instanceIdNode.IsScalar())
				{
					const InstanceId sourceId = instanceIdNode.as<InstanceId>();
					if (internalDependencies.ContainsKey(sourceId))
					{
						value["instanceId"] = internalDependencies[sourceId]->GetInstanceId();
					}
				}
			}
			properties[property.m_first] = std::move(value);
		}
		return Reflection::CreateReflectedData(reflection.GetTypeInfo(), properties);
	}
}

namespace Sailor
{
	class World::PrefabInstantiationTransaction final
	{
	public:

		PrefabInstantiationTransaction(
			World& world,
			TVector<GameObjectPtr>& gameObjects) :
			m_world(world),
			m_gameObjects(gameObjects)
		{}

		~PrefabInstantiationTransaction() noexcept
		{
			if (m_bCommitted)
			{
				return;
			}

			for (const auto& object : m_gameObjects)
			{
				for (const auto& component : object->GetComponents())
				{
					m_world.RemovePendingDependencyResolutions(component);
				}
			}

			for (size_t index = m_gameObjects.Num(); index > 0; --index)
			{
				m_world.DestroyImmediate(m_gameObjects[index - 1]);
			}
		}

		void Commit() { m_bCommitted = true; }

	private:

		World& m_world;
		TVector<GameObjectPtr>& m_gameObjects;
		bool m_bCommitted = false;
	};
}

bool World::ValidatePrefabInstanceIds(const PrefabPtr& prefab, EPrefabInstanceIdPolicy idPolicy) const
{
	if (prefab->IsLinkedInstanceRecord())
	{
		TMap<InstanceId, InstanceId> dependencyAliasTargets;
		auto registerDependencyAlias =
			[&dependencyAliasTargets](
				const InstanceId& alias,
				const InstanceId& target)
			{
				if (!alias || !target)
				{
					return false;
				}

				if (dependencyAliasTargets.ContainsKey(alias))
				{
					return dependencyAliasTargets[alias] ==
						target;
				}

				dependencyAliasTargets[alias] = target;
				return true;
			};

		for (const auto& sourceGameObject :
			prefab->m_gameObjects)
		{
			const InstanceId& sourceInstanceId =
				sourceGameObject.m_instanceId;
			const InstanceId desiredGameObjectId =
				prefab->m_linkedInstanceIds.ContainsKey(
					sourceInstanceId)
					? prefab->m_linkedInstanceIds[
						sourceInstanceId]
					: sourceInstanceId;
			if (!registerDependencyAlias(
					sourceInstanceId,
					desiredGameObjectId) ||
				!registerDependencyAlias(
					desiredGameObjectId,
					desiredGameObjectId))
			{
				SAILOR_LOG_ERROR(
					"Cannot instantiate linked prefab '%s': source and live game object dependency aliases are ambiguous.",
					prefab->GetFileId().ToString().c_str());
				return false;
			}

			for (const uint32_t componentIndex :
				sourceGameObject.m_components)
			{
				const ReflectedData& reflection =
					prefab->m_components[componentIndex];
				InstanceId sourceComponentId;
				std::string conversionDiagnostic;
				if (!Utils::TryGetComponentInstanceId(
						reflection,
						sourceComponentId,
						conversionDiagnostic))
				{
					SAILOR_LOG_ERROR(
						"Cannot instantiate reflected component %u from linked prefab '%s': %s.",
						componentIndex,
						prefab->GetFileId().ToString().c_str(),
						conversionDiagnostic.c_str());
					return false;
				}

				const InstanceId desiredComponentId(
					sourceComponentId.ComponentId(),
					desiredGameObjectId);
				if (!registerDependencyAlias(
						sourceComponentId,
						desiredComponentId) ||
					!registerDependencyAlias(
						desiredComponentId,
						desiredComponentId))
				{
					SAILOR_LOG_ERROR(
						"Cannot instantiate linked prefab '%s': source and live component dependency aliases are ambiguous.",
						prefab->GetFileId().ToString().c_str());
					return false;
				}
			}
		}
	}

	if (idPolicy == EPrefabInstanceIdPolicy::RequireExact)
	{
		TSet<InstanceId> desiredGameObjectIds;
		TSet<InstanceId> desiredComponentIds;

		for (const auto& sourceGameObject : prefab->m_gameObjects)
		{
			const InstanceId& sourceInstanceId =
				sourceGameObject.m_instanceId;
			InstanceId desiredGameObjectId = sourceInstanceId;
			if (prefab->IsLinkedInstanceRecord())
			{
				if (prefab->m_linkedInstanceIds.ContainsKey(
						sourceInstanceId))
				{
					desiredGameObjectId =
						prefab->m_linkedInstanceIds[
							sourceInstanceId];
				}
				else if (prefab->m_detachedSupplementalInstanceIds.Contains(
						sourceInstanceId))
				{
					desiredGameObjectId = sourceInstanceId;
				}
				else
				{
					SAILOR_LOG_ERROR(
						"Cannot strictly instantiate linked prefab '%s': a game object is neither mapped source data nor detached supplemental data.",
						prefab->GetFileId().ToString().c_str());
					return false;
				}
			}

			if (!desiredGameObjectId.IsGameObjectId() ||
				m_objectsMap.ContainsKey(desiredGameObjectId) ||
				!desiredGameObjectIds.Insert(desiredGameObjectId))
			{
				SAILOR_LOG_ERROR(
					"Cannot strictly instantiate prefab '%s': game object id '%s' is invalid, duplicated, or already in use.",
					prefab->GetFileId().ToString().c_str(),
					desiredGameObjectId.ToString().c_str());
				return false;
			}

			for (const uint32_t componentIndex :
				sourceGameObject.m_components)
			{
				const ReflectedData& reflection =
					prefab->m_components[componentIndex];
				InstanceId sourceComponentId;
				std::string conversionDiagnostic;
				if (!Utils::TryGetComponentInstanceId(
						reflection,
						sourceComponentId,
						conversionDiagnostic))
				{
					SAILOR_LOG_ERROR(
						"Cannot strictly instantiate reflected component %u from prefab '%s': %s.",
						componentIndex,
						prefab->GetFileId().ToString().c_str(),
						conversionDiagnostic.c_str());
					return false;
				}

				// Component IDs include their owner, whose ID was checked free above.
				const InstanceId desiredComponentId(
					sourceComponentId.ComponentId(),
					desiredGameObjectId);
				if (!desiredComponentId ||
					!desiredComponentIds.Insert(desiredComponentId))
				{
					SAILOR_LOG_ERROR(
						"Cannot strictly instantiate prefab '%s': component id '%s' is invalid, duplicated, or already in use.",
						prefab->GetFileId().ToString().c_str(),
						desiredComponentId.ToString().c_str());
					return false;
				}
			}
		}
	}

	return true;
}

GameObjectPtr World::Instantiate(
	PrefabPtr prefab,
	EPrefabInstanceIdPolicy idPolicy)
{
	const bool bStrictInstanceIds = idPolicy == EPrefabInstanceIdPolicy::RequireExact;
	if (!prefab || prefab->m_gameObjects.IsEmpty())
	{
		SAILOR_LOG_ERROR("Cannot instantiate an invalid or empty prefab.");
		return {};
	}

	std::string validationDiagnostic;
	if (!prefab->ValidateForInstantiation(validationDiagnostic))
	{
		SAILOR_LOG_ERROR("Cannot instantiate prefab '%s': %s.",
			prefab->GetFileId().ToString().c_str(),
			validationDiagnostic.c_str());
		return {};
	}

	if (prefab->IsLinkedPrefabSnapshotRecord())
	{
		SAILOR_LOG_ERROR(
			"Cannot instantiate linked prefab snapshot directly; it must be resolved against its current source first.");
		return {};
	}

	if (prefab->m_recordType == Prefab::ERecordType::ExpandedLinkedInstance)
	{
		SAILOR_LOG_ERROR(
			"Cannot instantiate an expanded linked serialization record directly.");
		return {};
	}

	GameObjectPtr detachedParent;
	if (prefab->IsDetachedFromPrefabRecord())
	{
		if (!bStrictInstanceIds)
		{
			SAILOR_LOG_ERROR(
				"Cannot instantiate detached prefab snapshot: exact instance ids are required.");
			return {};
		}

		detachedParent = GetObjectByInstanceId(
			prefab->m_detachedParentInstanceId).DynamicCast<GameObject>();
		if (!detachedParent ||
			!IsPrefabLinked(
				prefab->m_detachedParentInstanceId))
		{
			SAILOR_LOG_ERROR(
				"Cannot instantiate detached prefab snapshot: parent '%s' is missing or is not a linked prefab member.",
				prefab->m_detachedParentInstanceId.ToString().c_str());
			return {};
		}
	}

	if (!ValidatePrefabInstanceIds(prefab, idPolicy))
	{
		return {};
	}

	TVector<GameObjectPtr> gameObjects;
	gameObjects.Reserve(prefab->m_gameObjects.Num());
	TMap<InstanceId, ObjectPtr> internalDependencies;
	TMap<InstanceId, InstanceId> sourceToInstanceIds;
	TSet<InstanceId> reservedInstanceIds;
	PrefabInstantiationTransaction transaction(*this, gameObjects);

	for (uint32_t j = 0; j < prefab->m_gameObjects.Num(); j++)
	{
		const InstanceId& sourceInstanceId = prefab->m_gameObjects[j].m_instanceId;
		InstanceId gameObjectId;
		if (prefab->IsLinkedInstanceRecord())
		{
			if (prefab->m_linkedInstanceIds.ContainsKey(
					sourceInstanceId))
			{
				gameObjectId =
					prefab->m_linkedInstanceIds[
						sourceInstanceId];
				sourceToInstanceIds[sourceInstanceId] =
					gameObjectId;
			}
			else if (prefab->m_detachedSupplementalInstanceIds.Contains(
					sourceInstanceId))
			{
				gameObjectId = sourceInstanceId;
			}
			else
			{
				SAILOR_LOG_ERROR(
					"Cannot instantiate linked prefab '%s': a game object is neither mapped source data nor detached supplemental data.",
					prefab->GetFileId().ToString().c_str());
				return {};
			}

			if (!gameObjectId.IsGameObjectId() ||
				m_objectsMap.ContainsKey(gameObjectId) ||
				!reservedInstanceIds.Insert(gameObjectId))
			{
				SAILOR_LOG_ERROR(
					"Cannot instantiate linked prefab '%s': preferred game object id '%s' is invalid or already in use.",
					prefab->GetFileId().ToString().c_str(),
					gameObjectId.ToString().c_str());
				return {};
			}
		}
		else
		{
			gameObjectId = sourceInstanceId;
			if (!bStrictInstanceIds &&
				(idPolicy == EPrefabInstanceIdPolicy::GenerateNew ||
					!gameObjectId ||
					m_objectsMap.ContainsKey(gameObjectId) ||
					reservedInstanceIds.Contains(gameObjectId)))
			{
				do
				{
					gameObjectId = InstanceId::GenerateNewInstanceId();
				}
				while (m_objectsMap.ContainsKey(gameObjectId) || reservedInstanceIds.Contains(gameObjectId));
			}

			reservedInstanceIds.Insert(gameObjectId);
			sourceToInstanceIds[sourceInstanceId] =
				gameObjectId;
		}

		GameObjectPtr gameObject = NewGameObject(prefab->m_gameObjects[j].m_name, gameObjectId);
		gameObjects.Add(gameObject);
		gameObject->SetMobilityType(
			prefab->m_gameObjects[j].m_mobilityType);

		auto& transform = gameObject->GetTransformComponent();
		transform.SetPosition(prefab->m_gameObjects[j].m_position);
		transform.SetRotation(prefab->m_gameObjects[j].m_rotation);
		transform.SetScale(prefab->m_gameObjects[j].m_scale);

		for (uint32_t i = 0; i < prefab->m_gameObjects[j].m_components.Num(); i++)
		{
			const uint32_t componentIndex = prefab->m_gameObjects[j].m_components[i];
			const ReflectedData& reflection = prefab->m_components[componentIndex];
			InstanceId oldInstanceId;
			std::string conversionDiagnostic;
			if (!Utils::TryGetComponentInstanceId(
					reflection,
					oldInstanceId,
					conversionDiagnostic))
			{
				SAILOR_LOG_ERROR(
					"Cannot instantiate reflected component %u from prefab '%s': %s.",
					componentIndex,
					prefab->GetFileId().ToString().c_str(),
					conversionDiagnostic.c_str());
				return {};
			}

			ComponentPtr newComponent = Reflection::CreateObject<Component>(reflection.GetTypeInfo(), GetAllocator());
			if (!newComponent)
			{
				SAILOR_LOG_ERROR(
					"Cannot instantiate reflected component type '%s' from prefab '%s'.",
					reflection.GetTypeInfo().Name().c_str(),
					prefab->GetFileId().ToString().c_str());
				return {};
			}

			const InstanceId newComponentInstanceId(oldInstanceId.ComponentId(), gameObject->GetInstanceId());
			if (!gameObject->AddComponentRaw(newComponent, newComponentInstanceId))
			{
				SAILOR_LOG_ERROR(
					"Cannot instantiate reflected component %u from prefab '%s': component id '%s' is invalid or already in use.",
					componentIndex,
					prefab->GetFileId().ToString().c_str(),
					newComponentInstanceId.ToString().c_str());
				return {};
			}

			std::string applyDiagnostic;
			if (!External::GuardYamlExceptions(
					[newComponent, &reflection]() mutable
					{
						newComponent->ApplyReflection(reflection);
					},
					applyDiagnostic))
			{
				SAILOR_LOG_ERROR(
					"Cannot apply reflected component %u from prefab '%s': %s.",
					componentIndex,
					prefab->GetFileId().ToString().c_str(),
					applyDiagnostic.c_str());
				return {};
			}

			// We store the old ids for internal dependencies during resolve
			internalDependencies[oldInstanceId] = newComponent;
			internalDependencies[newComponentInstanceId] =
				newComponent;
		}

		// We store the old ids for internal dependencies during resolve
		internalDependencies[prefab->m_gameObjects[j].m_instanceId] = gameObject;
		internalDependencies[gameObject->GetInstanceId()] =
			gameObject;
	}

	TMap<InstanceId, ObjectPtr> resolveContext = internalDependencies;

	for (uint32_t goIndex = 0; goIndex < gameObjects.Num(); goIndex++)
	{
		auto& go = gameObjects[goIndex];
		check(goIndex < prefab->m_gameObjects.Num());
		const auto& prefabGo = prefab->m_gameObjects[goIndex];

		for (uint32_t componentOrder = 0; componentOrder < go->m_components.Num(); componentOrder++)
		{
			check(componentOrder < prefabGo.m_components.Num());

			auto& newComp = go->m_components[componentOrder];
			const uint32_t componentIndex = prefabGo.m_components[componentOrder];
			check(componentIndex < prefab->m_components.Num());

			const ReflectedData& reflection = prefab->m_components[componentIndex];

			bool bResolved = false;
			std::string resolveDiagnostic;
			if (!External::TryInvokeYaml(
					[this, newComp, &reflection, &resolveContext]() mutable
					{
						AddReferencedWorldObjects(reflection, *this, resolveContext);
						return newComp->ResolveRefs(
							reflection,
							resolveContext,
							true);
					},
					bResolved,
					resolveDiagnostic))
			{
				SAILOR_LOG_ERROR(
					"Cannot resolve reflected component %u from prefab '%s': %s.",
					componentIndex,
					prefab->GetFileId().ToString().c_str(),
					resolveDiagnostic.c_str());
				return {};
			}
			newComp->m_bDependenciesResolved = bResolved;
			if (!bResolved)
			{
				// Retry against live IDs; source IDs may belong to another prefab instance.
				QueuePendingDependencyResolution(newComp, RemapPendingReferences(reflection, internalDependencies));
			}
		}
	}

	GameObjectPtr root;

	for (uint32_t i = 0; i < gameObjects.Num(); i++)
	{
		auto& go = gameObjects[i];
		uint32_t parentIndex = prefab->m_gameObjects[i].m_parentIndex;

		if (parentIndex != -1)
		{
			go->SetParent(gameObjects[parentIndex]);
		}
		else
		{
			root = go;
		}
	}

	if (!root)
	{
		SAILOR_LOG_ERROR("Cannot instantiate prefab '%s': no root game object was found.",
			prefab->GetFileId().ToString().c_str());
		return {};
	}

	if (prefab->IsLinkedInstanceRecord() && prefab->m_linkedParentInstanceId)
	{
		GameObjectPtr externalParent =
			GetObjectByInstanceId(prefab->m_linkedParentInstanceId).DynamicCast<GameObject>();
		if (!externalParent)
		{
			SAILOR_LOG_ERROR(
				"Cannot instantiate linked prefab '%s': external parent '%s' does not exist.",
				prefab->GetFileId().ToString().c_str(),
				prefab->m_linkedParentInstanceId.ToString().c_str());
			return {};
		}

		root->SetParent(externalParent);
		if (root->GetParent() != externalParent)
		{
			SAILOR_LOG_ERROR(
				"Cannot instantiate linked prefab '%s': external parent '%s' rejects structural changes.",
				prefab->GetFileId().ToString().c_str(),
				prefab->m_linkedParentInstanceId.ToString().c_str());
			return {};
		}
	}

	if (prefab->IsDetachedFromPrefabRecord())
	{
		root->SetParentInternal(
			detachedParent,
			true);
		if (root->GetParent() != detachedParent)
		{
			SAILOR_LOG_ERROR(
				"Cannot restore detached prefab snapshot under linked parent '%s'.",
				prefab->m_detachedParentInstanceId.ToString().c_str());
			return {};
		}
	}

	// Gameplay copies do not retain authoring links to the source asset.
	if (prefab->GetFileId() && !(m_mask & (uint8_t)EWorldBehaviourBit::CallBeginPlay))
	{
		std::string linkDiagnostic;
		if (!RegisterPrefabInstance(
				root,
				prefab->GetFileId(),
				sourceToInstanceIds,
				prefab,
				linkDiagnostic))
		{
			SAILOR_LOG_ERROR(
				"Cannot register linked prefab '%s': %s.",
				prefab->GetFileId().ToString().c_str(),
				linkDiagnostic.c_str());
			return {};
		}
	}

	transaction.Commit();
	return root;
}
