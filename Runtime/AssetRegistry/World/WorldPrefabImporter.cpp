#include "WorldPrefabImporter.h"
#include "AssetRegistry/FileId.h"
#include "AssetRegistry/AssetRegistry.h"
#include "WorldPrefabAssetInfo.h"
#include "Core/Utils.h"
#include <filesystem>
#include <fstream>
#include <algorithm>
#include <iomanip>
#include <iostream>
#include <sstream>
#include "Memory/ObjectAllocator.hpp"
#include "Tasks/Scheduler.h"
#include "Engine/GameObject.h"
#include "Engine/World.h"
#include "ECS/TransformECS.h"
#include "Core/LogMacros.h"
#include "Containers/Hash.h"
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

	YAML::Node NormalizeInstanceReferences(
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
				normalized.push_back(NormalizeInstanceReferences(child, instanceToSourceIds));
			}
			return normalized;
		}

		normalized = YAML::Node(YAML::NodeType::Map);
		for (const auto& property : node)
		{
			normalized[YAML::Clone(property.first)] =
				NormalizeInstanceReferences(property.second, instanceToSourceIds);
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

	InstanceId MakeDeterministicLinkedInstanceId(
		const FileId& sourcePrefabId,
		const std::string& instanceSeed,
		const InstanceId& sourceInstanceId,
		uint32_t collisionAttempt)
	{
		const std::string key =
			sourcePrefabId.ToString() + "|" +
			instanceSeed + "|" +
			sourceInstanceId.ToString() + "|" +
			std::to_string(collisionAttempt);
		const uint64_t primaryHash = Sailor::fnv1a(key.data(), key.size());
		const std::string suffixKey = key + "|suffix";
		const uint16_t suffixHash = static_cast<uint16_t>(
			Sailor::fnv1a(suffixKey.data(), suffixKey.size()));

		std::stringstream stream;
		stream << std::uppercase << std::hex << std::setfill('0')
			<< std::setw(16) << primaryHash
			<< std::setw(4) << suffixHash;

		InstanceId result;
		result.Deserialize(YAML::Node(stream.str()));
		return result;
	}
}

YAML::Node WorldPrefab::Serialize() const
{
	if (!IsReady() || !m_loadDiagnostic.empty())
	{
		return YAML::Node();
	}

	YAML::Node outData;
	::Serialize(outData, "name", m_name);
	if (!m_globalIllumination.m_probes.IsEmpty() ||
		m_globalIllumination.m_mode !=
			EGlobalIlluminationMode::RealtimeAndBaked)
	{
		outData["globalIllumination"] = m_globalIllumination.Serialize();
	}

	TVector<YAML::Node> nodes;
	for (const auto& prefab : m_gameObjects)
	{
		YAML::Node prefabNode = prefab->Serialize();
		if (prefab->m_bLinkedInstanceRecord)
		{
			::Serialize(prefabNode, "fileId", prefab->GetFileId());
			::Serialize(prefabNode, "parentInstanceId", prefab->m_linkedParentInstanceId);
			::Serialize(prefabNode, "instanceIds", prefab->m_linkedInstanceIds);
			::Serialize(
				prefabNode,
				"gameObjectOverrides",
				prefab->m_gameObjectOverrides);
			::Serialize(
				prefabNode,
				"componentOverrides",
				prefab->m_componentOverrides);
		}

		nodes.Add(std::move(prefabNode));
	}

	outData["prefabs"] = nodes;

	return outData;
}

void WorldPrefab::Deserialize(const YAML::Node& inData)
{
	m_bIsReady.store(false, std::memory_order_release);
	m_loadDiagnostic.clear();
	m_name.clear();
	m_globalIllumination = {};
	m_gameObjects.Clear();
	::Deserialize(inData, "name", m_name);
	if (!m_globalIllumination.Deserialize(inData, m_loadDiagnostic))
	{
		m_loadDiagnostic = "invalid world global illumination settings: " +
			m_loadDiagnostic;
		return;
	}

	if (!inData["prefabs"] || !inData["prefabs"].IsSequence())
	{
		m_loadDiagnostic = "the world has no prefab sequence";
		return;
	}

	const size_t numPrefabs = inData["prefabs"].size();
	m_gameObjects.Reserve(numPrefabs);
	TVector<PrefabPtr> linkedPrefabs;
	linkedPrefabs.Reserve(numPrefabs);
	TSet<InstanceId> reservedInstanceIds;
	for (const auto& prefabNode : inData["prefabs"])
	{
		if (prefabNode["gameObjects"] &&
			prefabNode["gameObjects"].IsSequence())
		{
			for (const auto& gameObjectNode :
				prefabNode["gameObjects"])
			{
				InstanceId instanceId;
				if (::Deserialize(
						gameObjectNode,
						"instanceId",
						instanceId) &&
					instanceId.IsGameObjectId())
				{
					reservedInstanceIds.Insert(instanceId);
				}
			}
		}

		if (prefabNode["instanceIds"])
		{
			TMap<InstanceId, InstanceId> savedMappings;
			::Deserialize(
				prefabNode,
				"instanceIds",
				savedMappings);
			for (const auto& savedMapping : savedMappings)
			{
				if (savedMapping.m_second->IsGameObjectId())
				{
					reservedInstanceIds.Insert(*savedMapping.m_second);
				}
			}
		}
	}

	for (uint32_t prefabIndex = 0; prefabIndex < numPrefabs; ++prefabIndex)
	{
		const YAML::Node& prefabNode = inData["prefabs"][prefabIndex];
		FileId sourcePrefabId;
		::Deserialize(prefabNode, "fileId", sourcePrefabId);

		if (!sourcePrefabId)
		{
			PrefabPtr inlinePrefab = App::GetSubmodule<PrefabImporter>()->Create();
			inlinePrefab->Deserialize(prefabNode);
			if (!inlinePrefab->ValidateForInstantiation(m_loadDiagnostic))
			{
				m_loadDiagnostic = "inline prefab " + std::to_string(prefabIndex) +
					" is invalid: " + m_loadDiagnostic;
				return;
			}

			inlinePrefab->m_bIsReady.store(true, std::memory_order_release);
			m_gameObjects.Add(inlinePrefab);
			continue;
		}

		PrefabAssetInfoPtr sourceAssetInfo =
			App::GetSubmodule<AssetRegistry>()->GetAssetInfoPtr<PrefabAssetInfoPtr>(
				sourcePrefabId);
		if (!sourceAssetInfo)
		{
			m_loadDiagnostic = "linked prefab " + std::to_string(prefabIndex) +
				" references an unknown source asset " + sourcePrefabId.ToString();
			return;
		}

		std::string sourceText;
		if (!AssetRegistry::ReadTextFile(
				sourceAssetInfo->GetAssetFilepath(),
				sourceText))
		{
			m_loadDiagnostic = "cannot read linked prefab source " +
				sourceAssetInfo->GetAssetFilepath();
			return;
		}

		PrefabPtr sourcePrefab =
			App::GetSubmodule<PrefabImporter>()->Create(sourcePrefabId);
		sourcePrefab->Deserialize(YAML::Load(sourceText));
		if (!sourcePrefab->ValidateForInstantiation(m_loadDiagnostic))
		{
			m_loadDiagnostic = "linked prefab source '" +
				sourceAssetInfo->GetAssetFilepath() + "' is invalid: " +
				m_loadDiagnostic;
			return;
		}

		PrefabPtr expandedPrefab = App::GetSubmodule<PrefabImporter>()->Create();
		expandedPrefab->Deserialize(prefabNode);
		if (!expandedPrefab->ValidateForInstantiation(m_loadDiagnostic))
		{
			m_loadDiagnostic = "expanded linked prefab " +
				std::to_string(prefabIndex) + " is invalid: " +
				m_loadDiagnostic;
			return;
		}

		TMap<InstanceId, InstanceId> savedSourceToInstanceIds;
		if (prefabNode["instanceIds"])
		{
			::Deserialize(
				prefabNode,
				"instanceIds",
				savedSourceToInstanceIds);
		}
		else if (expandedPrefab->m_gameObjects.Num() ==
			sourcePrefab->m_gameObjects.Num())
		{
			for (uint32_t gameObjectIndex = 0;
				gameObjectIndex < sourcePrefab->m_gameObjects.Num();
				++gameObjectIndex)
			{
				savedSourceToInstanceIds[
					sourcePrefab->m_gameObjects[gameObjectIndex].m_instanceId] =
					expandedPrefab->m_gameObjects[gameObjectIndex].m_instanceId;
			}
		}
		else
		{
			for (const auto& sourceGameObject :
				sourcePrefab->m_gameObjects)
			{
				for (const auto& expandedGameObject :
					expandedPrefab->m_gameObjects)
				{
					if (sourceGameObject.m_instanceId ==
						expandedGameObject.m_instanceId)
					{
						savedSourceToInstanceIds[
							sourceGameObject.m_instanceId] =
							expandedGameObject.m_instanceId;
						break;
					}
				}
			}
		}

		TMap<InstanceId, InstanceId> sourceToInstanceIds;
		if (!ReconcileLinkedInstanceIds(
				expandedPrefab,
				sourcePrefab,
				savedSourceToInstanceIds,
				reservedInstanceIds,
				sourceToInstanceIds,
				m_loadDiagnostic))
		{
			m_loadDiagnostic = "cannot reconcile linked prefab " +
				std::to_string(prefabIndex) + " identities: " +
				m_loadDiagnostic;
			return;
		}

		InstanceId parentInstanceId;
		::Deserialize(prefabNode, "parentInstanceId", parentInstanceId);

		TMap<InstanceId, YAML::Node> gameObjectOverrides;
		TMap<InstanceId, ReflectedData> componentOverrides;
		if (prefabNode["gameObjectOverrides"])
		{
			::Deserialize(
				prefabNode,
				"gameObjectOverrides",
				gameObjectOverrides);
		}
		if (prefabNode["componentOverrides"])
		{
			::Deserialize(
				prefabNode,
				"componentOverrides",
				componentOverrides);
		}

		TSet<InstanceId> currentSourceGameObjectIds;
		TSet<InstanceId> currentSourceComponentIds;
		TMap<InstanceId, std::string> currentSourceComponentTypes;
		for (const auto& sourceGameObject :
			sourcePrefab->m_gameObjects)
		{
			currentSourceGameObjectIds.Insert(
				sourceGameObject.m_instanceId);
		}
		for (const auto& sourceComponent :
			sourcePrefab->m_components)
		{
			InstanceId sourceComponentId;
			std::string conversionDiagnostic;
			if (!Utils::TryGetComponentInstanceId(
					sourceComponent,
					sourceComponentId,
					conversionDiagnostic))
			{
				m_loadDiagnostic =
					"linked prefab source contains an invalid component identity: " +
					conversionDiagnostic;
				return;
			}
			currentSourceComponentIds.Insert(sourceComponentId);
			currentSourceComponentTypes[sourceComponentId] =
				sourceComponent.GetTypeInfo().Name();
		}

		TMap<InstanceId, YAML::Node> filteredGameObjectOverrides;
		for (const auto& overrideEntry : gameObjectOverrides)
		{
			if (currentSourceGameObjectIds.Contains(
					overrideEntry.m_first))
			{
				filteredGameObjectOverrides[
					overrideEntry.m_first] =
					*overrideEntry.m_second;
			}
		}
		gameObjectOverrides = std::move(
			filteredGameObjectOverrides);

		TMap<InstanceId, ReflectedData> filteredComponentOverrides;
		for (const auto& overrideEntry : componentOverrides)
		{
			if (currentSourceComponentIds.Contains(
					overrideEntry.m_first) &&
				overrideEntry.m_second->IsValid() &&
				overrideEntry.m_second->GetTypeInfo().Name() ==
					currentSourceComponentTypes[overrideEntry.m_first])
			{
				filteredComponentOverrides[
					overrideEntry.m_first] =
					*overrideEntry.m_second;
			}
		}
		componentOverrides = std::move(filteredComponentOverrides);

		if (!prefabNode["gameObjectOverrides"] ||
			!prefabNode["componentOverrides"])
		{
			TMap<InstanceId, YAML::Node> derivedGameObjectOverrides;
			TMap<InstanceId, ReflectedData> derivedComponentOverrides;
			if (!BuildLinkedOverrides(
					expandedPrefab,
					sourcePrefab,
					sourceToInstanceIds,
					derivedGameObjectOverrides,
					derivedComponentOverrides,
					m_loadDiagnostic))
			{
				m_loadDiagnostic = "cannot derive linked prefab " +
					std::to_string(prefabIndex) + " overrides: " +
					m_loadDiagnostic;
				return;
			}

			if (!prefabNode["gameObjectOverrides"])
			{
				gameObjectOverrides = std::move(derivedGameObjectOverrides);
			}
			if (!prefabNode["componentOverrides"])
			{
				componentOverrides = std::move(derivedComponentOverrides);
			}
		}

		PrefabPtr linkedPrefab =
			App::GetSubmodule<PrefabImporter>()->Create(sourcePrefabId);
		if (!linkedPrefab->ConfigureLinkedInstance(
				sourcePrefab,
				sourceToInstanceIds,
				parentInstanceId,
				gameObjectOverrides,
				componentOverrides,
				m_loadDiagnostic))
		{
			m_loadDiagnostic = "linked prefab " + std::to_string(prefabIndex) +
				" is invalid: " + m_loadDiagnostic;
			return;
		}

		if (!linkedPrefab->AppendDetachedSupplementalHierarchy(
				expandedPrefab,
				m_loadDiagnostic))
		{
			m_loadDiagnostic =
				"linked prefab " + std::to_string(prefabIndex) +
				" has invalid detached supplemental data: " +
				m_loadDiagnostic;
			return;
		}

		linkedPrefabs.Add(linkedPrefab);
	}

	m_gameObjects.AddRange(linkedPrefabs);
	m_bIsReady.store(true, std::memory_order_release);
}

bool WorldPrefab::ReconcileLinkedInstanceIds(
	const PrefabPtr& expandedPrefab,
	const PrefabPtr& sourcePrefab,
	const TMap<InstanceId, InstanceId>& savedSourceToInstanceIds,
	TSet<InstanceId>& reservedInstanceIds,
	TMap<InstanceId, InstanceId>& outSourceToInstanceIds,
	std::string& outDiagnostic)
{
	outDiagnostic.clear();
	outSourceToInstanceIds.Clear();
	if (!expandedPrefab || !sourcePrefab || !sourcePrefab->GetFileId())
	{
		outDiagnostic = "the expanded or source prefab is missing";
		return false;
	}

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
	outDiagnostic.clear();
	outGameObjectOverrides.Clear();
	outComponentOverrides.Clear();

	if (!expandedPrefab || !sourcePrefab)
	{
		outDiagnostic = "the expanded or source prefab is missing";
		return false;
	}

	if (!expandedPrefab->ValidateForInstantiation(outDiagnostic) ||
		!sourcePrefab->ValidateForInstantiation(outDiagnostic))
	{
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
		const Prefab::ReflectedGameObject* expandedGameObject = nullptr;
		for (uint32_t candidateIndex = 0;
			candidateIndex < expandedPrefab->m_gameObjects.Num();
			++candidateIndex)
		{
			if (expandedPrefab->m_gameObjects[candidateIndex].m_instanceId ==
				liveInstanceId)
			{
				expandedGameObject =
					&expandedPrefab->m_gameObjects[candidateIndex];
				break;
			}
		}

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
			InstanceId sourceComponentId;
			if (!Utils::TryGetComponentInstanceId(
					sourceReflection,
					sourceComponentId,
					outDiagnostic))
			{
				return false;
			}

			const InstanceId expectedLiveComponentId(
				sourceComponentId.ComponentId(),
				liveInstanceId);
			const ReflectedData* expandedReflection = nullptr;
			for (const uint32_t expandedComponentIndex :
				expandedGameObject->m_components)
			{
				const ReflectedData& candidate =
					expandedPrefab->m_components[expandedComponentIndex];
				InstanceId candidateInstanceId;
				std::string conversionDiagnostic;
				if (!Utils::TryGetComponentInstanceId(
						candidate,
						candidateInstanceId,
						conversionDiagnostic))
				{
					outDiagnostic = conversionDiagnostic;
					return false;
				}

				if (candidateInstanceId == expectedLiveComponentId)
				{
					expandedReflection = &candidate;
					break;
				}
			}

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
					NormalizeInstanceReferences(
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
	outDiagnostic.clear();
	outGameObjectOverrides.Clear();
	outComponentOverrides.Clear();

	if (!expandedPrefab ||
		!sourcePrefab ||
		!effectiveBaseline ||
		sourcePrefab->GetFileId() != effectiveBaseline->GetFileId())
	{
		outDiagnostic = "the expanded, source, or effective baseline prefab is missing or mismatched";
		return false;
	}

	if (!expandedPrefab->ValidateForInstantiation(outDiagnostic) ||
		!sourcePrefab->ValidateForInstantiation(outDiagnostic) ||
		!effectiveBaseline->ValidateForInstantiation(outDiagnostic))
	{
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
		const Prefab::ReflectedGameObject* expandedGameObject = nullptr;
		for (const Prefab::ReflectedGameObject& candidate :
			expandedPrefab->m_gameObjects)
		{
			if (candidate.m_instanceId == liveInstanceId)
			{
				expandedGameObject = &candidate;
				break;
			}
		}

		const Prefab::ReflectedGameObject* baselineGameObject = nullptr;
		for (const Prefab::ReflectedGameObject& candidate :
			effectiveBaseline->m_gameObjects)
		{
			if (candidate.m_instanceId == sourceGameObject.m_instanceId)
			{
				baselineGameObject = &candidate;
				break;
			}
		}

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
			InstanceId sourceComponentId;
			if (!Utils::TryGetComponentInstanceId(
					sourceReflection,
					sourceComponentId,
					outDiagnostic))
			{
				return false;
			}

			const InstanceId expectedLiveComponentId(
				sourceComponentId.ComponentId(),
				liveInstanceId);
			const ReflectedData* expandedReflection = nullptr;
			if (expandedGameObject)
			{
				for (const uint32_t componentIndex :
					expandedGameObject->m_components)
				{
					const ReflectedData& candidate =
						expandedPrefab->m_components[componentIndex];
					InstanceId candidateInstanceId;
					std::string conversionDiagnostic;
					if (!Utils::TryGetComponentInstanceId(
							candidate,
							candidateInstanceId,
							conversionDiagnostic))
					{
						outDiagnostic = conversionDiagnostic;
						return false;
					}

					if (candidateInstanceId == expectedLiveComponentId &&
						candidate.GetTypeInfo() ==
							sourceReflection.GetTypeInfo())
					{
						expandedReflection = &candidate;
						break;
					}
				}
			}

			const ReflectedData* baselineReflection = nullptr;
			if (baselineGameObject)
			{
				for (const uint32_t componentIndex :
					baselineGameObject->m_components)
				{
					const ReflectedData& candidate =
						effectiveBaseline->m_components[componentIndex];
					InstanceId candidateInstanceId;
					std::string conversionDiagnostic;
					if (!Utils::TryGetComponentInstanceId(
							candidate,
							candidateInstanceId,
							conversionDiagnostic))
					{
						outDiagnostic = conversionDiagnostic;
						return false;
					}

					if (candidateInstanceId == sourceComponentId &&
						candidate.GetTypeInfo() ==
							sourceReflection.GetTypeInfo())
					{
						baselineReflection = &candidate;
						break;
					}
				}
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
					NormalizeInstanceReferences(
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

bool WorldPrefab::CommitLinkedInstanceUpdates(
	WorldPtr world,
	TVector<PendingPrefabLinkUpdate>& pendingUpdates,
	std::string& outDiagnostic)
{
	outDiagnostic.clear();
	if (!world)
	{
		outDiagnostic = "the target world is missing";
		return false;
	}

	size_t numPrefabInstanceRoots = 0;
	for (const auto& object : world->m_objects)
	{
		if (object && object->GetFileId())
		{
			++numPrefabInstanceRoots;
		}
	}

	if (pendingUpdates.Num() != numPrefabInstanceRoots ||
		world->m_prefabInstances.Num() != numPrefabInstanceRoots)
	{
		outDiagnostic =
			"the linked prefab set changed before its baselines could be committed";
		return false;
	}

	TSet<InstanceId> pendingRoots;
	TMap<InstanceId, InstanceId> nextPrefabInstanceRootsByObject;
	for (const auto& pendingUpdate : pendingUpdates)
	{
		if (!pendingRoots.Insert(pendingUpdate.m_rootInstanceId) ||
			!world->m_prefabInstances.ContainsKey(
				pendingUpdate.m_rootInstanceId))
		{
			outDiagnostic =
				"a linked prefab instance disappeared or was duplicated before commit";
			return false;
		}

		GameObjectPtr root = world->GetObjectByInstanceId(
			pendingUpdate.m_rootInstanceId).DynamicCast<GameObject>();
		const PrefabInstanceLink* currentLink = nullptr;
		std::string baselineDiagnostic;
		if (!root ||
			!root->GetFileId() ||
			!pendingUpdate.m_effectiveBaseline ||
			pendingUpdate.m_effectiveBaseline->GetFileId() !=
				root->GetFileId() ||
			!pendingUpdate.m_effectiveBaseline->
				ValidateForInstantiation(
					baselineDiagnostic) ||
			!world->TryGetPrefabInstance(
				pendingUpdate.m_rootInstanceId,
				currentLink) ||
			!currentLink)
		{
			outDiagnostic =
				"a linked prefab root, source FileId, baseline, or derived metadata changed before commit";
			if (!baselineDiagnostic.empty())
			{
				outDiagnostic += ": " +
					baselineDiagnostic;
			}
			return false;
		}

		for (const auto& mapping :
			pendingUpdate.m_sourceToInstanceIds)
		{
			const InstanceId& liveInstanceId = *mapping.m_second;
			if (!liveInstanceId.IsGameObjectId())
			{
				outDiagnostic =
					"a linked prefab mapping contains an invalid live game object id";
				return false;
			}

			// Newly introduced source objects receive deterministic ids now but
			// do not become live members until the scene is instantiated again.
			if (!world->m_objectsMap.ContainsKey(liveInstanceId))
			{
				continue;
			}

			GameObjectPtr liveObject = world->GetObjectByInstanceId(
				liveInstanceId).DynamicCast<GameObject>();
			bool bIsInRootHierarchy = false;
			for (GameObjectPtr current = liveObject;
				current;
				current = current->GetParent())
			{
				if (current == root)
				{
					bIsInRootHierarchy = true;
					break;
				}
			}
			if (!liveObject ||
				!bIsInRootHierarchy ||
				(liveObject != root &&
					liveObject->GetFileId()))
			{
				outDiagnostic =
					"a linked prefab mapping references a live object outside its authoritative root";
				return false;
			}

			if (nextPrefabInstanceRootsByObject.ContainsKey(
					liveInstanceId))
			{
				outDiagnostic =
					"multiple linked prefab instances claim the same live game object";
				return false;
			}

			nextPrefabInstanceRootsByObject[liveInstanceId] =
				pendingUpdate.m_rootInstanceId;
		}

		if (!nextPrefabInstanceRootsByObject.ContainsKey(
				pendingUpdate.m_rootInstanceId))
		{
			outDiagnostic =
				"a linked prefab mapping no longer contains its live root";
			return false;
		}
	}

	for (auto& pendingUpdate : pendingUpdates)
	{
		PrefabInstanceLink& link =
			world->m_prefabInstances[
				pendingUpdate.m_rootInstanceId];
		link.m_sourceToInstanceIds =
			std::move(pendingUpdate.m_sourceToInstanceIds);
		link.m_effectiveBaseline =
			std::move(pendingUpdate.m_effectiveBaseline);
	}
	world->m_prefabInstanceRootsByObject =
		std::move(nextPrefabInstanceRootsByObject);

	return true;
}

bool WorldPrefab::SaveToFile(const std::string& path) const
{
	if (!IsReady() || !m_loadDiagnostic.empty())
	{
		return false;
	}

	AssetRegistry::WriteTextFile(path, Serialize());
	return true;
}

WorldPrefabPtr WorldPrefab::FromWorld(WorldPtr world)
{
	auto res = App::GetSubmodule<WorldPrefabImporter>()->Create();
	res->m_name = world->GetName();
	res->m_globalIllumination = world->GetGlobalIlluminationSettings();

	TVector<PendingPrefabLinkUpdate> pendingLinkUpdates;

	const auto& gameObjects = world->GetGameObjects();
	TSet<InstanceId> reservedInstanceIds;
	TSet<InstanceId> linkedRoots;
	TVector<GameObjectPtr> linkedRootObjects;
	for (const auto& gameObject : gameObjects)
	{
		if (gameObject)
		{
			reservedInstanceIds.Insert(gameObject->GetInstanceId());
			if (gameObject->GetFileId())
			{
				linkedRoots.Insert(gameObject->GetInstanceId());
				linkedRootObjects.Add(gameObject);
			}
		}
	}

	if (linkedRoots.Num() != world->m_prefabInstances.Num())
	{
		res->m_loadDiagnostic =
			"cannot serialize world: authoritative prefab roots and derived metadata are inconsistent";
		SAILOR_LOG_ERROR("%s.", res->m_loadDiagnostic.c_str());
		return res;
	}

	for (const auto& go : gameObjects)
	{
		if (go->GetParent() || linkedRoots.Contains(go->GetInstanceId()))
		{
			continue;
		}

		PrefabPtr inlinePrefab =
			Prefab::FromGameObject(go, FileId::Invalid, &linkedRoots);
		if (!inlinePrefab->m_gameObjects.IsEmpty())
		{
			res->m_gameObjects.Add(inlinePrefab);
		}
	}

	TSet<InstanceId> validatedLinkedMembers;
	for (const GameObjectPtr& root : linkedRootObjects)
	{
		const InstanceId& rootInstanceId =
			root->GetInstanceId();
		const FileId& sourcePrefabId =
			root->GetFileId();
		const PrefabInstanceLink* validatedLink = nullptr;
		if (!world->m_prefabInstances.ContainsKey(
				rootInstanceId) ||
			!world->TryGetPrefabInstance(
				rootInstanceId,
				validatedLink) ||
			!validatedLink)
		{
			res->m_loadDiagnostic =
				"cannot serialize linked prefab '" +
				sourcePrefabId.ToString() +
				"': its derived runtime metadata is missing or inconsistent";
			SAILOR_LOG_ERROR("%s.", res->m_loadDiagnostic.c_str());
			res->m_gameObjects.Clear();
			return res;
		}
		PrefabInstanceLink& link =
			world->m_prefabInstances[rootInstanceId];
		for (const auto& mapping :
			link.m_sourceToInstanceIds)
		{
			const InstanceId& liveInstanceId =
				*mapping.m_second;
			if (!world->m_objectsMap.ContainsKey(
					liveInstanceId))
			{
				continue;
			}

			if (!validatedLinkedMembers.Insert(
					liveInstanceId) ||
				!world->m_prefabInstanceRootsByObject.
					ContainsKey(liveInstanceId) ||
				world->m_prefabInstanceRootsByObject[
					liveInstanceId] != rootInstanceId)
			{
				res->m_loadDiagnostic =
					"cannot serialize linked prefab '" +
					sourcePrefabId.ToString() +
					"': its derived membership cache is inconsistent";
				SAILOR_LOG_ERROR("%s.",
					res->m_loadDiagnostic.c_str());
				res->m_gameObjects.Clear();
				return res;
			}
		}

		if (!link.m_effectiveBaseline)
		{
			res->m_loadDiagnostic =
				"cannot serialize linked prefab '" +
				sourcePrefabId.ToString() +
				"': its effective baseline is unavailable";
			SAILOR_LOG_ERROR("%s.", res->m_loadDiagnostic.c_str());
			res->m_gameObjects.Clear();
			return res;
		}

		PrefabPtr sourcePrefab;
		if (!App::GetSubmodule<PrefabImporter>()->LoadPrefab_Immediate(
				sourcePrefabId,
				sourcePrefab))
		{
			res->m_loadDiagnostic =
				"cannot serialize linked prefab '" +
				sourcePrefabId.ToString() +
				"': source asset is unavailable";
			SAILOR_LOG_ERROR("%s.", res->m_loadDiagnostic.c_str());
			res->m_gameObjects.Clear();
			return res;
		}

		PrefabPtr expandedPrefab =
			Prefab::FromGameObject(root, sourcePrefabId);
		std::string diagnostic;
		TMap<InstanceId, InstanceId> reconciledInstanceIds;
		if (!ReconcileLinkedInstanceIds(
				expandedPrefab,
				sourcePrefab,
				link.m_sourceToInstanceIds,
				reservedInstanceIds,
				reconciledInstanceIds,
				diagnostic))
		{
			res->m_loadDiagnostic =
				"cannot reconcile linked prefab '" +
				sourcePrefabId.ToString() +
				"': " +
				diagnostic;
			SAILOR_LOG_ERROR("%s.", res->m_loadDiagnostic.c_str());
			res->m_gameObjects.Clear();
			return res;
		}

		if (!BuildUpdatedLinkedOverrides(
				expandedPrefab,
				sourcePrefab,
				link.m_effectiveBaseline,
				reconciledInstanceIds,
				expandedPrefab->m_gameObjectOverrides,
				expandedPrefab->m_componentOverrides,
				diagnostic))
		{
			res->m_loadDiagnostic =
				"cannot serialize linked prefab '" +
				sourcePrefabId.ToString() +
				"': " +
				diagnostic;
			SAILOR_LOG_ERROR("%s.", res->m_loadDiagnostic.c_str());
			res->m_gameObjects.Clear();
			return res;
		}

		PrefabPtr nextEffectiveBaseline =
			PrefabPtr::Make(
				world->GetAllocator(),
				sourcePrefabId);
		nextEffectiveBaseline->m_gameObjects =
			sourcePrefab->m_gameObjects;
		nextEffectiveBaseline->m_components =
			sourcePrefab->m_components;

		TMap<InstanceId, InstanceId> instanceToSourceIds;
		for (const auto& mapping : reconciledInstanceIds)
		{
			instanceToSourceIds[*mapping.m_second] =
				mapping.m_first;
		}

		bool bBuiltNextBaseline = true;
		for (uint32_t sourceGameObjectIndex = 0;
			sourceGameObjectIndex < sourcePrefab->m_gameObjects.Num();
			++sourceGameObjectIndex)
		{
			const Prefab::ReflectedGameObject& sourceGameObject =
				sourcePrefab->m_gameObjects[sourceGameObjectIndex];
			if (!reconciledInstanceIds.ContainsKey(
					sourceGameObject.m_instanceId))
			{
				diagnostic =
					"the reconciled mapping is missing a baseline game object";
				bBuiltNextBaseline = false;
				break;
			}

			const InstanceId& liveInstanceId =
				reconciledInstanceIds[sourceGameObject.m_instanceId];
			const Prefab::ReflectedGameObject* liveGameObject = nullptr;
			for (const Prefab::ReflectedGameObject& candidate :
				expandedPrefab->m_gameObjects)
			{
				if (candidate.m_instanceId == liveInstanceId)
				{
					liveGameObject = &candidate;
					break;
				}
			}

			if (!liveGameObject)
			{
				continue;
			}

			Prefab::ReflectedGameObject& baselineGameObject =
				nextEffectiveBaseline->m_gameObjects[
					sourceGameObjectIndex];
			baselineGameObject.m_name = liveGameObject->m_name;
			baselineGameObject.m_mobilityType =
				liveGameObject->m_mobilityType;
			baselineGameObject.m_position =
				liveGameObject->m_position;
			baselineGameObject.m_rotation =
				liveGameObject->m_rotation;
			baselineGameObject.m_scale =
				liveGameObject->m_scale;

			for (const uint32_t sourceComponentIndex :
				sourceGameObject.m_components)
			{
				const ReflectedData& sourceReflection =
					sourcePrefab->m_components[
						sourceComponentIndex];
				InstanceId sourceComponentId;
				if (!Utils::TryGetComponentInstanceId(
						sourceReflection,
						sourceComponentId,
						diagnostic))
				{
					bBuiltNextBaseline = false;
					break;
				}

				const InstanceId liveComponentId(
					sourceComponentId.ComponentId(),
					liveInstanceId);
				const ReflectedData* liveReflection = nullptr;
				for (const uint32_t liveComponentIndex :
					liveGameObject->m_components)
				{
					const ReflectedData& candidate =
						expandedPrefab->m_components[
							liveComponentIndex];
					InstanceId candidateInstanceId;
					std::string conversionDiagnostic;
					if (!Utils::TryGetComponentInstanceId(
							candidate,
							candidateInstanceId,
							conversionDiagnostic))
					{
						diagnostic = conversionDiagnostic;
						bBuiltNextBaseline = false;
						break;
					}

					if (candidateInstanceId == liveComponentId &&
						candidate.GetTypeInfo() ==
							sourceReflection.GetTypeInfo())
					{
						liveReflection = &candidate;
						break;
					}
				}

				if (!bBuiltNextBaseline)
				{
					break;
				}

				if (!liveReflection)
				{
					continue;
				}

				const YAML::Node normalizedReflection =
					NormalizeInstanceReferences(
						liveReflection->Serialize(),
						instanceToSourceIds);
				ReflectedData baselineReflection;
				if (!External::GuardYamlExceptions(
						[&baselineReflection, &normalizedReflection]()
							{
								baselineReflection.Deserialize(
									normalizedReflection);
							},
						diagnostic) ||
					!baselineReflection.IsValid())
				{
					if (diagnostic.empty())
					{
						diagnostic =
							"cannot normalize a live component baseline";
					}
					bBuiltNextBaseline = false;
					break;
				}

				nextEffectiveBaseline->m_components[
					sourceComponentIndex] =
					std::move(baselineReflection);
			}

			if (!bBuiltNextBaseline)
			{
				break;
			}
		}

		nextEffectiveBaseline->m_linkedInstanceIds =
			reconciledInstanceIds;
		nextEffectiveBaseline->m_gameObjectOverrides =
			expandedPrefab->m_gameObjectOverrides;
		nextEffectiveBaseline->m_componentOverrides =
			expandedPrefab->m_componentOverrides;
		nextEffectiveBaseline->m_linkedParentInstanceId =
			root->GetParent()
				? root->GetParent()->GetInstanceId()
				: InstanceId::Invalid;
		nextEffectiveBaseline->m_bLinkedInstanceRecord = true;
		if (!bBuiltNextBaseline ||
			!nextEffectiveBaseline->ValidateForInstantiation(
				diagnostic))
		{
			res->m_loadDiagnostic =
				"cannot update linked prefab '" +
				sourcePrefabId.ToString() +
				"' baseline: " +
				diagnostic;
			SAILOR_LOG_ERROR("%s.", res->m_loadDiagnostic.c_str());
			res->m_gameObjects.Clear();
			return res;
		}
		nextEffectiveBaseline->m_bIsReady.store(
			true,
			std::memory_order_release);

		expandedPrefab->m_linkedInstanceIds =
			reconciledInstanceIds;
		expandedPrefab->m_linkedParentInstanceId = root->GetParent()
			? root->GetParent()->GetInstanceId()
			: InstanceId::Invalid;
		expandedPrefab->m_bLinkedInstanceRecord = true;
		expandedPrefab->m_bExpandedLinkedInstanceRecord =
			true;
		expandedPrefab->m_detachedSupplementalInstanceIds.
			Clear();
		TSet<InstanceId> mappedLiveInstanceIds;
		for (const auto& mapping :
			expandedPrefab->m_linkedInstanceIds)
		{
			mappedLiveInstanceIds.Insert(
				*mapping.m_second);
		}
		for (const auto& expandedGameObject :
			expandedPrefab->m_gameObjects)
		{
			if (!mappedLiveInstanceIds.Contains(
					expandedGameObject.m_instanceId))
			{
				expandedPrefab->
					m_detachedSupplementalInstanceIds.Insert(
						expandedGameObject.m_instanceId);
			}
		}
		if (!expandedPrefab->ValidateForInstantiation(
				diagnostic))
		{
			res->m_loadDiagnostic =
				"cannot validate expanded linked prefab '" +
				sourcePrefabId.ToString() +
				"': " +
				diagnostic;
			SAILOR_LOG_ERROR("%s.",
				res->m_loadDiagnostic.c_str());
			res->m_gameObjects.Clear();
			return res;
		}
		res->m_gameObjects.Add(expandedPrefab);

		PendingPrefabLinkUpdate pendingUpdate;
		pendingUpdate.m_rootInstanceId =
			rootInstanceId;
		pendingUpdate.m_sourceToInstanceIds =
			std::move(reconciledInstanceIds);
		pendingUpdate.m_effectiveBaseline =
			std::move(nextEffectiveBaseline);
		pendingLinkUpdates.Add(std::move(pendingUpdate));
	}

	if (validatedLinkedMembers.Num() !=
		world->m_prefabInstanceRootsByObject.Num())
	{
		res->m_loadDiagnostic =
			"cannot serialize world: the derived prefab membership cache contains stale entries";
		SAILOR_LOG_ERROR("%s.", res->m_loadDiagnostic.c_str());
		res->m_gameObjects.Clear();
		return res;
	}

	std::string commitDiagnostic;
	if (!CommitLinkedInstanceUpdates(
			world,
			pendingLinkUpdates,
			commitDiagnostic))
	{
		res->m_loadDiagnostic =
			"cannot commit linked prefab baselines: " +
			commitDiagnostic;
		SAILOR_LOG_ERROR("%s.", res->m_loadDiagnostic.c_str());
		res->m_gameObjects.Clear();
		return res;
	}

	res->m_bIsReady.store(true, std::memory_order_release);
	return res;
}

WorldPrefabImporter::WorldPrefabImporter(WorldPrefabAssetInfoHandler* infoHandler)
{
	SAILOR_PROFILE_FUNCTION();
	m_allocator = ObjectAllocatorPtr::Make(EAllocationPolicy::SharedMemory_MultiThreaded);
	m_worldInfoHandler = infoHandler;
	m_worldInfoHandler->Subscribe(this);

	m_prefabInfoHandler = App::GetSubmodule<PrefabAssetInfoHandler>();
	if (m_prefabInfoHandler)
	{
		m_prefabInfoHandler->Subscribe(this);
	}
}

WorldPrefabImporter::~WorldPrefabImporter()
{
	if (m_worldInfoHandler)
	{
		m_worldInfoHandler->Unsubscribe(this);
	}
	if (m_prefabInfoHandler)
	{
		m_prefabInfoHandler->Unsubscribe(this);
	}

	for (auto& model : m_loadedWorldPrefabs)
	{
		model.m_second.DestroyObject(m_allocator);
	}
}

WorldPrefabPtr WorldPrefabImporter::Create()
{
	return WorldPrefabPtr::Make(m_allocator, FileId());
}

void WorldPrefabImporter::OnUpdateAssetInfo(AssetInfoPtr assetInfo, bool bWasExpired)
{
	SAILOR_PROFILE_FUNCTION();
	if (!assetInfo)
	{
		return;
	}

	SAILOR_PROFILE_TEXT(assetInfo->GetAssetFilepath().c_str());
	if (!bWasExpired)
	{
		return;
	}

	if (dynamic_cast<PrefabAssetInfo*>(assetInfo))
	{
		m_loadedWorldPrefabs.Clear();
		m_promises.Clear();
		return;
	}

	const FileId uid = assetInfo->GetFileId();
	m_loadedWorldPrefabs.Remove(uid);
	m_promises.Remove(uid);
}

void WorldPrefabImporter::OnImportAsset(AssetInfoPtr assetInfo) {}

bool WorldPrefabImporter::LoadWorld_Immediate(FileId uid, WorldPrefabPtr& outWorldPrefab)
{
	SAILOR_PROFILE_FUNCTION();

	auto task = LoadWorld(uid, outWorldPrefab);
	if (!task)
	{
		return false;
	}

	task->Wait();
	return task->GetResult().IsValid() &&
		outWorldPrefab &&
		outWorldPrefab->IsReady();
}

Tasks::TaskPtr<WorldPrefabPtr> WorldPrefabImporter::LoadWorld(FileId uid, WorldPrefabPtr& outWorldPrefab)
{
	SAILOR_PROFILE_FUNCTION();

	// Check promises first
	auto& promise = m_promises.At_Lock(uid, nullptr);
	auto& loadedWorldPrefab = m_loadedWorldPrefabs.At_Lock(uid, WorldPrefabPtr());

	// Check loaded assets
	if (loadedWorldPrefab)
	{
		if (promise && !promise->IsFinished())
		{
			outWorldPrefab = loadedWorldPrefab;
			auto res = promise;

			m_loadedWorldPrefabs.Unlock(uid);
			m_promises.Unlock(uid);

			return res;
		}

		if (loadedWorldPrefab->IsReady())
		{
			outWorldPrefab = loadedWorldPrefab;
			auto res = Tasks::TaskPtr<WorldPrefabPtr>::Make(outWorldPrefab);

			m_loadedWorldPrefabs.Unlock(uid);
			m_promises.Unlock(uid);

			return res;
		}

		loadedWorldPrefab = nullptr;
		promise = nullptr;
	}

	// There is no promise, we need to load WorldPrefab
	if (WorldPrefabAssetInfoPtr assetInfo = App::GetSubmodule<AssetRegistry>()->GetAssetInfoPtr<WorldPrefabAssetInfoPtr>(uid))
	{
		SAILOR_PROFILE_TEXT(assetInfo->GetAssetFilepath().c_str());

		WorldPrefabPtr pWorldPrefab = WorldPrefabPtr::Make(m_allocator, uid);

		struct Data {};
		promise = Tasks::CreateTaskWithResult<TSharedPtr<Data>>("Load WorldPrefab",
			[pWorldPrefab, assetInfo]() mutable
			{
				TSharedPtr<Data> res = TSharedPtr<Data>::Make();

				std::string text;
				std::string diagnostic;
				bool bLoaded =
					AssetRegistry::ReadTextFile(
						assetInfo->GetAssetFilepath(),
						text);
				if (bLoaded)
				{
					bLoaded = External::GuardYamlExceptions(
						[&pWorldPrefab, &text]()
						{
							pWorldPrefab->Deserialize(YAML::Load(text));
						},
						diagnostic);
				}

				bLoaded = bLoaded && pWorldPrefab->IsReady();
				if (!bLoaded)
				{
					if (diagnostic.empty())
					{
						diagnostic = pWorldPrefab->GetLoadDiagnostic();
					}
					SAILOR_LOG_ERROR(
						"Cannot load world '%s': %s.",
						assetInfo->GetAssetFilepath().c_str(),
						diagnostic.empty()
							? "cannot read or validate the world file"
							: diagnostic.c_str());
				}

				return res;

			})->Then<WorldPrefabPtr>([pWorldPrefab](TSharedPtr<Data> data) mutable
				{
					return pWorldPrefab;
				}, "Preload resources", EThreadType::RHI)->ToTaskWithResult();

			outWorldPrefab = loadedWorldPrefab = pWorldPrefab;
			promise->Run();

			m_loadedWorldPrefabs.Unlock(uid);
			m_promises.Unlock(uid);

			return promise;
	}

	outWorldPrefab = nullptr;
	m_loadedWorldPrefabs.Unlock(uid);
	m_promises.Unlock(uid);

	return Tasks::TaskPtr<WorldPrefabPtr>();
}

bool WorldPrefabImporter::LoadAsset(FileId uid, TObjectPtr<Object>& out, bool bImmediate)
{
	WorldPrefabPtr outAsset;
	if (bImmediate)
	{
		bool bRes = LoadWorld_Immediate(uid, outAsset);
		out = outAsset;
		return bRes;
	}

	LoadWorld(uid, outAsset);
	out = outAsset;
	return true;
}

void WorldPrefabImporter::CollectGarbage()
{
	TVector<FileId> uidsToRemove;

	m_promises.LockAll();
	auto ids = m_promises.GetKeys();
	m_promises.UnlockAll();

	for (const auto& id : ids)
	{
		auto& promise = m_promises.At_Lock(id);

		if (!promise.IsValid() || (promise.IsValid() && promise->IsFinished()))
		{
			FileId uid = id;
			uidsToRemove.Emplace(uid);
		}

		m_promises.Unlock(id);
	}

	for (auto& uid : uidsToRemove)
	{
		m_promises.Remove(uid);
	}
}
