#include "Engine/World.h"
#include "Engine/GameObject.h"
#include "Engine/EngineLoop.h"
#include "AssetRegistry/Prefab/PrefabImporter.h"
#include <Components/TestComponent.h>
#include <ECS/TransformECS.h>

using namespace Sailor;

World::World(std::string name) : m_currentFrame(1), m_name(std::move(name)), m_frameInput(), m_bIsBeginPlayCalled(false)
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
		if (!el->m_bBeginPlayCalled)
		{
			el->m_bBeginPlayCalled = true;
			el->BeginPlay();
		}
		else
		{
			el->Tick(deltaTime);
		}
	}

	for (auto& ecs : m_sortedEcs)
	{
		m_ecs[ecs]->Tick(deltaTime);
	}

	for (auto& ecs : m_sortedEcs)
	{
		m_ecs[ecs]->PostTick();
	}

	for (auto& el : m_pendingDestroyObjects)
	{
		if (!el)
			continue;

		check(el->m_bPendingDestroy);

		TVector<GameObjectPtr> destroyingObjects;
		destroyingObjects.Reserve(el->GetChildren().Num() + 1);

		destroyingObjects.Add(el);
		while (destroyingObjects.Num() > 0)
		{
			auto go = destroyingObjects[0];
			destroyingObjects.RemoveAt(0);

			ComponentsToResolveDependencies.RemoveAll([&](const auto& el) { return el.m_first->m_instanceId.GameObjectId() == go->GetInstanceId(); });

			go->RemoveAllComponents();

			go->EndPlay();
			destroyingObjects.AddRange(go->GetChildren());

			m_objectsMap.Remove(go->m_instanceId);
			m_objects.RemoveFirst(go);

			go.DestroyObject(m_allocator);
		}
	}

	m_pendingDestroyObjects.Clear();

	GetDebugContext()->Tick(m_commandList, deltaTime);
	RHI::Renderer::GetDriverCommands()->EndCommandList(m_commandList);
}

GameObjectPtr World::Instantiate(PrefabPtr prefab)
{
	check(prefab->m_gameObjects.Num() > 0);

	TVector<GameObjectPtr> gameObjects;
	TMap<InstanceId, ObjectPtr> internalDependencies;

	for (uint32_t j = 0; j < prefab->m_gameObjects.Num(); j++)
	{
		// We generate new instance id for game objects if the same is already in use
		const bool bShouldGenerateNewId = !prefab->m_gameObjects[j].m_instanceId || m_objectsMap.ContainsKey(prefab->m_gameObjects[j].m_instanceId);
		const InstanceId gameObjectId = bShouldGenerateNewId ? InstanceId::GenerateNewInstanceId() : prefab->m_gameObjects[j].m_instanceId;

		GameObjectPtr gameObject = NewGameObject(prefab->m_gameObjects[j].m_name, gameObjectId);

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
			gameObject->AddComponentRaw(newComponent);
			newComponent->ApplyReflection(reflection);
			newComponent->m_instanceId = InstanceId(oldInstanceId.ComponentId(), gameObject->GetInstanceId());

			// We store the old ids for internal dependencies during resolve
			internalDependencies[oldInstanceId] = newComponent;
		}

		// We store the old ids for internal dependencies during resolve
		internalDependencies[prefab->m_gameObjects[j].m_instanceId] = gameObject;

		gameObjects.Add(gameObject);
	}

	for (auto& go : gameObjects)
	{
		for (uint32_t i = 0; i < go->m_components.Num(); i++)
		{
			auto& newComp = go->m_components[i];
			const ReflectedData& reflection = prefab->m_components[i];

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

	// Should we try to resolve the previous open dependencies?
	// ResolveExternalDependencies();

	return root;
}

void World::ResolveExternalDependencies()
{
	for (uint32_t i = 0; i < ComponentsToResolveDependencies.Num(); i++)
	{
		auto& el = ComponentsToResolveDependencies[i];
		if (const bool bResolved = el.m_first->ResolveRefs(el.m_second, m_objectsMap, false))
		{
			ComponentsToResolveDependencies.RemoveAt(i);
			i--;
		}
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

void World::Clear()
{
	ComponentsToResolveDependencies.Clear();

	for (auto& el : m_objects)
	{
		if (!el)
			continue;

		TVector<GameObjectPtr> destroyingObjects;
		destroyingObjects.Reserve(el->GetChildren().Num() + 1);

		destroyingObjects.Add(el);
		while (destroyingObjects.Num() > 0)
		{
			auto go = destroyingObjects[0];
			destroyingObjects.RemoveAtSwap(0);

			go->RemoveAllComponents();

			go->EndPlay();
			destroyingObjects.AddRange(go->GetChildren());

			m_objectsMap.Remove(go->m_instanceId);
			go.DestroyObject(m_allocator);
		}
	}

	m_objects.Clear();
	m_pendingDestroyObjects.Clear();
	m_pDebugContext.Clear();

	for (const auto& ecs : m_ecs)
	{
		(*ecs.m_second)->EndPlay();
	}

	check(m_objectsMap.Num() == 0);
}