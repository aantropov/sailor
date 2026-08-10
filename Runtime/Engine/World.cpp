#include "Engine/World.h"
#include "Engine/GameObject.h"
#include "Engine/EngineLoop.h"
#include "AssetRegistry/Prefab/PrefabImporter.h"
#include "AssetRegistry/World/WorldPrefabImporter.h"
#include "Containers/Set.h"
#include "Core/LogMacros.h"
#include "Core/Utils.h"
#include "YamlExceptionBoundary.h"
#include <Components/TestComponent.h>
#include <ECS/TransformECS.h>
#include <Submodules/Editor.h>

using namespace Sailor;

namespace
{
	TVector<ECS::TBaseSystemPtr> CreateRegisteredEcs()
	{
		auto* ecsFactory = App::GetSubmodule<ECS::ECSFactory>();
		check(ecsFactory);
		return ecsFactory->CreateECS();
	}

	class PrefabInstantiationTransaction final
	{
	public:

		PrefabInstantiationTransaction(
			World& world,
			TVector<GameObjectPtr>& gameObjects,
			TVector<TPair<ComponentPtr, ReflectedData>>& pendingDependencies) :
			m_world(world),
			m_gameObjects(gameObjects),
			m_pendingDependencies(pendingDependencies),
			m_initialPendingDependencies(pendingDependencies.Num())
		{}

		~PrefabInstantiationTransaction() noexcept
		{
			if (m_bCommitted)
			{
				return;
			}

			while (m_pendingDependencies.Num() > m_initialPendingDependencies)
			{
				m_pendingDependencies.RemoveAt(m_pendingDependencies.Num() - 1);
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
		TVector<TPair<ComponentPtr, ReflectedData>>& m_pendingDependencies;
		size_t m_initialPendingDependencies;
		bool m_bCommitted = false;
	};
}

World::World(std::string name, EWorldBehaviourMask mask) :
	World(std::move(name), mask, CreateRegisteredEcs())
{}

World::World(
	std::string name,
	EWorldBehaviourMask mask,
	TVector<ECS::TBaseSystemPtr>&& ecsArray) :
	m_mask(mask),
	m_currentFrame(1),
	m_name(std::move(name)),
	m_frameInput(),
	m_bIsBeginPlayCalled(false),
	m_bPhysicsSimulationEnabled(
		(mask & (uint8_t)EWorldBehaviourBit::Tickable) != 0)
{
	m_allocator = Memory::ObjectAllocatorPtr::Make(EAllocationPolicy::LocalMemory_SingleThread);

	for (auto& ecs : ecsArray)
	{
		ecs->Initialize(this);
		m_ecs[ecs->GetComponentType()] = std::move(ecs);
	}

	m_sortedEcs.Reserve(ecsArray.Num());
	for (const auto& ecs : m_ecs)
	{
		auto it = upper_bound(m_sortedEcs.begin(), m_sortedEcs.end(), ecs.m_first,
			[&](auto& lhs, auto& rhs) { return m_ecs[lhs]->GetOrder() < m_ecs[rhs]->GetOrder(); });

		m_sortedEcs.Insert(ecs.m_first, it - m_sortedEcs.begin());
	}

	m_pDebugContext = TUniquePtr<RHI::DebugContext>::Make();
}

ObjectPtr World::GetObjectByInstanceId(const InstanceId& instanceId) const
{
	if (m_objectsMap.ContainsKey(instanceId))
	{
		return m_objectsMap[instanceId];
	}

	return ObjectPtr();
}

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
	else if (m_prefabInstanceRootsByObject.ContainsKey(
			objectInstanceId))
	{
		rootInstanceId =
			m_prefabInstanceRootsByObject[objectInstanceId];
	}
	else
	{
		return false;
	}

	if (!m_prefabInstances.ContainsKey(rootInstanceId))
	{
		return false;
	}

	GameObjectPtr root = GetObjectByInstanceId(
		rootInstanceId).DynamicCast<GameObject>();
	const PrefabInstanceLink& link =
		m_prefabInstances[rootInstanceId];
	if (!root ||
		!root->GetFileId() ||
		link.m_rootInstanceId != rootInstanceId ||
		!link.m_effectiveBaseline ||
		link.m_effectiveBaseline->GetFileId() !=
			root->GetFileId() ||
		!m_prefabInstanceRootsByObject.ContainsKey(
			objectInstanceId) ||
		m_prefabInstanceRootsByObject[objectInstanceId] !=
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
		m_prefabInstances.ContainsKey(root->GetInstanceId()) ||
		m_prefabInstanceRootsByObject.ContainsKey(
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

		if (m_prefabInstanceRootsByObject.ContainsKey(liveInstanceId))
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
	link.m_effectiveBaseline->m_bLinkedInstanceRecord =
		effectiveBaseline->m_bLinkedInstanceRecord;
	link.m_effectiveBaseline->m_bExpandedLinkedInstanceRecord =
		effectiveBaseline->m_bExpandedLinkedInstanceRecord;
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
			baselineGameObject.m_position =
				liveGameObject->GetTransformComponent().GetPosition();
			baselineGameObject.m_rotation =
				liveGameObject->GetTransformComponent().GetRotation();
			baselineGameObject.m_scale =
				liveGameObject->GetTransformComponent().GetScale();
			break;
		}
	}

	m_prefabInstances[link.m_rootInstanceId] = link;
	for (const auto& mapping : sourceToInstanceIds)
	{
		m_prefabInstanceRootsByObject[*mapping.m_second] = link.m_rootInstanceId;
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
				m_prefabInstanceRootsByObject)
			{
				if (*membership.m_second == rootInstanceId)
				{
					memberInstanceIds.Add(membership.m_first);
				}
			}

			for (const InstanceId& memberInstanceId :
				memberInstanceIds)
			{
				m_prefabInstanceRootsByObject.Remove(
					memberInstanceId);
			}
		};

	if (m_prefabInstances.ContainsKey(rootInstanceId))
	{
		const PrefabInstanceLink previousLink =
			m_prefabInstances[rootInstanceId];
		if (outPreviousLink)
		{
			*outPreviousLink = previousLink;
		}
	}

	purgeRootMembership();
	m_prefabInstances.Remove(rootInstanceId);

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

void World::Tick(FrameState& frameState)
{
	const bool bShouldCallBeginPlay = (m_mask & (uint8_t)EWorldBehaviourBit::CallBeginPlay) != 0;
	const bool bShouldTick = (m_mask & (uint8_t)EWorldBehaviourBit::Tickable) != 0;
	const bool bShouldEcsTick = (m_mask & (uint8_t)EWorldBehaviourBit::EcsTickable) != 0;
	const bool bShouldEditorTick = (m_mask & (uint8_t)EWorldBehaviourBit::EditorTick) != 0;

	m_currentFrame++;

	if (!m_bIsBeginPlayCalled)
	{
		for (auto& ecs : m_sortedEcs)
		{
			m_ecs[ecs]->BeginPlay();
		}

		m_bIsBeginPlayCalled = true;
	}

	m_frameInput = frameState.GetInputState();
	m_commandList = frameState.CreateCommandBuffer(0);

	const float c_smoothFactor = 0.1f;
	const float deltaTime = frameState.GetDeltaTime();
	m_smoothDeltaTime += (deltaTime - m_smoothDeltaTime) * c_smoothFactor;

	m_time += deltaTime;

	RHI::Renderer::GetDriverCommands()->BeginCommandList(m_commandList, true);

	for (uint32_t i = 0; i < m_objects.Num(); i++)
	{
		auto& el = m_objects[i];
		if (!el->m_bBeginPlayCalled && bShouldCallBeginPlay)
		{
			el->m_bBeginPlayCalled = true;
			el->BeginPlay();
		}
		else if (bShouldTick)
		{
			el->Tick(deltaTime);
		}
	}

	if (bShouldEditorTick)
	{
		for (auto& el : m_objects)
		{
			el->EditorTick(deltaTime);
		}

		if (auto editor = App::GetSubmodule<Editor>())
		{
			editor->TickViewportTools();
		}

		for (auto& el : m_objects)
		{
			if (el && IsEditorSelected(el->GetInstanceId().GameObjectId()))
			{
				el->DrawEditorSelectedGizmo();
			}
		}
	}

	if (bShouldEcsTick)
	{
		for (auto& ecs : m_sortedEcs)
		{
			m_ecs[ecs]->Tick(deltaTime);
		}

		for (auto& ecs : m_sortedEcs)
		{
			m_ecs[ecs]->PostTick();
		}
	}

	for (auto& el : m_pendingDestroyObjects)
	{
		if (!el)
		{
			continue;
		}

		check(el->m_bPendingDestroy);

		if (!m_objectsMap.ContainsKey(el->m_instanceId))
		{
			continue;
		}

		DestroyGameObjectHierarchy(el);
	}

	m_pendingDestroyObjects.Clear();

	GetDebugContext()->Tick(m_commandList, deltaTime);
	RHI::Renderer::GetDriverCommands()->EndCommandList(m_commandList);
}

GameObjectPtr World::Instantiate(PrefabPtr prefab)
{
	return Instantiate(prefab, false);
}

GameObjectPtr World::Instantiate(PrefabPtr prefab, bool bStrictInstanceIds)
{
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

	if (prefab->m_bLinkedPrefabSnapshotRecord)
	{
		SAILOR_LOG_ERROR(
			"Cannot instantiate linked prefab snapshot directly; it must be resolved against its current source first.");
		return {};
	}

	if (prefab->m_bExpandedLinkedInstanceRecord)
	{
		SAILOR_LOG_ERROR(
			"Cannot instantiate an expanded linked serialization record directly.");
		return {};
	}

	GameObjectPtr detachedParent;
	if (prefab->m_bDetachedFromPrefabRecord)
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

	if (prefab->m_bLinkedInstanceRecord)
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
				return {};
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
					return {};
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
					return {};
				}
			}
		}
	}

	TMap<InstanceId, InstanceId> strictSourceToInstanceIds;
	if (bStrictInstanceIds)
	{
		TSet<InstanceId> desiredGameObjectIds;
		TSet<InstanceId> desiredComponentIds;
		TSet<InstanceId> existingComponentIds;
		for (const auto& existingGameObject : m_objects)
		{
			if (!existingGameObject)
			{
				continue;
			}

			for (const auto& existingComponent :
				existingGameObject->GetComponents())
			{
				if (existingComponent)
				{
					existingComponentIds.Insert(
						existingComponent->GetInstanceId());
				}
			}
		}

		for (const auto& sourceGameObject : prefab->m_gameObjects)
		{
			const InstanceId& sourceInstanceId =
				sourceGameObject.m_instanceId;
			InstanceId desiredGameObjectId = sourceInstanceId;
			if (prefab->m_bLinkedInstanceRecord)
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
					return {};
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
				return {};
			}

			strictSourceToInstanceIds[sourceInstanceId] =
				desiredGameObjectId;
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
					return {};
				}

				const InstanceId desiredComponentId(
					sourceComponentId.ComponentId(),
					desiredGameObjectId);
				if (!desiredComponentId ||
					existingComponentIds.Contains(desiredComponentId) ||
					!desiredComponentIds.Insert(desiredComponentId))
				{
					SAILOR_LOG_ERROR(
						"Cannot strictly instantiate prefab '%s': component id '%s' is invalid, duplicated, or already in use.",
						prefab->GetFileId().ToString().c_str(),
						desiredComponentId.ToString().c_str());
					return {};
				}
			}
		}
	}

	TVector<GameObjectPtr> gameObjects;
	gameObjects.Reserve(prefab->m_gameObjects.Num());
	TMap<InstanceId, ObjectPtr> internalDependencies;
	TMap<InstanceId, InstanceId> sourceToInstanceIds;
	TSet<InstanceId> reservedInstanceIds;
	PrefabInstantiationTransaction transaction(*this, gameObjects, ComponentsToResolveDependencies);

	for (uint32_t j = 0; j < prefab->m_gameObjects.Num(); j++)
	{
		const InstanceId& sourceInstanceId = prefab->m_gameObjects[j].m_instanceId;
		InstanceId gameObjectId;
		if (prefab->m_bLinkedInstanceRecord)
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
			gameObjectId = bStrictInstanceIds
				? strictSourceToInstanceIds[sourceInstanceId]
				: sourceInstanceId;
			if (!bStrictInstanceIds &&
				(!gameObjectId ||
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

			// Resolve internal dependencies first
			bool bResolved = false;
			std::string resolveDiagnostic;
			if (!External::TryInvokeYaml(
					[newComp, &reflection, &internalDependencies]() mutable
					{
						return newComp->ResolveRefs(
							reflection,
							internalDependencies,
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

			// Resolve external dependencies
			if (!bResolved)
			{
				if (!External::TryInvokeYaml(
						[newComp, &reflection, this]() mutable
						{
							return newComp->ResolveRefs(reflection, m_objectsMap, true);
						},
						bResolved,
						resolveDiagnostic))
				{
					SAILOR_LOG_ERROR(
						"Cannot resolve external references for component %u from prefab '%s': %s.",
						componentIndex,
						prefab->GetFileId().ToString().c_str(),
						resolveDiagnostic.c_str());
					return {};
				}
			}

			if (!bResolved)
			{
				ComponentsToResolveDependencies.Add(TPair(newComp, reflection));
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

	if (prefab->m_bLinkedInstanceRecord && prefab->m_linkedParentInstanceId)
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

	if (prefab->m_bDetachedFromPrefabRecord)
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

	if (prefab->GetFileId())
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

	// Should we try to resolve the previous open dependencies?
	// ResolveExternalDependencies();

	transaction.Commit();
	return root;
}

void World::ResolveExternalDependencies()
{
	for (size_t i = 0; i < ComponentsToResolveDependencies.Num();)
	{
		auto& el = ComponentsToResolveDependencies[i];
		if (!el.m_first || el.m_first->ResolveRefs(el.m_second, m_objectsMap, false))
		{
			ComponentsToResolveDependencies.RemoveAt(i);
			continue;
		}

		i++;
	}
}

void World::RemovePendingDependencyResolutions(const ComponentPtr& component)
{
	for (size_t i = 0; i < ComponentsToResolveDependencies.Num();)
	{
		const auto& pendingComponent = ComponentsToResolveDependencies[i].m_first;
		if (!pendingComponent || pendingComponent == component)
		{
			ComponentsToResolveDependencies.RemoveAt(i);
			continue;
		}

		i++;
	}
}

void World::ApplyComponentReflection(ComponentPtr component, const ReflectedData& reflection, bool bImmediate)
{
	if (!component)
	{
		return;
	}

	component->ApplyReflection(reflection);
	RemovePendingDependencyResolutions(component);
	if (!component->ResolveRefs(reflection, m_objectsMap, bImmediate))
	{
		ComponentsToResolveDependencies.Add(TPair(component, reflection));
	}
}

void World::SetEditorSelection(const TVector<InstanceId>& selection)
{
	TSet<InstanceId> nextSelection;

	for (const auto& instanceId : selection)
	{
		if (!instanceId)
		{
			continue;
		}

		const InstanceId gameObjectId = instanceId.GameObjectId();
		if (gameObjectId)
		{
			nextSelection.Insert(gameObjectId);
		}
	}

	m_editorSelection = std::move(nextSelection);
}

bool World::IsEditorSelected(const InstanceId& instanceId) const
{
	return instanceId && m_editorSelection.Contains(instanceId.GameObjectId());
}

void World::DestroyGameObjectHierarchy(GameObjectPtr root)
{
	if (!root)
	{
		return;
	}

	RemovePrefabLinksInHierarchy(root);

	TVector<GameObjectPtr> destroyingObjects;
	destroyingObjects.Reserve(root->GetChildren().Num() + 1);
	destroyingObjects.Add(root);

	while (!destroyingObjects.IsEmpty())
	{
		auto go = destroyingObjects[destroyingObjects.Num() - 1];
		destroyingObjects.RemoveLast();

		if (!go || !m_objectsMap.ContainsKey(go->m_instanceId))
		{
			continue;
		}

		for (size_t idx = 0; idx < ComponentsToResolveDependencies.Num();)
		{
			const auto& el = ComponentsToResolveDependencies[idx];
			if (!el.m_first || el.m_first->m_instanceId.GameObjectId() == go->m_instanceId)
			{
				ComponentsToResolveDependencies.RemoveAt(idx);
				continue;
			}

			idx++;
		}

		destroyingObjects.AddRange(go->GetChildren());
		m_editorSelection.Remove(go->m_instanceId);

		go->RemoveAllComponents();
		go->EndPlay();

		m_objectsMap.Remove(go->m_instanceId);
		m_objects.RemoveFirst(go);
		go.DestroyObject(m_allocator);
	}
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

GameObjectPtr World::NewGameObject(const std::string& name, const InstanceId& instanceId)
{
	auto newObject = GameObjectPtr::Make(m_allocator, this, name);

	check(newObject);
	check(instanceId);

	newObject->m_self = newObject;
	newObject->m_instanceId = instanceId;

	newObject->Initialize();

	if (m_bIsBeginPlayCalled)
	{
		newObject->BeginPlay();
		newObject->m_bBeginPlayCalled = true;
	}

	newObject->GetTransformComponent().SetOwner(newObject);

	m_objects.Add(newObject);
	m_objectsMap[newObject->m_instanceId] = newObject;

	return newObject;
}

GameObjectPtr World::Instantiate(const std::string& name)
{
	auto newObject = NewGameObject(name, InstanceId::GenerateNewInstanceId());

	return newObject;
}

GameObjectPtr World::Instantiate(const std::string& name, const InstanceId& preferredInstanceId)
{
	if (!preferredInstanceId.IsGameObjectId() || m_objectsMap.ContainsKey(preferredInstanceId))
	{
		return {};
	}

	return NewGameObject(name, preferredInstanceId);
}

void World::Destroy(GameObjectPtr object)
{
	if (object && !object->m_bPendingDestroy)
	{
		if (IsPrefabLinked(object->GetInstanceId()) &&
			!IsPrefabInstanceRoot(object->GetInstanceId()))
		{
			SAILOR_LOG_ERROR(
				"Cannot destroy internal linked prefab game object '%s'; break the prefab link first.",
				object->GetInstanceId().ToString().c_str());
			return;
		}

		object->m_bPendingDestroy = true;
		m_pendingDestroyObjects.PushBack(std::move(object));
	}
}

void World::DestroyImmediate(GameObjectPtr object)
{
	if (!object || !m_objectsMap.ContainsKey(object->m_instanceId))
	{
		return;
	}

	if (IsPrefabLinked(object->GetInstanceId()) &&
		!IsPrefabInstanceRoot(object->GetInstanceId()))
	{
		SAILOR_LOG_ERROR(
			"Cannot destroy internal linked prefab game object '%s'; break the prefab link first.",
			object->GetInstanceId().ToString().c_str());
		return;
	}

	object->SetParent(GameObjectPtr());
	DestroyGameObjectHierarchy(object);
}

void World::Clear()
{
	ComponentsToResolveDependencies.Clear();

	TVector<GameObjectPtr> objectsToDestroy = m_objects;
	for (auto& go : objectsToDestroy)
	{
		if (!go || !m_objectsMap.ContainsKey(go->m_instanceId))
		{
			continue;
		}

		DestroyGameObjectHierarchy(go);
	}

	m_objects.Clear();
	m_pendingDestroyObjects.Clear();
	m_prefabInstances.Clear();
	m_prefabInstanceRootsByObject.Clear();
	m_editorSelection.Clear();
	m_pDebugContext.Clear();

	for (const auto& ecs : m_ecs)
	{
		(*ecs.m_second)->EndPlay();
	}

	check(m_objectsMap.Num() == 0);
}
