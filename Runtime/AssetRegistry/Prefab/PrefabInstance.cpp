#include "PrefabInstance.h"
#include "AssetRegistry/World/WorldPrefabImporter.h"
#include "Core/Utils.h"
#include "YamlExceptionBoundary.h"

using namespace Sailor;

namespace
{
	InstanceId NormalizeInstanceId(
		const InstanceId& liveInstanceId,
		const TMap<InstanceId, InstanceId>& instanceToSourceIds)
	{
		if (!liveInstanceId)
		{
			return liveInstanceId;
		}

		const InstanceId liveGameObjectId = liveInstanceId.GameObjectId();
		if (!instanceToSourceIds.ContainsKey(liveGameObjectId))
		{
			return liveInstanceId;
		}

		const InstanceId& sourceGameObjectId = instanceToSourceIds[liveGameObjectId];
		return liveInstanceId.IsGameObjectId()
			? sourceGameObjectId
			: InstanceId(liveInstanceId.ComponentId(), sourceGameObjectId);
	}

	InstanceId MakeDeterministicLinkedInstanceId(
		const FileId& sourcePrefabId,
		const std::string& instanceSeed,
		const InstanceId& sourceInstanceId,
		uint32_t collisionAttempt)
	{
		return InstanceId::GenerateDeterministic(
			{
				sourcePrefabId.ToString(),
				instanceSeed,
				sourceInstanceId.ToString()
			},
			collisionAttempt);
	}

	bool TryMergeComponentOverride(
		const ReflectedData& base,
		const ReflectedData& delta,
		ReflectedData& outMerged,
		std::string& outDiagnostic)
	{
		if (!base.IsValid() || !delta.IsValid() || base.GetTypeInfo() != delta.GetTypeInfo())
		{
			outDiagnostic = "the component override type does not match the source component";
			return false;
		}

		YAML::Node merged = base.Serialize();
		YAML::Node mergedProperties = merged["overrideProperties"];
		for (const auto& property : delta.GetProperties())
		{
			if (property.m_first == "instanceId" || property.m_first == "fileId")
			{
				outDiagnostic = "component identity properties cannot be overridden";
				return false;
			}

			mergedProperties[property.m_first] = YAML::Clone(*property.m_second);
		}

		if (!External::GuardYamlExceptions(
				[&outMerged, &merged]()
				{
					outMerged.Deserialize(merged);
				},
				outDiagnostic))
		{
			return false;
		}

		return outMerged.IsValid();
	}
}

bool PrefabInstance::Snapshot::Build(PrefabPtr prefab, std::string& outDiagnostic)
{
	m_prefab = nullptr;
	m_gameObjects.Clear();
	m_components.Clear();
	m_componentIds.Clear();
	if (!prefab || !prefab->ValidateForInstantiation(outDiagnostic))
	{
		return false;
	}

	for (uint32_t i = 0; i < prefab->m_gameObjects.Num(); ++i)
	{
		m_gameObjects[prefab->m_gameObjects[i].m_instanceId] = i;
	}
	m_componentIds.Reserve(prefab->m_components.Num());
	for (uint32_t i = 0; i < prefab->m_components.Num(); ++i)
	{
		const InstanceId id = prefab->m_components[i].GetProperties()["instanceId"].as<InstanceId>();
		m_components[id] = i;
		m_componentIds.Add(id);
	}
	m_prefab = std::move(prefab);
	return true;
}

const Prefab::ReflectedGameObject* PrefabInstance::Snapshot::FindGameObject(const InstanceId& id) const
{
	const uint32_t* index = nullptr;
	return m_gameObjects.Find(id, index) ? &m_prefab->m_gameObjects[*index] : nullptr;
}

const ReflectedData* PrefabInstance::Snapshot::FindComponent(const InstanceId& id) const
{
	const uint32_t* index = nullptr;
	return m_components.Find(id, index) ? &m_prefab->m_components[*index] : nullptr;
}

YAML::Node PrefabInstance::NormalizeReferences(
	const YAML::Node& node,
	const TMap<InstanceId, InstanceId>& instanceToSourceIds)
{
	if (!node)
	{
		return YAML::Node();
	}

	if (node.IsScalar() || node.IsNull())
	{
		return YAML::Clone(node);
	}

	YAML::Node normalized;
	if (node.IsSequence())
	{
		normalized = YAML::Node(YAML::NodeType::Sequence);
		for (const auto& child : node)
		{
			normalized.push_back(NormalizeReferences(child, instanceToSourceIds));
		}
		return normalized;
	}

	normalized = YAML::Node(YAML::NodeType::Map);
	for (const auto& property : node)
	{
		normalized[YAML::Clone(property.first)] =
			NormalizeReferences(property.second, instanceToSourceIds);
	}

	if (normalized["instanceId"])
	{
		InstanceId liveInstanceId;
		std::string conversionDiagnostic;
		if (External::TryConvertYaml(
				normalized["instanceId"],
				liveInstanceId,
				conversionDiagnostic))
		{
			normalized["instanceId"] = NormalizeInstanceId(
				liveInstanceId,
				instanceToSourceIds);
		}
	}

	return normalized;
}

bool Prefab::ConfigureLinkedInstance(
	const PrefabPtr& basePrefab,
	const TMap<InstanceId, InstanceId>& sourceToInstanceIds,
	const InstanceId& parentInstanceId,
	const TMap<InstanceId, YAML::Node>& gameObjectOverrides,
	const TMap<InstanceId, ReflectedData>& componentOverrides,
	std::string& outDiagnostic)
{
	if (basePrefab && basePrefab.GetRawPtr() == this)
	{
		ResetData();
		outDiagnostic = "a linked prefab cannot use itself as its source";
		return false;
	}
	if (!basePrefab || !basePrefab->GetFileId() || basePrefab->GetFileId() != GetFileId())
	{
		ResetData();
		outDiagnostic = "the linked prefab source is missing or has a mismatched FileId";
		return false;
	}

	PrefabInstance::Snapshot source;
	if (!source.Build(basePrefab, outDiagnostic))
	{
		ResetData();
		return false;
	}
	return ConfigureLinkedInstance(source, sourceToInstanceIds, parentInstanceId,
		gameObjectOverrides, componentOverrides, outDiagnostic);
}

bool Prefab::ConfigureLinkedInstance(
	const PrefabInstance::Snapshot& source,
	const TMap<InstanceId, InstanceId>& sourceToInstanceIds,
	const InstanceId& parentInstanceId,
	const TMap<InstanceId, YAML::Node>& gameObjectOverrides,
	const TMap<InstanceId, ReflectedData>& componentOverrides,
	std::string& outDiagnostic)
{
	outDiagnostic.clear();
	ResetData();
	const auto& basePrefab = source.m_prefab;

	if (sourceToInstanceIds.Num() != basePrefab->m_gameObjects.Num())
	{
		outDiagnostic = "the linked prefab instance mapping does not cover every source game object";
		return false;
	}

	TSet<InstanceId> liveInstanceIds;
	for (const auto& gameObject : basePrefab->m_gameObjects)
	{
		if (!sourceToInstanceIds.ContainsKey(gameObject.m_instanceId))
		{
			outDiagnostic = "the linked prefab instance mapping is missing source game object " +
				gameObject.m_instanceId.ToString();
			return false;
		}

		const InstanceId& liveInstanceId = sourceToInstanceIds[gameObject.m_instanceId];
		if (!liveInstanceId.IsGameObjectId() || !liveInstanceIds.Insert(liveInstanceId))
		{
			outDiagnostic = "the linked prefab instance mapping contains an invalid or duplicate live game object id";
			return false;
		}
	}

	m_gameObjects = basePrefab->m_gameObjects;
	m_components = basePrefab->m_components;

	for (const auto& overrideEntry : gameObjectOverrides)
	{
		const uint32_t* objectIndex = nullptr;
		ReflectedGameObject* target = source.m_gameObjects.Find(overrideEntry.m_first, objectIndex)
			? &m_gameObjects[*objectIndex] : nullptr;

		if (!target || !overrideEntry.m_second || !overrideEntry.m_second->IsMap())
		{
			outDiagnostic = "the linked prefab contains a game object override for an unknown source id";
			return false;
		}

		const YAML::Node& properties = *overrideEntry.m_second;
		for (const auto& property : properties)
		{
			const std::string name = property.first.as<std::string>();
			if (name != "name" && name != "mobilityType" && name != "position" && name != "rotation" && name != "scale")
			{
				outDiagnostic = "unsupported linked game object override property '" + name + "'";
				return false;
			}
		}

		if (!External::GuardYamlExceptions(
				[&properties, target]()
				{
					::Deserialize(properties, "name", target->m_name);
					::Deserialize(properties, "mobilityType", target->m_mobilityType);
					::Deserialize(properties, "position", target->m_position);
					::Deserialize(properties, "rotation", target->m_rotation);
					::Deserialize(properties, "scale", target->m_scale);
				},
				outDiagnostic))
		{
			return false;
		}
	}

	for (const auto& overrideEntry : componentOverrides)
	{
		const uint32_t* componentIndex = nullptr;
		if (!source.m_components.Find(overrideEntry.m_first, componentIndex))
		{
			outDiagnostic = "the linked prefab contains a component override for an unknown source id";
			return false;
		}

		ReflectedData merged;
		if (!TryMergeComponentOverride(
				m_components[*componentIndex],
				*overrideEntry.m_second,
				merged,
				outDiagnostic))
		{
			return false;
		}

		m_components[*componentIndex] = std::move(merged);
	}

	m_linkedInstanceIds = sourceToInstanceIds;
	m_linkedParentInstanceId = parentInstanceId;
	m_gameObjectOverrides = gameObjectOverrides;
	m_componentOverrides = componentOverrides;
	m_bLinkedInstanceRecord = true;

	if (!ValidateForInstantiation(outDiagnostic))
	{
		return false;
	}

	m_bIsReady.store(true, std::memory_order_release);
	return true;
}

bool WorldPrefab::ReconcileLinkedInstanceIds(
	const PrefabPtr& expandedPrefab,
	const PrefabPtr& sourcePrefab,
	const TMap<InstanceId, InstanceId>& savedSourceToInstanceIds,
	TSet<InstanceId>& reservedInstanceIds,
	TMap<InstanceId, InstanceId>& outSourceToInstanceIds,
	std::string& outDiagnostic)
{
	outSourceToInstanceIds.Clear();
	if (!expandedPrefab || !sourcePrefab || !sourcePrefab->GetFileId())
	{
		outDiagnostic = "the expanded or source prefab is missing";
		return false;
	}
	PrefabInstance::Snapshot expanded, source;
	return expanded.Build(expandedPrefab, outDiagnostic) && source.Build(sourcePrefab, outDiagnostic) &&
		ReconcileLinkedInstanceIds(expanded, source, savedSourceToInstanceIds,
			reservedInstanceIds, outSourceToInstanceIds, outDiagnostic);
}

bool WorldPrefab::ReconcileLinkedInstanceIds(
	const PrefabInstance::Snapshot& expanded,
	const PrefabInstance::Snapshot& source,
	const TMap<InstanceId, InstanceId>& savedSourceToInstanceIds,
	TSet<InstanceId>& reservedInstanceIds,
	TMap<InstanceId, InstanceId>& outSourceToInstanceIds,
	std::string& outDiagnostic)
{
	const auto& expandedPrefab = expanded.m_prefab;
	const auto& sourcePrefab = source.m_prefab;
	outDiagnostic.clear();
	outSourceToInstanceIds.Clear();
	std::string instanceSeed;
	auto considerSeed = [&instanceSeed](const InstanceId& instanceId)
		{
			if (!instanceId.IsGameObjectId())
			{
				return;
			}

			const std::string& candidate = instanceId.ToString();
			if (instanceSeed.empty() || candidate < instanceSeed)
			{
				instanceSeed = candidate;
			}
		};

	for (const auto& savedMapping : savedSourceToInstanceIds)
	{
		considerSeed(*savedMapping.m_second);
	}
	for (const auto& expandedGameObject : expandedPrefab->m_gameObjects)
	{
		considerSeed(expandedGameObject.m_instanceId);
	}

	if (instanceSeed.empty())
	{
		outDiagnostic = "the linked prefab record has no stable live instance seed";
		return false;
	}

	TSet<InstanceId> assignedInstanceIds;
	for (const auto& sourceGameObject : sourcePrefab->m_gameObjects)
	{
		InstanceId liveInstanceId;
		if (savedSourceToInstanceIds.ContainsKey(
				sourceGameObject.m_instanceId))
		{
			liveInstanceId = savedSourceToInstanceIds[
				sourceGameObject.m_instanceId];
			if (!liveInstanceId.IsGameObjectId())
			{
				outDiagnostic = "the saved linked prefab mapping contains an invalid live game object id";
				return false;
			}

			if (!assignedInstanceIds.Insert(liveInstanceId))
			{
				outDiagnostic = "the saved linked prefab mapping contains duplicate live game object ids";
				return false;
			}
		}
		else
		{
			uint32_t collisionAttempt = 0;
			do
			{
				liveInstanceId = MakeDeterministicLinkedInstanceId(
					sourcePrefab->GetFileId(),
					instanceSeed,
					sourceGameObject.m_instanceId,
					collisionAttempt++);
			}
			while (reservedInstanceIds.Contains(liveInstanceId) ||
				assignedInstanceIds.Contains(liveInstanceId));

			assignedInstanceIds.Insert(liveInstanceId);
			reservedInstanceIds.Insert(liveInstanceId);
		}

		outSourceToInstanceIds[sourceGameObject.m_instanceId] =
			liveInstanceId;
	}

	return true;
}

bool WorldPrefab::BuildLinkedOverrides(
	const PrefabPtr& expandedPrefab,
	const PrefabPtr& sourcePrefab,
	const TMap<InstanceId, InstanceId>& sourceToInstanceIds,
	TMap<InstanceId, YAML::Node>& outGameObjectOverrides,
	TMap<InstanceId, ReflectedData>& outComponentOverrides,
	std::string& outDiagnostic)
{
	outGameObjectOverrides.Clear();
	outComponentOverrides.Clear();
	if (!expandedPrefab || !sourcePrefab)
	{
		outDiagnostic = "the expanded or source prefab is missing";
		return false;
	}
	PrefabInstance::Snapshot expanded, source;
	return expanded.Build(expandedPrefab, outDiagnostic) && source.Build(sourcePrefab, outDiagnostic) &&
		BuildLinkedOverrides(expanded, source, sourceToInstanceIds,
			outGameObjectOverrides, outComponentOverrides, outDiagnostic);
}

bool WorldPrefab::BuildLinkedOverrides(
	const PrefabInstance::Snapshot& expanded,
	const PrefabInstance::Snapshot& source,
	const TMap<InstanceId, InstanceId>& sourceToInstanceIds,
	TMap<InstanceId, YAML::Node>& outGameObjectOverrides,
	TMap<InstanceId, ReflectedData>& outComponentOverrides,
	std::string& outDiagnostic)
{
	const auto& sourcePrefab = source.m_prefab;
	outDiagnostic.clear();
	outGameObjectOverrides.Clear();
	outComponentOverrides.Clear();

	if (sourceToInstanceIds.Num() != sourcePrefab->m_gameObjects.Num())
	{
		outDiagnostic = "the reconciled source-to-instance mapping is incomplete";
		return false;
	}

	TMap<InstanceId, InstanceId> instanceToSourceIds;
	for (const auto& mapping : sourceToInstanceIds)
	{
		if (!mapping.m_first.IsGameObjectId() ||
			!mapping.m_second->IsGameObjectId() ||
			instanceToSourceIds.ContainsKey(*mapping.m_second))
		{
			outDiagnostic = "the source-to-instance mapping contains an invalid or duplicate id";
			return false;
		}

		instanceToSourceIds[*mapping.m_second] = mapping.m_first;
	}

	for (uint32_t sourceGameObjectIndex = 0;
		sourceGameObjectIndex < sourcePrefab->m_gameObjects.Num();
		++sourceGameObjectIndex)
	{
		const Prefab::ReflectedGameObject& sourceGameObject =
			sourcePrefab->m_gameObjects[sourceGameObjectIndex];
		if (!sourceToInstanceIds.ContainsKey(sourceGameObject.m_instanceId))
		{
			outDiagnostic = "the source-to-instance mapping is incomplete";
			return false;
		}

		const InstanceId& liveInstanceId =
			sourceToInstanceIds[sourceGameObject.m_instanceId];
		const auto* expandedGameObject = expanded.FindGameObject(liveInstanceId);

		if (!expandedGameObject)
		{
			// The source added this game object after the scene record was saved.
			// It inherits source values and receives no instance override yet.
			continue;
		}

		YAML::Node gameObjectOverride;
		if (expandedGameObject->m_name != sourceGameObject.m_name)
		{
			gameObjectOverride["name"] = expandedGameObject->m_name;
		}

		if (expandedGameObject->m_mobilityType !=
			sourceGameObject.m_mobilityType)
		{
			gameObjectOverride["mobilityType"] =
				SerializeEnum<EMobilityType>(
					expandedGameObject->m_mobilityType);
		}

		if (!Utils::AreYamlNodesEqual(
				YAML::Node(expandedGameObject->m_position),
				YAML::Node(sourceGameObject.m_position)))
		{
			gameObjectOverride["position"] = expandedGameObject->m_position;
		}

		if (!Utils::AreYamlNodesEqual(
				YAML::Node(expandedGameObject->m_rotation),
				YAML::Node(sourceGameObject.m_rotation)))
		{
			gameObjectOverride["rotation"] = expandedGameObject->m_rotation;
		}

		if (!Utils::AreYamlNodesEqual(
				YAML::Node(expandedGameObject->m_scale),
				YAML::Node(sourceGameObject.m_scale)))
		{
			gameObjectOverride["scale"] = expandedGameObject->m_scale;
		}

		if (gameObjectOverride.size() > 0)
		{
			outGameObjectOverrides[
				sourceGameObject.m_instanceId] = std::move(gameObjectOverride);
		}

		for (const uint32_t sourceComponentIndex :
			sourceGameObject.m_components)
		{
			const ReflectedData& sourceReflection =
				sourcePrefab->m_components[sourceComponentIndex];
			const InstanceId& sourceComponentId = source.m_componentIds[sourceComponentIndex];

			const InstanceId expectedLiveComponentId(
				sourceComponentId.ComponentId(),
				liveInstanceId);
			const ReflectedData* expandedReflection = expanded.FindComponent(expectedLiveComponentId);

			if (!expandedReflection ||
				expandedReflection->GetTypeInfo() !=
					sourceReflection.GetTypeInfo())
			{
				// The source added or replaced this component after the linked
				// scene record was saved. Source values are authoritative.
				continue;
			}

			YAML::Node overrideProperties;
			for (const auto& liveProperty :
				expandedReflection->GetProperties())
			{
				if (liveProperty.m_first == "instanceId" ||
					liveProperty.m_first == "fileId")
				{
					continue;
				}

				const YAML::Node normalizedLiveValue =
					PrefabInstance::NormalizeReferences(
						*liveProperty.m_second,
						instanceToSourceIds);
				if (!sourceReflection.GetProperties().ContainsKey(
						liveProperty.m_first) ||
					!Utils::AreYamlNodesEqual(
						normalizedLiveValue,
						sourceReflection.GetProperties()[
							liveProperty.m_first]))
				{
					overrideProperties[liveProperty.m_first] =
						normalizedLiveValue;
				}
			}

			if (overrideProperties.size() > 0)
			{
				YAML::Node reflectedOverride;
				reflectedOverride["typename"] =
					sourceReflection.GetTypeInfo().Name();
				reflectedOverride["overrideProperties"] =
					std::move(overrideProperties);

				ReflectedData componentOverride;
				componentOverride.Deserialize(reflectedOverride);
				if (!componentOverride.IsValid())
				{
					outDiagnostic = "cannot create a reflected component override";
					return false;
				}

				outComponentOverrides[sourceComponentId] =
					std::move(componentOverride);
			}
		}

	}

	return true;
}

bool WorldPrefab::BuildUpdatedLinkedOverrides(
	const PrefabPtr& expandedPrefab,
	const PrefabPtr& sourcePrefab,
	const PrefabPtr& effectiveBaseline,
	const TMap<InstanceId, InstanceId>& sourceToInstanceIds,
	TMap<InstanceId, YAML::Node>& outGameObjectOverrides,
	TMap<InstanceId, ReflectedData>& outComponentOverrides,
	std::string& outDiagnostic)
{
	outGameObjectOverrides.Clear();
	outComponentOverrides.Clear();
	if (!expandedPrefab || !sourcePrefab || !effectiveBaseline)
	{
		outDiagnostic = "the expanded, source, or effective baseline prefab is missing or mismatched";
		return false;
	}
	PrefabInstance::Snapshot expanded, source, baseline;
	return expanded.Build(expandedPrefab, outDiagnostic) && source.Build(sourcePrefab, outDiagnostic) &&
		baseline.Build(effectiveBaseline, outDiagnostic) &&
		BuildUpdatedLinkedOverrides(expanded, source, baseline, sourceToInstanceIds,
			outGameObjectOverrides, outComponentOverrides, outDiagnostic);
}

bool WorldPrefab::BuildUpdatedLinkedOverrides(
	const PrefabInstance::Snapshot& expanded,
	const PrefabInstance::Snapshot& source,
	const PrefabInstance::Snapshot& baseline,
	const TMap<InstanceId, InstanceId>& sourceToInstanceIds,
	TMap<InstanceId, YAML::Node>& outGameObjectOverrides,
	TMap<InstanceId, ReflectedData>& outComponentOverrides,
	std::string& outDiagnostic)
{
	const auto& sourcePrefab = source.m_prefab;
	const auto& effectiveBaseline = baseline.m_prefab;
	outDiagnostic.clear();
	outGameObjectOverrides.Clear();
	outComponentOverrides.Clear();

	if (sourcePrefab->GetFileId() != effectiveBaseline->GetFileId())
	{
		outDiagnostic = "the expanded, source, or effective baseline prefab is missing or mismatched";
		return false;
	}

	if (sourceToInstanceIds.Num() != sourcePrefab->m_gameObjects.Num())
	{
		outDiagnostic = "the reconciled source-to-instance mapping is incomplete";
		return false;
	}

	TMap<InstanceId, InstanceId> instanceToSourceIds;
	for (const auto& mapping : sourceToInstanceIds)
	{
		if (!mapping.m_first.IsGameObjectId() ||
			!mapping.m_second->IsGameObjectId() ||
			instanceToSourceIds.ContainsKey(*mapping.m_second))
		{
			outDiagnostic = "the source-to-instance mapping contains an invalid or duplicate id";
			return false;
		}

		instanceToSourceIds[*mapping.m_second] = mapping.m_first;
	}

	for (const Prefab::ReflectedGameObject& sourceGameObject :
		sourcePrefab->m_gameObjects)
	{
		if (!sourceToInstanceIds.ContainsKey(sourceGameObject.m_instanceId))
		{
			outDiagnostic = "the source-to-instance mapping is incomplete";
			return false;
		}

		const InstanceId& liveInstanceId =
			sourceToInstanceIds[sourceGameObject.m_instanceId];
		const auto* expandedGameObject = expanded.FindGameObject(liveInstanceId);
		const auto* baselineGameObject = baseline.FindGameObject(sourceGameObject.m_instanceId);

		const bool bHasPriorGameObjectOverride =
			effectiveBaseline->m_bLinkedInstanceRecord &&
			effectiveBaseline->m_gameObjectOverrides.ContainsKey(
				sourceGameObject.m_instanceId);
		const YAML::Node priorGameObjectOverride =
			bHasPriorGameObjectOverride
				? effectiveBaseline->m_gameObjectOverrides[
					sourceGameObject.m_instanceId]
				: YAML::Node();

		YAML::Node gameObjectOverride;
		if (expandedGameObject)
		{
			const bool bNameChangedFromBaseline =
				baselineGameObject &&
				expandedGameObject->m_name != baselineGameObject->m_name;
			if (bNameChangedFromBaseline)
			{
				if (expandedGameObject->m_name != sourceGameObject.m_name)
				{
					gameObjectOverride["name"] =
						expandedGameObject->m_name;
				}
			}
			else if (priorGameObjectOverride["name"])
			{
				gameObjectOverride["name"] =
					YAML::Clone(priorGameObjectOverride["name"]);
			}

			const bool bMobilityChangedFromBaseline =
				baselineGameObject &&
				expandedGameObject->m_mobilityType !=
					baselineGameObject->m_mobilityType;
			if (bMobilityChangedFromBaseline)
			{
				if (expandedGameObject->m_mobilityType !=
					sourceGameObject.m_mobilityType)
				{
					gameObjectOverride["mobilityType"] =
						SerializeEnum<EMobilityType>(
							expandedGameObject->m_mobilityType);
				}
			}
			else if (priorGameObjectOverride["mobilityType"])
			{
				gameObjectOverride["mobilityType"] =
					YAML::Clone(
						priorGameObjectOverride["mobilityType"]);
			}

			auto mergeTransformOverride = [
				&gameObjectOverride,
				&priorGameObjectOverride](
					const char* propertyName,
					const YAML::Node& liveValue,
					const YAML::Node& baselineValue,
					const YAML::Node& sourceValue,
					bool bHasBaseline)
				{
					const bool bChangedFromBaseline =
						bHasBaseline &&
						!Utils::AreYamlNodesEqual(liveValue, baselineValue);
					if (bChangedFromBaseline)
					{
						if (!Utils::AreYamlNodesEqual(liveValue, sourceValue))
						{
							gameObjectOverride[propertyName] =
								YAML::Clone(liveValue);
						}
					}
					else if (priorGameObjectOverride[propertyName])
					{
						gameObjectOverride[propertyName] =
							YAML::Clone(
								priorGameObjectOverride[propertyName]);
					}
				};

			mergeTransformOverride(
				"position",
				YAML::Node(expandedGameObject->m_position),
				baselineGameObject
					? YAML::Node(baselineGameObject->m_position)
					: YAML::Node(),
				YAML::Node(sourceGameObject.m_position),
				baselineGameObject != nullptr);
			mergeTransformOverride(
				"rotation",
				YAML::Node(expandedGameObject->m_rotation),
				baselineGameObject
					? YAML::Node(baselineGameObject->m_rotation)
					: YAML::Node(),
				YAML::Node(sourceGameObject.m_rotation),
				baselineGameObject != nullptr);
			mergeTransformOverride(
				"scale",
				YAML::Node(expandedGameObject->m_scale),
				baselineGameObject
					? YAML::Node(baselineGameObject->m_scale)
					: YAML::Node(),
				YAML::Node(sourceGameObject.m_scale),
				baselineGameObject != nullptr);
		}

		if (gameObjectOverride.size() > 0)
		{
			outGameObjectOverrides[sourceGameObject.m_instanceId] =
				std::move(gameObjectOverride);
		}

		for (const uint32_t sourceComponentIndex :
			sourceGameObject.m_components)
		{
			const ReflectedData& sourceReflection =
				sourcePrefab->m_components[sourceComponentIndex];
			const InstanceId& sourceComponentId = source.m_componentIds[sourceComponentIndex];

			const InstanceId expectedLiveComponentId(
				sourceComponentId.ComponentId(),
				liveInstanceId);
			const ReflectedData* expandedReflection = expandedGameObject ? expanded.FindComponent(expectedLiveComponentId) : nullptr;
			if (expandedReflection && expandedReflection->GetTypeInfo() != sourceReflection.GetTypeInfo())
			{
				expandedReflection = nullptr;
			}
			const ReflectedData* baselineReflection = baselineGameObject ? baseline.FindComponent(sourceComponentId) : nullptr;
			if (baselineReflection && baselineReflection->GetTypeInfo() != sourceReflection.GetTypeInfo())
			{
				baselineReflection = nullptr;
			}

			const ReflectedData* priorComponentOverride = nullptr;
			if (effectiveBaseline->m_bLinkedInstanceRecord &&
				effectiveBaseline->m_componentOverrides.ContainsKey(
					sourceComponentId))
			{
				const ReflectedData& candidate =
					effectiveBaseline->m_componentOverrides[
						sourceComponentId];
				if (candidate.IsValid() &&
					candidate.GetTypeInfo() ==
						sourceReflection.GetTypeInfo())
				{
					priorComponentOverride = &candidate;
				}
			}

			if (!expandedReflection)
			{
				continue;
			}

			YAML::Node overrideProperties;
			for (const auto& liveProperty :
				expandedReflection->GetProperties())
			{
				if (liveProperty.m_first == "instanceId" ||
					liveProperty.m_first == "fileId")
				{
					continue;
				}

				const YAML::Node normalizedLiveValue =
					PrefabInstance::NormalizeReferences(
						*liveProperty.m_second,
						instanceToSourceIds);
				const bool bHasBaselineProperty =
					baselineReflection &&
					baselineReflection->GetProperties().ContainsKey(
						liveProperty.m_first);
				const bool bChangedFromBaseline =
					baselineReflection &&
					(!bHasBaselineProperty ||
						!Utils::AreYamlNodesEqual(
							normalizedLiveValue,
							baselineReflection->GetProperties()[
								liveProperty.m_first]));

				if (bChangedFromBaseline)
				{
					if (!sourceReflection.GetProperties().ContainsKey(
							liveProperty.m_first) ||
						!Utils::AreYamlNodesEqual(
							normalizedLiveValue,
							sourceReflection.GetProperties()[
								liveProperty.m_first]))
					{
						overrideProperties[liveProperty.m_first] =
							normalizedLiveValue;
					}
				}
				else if (priorComponentOverride &&
					priorComponentOverride->GetProperties().ContainsKey(
						liveProperty.m_first))
				{
					overrideProperties[liveProperty.m_first] =
						YAML::Clone(
							priorComponentOverride->GetProperties()[
								liveProperty.m_first]);
				}
			}

			if (overrideProperties.size() > 0)
			{
				YAML::Node reflectedOverride;
				reflectedOverride["typename"] =
					sourceReflection.GetTypeInfo().Name();
				reflectedOverride["overrideProperties"] =
					std::move(overrideProperties);

				ReflectedData componentOverride;
				componentOverride.Deserialize(reflectedOverride);
				if (!componentOverride.IsValid())
				{
					outDiagnostic =
						"cannot create an updated reflected component override";
					return false;
				}

				outComponentOverrides[sourceComponentId] =
					std::move(componentOverride);
			}
		}
	}

	return true;
}
