#include "PrefabImporter.h"
#include "AssetRegistry/FileId.h"
#include "AssetRegistry/AssetRegistry.h"
#include "PrefabAssetInfo.h"
#include "Core/Utils.h"
#include <filesystem>
#include <fstream>
#include <algorithm>
#include <iostream>
#include "Memory/ObjectAllocator.hpp"
#include "Tasks/Scheduler.h"
#include "Engine/GameObject.h"
#include "ECS/TransformECS.h"
#include "Containers/Set.h"
#include "Core/LogMacros.h"
#include "YamlExceptionBoundary.h"

using namespace Sailor;

namespace
{
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

YAML::Node Prefab::ReflectedGameObject::Serialize() const
{
	YAML::Node outData;

	SERIALIZE_PROPERTY(outData, m_name);
	SERIALIZE_PROPERTY(outData, m_position);
	SERIALIZE_PROPERTY(outData, m_rotation);
	SERIALIZE_PROPERTY(outData, m_scale);
	SERIALIZE_PROPERTY(outData, m_parentIndex);
	SERIALIZE_PROPERTY(outData, m_instanceId);
	SERIALIZE_PROPERTY(outData, m_components);

	return outData;
}

void Prefab::ReflectedGameObject::Deserialize(const YAML::Node& inData)
{
	DESERIALIZE_PROPERTY(inData, m_name);
	DESERIALIZE_PROPERTY(inData, m_position);
	DESERIALIZE_PROPERTY(inData, m_rotation);
	DESERIALIZE_PROPERTY(inData, m_scale);
	m_bHasParentIndex = Sailor::Deserialize(inData, "parentIndex", m_parentIndex);
	DESERIALIZE_PROPERTY(inData, m_instanceId);
	DESERIALIZE_PROPERTY(inData, m_components);
}

YAML::Node Prefab::Serialize() const
{
	YAML::Node outData;

	SERIALIZE_PROPERTY(outData, m_gameObjects);
	SERIALIZE_PROPERTY(outData, m_components);
	if (m_bDetachedFromPrefabRecord)
	{
		::Serialize(outData, "detachedFromPrefab", true);
		::Serialize(outData, "parentInstanceId", m_detachedParentInstanceId);
	}
	if (m_bLinkedPrefabSnapshotRecord)
	{
		::Serialize(outData, "linkedPrefabSnapshot", true);
		::Serialize(outData, "fileId", m_linkedSnapshotSourceFileId);
		::Serialize(outData, "parentInstanceId", m_linkedParentInstanceId);
		::Serialize(outData, "instanceIds", m_linkedInstanceIds);
		::Serialize(
			outData,
			"gameObjectOverrides",
			m_gameObjectOverrides);
		::Serialize(
			outData,
			"componentOverrides",
			m_componentOverrides);
	}

	return outData;
}

void Prefab::Deserialize(const YAML::Node& inData)
{
	m_gameObjects.Clear();
	m_components.Clear();
	m_linkedInstanceIds.Clear();
	m_gameObjectOverrides.Clear();
	m_componentOverrides.Clear();
	m_detachedSupplementalInstanceIds.Clear();
	m_linkedSnapshotSourceFileId = FileId::Invalid;
	m_linkedParentInstanceId = InstanceId::Invalid;
	m_detachedParentInstanceId = InstanceId::Invalid;
	m_bLinkedInstanceRecord = false;
	m_bExpandedLinkedInstanceRecord = false;
	m_bDetachedFromPrefabRecord = false;
	m_bLinkedPrefabSnapshotRecord = false;
	m_bIsReady.store(false, std::memory_order_release);

	DESERIALIZE_PROPERTY(inData, m_gameObjects);
	DESERIALIZE_PROPERTY(inData, m_components);
	::Deserialize(
		inData,
		"detachedFromPrefab",
		m_bDetachedFromPrefabRecord);
	::Deserialize(
		inData,
		"linkedPrefabSnapshot",
		m_bLinkedPrefabSnapshotRecord);
	if (m_bDetachedFromPrefabRecord)
	{
		::Deserialize(
			inData,
			"parentInstanceId",
			m_detachedParentInstanceId);
	}
	if (m_bLinkedPrefabSnapshotRecord)
	{
		::Deserialize(
			inData,
			"fileId",
			m_linkedSnapshotSourceFileId);
		::Deserialize(
			inData,
			"parentInstanceId",
			m_linkedParentInstanceId);
		::Deserialize(
			inData,
			"instanceIds",
			m_linkedInstanceIds);
		::Deserialize(
			inData,
			"gameObjectOverrides",
			m_gameObjectOverrides);
		::Deserialize(
			inData,
			"componentOverrides",
			m_componentOverrides);
	}
}

bool Prefab::ValidateForInstantiation(std::string& outDiagnostic) const
{
	outDiagnostic.clear();
	if (m_gameObjects.IsEmpty())
	{
		outDiagnostic = "the prefab has no game objects";
		return false;
	}

	if (m_bDetachedFromPrefabRecord &&
		m_bLinkedPrefabSnapshotRecord)
	{
		outDiagnostic =
			"a prefab snapshot cannot be both detached and linked";
		return false;
	}

	if (m_bDetachedFromPrefabRecord)
	{
		if (GetFileId() ||
			m_bLinkedInstanceRecord ||
			m_bExpandedLinkedInstanceRecord ||
			!m_detachedParentInstanceId.IsGameObjectId())
		{
			outDiagnostic =
				"a detached prefab snapshot must have no source asset, no linked-instance metadata, and a valid parent";
			return false;
		}
	}
	else if (m_detachedParentInstanceId)
	{
		outDiagnostic =
			"a prefab without detachedFromPrefab metadata cannot specify a detached parent";
		return false;
	}

	if (m_bLinkedPrefabSnapshotRecord)
	{
		if (GetFileId() ||
			m_bLinkedInstanceRecord ||
			m_bExpandedLinkedInstanceRecord ||
			!m_linkedSnapshotSourceFileId ||
			m_linkedInstanceIds.IsEmpty() ||
			(m_linkedParentInstanceId &&
				!m_linkedParentInstanceId.IsGameObjectId()))
		{
			outDiagnostic =
				"a linked prefab snapshot must have no runtime FileId and must contain a source, mapping, and valid optional parent";
			return false;
		}
	}
	else if (m_linkedSnapshotSourceFileId)
	{
		outDiagnostic =
			"a prefab without linkedPrefabSnapshot metadata cannot specify a snapshot source";
		return false;
	}

	if (!m_bLinkedInstanceRecord &&
		!m_detachedSupplementalInstanceIds.IsEmpty())
	{
		outDiagnostic =
			"only a linked prefab instance record can contain detached supplemental game objects";
		return false;
	}

	TSet<InstanceId> componentInstanceIds;
	TVector<InstanceId> componentInstanceIdsByIndex;
	componentInstanceIdsByIndex.Reserve(m_components.Num());
	for (uint32_t componentIndex = 0; componentIndex < m_components.Num(); ++componentIndex)
	{
		const ReflectedData& reflection = m_components[componentIndex];
		if (!reflection.IsValid())
		{
			outDiagnostic = "reflected component " + std::to_string(componentIndex) +
				" has an unknown type; load or rebuild the workspace logic module";
			return false;
		}

		InstanceId componentInstanceId;
		std::string conversionDiagnostic;
		if (!Utils::TryGetComponentInstanceId(
				reflection,
				componentInstanceId,
				conversionDiagnostic))
		{
			outDiagnostic = "reflected component " + std::to_string(componentIndex) +
				" has an invalid identity: " + conversionDiagnostic;
			return false;
		}

		if (!componentInstanceIds.Insert(componentInstanceId))
		{
			outDiagnostic = "reflected component " + std::to_string(componentIndex) +
				" has a duplicate instanceId";
			return false;
		}

		componentInstanceIdsByIndex.Add(componentInstanceId);
	}

	const uint32_t invalidParentIndex = static_cast<uint32_t>(-1);
	uint32_t numRoots = 0;
	TSet<InstanceId> gameObjectInstanceIds;
	TSet<uint32_t> referencedComponentIndices;
	for (uint32_t gameObjectIndex = 0; gameObjectIndex < m_gameObjects.Num(); ++gameObjectIndex)
	{
		const auto& gameObject = m_gameObjects[gameObjectIndex];
		if (!gameObject.m_bHasParentIndex)
		{
			outDiagnostic = "game object " + std::to_string(gameObjectIndex) +
				" has no parentIndex";
			return false;
		}

		if (!gameObject.m_instanceId.IsGameObjectId())
		{
			outDiagnostic = "game object " + std::to_string(gameObjectIndex) +
				" has an invalid instanceId";
			return false;
		}

		if (!gameObjectInstanceIds.Insert(gameObject.m_instanceId))
		{
			outDiagnostic = "game object " + std::to_string(gameObjectIndex) +
				" has a duplicate instanceId";
			return false;
		}

		if (gameObject.m_parentIndex == invalidParentIndex)
		{
			++numRoots;
		}
		else if (gameObject.m_parentIndex >= m_gameObjects.Num())
		{
			outDiagnostic = "game object " + std::to_string(gameObjectIndex) +
				" has invalid parent index " + std::to_string(gameObject.m_parentIndex);
			return false;
		}

		for (const uint32_t componentIndex : gameObject.m_components)
		{
			if (componentIndex >= m_components.Num())
			{
				outDiagnostic = "game object " + std::to_string(gameObjectIndex) +
					" references invalid component index " + std::to_string(componentIndex);
				return false;
			}

			if (!referencedComponentIndices.Insert(componentIndex))
			{
				outDiagnostic = "component index " + std::to_string(componentIndex) +
					" is referenced more than once";
				return false;
			}

			if (componentInstanceIdsByIndex[componentIndex].GameObjectId() != gameObject.m_instanceId)
			{
				outDiagnostic = "reflected component " + std::to_string(componentIndex) +
					" belongs to a different game object";
				return false;
			}
		}
	}

	if (m_bLinkedInstanceRecord)
	{
		TSet<InstanceId> mappedExpandedInstanceIds;
		if (m_bExpandedLinkedInstanceRecord)
		{
			for (const auto& mapping :
				m_linkedInstanceIds)
			{
				mappedExpandedInstanceIds.Insert(
					*mapping.m_second);
			}
		}

		for (const auto& gameObject : m_gameObjects)
		{
			const bool bMappedSource =
				m_bExpandedLinkedInstanceRecord
					? mappedExpandedInstanceIds.Contains(
						gameObject.m_instanceId)
					: m_linkedInstanceIds.ContainsKey(
						gameObject.m_instanceId);
			const bool bDetachedSupplemental =
				m_detachedSupplementalInstanceIds.Contains(
					gameObject.m_instanceId);
			if (bMappedSource == bDetachedSupplemental)
			{
				outDiagnostic =
					"every linked prefab game object must be either a mapped source object or detached supplemental data";
				return false;
			}
		}

		for (const InstanceId& supplementalInstanceId :
			m_detachedSupplementalInstanceIds)
		{
			if (!gameObjectInstanceIds.Contains(
					supplementalInstanceId))
			{
				outDiagnostic =
					"the linked prefab contains detached supplemental metadata for an unknown game object";
				return false;
			}
		}
	}

	if (referencedComponentIndices.Num() != m_components.Num())
	{
		outDiagnostic = "the prefab contains an unreferenced reflected component";
		return false;
	}

	if (numRoots != 1)
	{
		outDiagnostic = "the prefab must contain exactly one root game object";
		return false;
	}

	for (uint32_t gameObjectIndex = 0; gameObjectIndex < m_gameObjects.Num(); ++gameObjectIndex)
	{
		uint32_t ancestorIndex = gameObjectIndex;
		bool bReachedRoot = false;
		for (uint32_t depth = 0; depth < m_gameObjects.Num(); ++depth)
		{
			const uint32_t parentIndex = m_gameObjects[ancestorIndex].m_parentIndex;
			if (parentIndex == invalidParentIndex)
			{
				bReachedRoot = true;
				break;
			}

			ancestorIndex = parentIndex;
		}

		if (!bReachedRoot)
		{
			outDiagnostic = "the prefab hierarchy contains a parent cycle at game object " +
				std::to_string(gameObjectIndex);
			return false;
		}
	}

	return true;
}

bool Prefab::SaveToFile(const std::string& path) const
{
	AssetRegistry::WriteTextFile(path, Serialize());
	return true;
}

bool Prefab::GetOverridePrefab(
	const PrefabPtr base,
	PrefabPtr outOverride) const
{
	if (base->GetFileId() != GetFileId())
	{
		return false;
	}

	if (base->m_gameObjects.Num() != m_gameObjects.Num() ||
		base->m_components.Num() != m_components.Num())
	{
		return false;
	}

	PrefabPtr res = App::GetSubmodule<PrefabImporter>()->Create();

	res->m_components.Reserve(m_components.Num());
	res->m_gameObjects = m_gameObjects;

	for (uint32_t i = 0; i < m_components.Num(); i++)
	{
		res->m_components.Add(m_components[i].DiffTo(base->m_components[i]));
	}

	return true;
}

void Prefab::SerializeGameObject(
	GameObjectPtr root,
	uint32_t parentIndex,
	TVector<ReflectedData>& components,
	TVector<Prefab::ReflectedGameObject>& gameObjects,
	const TSet<InstanceId>* excludedRoots)
{
	if (!root || (excludedRoots && excludedRoots->Contains(root->GetInstanceId())))
	{
		return;
	}

	Prefab::ReflectedGameObject rootData{};

	auto& transform = root->GetTransformComponent();
	rootData.m_position = transform.GetPosition();
	rootData.m_rotation = transform.GetRotation();
	rootData.m_scale = transform.GetScale();

	rootData.m_name = root->GetName();
	rootData.m_parentIndex = parentIndex;
	rootData.m_instanceId = root->GetInstanceId();

	for (auto& component : root->GetComponents())
	{
		auto reflection = component->GetReflectedData();

		rootData.m_components.Add((uint32_t)components.Num());
		components.Add(reflection);
	}

	parentIndex = (uint32_t)gameObjects.Num();
	gameObjects.Add(rootData);

	for (auto& child : root->GetChildren())
	{
		SerializeGameObject(child, parentIndex, components, gameObjects, excludedRoots);
	}
}

bool Prefab::ConfigureLinkedInstance(
	const PrefabPtr& basePrefab,
	const TMap<InstanceId, InstanceId>& sourceToInstanceIds,
	const InstanceId& parentInstanceId,
	const TMap<InstanceId, YAML::Node>& gameObjectOverrides,
	const TMap<InstanceId, ReflectedData>& componentOverrides,
	std::string& outDiagnostic)
{
	outDiagnostic.clear();
	m_bIsReady.store(false, std::memory_order_release);
	m_gameObjects.Clear();
	m_components.Clear();
	m_linkedInstanceIds.Clear();
	m_gameObjectOverrides.Clear();
	m_componentOverrides.Clear();
	m_detachedSupplementalInstanceIds.Clear();
	m_linkedSnapshotSourceFileId = FileId::Invalid;
	m_linkedParentInstanceId = InstanceId::Invalid;
	m_detachedParentInstanceId = InstanceId::Invalid;
	m_bLinkedInstanceRecord = false;
	m_bExpandedLinkedInstanceRecord = false;
	m_bDetachedFromPrefabRecord = false;
	m_bLinkedPrefabSnapshotRecord = false;

	if (basePrefab && basePrefab.GetRawPtr() == this)
	{
		outDiagnostic = "a linked prefab cannot use itself as its source";
		return false;
	}

	if (!basePrefab || !basePrefab->GetFileId() || basePrefab->GetFileId() != GetFileId())
	{
		outDiagnostic = "the linked prefab source is missing or has a mismatched FileId";
		return false;
	}

	if (!basePrefab->ValidateForInstantiation(outDiagnostic))
	{
		return false;
	}

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

	for (const auto& mapping : sourceToInstanceIds)
	{
		bool bKnownSource = false;
		for (const auto& gameObject : basePrefab->m_gameObjects)
		{
			if (gameObject.m_instanceId == mapping.m_first)
			{
				bKnownSource = true;
				break;
			}
		}

		if (!bKnownSource)
		{
			outDiagnostic = "the linked prefab instance mapping contains an unknown source game object id";
			return false;
		}
	}

	m_gameObjects = basePrefab->m_gameObjects;
	m_components = basePrefab->m_components;

	for (const auto& overrideEntry : gameObjectOverrides)
	{
		ReflectedGameObject* target = nullptr;
		for (auto& gameObject : m_gameObjects)
		{
			if (gameObject.m_instanceId == overrideEntry.m_first)
			{
				target = &gameObject;
				break;
			}
		}

		if (!target || !overrideEntry.m_second || !overrideEntry.m_second->IsMap())
		{
			outDiagnostic = "the linked prefab contains a game object override for an unknown source id";
			return false;
		}

		const YAML::Node& properties = *overrideEntry.m_second;
		for (const auto& property : properties)
		{
			const std::string name = property.first.as<std::string>();
			if (name != "name" && name != "position" && name != "rotation" && name != "scale")
			{
				outDiagnostic = "unsupported linked game object override property '" + name + "'";
				return false;
			}
		}

		if (!External::GuardYamlExceptions(
				[&properties, target]()
				{
					::Deserialize(properties, "name", target->m_name);
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
		uint32_t componentIndex = static_cast<uint32_t>(-1);
		for (uint32_t index = 0; index < m_components.Num(); ++index)
		{
			InstanceId componentInstanceId;
			std::string conversionDiagnostic;
			if (!Utils::TryGetComponentInstanceId(m_components[index], componentInstanceId, conversionDiagnostic))
			{
				outDiagnostic = conversionDiagnostic;
				return false;
			}

			if (componentInstanceId == overrideEntry.m_first)
			{
				componentIndex = index;
				break;
			}
		}

		if (componentIndex == static_cast<uint32_t>(-1))
		{
			outDiagnostic = "the linked prefab contains a component override for an unknown source id";
			return false;
		}

		ReflectedData merged;
		if (!TryMergeComponentOverride(
				m_components[componentIndex],
				*overrideEntry.m_second,
				merged,
				outDiagnostic))
		{
			return false;
		}

		m_components[componentIndex] = std::move(merged);
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

bool Prefab::AppendDetachedSupplementalHierarchy(
	const PrefabPtr& expandedPrefab,
	std::string& outDiagnostic)
{
	outDiagnostic.clear();
	if (!m_bLinkedInstanceRecord ||
		!expandedPrefab ||
		expandedPrefab.GetRawPtr() == this)
	{
		outDiagnostic =
			"detached supplemental hierarchy requires distinct expanded and linked prefab records";
		return false;
	}

	if (!ValidateForInstantiation(outDiagnostic) ||
		!expandedPrefab->ValidateForInstantiation(outDiagnostic))
	{
		return false;
	}

	TMap<InstanceId, uint32_t> sourceIndices;
	for (uint32_t sourceIndex = 0;
		sourceIndex < m_gameObjects.Num();
		++sourceIndex)
	{
		sourceIndices[
			m_gameObjects[sourceIndex].m_instanceId] =
			sourceIndex;
	}

	TMap<InstanceId, InstanceId> liveToSourceIds;
	for (const auto& mapping : m_linkedInstanceIds)
	{
		if (!mapping.m_first.IsGameObjectId() ||
			!mapping.m_second->IsGameObjectId() ||
			liveToSourceIds.ContainsKey(*mapping.m_second))
		{
			outDiagnostic =
				"the linked prefab mapping contains an invalid or duplicate live game object id";
			return false;
		}

		liveToSourceIds[*mapping.m_second] =
			mapping.m_first;
	}

	const uint32_t invalidIndex =
		static_cast<uint32_t>(-1);
	TVector<uint32_t> expandedToResultIndices;
	expandedToResultIndices.Reserve(
		expandedPrefab->m_gameObjects.Num());

	uint32_t nextResultIndex =
		static_cast<uint32_t>(m_gameObjects.Num());
	for (const ReflectedGameObject& expandedGameObject :
		expandedPrefab->m_gameObjects)
	{
		if (liveToSourceIds.ContainsKey(
				expandedGameObject.m_instanceId))
		{
			const InstanceId& sourceInstanceId =
				liveToSourceIds[
					expandedGameObject.m_instanceId];
			if (!sourceIndices.ContainsKey(sourceInstanceId))
			{
				outDiagnostic =
					"the expanded linked prefab maps to an unknown source game object";
				return false;
			}

			expandedToResultIndices.Add(
				sourceIndices[sourceInstanceId]);
			continue;
		}

		if (sourceIndices.ContainsKey(
				expandedGameObject.m_instanceId))
		{
			outDiagnostic =
				"a detached supplemental game object id collides with a current source id";
			return false;
		}

		expandedToResultIndices.Add(nextResultIndex++);
	}

	TVector<ReflectedGameObject> nextGameObjects =
		m_gameObjects;
	TVector<ReflectedData> nextComponents = m_components;
	TSet<InstanceId> nextSupplementalInstanceIds =
		m_detachedSupplementalInstanceIds;

	for (uint32_t expandedIndex = 0;
		expandedIndex < expandedPrefab->m_gameObjects.Num();
		++expandedIndex)
	{
		const ReflectedGameObject& expandedGameObject =
			expandedPrefab->m_gameObjects[expandedIndex];
		if (liveToSourceIds.ContainsKey(
				expandedGameObject.m_instanceId))
		{
			continue;
		}

		ReflectedGameObject supplementalGameObject =
			expandedGameObject;
		supplementalGameObject.m_components.Clear();

		if (expandedGameObject.m_parentIndex ==
			invalidIndex)
		{
			outDiagnostic =
				"the expanded linked prefab root is not covered by the current source mapping";
			return false;
		}

		if (expandedGameObject.m_parentIndex >=
			expandedToResultIndices.Num())
		{
			outDiagnostic =
				"a detached supplemental game object has an invalid expanded parent";
			return false;
		}

		supplementalGameObject.m_parentIndex =
			expandedToResultIndices[
				expandedGameObject.m_parentIndex];
		for (const uint32_t expandedComponentIndex :
			expandedGameObject.m_components)
		{
			if (expandedComponentIndex >=
				expandedPrefab->m_components.Num())
			{
				outDiagnostic =
					"a detached supplemental game object references an invalid component";
				return false;
			}

			supplementalGameObject.m_components.Add(
				static_cast<uint32_t>(
					nextComponents.Num()));
			nextComponents.Add(
				expandedPrefab->m_components[
					expandedComponentIndex]);
		}

		if (!nextSupplementalInstanceIds.Insert(
				supplementalGameObject.m_instanceId))
		{
			outDiagnostic =
				"the expanded linked prefab contains duplicate detached supplemental data";
			return false;
		}

		nextGameObjects.Add(
			std::move(supplementalGameObject));
	}

	m_bIsReady.store(false, std::memory_order_release);
	m_gameObjects = std::move(nextGameObjects);
	m_components = std::move(nextComponents);
	m_detachedSupplementalInstanceIds =
		std::move(nextSupplementalInstanceIds);
	if (!ValidateForInstantiation(outDiagnostic))
	{
		return false;
	}

	m_bIsReady.store(true, std::memory_order_release);
	return true;
}

PrefabPtr Prefab::FromGameObject(
	GameObjectPtr root,
	const FileId& sourcePrefabId,
	const TSet<InstanceId>* excludedRoots)
{
	PrefabPtr res = App::GetSubmodule<PrefabImporter>()->Create(sourcePrefabId);

	SerializeGameObject(root, -1, res->m_components, res->m_gameObjects, excludedRoots);
	std::string diagnostic;
	res->m_bIsReady.store(res->ValidateForInstantiation(diagnostic), std::memory_order_release);

	return res;
}

PrefabImporter::PrefabImporter(PrefabAssetInfoHandler* infoHandler)
{
	SAILOR_PROFILE_FUNCTION();
	m_allocator = ObjectAllocatorPtr::Make(EAllocationPolicy::SharedMemory_MultiThreaded);
	infoHandler->Subscribe(this);
}

PrefabImporter::~PrefabImporter()
{
	for (auto& model : m_loadedPrefabs)
	{
		model.m_second.DestroyObject(m_allocator);
	}
}

PrefabPtr PrefabImporter::Create()
{
	return Create(FileId::Invalid);
}

PrefabPtr PrefabImporter::Create(const FileId& uid)
{
	return PrefabPtr::Make(m_allocator, uid);
}

void PrefabImporter::OnUpdateAssetInfo(AssetInfoPtr assetInfo, bool bWasExpired)
{
	SAILOR_PROFILE_FUNCTION();
	if (!assetInfo || !bWasExpired)
	{
		return;
	}

	const FileId uid = assetInfo->GetFileId();
	m_loadedPrefabs.Remove(uid);
	m_promises.Remove(uid);
}

void PrefabImporter::OnImportAsset(AssetInfoPtr assetInfo) {}

bool PrefabImporter::LoadPrefab_Immediate(FileId uid, PrefabPtr& outPrefab)
{
	SAILOR_PROFILE_FUNCTION();

	auto task = LoadPrefab(uid, outPrefab);
	if (!task)
	{
		return false;
	}

	task->Wait();
	return task->GetResult().IsValid() && outPrefab && outPrefab->IsReady();
}

Tasks::TaskPtr<PrefabPtr> PrefabImporter::LoadPrefab(FileId uid, PrefabPtr& outPrefab)
{
	SAILOR_PROFILE_FUNCTION();

	// Check promises first
	auto& promise = m_promises.At_Lock(uid, nullptr);
	auto& loadedPrefab = m_loadedPrefabs.At_Lock(uid, PrefabPtr());

	// Check loaded assets
	if (loadedPrefab)
	{
		if (promise && !promise->IsFinished())
		{
			outPrefab = loadedPrefab;
			auto res = promise;

			m_loadedPrefabs.Unlock(uid);
			m_promises.Unlock(uid);

			return res;
		}

		if (loadedPrefab->IsReady())
		{
			outPrefab = loadedPrefab;
			auto res = Tasks::TaskPtr<PrefabPtr>::Make(outPrefab);

			m_loadedPrefabs.Unlock(uid);
			m_promises.Unlock(uid);

			return res;
		}

		loadedPrefab = nullptr;
		promise = nullptr;
	}

	// There is no promise, we need to load prefab
	if (PrefabAssetInfoPtr assetInfo = App::GetSubmodule<AssetRegistry>()->GetAssetInfoPtr<PrefabAssetInfoPtr>(uid))
	{
		SAILOR_PROFILE_TEXT(assetInfo->GetAssetFilepath().c_str());

		PrefabPtr prefab = PrefabPtr::Make(m_allocator, uid);

		struct Data {};
		promise = Tasks::CreateTaskWithResult<TSharedPtr<Data>>("Load prefab",
			[prefab, assetInfo]() mutable
			{
				TSharedPtr<Data> res = TSharedPtr<Data>::Make();

				std::string text;
				std::string diagnostic;
				bool bLoaded = AssetRegistry::ReadTextFile(assetInfo->GetAssetFilepath(), text);
				if (bLoaded)
				{
					bLoaded = External::GuardYamlExceptions(
						[&prefab, &text]()
						{
							prefab->Deserialize(YAML::Load(text));
						},
						diagnostic);
				}

				if (bLoaded)
				{
					bLoaded = prefab->ValidateForInstantiation(diagnostic);
				}

				prefab->m_bIsReady.store(bLoaded, std::memory_order_release);
				if (!bLoaded)
				{
					SAILOR_LOG_ERROR(
						"Cannot load prefab '%s': %s.",
						assetInfo->GetAssetFilepath().c_str(),
						diagnostic.empty() ? "cannot read the prefab file" : diagnostic.c_str());
				}

				return res;

			}, EThreadType::Worker)->Then<PrefabPtr>([prefab](TSharedPtr<Data> data) mutable
				{
					return prefab;
				}, "Preload resources", EThreadType::RHI)->ToTaskWithResult();

				outPrefab = loadedPrefab = prefab;
				promise->Run();

				m_loadedPrefabs.Unlock(uid);
				m_promises.Unlock(uid);

				return promise;
	}

	outPrefab = nullptr;
	m_loadedPrefabs.Unlock(uid);
	m_promises.Unlock(uid);

	return Tasks::TaskPtr<PrefabPtr>();
}

bool PrefabImporter::LoadAsset(FileId uid, TObjectPtr<Object>& out, bool bImmediate)
{
	PrefabPtr outAsset;
	if (bImmediate)
	{
		bool bRes = LoadPrefab_Immediate(uid, outAsset);
		out = outAsset;
		return bRes;
	}

	LoadPrefab(uid, outAsset);
	out = outAsset;
	return true;
}

void PrefabImporter::CollectGarbage()
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
