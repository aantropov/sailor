#include "WorldPrefabImporter.h"
#include "AssetRegistry/Prefab/PrefabInstance.h"
#include "Platform/AtomicFile.h"
#include "AssetRegistry/FileId.h"
#include "AssetRegistry/AssetRegistry.h"
#include "WorldPrefabAssetInfo.h"
#include "Core/Utils.h"
#include <filesystem>
#include <fstream>
#include <algorithm>
#include <iostream>
#include "Memory/ObjectAllocator.hpp"
#include "Tasks/Scheduler.h"
#include "Engine/GameObject.h"
#include "Engine/World.h"
#include "ECS/TransformECS.h"
#include "Core/LogMacros.h"
#include "YamlExceptionBoundary.h"

using namespace Sailor;

YAML::Node WorldPrefab::Serialize() const
{
	if (!IsReady() || !m_loadDiagnostic.empty())
	{
		return YAML::Node();
	}

	YAML::Node outData;
	SERIALIZE_PROPERTY(outData, m_name);
	if (!m_globalIllumination.m_probes.IsEmpty() ||
		m_globalIllumination.m_mode !=
			EGlobalIlluminationMode::Baked)
	{
		outData["globalIllumination"] = m_globalIllumination.Serialize();
	}

	TVector<YAML::Node> nodes;
	for (const auto& prefab : m_gameObjects)
	{
		YAML::Node prefabNode = prefab->Serialize();
		if (prefab->IsLinkedInstanceRecord())
		{
			prefab->SerializeLinkedProperties(prefabNode, prefab->GetFileId());
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
	DESERIALIZE_PROPERTY(inData, m_name);
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
		FileId sourcePrefabId;
		::Deserialize(prefabNode, "fileId", sourcePrefabId);
		if (sourcePrefabId)
		{
			for (const auto* field : { "instanceIds", "gameObjectOverrides", "componentOverrides" })
			{
				const auto value = prefabNode[field];
				if (!value || !value.IsMap())
				{
					m_loadDiagnostic = "linked prefab '" + sourcePrefabId.ToString() +
						"' requires a mapping for '" + field + "'";
					return;
				}
			}
		}

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

	TMap<FileId, PrefabInstance::Snapshot> sourcePrefabs;
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

		auto& source = sourcePrefabs[sourcePrefabId];
		if (!source.m_prefab)
		{
			PrefabAssetInfoPtr sourceAssetInfo =
				App::GetSubmodule<AssetRegistry>()->GetAssetInfoPtr<PrefabAssetInfoPtr>(sourcePrefabId);
			if (!sourceAssetInfo)
			{
				m_loadDiagnostic = "linked prefab " + std::to_string(prefabIndex) +
					" references an unknown source asset " + sourcePrefabId.ToString();
				return;
			}

			std::string sourceText;
			if (!AssetRegistry::ReadTextFile(sourceAssetInfo->GetAssetFilepath(), sourceText))
			{
				m_loadDiagnostic = "cannot read linked prefab source " + sourceAssetInfo->GetAssetFilepath();
				return;
			}

			auto sourcePrefab = App::GetSubmodule<PrefabImporter>()->Create(sourcePrefabId);
			sourcePrefab->Deserialize(YAML::Load(sourceText));
			if (!source.Build(sourcePrefab, m_loadDiagnostic))
			{
				m_loadDiagnostic = "linked prefab source '" + sourceAssetInfo->GetAssetFilepath() +
					"' is invalid: " + m_loadDiagnostic;
				return;
			}
		}

		PrefabPtr expandedPrefab = App::GetSubmodule<PrefabImporter>()->Create();
		expandedPrefab->Deserialize(prefabNode);
		PrefabInstance::Snapshot expanded;
		if (!expanded.Build(expandedPrefab, m_loadDiagnostic))
		{
			m_loadDiagnostic = "expanded linked prefab " +
				std::to_string(prefabIndex) + " is invalid: " +
				m_loadDiagnostic;
			return;
		}

		TMap<InstanceId, InstanceId> savedSourceToInstanceIds;
		::Deserialize(prefabNode, "instanceIds", savedSourceToInstanceIds);

		TMap<InstanceId, InstanceId> sourceToInstanceIds;
		if (!ReconcileLinkedInstanceIds(
				expanded,
				source,
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
		::Deserialize(prefabNode, "gameObjectOverrides", gameObjectOverrides);
		::Deserialize(prefabNode, "componentOverrides", componentOverrides);

		TMap<InstanceId, YAML::Node> filteredGameObjectOverrides;
		for (const auto& overrideEntry : gameObjectOverrides)
		{
			if (source.m_gameObjects.ContainsKey(
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
			const auto* sourceComponent = source.FindComponent(overrideEntry.m_first);
			if (sourceComponent &&
				overrideEntry.m_second->IsValid() &&
				overrideEntry.m_second->GetTypeInfo() == sourceComponent->GetTypeInfo())
			{
				filteredComponentOverrides[
					overrideEntry.m_first] =
					*overrideEntry.m_second;
			}
		}
		componentOverrides = std::move(filteredComponentOverrides);

		PrefabPtr linkedPrefab =
			App::GetSubmodule<PrefabImporter>()->Create(sourcePrefabId);
		if (!linkedPrefab->ConfigureLinkedInstance(
				source,
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
		world->m_prefabLinks.m_instances.Num() != numPrefabInstanceRoots)
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
			!world->m_prefabLinks.m_instances.ContainsKey(
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
			world->m_prefabLinks.m_instances[
				pendingUpdate.m_rootInstanceId];
		link.m_sourceToInstanceIds =
			std::move(pendingUpdate.m_sourceToInstanceIds);
		link.m_effectiveBaseline =
			std::move(pendingUpdate.m_effectiveBaseline);
	}
	world->m_prefabLinks.m_rootsByObject =
		std::move(nextPrefabInstanceRootsByObject);

	return true;
}

bool WorldPrefab::SaveToFile(const std::string& path) const
{
	if (!IsReady() || !m_loadDiagnostic.empty())
	{
		return false;
	}

	std::string contents, diagnostic;
	if (!External::GuardYamlExceptions(
		[this, &contents]() { contents = YAML::Dump(Serialize()); }, diagnostic) ||
		!Platform::IsAtomicWriteComplete(Platform::AtomicWriteFile(path, contents, diagnostic)))
	{
		SAILOR_LOG_ERROR("Cannot save world '%s': %s", path.c_str(), diagnostic.c_str());
		return false;
	}
	return true;
}

WorldPrefabPtr WorldPrefab::FromWorld(WorldPtr world)
{
	auto res = App::GetSubmodule<WorldPrefabImporter>()->Create();
	res->m_name = world->GetName();
	res->m_globalIllumination = world->GetGISettings();

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

	if (linkedRoots.Num() != world->m_prefabLinks.m_instances.Num())
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

	TMap<FileId, PrefabInstance::Snapshot> sourcePrefabs;
	TSet<InstanceId> validatedLinkedMembers;
	for (const GameObjectPtr& root : linkedRootObjects)
	{
		const InstanceId& rootInstanceId =
			root->GetInstanceId();
		const FileId& sourcePrefabId =
			root->GetFileId();
		const PrefabInstanceLink* validatedLink = nullptr;
		if (!world->m_prefabLinks.m_instances.ContainsKey(
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
			world->m_prefabLinks.m_instances[rootInstanceId];
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
				!world->m_prefabLinks.m_rootsByObject.
					ContainsKey(liveInstanceId) ||
				world->m_prefabLinks.m_rootsByObject[
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

		std::string diagnostic;
		auto& source = sourcePrefabs[sourcePrefabId];
		if (!source.m_prefab)
		{
			PrefabPtr sourcePrefab;
			if (!App::GetSubmodule<PrefabImporter>()->LoadPrefab_Immediate(sourcePrefabId, sourcePrefab) ||
				!source.Build(sourcePrefab, diagnostic))
			{
				res->m_loadDiagnostic = "cannot serialize linked prefab '" + sourcePrefabId.ToString() +
					"': " + (diagnostic.empty() ? "source asset is unavailable" : diagnostic);
				SAILOR_LOG_ERROR("%s.", res->m_loadDiagnostic.c_str());
				res->m_gameObjects.Clear();
				return res;
			}
		}

		const auto& sourcePrefab = source.m_prefab;
		PrefabPtr expandedPrefab =
			Prefab::FromGameObject(root, sourcePrefabId);
		PrefabInstance::Snapshot expanded, baseline;
		if (!expanded.Build(expandedPrefab, diagnostic) || !baseline.Build(link.m_effectiveBaseline, diagnostic))
		{
			res->m_loadDiagnostic = "cannot serialize linked prefab '" + sourcePrefabId.ToString() + "': " + diagnostic;
			SAILOR_LOG_ERROR("%s.", res->m_loadDiagnostic.c_str());
			res->m_gameObjects.Clear();
			return res;
		}
		TMap<InstanceId, InstanceId> reconciledInstanceIds;
		if (!ReconcileLinkedInstanceIds(
				expanded,
				source,
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
				expanded,
				source,
				baseline,
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
			const auto* liveGameObject = expanded.FindGameObject(liveInstanceId);

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
				const InstanceId& sourceComponentId = source.m_componentIds[sourceComponentIndex];

				const InstanceId liveComponentId(
					sourceComponentId.ComponentId(),
					liveInstanceId);
				const auto* liveReflection = expanded.FindComponent(liveComponentId);
				if (!liveReflection || liveReflection->GetTypeInfo() != sourceReflection.GetTypeInfo())
				{
					continue;
				}

				const YAML::Node normalizedReflection =
					PrefabInstance::NormalizeReferences(
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
		nextEffectiveBaseline->m_recordType = Prefab::ERecordType::LinkedInstance;
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
		expandedPrefab->m_recordType = Prefab::ERecordType::ExpandedLinkedInstance;
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
		world->m_prefabLinks.m_rootsByObject.Num())
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

	SAILOR_PROFILE_TEXT(assetInfo->GetAssetFilepath());
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
		SAILOR_PROFILE_TEXT(assetInfo->GetAssetFilepath());

		WorldPrefabPtr pWorldPrefab = WorldPrefabPtr::Make(m_allocator, uid);

		promise = Tasks::CreateTaskWithResult<WorldPrefabPtr>("Load WorldPrefab"_h,
			[pWorldPrefab, assetInfo]() mutable
			{
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

				return pWorldPrefab;
			}, EThreadType::Worker);

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
