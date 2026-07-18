#include "Engine/World.h"
#include "Engine/GameObject.h"
#include "Engine/EngineLoop.h"
#include "AssetRegistry/Prefab/PrefabImporter.h"
#include "Containers/Set.h"
#include "Core/LogMacros.h"
#include <Components/TestComponent.h>
#include <ECS/TransformECS.h>

using namespace Sailor;

World::World(std::string name, EWorldBehaviourMask mask) : m_mask(mask), m_currentFrame(1), m_name(std::move(name)), m_frameInput(), m_bIsBeginPlayCalled(false)
{
	m_allocator = Memory::ObjectAllocatorPtr::Make(EAllocationPolicy::LocalMemory_SingleThread);

	auto ecsArray = App::GetSubmodule<ECS::ECSFactory>()->CreateECS();

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
	if (!prefab || prefab->m_gameObjects.IsEmpty())
	{
		SAILOR_LOG_ERROR("Cannot instantiate an invalid or empty prefab.");
		return {};
	}

	for (uint32_t gameObjectIndex = 0; gameObjectIndex < prefab->m_gameObjects.Num(); ++gameObjectIndex)
	{
		const auto& prefabGameObject = prefab->m_gameObjects[gameObjectIndex];
		if (prefabGameObject.m_parentIndex != static_cast<uint32_t>(-1) &&
			prefabGameObject.m_parentIndex >= prefab->m_gameObjects.Num())
		{
			SAILOR_LOG_ERROR(
				"Cannot instantiate prefab '%s': game object %u has invalid parent index %u.",
				prefab->GetFileId().ToString().c_str(),
				gameObjectIndex,
				prefabGameObject.m_parentIndex);
			return {};
		}

		for (const uint32_t componentIndex : prefabGameObject.m_components)
		{
			if (componentIndex >= prefab->m_components.Num())
			{
				SAILOR_LOG_ERROR(
					"Cannot instantiate prefab '%s': game object %u references invalid component index %u.",
					prefab->GetFileId().ToString().c_str(),
					gameObjectIndex,
					componentIndex);
				return {};
			}

			const ReflectedData& reflection = prefab->m_components[componentIndex];
			if (!reflection.IsValid())
			{
				SAILOR_LOG_ERROR(
					"Cannot instantiate prefab '%s': reflected component %u has an unknown type. "
					"Load or rebuild the workspace logic module.",
					prefab->GetFileId().ToString().c_str(),
					componentIndex);
				return {};
			}
		}
	}

	TVector<GameObjectPtr> gameObjects;
	TMap<InstanceId, ObjectPtr> internalDependencies;
	auto rollback = [&]()
	{
		for (auto& gameObject : gameObjects)
		{
			DestroyImmediate(gameObject);
		}
	};

	for (uint32_t j = 0; j < prefab->m_gameObjects.Num(); j++)
	{
		// We generate new instance id for game objects if the same is already in use
		const bool bShouldGenerateNewId = !prefab->m_gameObjects[j].m_instanceId || m_objectsMap.ContainsKey(prefab->m_gameObjects[j].m_instanceId);
		const InstanceId gameObjectId = bShouldGenerateNewId ? InstanceId::GenerateNewInstanceId() : prefab->m_gameObjects[j].m_instanceId;

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
			const InstanceId oldInstanceId = reflection.GetProperties()["instanceId"].as<InstanceId>();

			ComponentPtr newComponent = Reflection::CreateObject<Component>(reflection.GetTypeInfo(), GetAllocator());
			if (!newComponent)
			{
				SAILOR_LOG_ERROR(
					"Cannot instantiate reflected component type '%s' from prefab '%s'.",
					reflection.GetTypeInfo().Name().c_str(),
					prefab->GetFileId().ToString().c_str());
				rollback();
				return {};
			}

			gameObject->AddComponentRaw(newComponent);
			newComponent->ApplyReflection(reflection);
			newComponent->m_instanceId = InstanceId(oldInstanceId.ComponentId(), gameObject->GetInstanceId());

			// We store the old ids for internal dependencies during resolve
			internalDependencies[oldInstanceId] = newComponent;
		}

		// We store the old ids for internal dependencies during resolve
		internalDependencies[prefab->m_gameObjects[j].m_instanceId] = gameObject;
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
			bool bResolved = newComp->ResolveRefs(reflection, internalDependencies, false);

			// Resolve external dependencies
			if (!bResolved)
			{
				bResolved = newComp->ResolveRefs(reflection, m_objectsMap, false);
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
			root->m_fileId = prefab->GetFileId();
		}
	}

	if (!root)
	{
		SAILOR_LOG_ERROR("Cannot instantiate prefab '%s': no root game object was found.",
			prefab->GetFileId().ToString().c_str());
		rollback();
		return {};
	}

	// Should we try to resolve the previous open dependencies?
	// ResolveExternalDependencies();

	return root;
}

void World::ResolveExternalDependencies()
{
	for (uint32_t i = 0; i < ComponentsToResolveDependencies.Num(); i++)
	{
		auto& el = ComponentsToResolveDependencies[i];
		if (el.m_first->ResolveRefs(el.m_second, m_objectsMap, false))
		{
			ComponentsToResolveDependencies.RemoveAt(i);
			i--;
		}
	}
}

void World::SetEditorSelection(const TVector<InstanceId>& selection)
{
	m_editorSelection.Clear();

	for (const auto& instanceId : selection)
	{
		if (!instanceId)
		{
			continue;
		}

		const InstanceId gameObjectId = instanceId.GameObjectId();
		if (gameObjectId)
		{
			m_editorSelection.Insert(gameObjectId);
		}
	}
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
			if (el.m_first->m_instanceId.GameObjectId() == go->m_instanceId)
			{
				ComponentsToResolveDependencies.RemoveAt(idx);
				continue;
			}

			idx++;
		}

		destroyingObjects.AddRange(go->GetChildren());

		go->RemoveAllComponents();
		go->EndPlay();

		m_objectsMap.Remove(go->m_instanceId);
		m_objects.RemoveFirst(go);
		go.DestroyObject(m_allocator);
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

void World::Destroy(GameObjectPtr object)
{
	if (object && !object->m_bPendingDestroy)
	{
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
	m_editorSelection.Clear();
	m_pDebugContext.Clear();

	for (const auto& ecs : m_ecs)
	{
		(*ecs.m_second)->EndPlay();
	}

	check(m_objectsMap.Num() == 0);
}
