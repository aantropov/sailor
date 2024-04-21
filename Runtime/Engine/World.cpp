#include "Engine/World.h"
#include "Engine/GameObject.h"
#include "Engine/EngineLoop.h"
#include "AssetRegistry/Prefab/PrefabImporter.h"
#include <Components/TestComponent.h>
#include <ECS/TransformECS.h>

using namespace Sailor;

World::World(std::string name) : m_name(std::move(name)), m_bIsBeginPlayCalled(false), m_currentFrame(0)
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

	for (auto& el : m_objects)
	{
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
		check(el->m_bPendingDestroy);

		el->EndPlay();
		el->RemoveAllComponents();
		el->m_self = nullptr;
		el.DestroyObject(m_allocator);

		m_objects.RemoveFirst(el);
	}

	m_pendingDestroyObjects.Clear();

	GetDebugContext()->Tick(m_commandList, deltaTime);
	RHI::Renderer::GetDriverCommands()->EndCommandList(m_commandList);
}

GameObjectPtr World::Instantiate(PrefabPtr prefab, const glm::vec3& worldPosition, const std::string& name)
{
	check(prefab->m_gameObjects.Num() > 0);

	TVector<GameObjectPtr> gameObjects;
	TMap<InstanceId, ObjectPtr> resolveContext;

	for (uint32_t j = 0; j < prefab->m_gameObjects.Num(); j++)
	{
		GameObjectPtr gameObject = Instantiate(worldPosition, prefab->m_gameObjects[j].m_name);

		for (uint32_t i = 0; i < prefab->m_gameObjects[j].m_components.Num(); i++)
		{
			const ReflectionInfo& reflection = prefab->m_components[i];

			ComponentPtr newComponent = Reflection::CreateObject<Component>(reflection.GetTypeInfo(), GetAllocator());
			gameObject->AddComponentRaw(newComponent);
			newComponent->ApplyReflection(reflection);

			resolveContext[newComponent->GetInstanceId()] = newComponent;
		}

		resolveContext[gameObject->GetInstanceId()] = gameObject;
		gameObjects.Add(gameObject);
	}

	for (auto& go : gameObjects)
	{
		for (uint32_t i = 0; i < go->m_components.Num(); i++)
		{
			auto& newComp = go->m_components[i];
			const ReflectionInfo& reflection = prefab->m_components[i];

			newComp->ResolveRefs(reflection, resolveContext);
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

	return root;
}

GameObjectPtr World::Instantiate(const glm::vec3& worldPosition, const std::string& name)
{
	auto newObject = GameObjectPtr::Make(m_allocator, this, name);
	check(newObject);
	newObject->m_self = newObject;

	if (m_bIsBeginPlayCalled)
	{
		newObject->BeginPlay();
		newObject->m_bBeginPlayCalled = true;
	}

	newObject->GetTransformComponent().SetOwner(newObject);
	newObject->GetTransformComponent().SetPosition(worldPosition);
	newObject->m_instanceId = InstanceId::CreateNewInstanceId();

	m_objects.Add(newObject);

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
	for (auto& el : m_objects)
	{
		el->EndPlay();
		el->RemoveAllComponents();
		el->m_self = nullptr;
		el.DestroyObject(m_allocator);
	}

	m_objects.Clear();
	m_pendingDestroyObjects.Clear();
	m_pDebugContext.Clear();

	for (const auto& ecs : m_ecs)
	{
		(*ecs.m_second)->EndPlay();
	}
}