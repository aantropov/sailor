#include "Engine/World.h"
#include "Engine/GameObject.h"
#include "AssetRegistry/Prefab/PrefabImporter.h"
#include "Core/LogMacros.h"
#include "Core/Reflection.h"
#include "ECS/TransformECS.h"
#include "AssetRegistry/World/WorldPrefabImporter.h"

using namespace Sailor;

bool World::TryGetPrefabInstance(
	const InstanceId& objectInstanceId,
	const PrefabInstanceLink*& outLink) const
{
	outLink = nullptr;
	GameObjectPtr object = GetObjectByInstanceId(
		objectInstanceId).DynamicCast<GameObject>();
	if (!object)
	{
		return false;
	}

	InstanceId rootInstanceId;
	if (object->GetFileId())
	{
		rootInstanceId = objectInstanceId;
	}
	else if (m_prefabLinks.m_rootsByObject.ContainsKey(
			objectInstanceId))
	{
		rootInstanceId =
			m_prefabLinks.m_rootsByObject[objectInstanceId];
	}
	else
	{
		return false;
	}

	if (!m_prefabLinks.m_instances.ContainsKey(rootInstanceId))
	{
		return false;
	}

	GameObjectPtr root = GetObjectByInstanceId(
		rootInstanceId).DynamicCast<GameObject>();
	const PrefabInstanceLink& link =
		m_prefabLinks.m_instances[rootInstanceId];
	if (!root ||
		!root->GetFileId() ||
		link.m_rootInstanceId != rootInstanceId ||
		!link.m_effectiveBaseline ||
		link.m_effectiveBaseline->GetFileId() !=
			root->GetFileId() ||
		!m_prefabLinks.m_rootsByObject.ContainsKey(
			objectInstanceId) ||
		m_prefabLinks.m_rootsByObject[objectInstanceId] !=
			rootInstanceId)
	{
		return false;
	}

	bool bContainsObject = false;
	bool bContainsRoot = false;
	for (const auto& mapping : link.m_sourceToInstanceIds)
	{
		bContainsObject |= *mapping.m_second ==
			objectInstanceId;
		bContainsRoot |= *mapping.m_second ==
			rootInstanceId;
	}

	if (!bContainsObject || !bContainsRoot)
	{
		return false;
	}

	outLink = &link;
	return true;
}

bool World::IsPrefabLinked(const InstanceId& objectInstanceId) const
{
	if (IsPrefabInstanceRoot(objectInstanceId))
	{
		return true;
	}

	const PrefabInstanceLink* link = nullptr;
	return TryGetPrefabInstance(objectInstanceId, link);
}

bool World::IsPrefabInstanceRoot(const InstanceId& objectInstanceId) const
{
	GameObjectPtr object = GetObjectByInstanceId(
		objectInstanceId).DynamicCast<GameObject>();
	return object && object->GetFileId();
}

bool World::RegisterPrefabInstance(
	GameObjectPtr root,
	const FileId& sourcePrefabId,
	const TMap<InstanceId, InstanceId>& sourceToInstanceIds,
	const PrefabPtr& effectiveBaseline,
	std::string& outDiagnostic)
{
	outDiagnostic.clear();
	if (!root ||
		!sourcePrefabId ||
		sourceToInstanceIds.IsEmpty() ||
		!effectiveBaseline ||
		effectiveBaseline->GetFileId() != sourcePrefabId)
	{
		outDiagnostic = "the prefab link has no root, source asset, instance mapping, or effective baseline";
		return false;
	}

	if (!effectiveBaseline->ValidateForInstantiation(outDiagnostic))
	{
		outDiagnostic = "the prefab link has an invalid effective baseline: " +
			outDiagnostic;
		return false;
	}

	if (root->GetWorld() != this ||
		!m_objectsMap.ContainsKey(root->GetInstanceId()) ||
		GetObjectByInstanceId(root->GetInstanceId()).DynamicCast<GameObject>() !=
			root)
	{
		outDiagnostic = "the prefab instance root does not belong to this world";
		return false;
	}

	if (root->GetFileId() ||
		m_prefabLinks.m_instances.ContainsKey(root->GetInstanceId()) ||
		m_prefabLinks.m_rootsByObject.ContainsKey(
			root->GetInstanceId()))
	{
		outDiagnostic = "the prefab instance root is already linked";
		return false;
	}

	TSet<InstanceId> liveInstanceIds;
	for (const auto& mapping : sourceToInstanceIds)
	{
		const InstanceId& liveInstanceId = *mapping.m_second;
		GameObjectPtr liveGameObject =
			GetObjectByInstanceId(
				liveInstanceId).DynamicCast<GameObject>();
		if (!mapping.m_first.IsGameObjectId() ||
			!liveInstanceId.IsGameObjectId() ||
			!liveInstanceIds.Insert(liveInstanceId) ||
			!liveGameObject ||
			(liveGameObject != root &&
				liveGameObject->GetFileId()))
		{
			outDiagnostic = "the prefab link contains an invalid, duplicate, or missing game object";
			return false;
		}

		for (GameObjectPtr current = liveGameObject;
			current;
			current = current->GetParent())
		{
			if (current == root)
			{
				break;
			}

			if (!current->GetParent())
			{
				outDiagnostic = "a mapped game object is outside the prefab instance hierarchy";
				return false;
			}
		}

		if (m_prefabLinks.m_rootsByObject.ContainsKey(liveInstanceId))
		{
			outDiagnostic = "a mapped game object already belongs to another linked prefab instance";
			return false;
		}
	}

	if (!liveInstanceIds.Contains(root->GetInstanceId()))
	{
		outDiagnostic = "the prefab instance mapping does not contain its live root";
		return false;
	}

	PrefabInstanceLink link;
	link.m_rootInstanceId = root->GetInstanceId();
	link.m_sourceToInstanceIds = sourceToInstanceIds;
	link.m_effectiveBaseline =
		PrefabPtr::Make(m_allocator, sourcePrefabId);
	link.m_effectiveBaseline->m_gameObjects =
		effectiveBaseline->m_gameObjects;
	link.m_effectiveBaseline->m_components =
		effectiveBaseline->m_components;
	link.m_effectiveBaseline->m_linkedInstanceIds =
		effectiveBaseline->m_linkedInstanceIds;
	link.m_effectiveBaseline->m_gameObjectOverrides =
		effectiveBaseline->m_gameObjectOverrides;
	link.m_effectiveBaseline->m_componentOverrides =
		effectiveBaseline->m_componentOverrides;
	link.m_effectiveBaseline->m_detachedSupplementalInstanceIds =
		effectiveBaseline->m_detachedSupplementalInstanceIds;
	link.m_effectiveBaseline->m_linkedParentInstanceId =
		effectiveBaseline->m_linkedParentInstanceId;
	link.m_effectiveBaseline->m_recordType = effectiveBaseline->m_recordType;
	link.m_effectiveBaseline->m_bIsReady.store(
		effectiveBaseline->IsReady(),
		std::memory_order_release);
	for (const auto& mapping : sourceToInstanceIds)
	{
		GameObjectPtr liveGameObject =
			GetObjectByInstanceId(*mapping.m_second).DynamicCast<GameObject>();
		if (!liveGameObject)
		{
			continue;
		}

		for (Prefab::ReflectedGameObject& baselineGameObject :
			link.m_effectiveBaseline->m_gameObjects)
		{
			if (baselineGameObject.m_instanceId != mapping.m_first)
			{
				continue;
			}

			baselineGameObject.m_name = liveGameObject->GetName();
			baselineGameObject.m_mobilityType =
				liveGameObject->GetMobilityType();
			baselineGameObject.m_position =
				liveGameObject->GetTransformComponent().GetPosition();
			baselineGameObject.m_rotation =
				liveGameObject->GetTransformComponent().GetRotation();
			baselineGameObject.m_scale =
				liveGameObject->GetTransformComponent().GetScale();
			break;
		}
	}

	m_prefabLinks.m_instances[link.m_rootInstanceId] = link;
	for (const auto& mapping : sourceToInstanceIds)
	{
		m_prefabLinks.m_rootsByObject[*mapping.m_second] = link.m_rootInstanceId;
	}

	// The root FileId is the authoritative prefab-link marker. Publish it only
	// after the derived metadata and membership cache are complete.
	root->m_fileId = sourcePrefabId;
	return true;
}

bool World::LinkPrefabInstance(
	GameObjectPtr root,
	const PrefabPtr& sourcePrefab,
	std::string& outDiagnostic)
{
	outDiagnostic.clear();
	if (!root || !sourcePrefab)
	{
		outDiagnostic = "the prefab instance root or source prefab is invalid";
		return false;
	}

	TVector<GameObjectPtr> liveGameObjects;
	TVector<GameObjectPtr> pendingGameObjects;
	pendingGameObjects.Add(root);
	while (!pendingGameObjects.IsEmpty())
	{
		GameObjectPtr current =
			pendingGameObjects[pendingGameObjects.Num() - 1];
		pendingGameObjects.RemoveLast();
		liveGameObjects.Add(current);

		const auto& children = current->GetChildren();
		for (size_t childIndex = children.Num();
			childIndex > 0;
			--childIndex)
		{
			pendingGameObjects.Add(children[childIndex - 1]);
		}
	}

	if (liveGameObjects.Num() != sourcePrefab->m_gameObjects.Num())
	{
		outDiagnostic = "the live hierarchy size does not match the source prefab";
		return false;
	}

	const uint32_t invalidParentIndex = static_cast<uint32_t>(-1);
	uint32_t sourceRootIndex = invalidParentIndex;
	for (uint32_t gameObjectIndex = 0;
		gameObjectIndex < sourcePrefab->m_gameObjects.Num();
		++gameObjectIndex)
	{
		if (sourcePrefab->m_gameObjects[gameObjectIndex].m_parentIndex ==
			invalidParentIndex)
		{
			sourceRootIndex = gameObjectIndex;
			break;
		}
	}

	if (sourceRootIndex == invalidParentIndex)
	{
		outDiagnostic = "the source prefab has no hierarchy root";
		return false;
	}

	TVector<uint32_t> sourcePreorder;
	TVector<uint32_t> pendingSourceIndices;
	pendingSourceIndices.Add(sourceRootIndex);
	while (!pendingSourceIndices.IsEmpty())
	{
		const uint32_t currentSourceIndex =
			pendingSourceIndices[pendingSourceIndices.Num() - 1];
		pendingSourceIndices.RemoveLast();
		sourcePreorder.Add(currentSourceIndex);

		for (uint32_t candidateIndex = static_cast<uint32_t>(
				sourcePrefab->m_gameObjects.Num());
			candidateIndex > 0;
			--candidateIndex)
		{
			const uint32_t childIndex = candidateIndex - 1;
			if (sourcePrefab->m_gameObjects[childIndex].m_parentIndex ==
				currentSourceIndex)
			{
				pendingSourceIndices.Add(childIndex);
			}
		}
	}

	if (sourcePreorder.Num() != sourcePrefab->m_gameObjects.Num())
	{
		outDiagnostic = "the source prefab hierarchy is disconnected";
		return false;
	}

	TMap<InstanceId, InstanceId> sourceToInstanceIds;
	for (uint32_t preorderIndex = 0;
		preorderIndex < sourcePreorder.Num();
		++preorderIndex)
	{
		sourceToInstanceIds[
			sourcePrefab->m_gameObjects[
				sourcePreorder[preorderIndex]].m_instanceId] =
			liveGameObjects[preorderIndex]->GetInstanceId();
	}

	return LinkPrefabInstance(
		root,
		sourcePrefab,
		sourceToInstanceIds,
		outDiagnostic);
}

bool World::LinkPrefabInstance(
	GameObjectPtr root,
	const PrefabPtr& sourcePrefab,
	const TMap<InstanceId, InstanceId>& sourceToInstanceIds,
	std::string& outDiagnostic)
{
	outDiagnostic.clear();
	if (!root || !sourcePrefab || !sourcePrefab->GetFileId())
	{
		outDiagnostic = "the prefab instance root or source prefab is invalid";
		return false;
	}

	if (!sourcePrefab->ValidateForInstantiation(outDiagnostic))
	{
		return false;
	}

	if (sourceToInstanceIds.Num() != sourcePrefab->m_gameObjects.Num())
	{
		outDiagnostic = "the source-to-instance mapping does not cover the complete prefab hierarchy";
		return false;
	}

	const uint32_t invalidParentIndex = static_cast<uint32_t>(-1);
	uint32_t numLiveObjects = 0;
	TVector<GameObjectPtr> pendingObjects;
	pendingObjects.Add(root);
	while (!pendingObjects.IsEmpty())
	{
		GameObjectPtr current = pendingObjects[pendingObjects.Num() - 1];
		pendingObjects.RemoveLast();
		++numLiveObjects;
		pendingObjects.AddRange(current->GetChildren());
	}

	if (numLiveObjects != sourcePrefab->m_gameObjects.Num())
	{
		outDiagnostic = "the live hierarchy has structural changes and cannot be linked";
		return false;
	}

	for (uint32_t gameObjectIndex = 0;
		gameObjectIndex < sourcePrefab->m_gameObjects.Num();
		++gameObjectIndex)
	{
		const auto& sourceGameObject = sourcePrefab->m_gameObjects[gameObjectIndex];
		if (!sourceToInstanceIds.ContainsKey(sourceGameObject.m_instanceId))
		{
			outDiagnostic = "the source-to-instance mapping is missing a source game object";
			return false;
		}

		GameObjectPtr liveGameObject = GetObjectByInstanceId(
			sourceToInstanceIds[sourceGameObject.m_instanceId]).DynamicCast<GameObject>();
		if (!liveGameObject)
		{
			outDiagnostic = "the source-to-instance mapping references a missing live game object";
			return false;
		}

		if (sourceGameObject.m_parentIndex == invalidParentIndex)
		{
			if (liveGameObject != root)
			{
				outDiagnostic = "the source prefab root maps to a different live game object";
				return false;
			}
		}
		else
		{
			const InstanceId& expectedParentId = sourceToInstanceIds[
				sourcePrefab->m_gameObjects[sourceGameObject.m_parentIndex].m_instanceId];
			if (!liveGameObject->GetParent() ||
				liveGameObject->GetParent()->GetInstanceId() != expectedParentId)
			{
				outDiagnostic = "the live prefab hierarchy does not match its source hierarchy";
				return false;
			}
		}

		if (liveGameObject->GetComponents().Num() != sourceGameObject.m_components.Num())
		{
			outDiagnostic = "the live prefab components do not match the source prefab";
			return false;
		}

		for (const uint32_t componentIndex : sourceGameObject.m_components)
		{
			const ReflectedData& sourceReflection = sourcePrefab->m_components[componentIndex];
			InstanceId sourceComponentId;
			std::string conversionDiagnostic;
			if (!Utils::TryGetComponentInstanceId(
					sourceReflection,
					sourceComponentId,
					conversionDiagnostic))
			{
				outDiagnostic = "the source prefab contains an invalid component identity: " +
					conversionDiagnostic;
				return false;
			}

			const InstanceId expectedLiveComponentId(
				sourceComponentId.ComponentId(),
				liveGameObject->GetInstanceId());
			bool bFoundComponent = false;
			for (const auto& liveComponent : liveGameObject->GetComponents())
			{
				if (liveComponent->GetInstanceId() == expectedLiveComponentId &&
					liveComponent->GetTypeInfo() == sourceReflection.GetTypeInfo())
				{
					bFoundComponent = true;
					break;
				}
			}

			if (!bFoundComponent)
			{
				outDiagnostic = "the live prefab component identities or types do not match the source prefab";
				return false;
			}
		}
	}

	PrefabPtr expandedPrefab =
		PrefabPtr::Make(m_allocator, sourcePrefab->GetFileId());
	Prefab::SerializeGameObject(
		root,
		static_cast<uint32_t>(-1),
		expandedPrefab->m_components,
		expandedPrefab->m_gameObjects,
		nullptr);
	if (!expandedPrefab->ValidateForInstantiation(outDiagnostic))
	{
		outDiagnostic = "cannot capture the linked prefab baseline: " +
			outDiagnostic;
		return false;
	}

	TMap<InstanceId, YAML::Node> gameObjectOverrides;
	TMap<InstanceId, ReflectedData> componentOverrides;
	if (!WorldPrefab::BuildLinkedOverrides(
			expandedPrefab,
			sourcePrefab,
			sourceToInstanceIds,
			gameObjectOverrides,
			componentOverrides,
			outDiagnostic))
	{
		outDiagnostic = "cannot derive the linked prefab baseline overrides: " +
			outDiagnostic;
		return false;
	}

	PrefabPtr effectiveBaseline =
		PrefabPtr::Make(m_allocator, sourcePrefab->GetFileId());
	const InstanceId parentInstanceId = root->GetParent()
		? root->GetParent()->GetInstanceId()
		: InstanceId::Invalid;
	if (!effectiveBaseline->ConfigureLinkedInstance(
			sourcePrefab,
			sourceToInstanceIds,
			parentInstanceId,
			gameObjectOverrides,
			componentOverrides,
			outDiagnostic))
	{
		outDiagnostic = "cannot configure the linked prefab baseline: " +
			outDiagnostic;
		return false;
	}

	return RegisterPrefabInstance(
		root,
		sourcePrefab->GetFileId(),
		sourceToInstanceIds,
		effectiveBaseline,
		outDiagnostic);
}

bool World::BreakPrefabLink(
	const InstanceId& objectInstanceId,
	PrefabInstanceLink* outPreviousLink)
{
	if (outPreviousLink)
	{
		*outPreviousLink = {};
	}

	GameObjectPtr object = GetObjectByInstanceId(
		objectInstanceId).DynamicCast<GameObject>();
	if (!object)
	{
		return false;
	}

	InstanceId rootInstanceId;
	if (object->GetFileId())
	{
		rootInstanceId = objectInstanceId;
	}
	else
	{
		const PrefabInstanceLink* link = nullptr;
		if (!TryGetPrefabInstance(objectInstanceId, link) ||
			!link)
		{
			return false;
		}
		rootInstanceId = link->m_rootInstanceId;
	}

	GameObjectPtr root = GetObjectByInstanceId(
		rootInstanceId).DynamicCast<GameObject>();
	if (!root || !root->GetFileId())
	{
		return false;
	}

	// Clear the authoritative marker first. Any observation while the derived
	// caches are being purged sees an unlinked root.
	root->m_fileId = FileId::Invalid;

	auto purgeRootMembership = [this, &rootInstanceId]()
		{
			TVector<InstanceId> memberInstanceIds;
			for (const auto& membership :
				m_prefabLinks.m_rootsByObject)
			{
				if (*membership.m_second == rootInstanceId)
				{
					memberInstanceIds.Add(membership.m_first);
				}
			}

			for (const InstanceId& memberInstanceId :
				memberInstanceIds)
			{
				m_prefabLinks.m_rootsByObject.Remove(
					memberInstanceId);
			}
		};

	if (m_prefabLinks.m_instances.ContainsKey(rootInstanceId))
	{
		const PrefabInstanceLink previousLink =
			m_prefabLinks.m_instances[rootInstanceId];
		if (outPreviousLink)
		{
			*outPreviousLink = previousLink;
		}
	}

	purgeRootMembership();
	m_prefabLinks.m_instances.Remove(rootInstanceId);

	return true;
}

bool World::CanModifyPrefabStructure(
	const InstanceId& objectInstanceId,
	std::string* outDiagnostic) const
{
	const bool bCanModify = !IsPrefabLinked(objectInstanceId);
	if (!bCanModify && outDiagnostic)
	{
		*outDiagnostic = "structural changes are disabled for linked prefab instances; break the prefab link first";
	}
	return bCanModify;
}

bool World::CanReparentPrefabObject(
	const InstanceId& objectInstanceId,
	const InstanceId& parentInstanceId,
	std::string* outDiagnostic) const
{
	if (IsPrefabLinked(objectInstanceId) &&
		!IsPrefabInstanceRoot(objectInstanceId))
	{
		if (outDiagnostic)
		{
			*outDiagnostic = "internal linked prefab game objects cannot be reparented";
		}
		return false;
	}

	if (parentInstanceId && IsPrefabLinked(parentInstanceId))
	{
		if (outDiagnostic)
		{
			*outDiagnostic = "game objects cannot be parented inside a linked prefab instance";
		}
		return false;
	}

	return true;
}

void World::RemovePrefabLinksInHierarchy(GameObjectPtr root)
{
	if (!root)
	{
		return;
	}

	TVector<InstanceId> linkedRoots;
	TVector<GameObjectPtr> pendingObjects;
	pendingObjects.Add(root);
	while (!pendingObjects.IsEmpty())
	{
		GameObjectPtr current = pendingObjects[pendingObjects.Num() - 1];
		pendingObjects.RemoveLast();
		if (!current)
		{
			continue;
		}

		if (IsPrefabInstanceRoot(current->GetInstanceId()))
		{
			linkedRoots.Add(current->GetInstanceId());
		}
		pendingObjects.AddRange(current->GetChildren());
	}

	for (const InstanceId& rootInstanceId : linkedRoots)
	{
		BreakPrefabLink(rootInstanceId);
	}
}
