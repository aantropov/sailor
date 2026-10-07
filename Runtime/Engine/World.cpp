#include "Engine/World.h"
#include "Engine/GameObject.h"
#include "Components/EditorComponent.h"
#include "Engine/EngineLoop.h"
#include "Containers/Set.h"
#include "Core/LogMacros.h"
#include "Core/Reflection.h"
#include <ECS/TransformECS.h>
#include <ECS/PhysicsECS.h>
#include <ECS/GlobalIlluminationECS.h>
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
	m_bEcsBeginPlayCalled(false),
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

const GISettings&
World::GetGISettings() const
{
	static const GISettings emptySettings{};
	const ECS::TBaseSystemPtr* system = nullptr;
	if (!m_ecs.Find(GlobalIlluminationECS::GetComponentStaticType(), system) ||
		!system || !system->GetRawPtr())
	{
		return emptySettings;
	}
	return static_cast<const GlobalIlluminationECS*>(
		system->GetRawPtr())->GetWorldSettings();
}

bool World::SetGISettings(
	GISettings settings,
	std::string& outDiagnostic)
{
	if (!settings.Validate(outDiagnostic))
	{
		return false;
	}
	ECS::TBaseSystemPtr* system = nullptr;
	if (!m_ecs.Find(GlobalIlluminationECS::GetComponentStaticType(), system) ||
		!system || !system->GetRawPtr())
	{
		outDiagnostic = "Global Illumination ECS is unavailable";
		return false;
	}
	auto* globalIllumination = static_cast<GlobalIlluminationECS*>(
		system->GetRawPtr());
	if (!globalIllumination->ApplyWorldSettings(settings, outDiagnostic))
	{
		return false;
	}
	outDiagnostic = "updated world Global Illumination ECS settings";
	return true;
}

ObjectPtr World::GetObjectByInstanceId(const InstanceId& instanceId) const
{
	if (m_objectsMap.ContainsKey(instanceId))
	{
		return m_objectsMap[instanceId];
	}

	return ObjectPtr();
}

void World::BeginPlayEcs()
{
	if (!m_bEcsBeginPlayCalled)
	{
		m_bEcsBeginPlayCalled = true;
		for (auto& ecs : m_sortedEcs)
		{
			m_ecs[ecs]->BeginPlay();
		}
	}
}

void World::TickGameObjects(float deltaTime)
{
	const bool bShouldCallBeginPlay = (m_mask & (uint8_t)EWorldBehaviourBit::CallBeginPlay) != 0;
	const bool bShouldTick = (m_mask & (uint8_t)EWorldBehaviourBit::Tickable) != 0;
	const bool bShouldEditorTick = (m_mask & (uint8_t)EWorldBehaviourBit::EditorTick) != 0;
	auto objects = GetGameObjects();
	for (auto& object : objects)
	{
		if (!object || object->m_bPendingDestroy)
		{
			continue;
		}
		if (!object->m_bBeginPlayCalled && bShouldCallBeginPlay)
		{
			object->m_bBeginPlayCalled = true;
			object->BeginPlay();
		}
		if (object && !object->m_bPendingDestroy && (bShouldCallBeginPlay || bShouldTick))
		{
			object->Tick(deltaTime);
		}
	}

	if (bShouldEditorTick)
	{
		for (auto& object : objects)
		{
			if (object && !object->m_bPendingDestroy)
			{
				object->EditorTick(deltaTime);
			}
		}
	}
}

void World::TickEcs(float deltaTime)
{
	auto* transforms = GetECS<TransformECS>();
	auto* physics = GetECS<PhysicsECS>();

	// Authored transforms are the input to physics; simulated poses are its output.
	if (transforms) transforms->Tick(deltaTime);
	if (physics) physics->Tick(deltaTime);
	if (transforms) transforms->Tick(0.0f);

	// Publish only after the final world matrices are available to every consumer.
	for (auto type : m_sortedEcs)
	{
		auto* system = m_ecs[type].GetRawPtr();
		if (system != transforms && system != physics) system->Tick(deltaTime);
	}
	for (auto type : m_sortedEcs) m_ecs[type]->PostTick();
}

void World::Tick(FrameState& frameState)
{
	SAILOR_PROFILE_FUNCTION();
	const bool bShouldEcsTick = (m_mask & (uint8_t)EWorldBehaviourBit::EcsTickable) != 0;
	const bool bShouldEditorTick = (m_mask & (uint8_t)EWorldBehaviourBit::EditorTick) != 0;

	m_currentFrame++;
	BeginPlayEcs();

	m_frameInput = frameState.GetInputState();
	m_commandList = frameState.CreateCommandBuffer(0);

	const float c_smoothFactor = 0.1f;
	const float deltaTime = frameState.GetDeltaTime();
	m_smoothDeltaTime += (deltaTime - m_smoothDeltaTime) * c_smoothFactor;

	m_time += deltaTime;

	RHI::Renderer::GetDriverCommands()->BeginCommandList(m_commandList, true);
	TickGameObjects(deltaTime);

	if (bShouldEditorTick)
	{
		if (auto editor = App::GetSubmodule<Editor>())
		{
			editor->TickViewportTools();
		}

		for (auto& el : *m_objects)
		{
			if (el && IsEditorSelected(el->GetInstanceId().GameObjectId()))
			{
				el->DrawEditorSelectedGizmo();
			}
		}
	}

	if (bShouldEcsTick)
	{
		TickEcs(deltaTime);
	}

	DestroyPendingGameObjects();

	GetDebugContext()->Tick(m_commandList, deltaTime);
	RHI::Renderer::GetDriverCommands()->EndCommandList(m_commandList);
}

void World::ResolveExternalDependencies()
{
	for (auto it = m_pendingDependencies.begin(); it != m_pendingDependencies.end();)
	{
		auto component = it->m_first;
		if (!component)
		{
			it = m_pendingDependencies.Erase(it);
			continue;
		}
		component->m_bDependenciesResolved = component->ResolveRefs(it->m_second, m_objectsMap, false);
		if (component->m_bDependenciesResolved)
		{
			component->m_pendingDependency = {};
			it = m_pendingDependencies.Erase(it);
			continue;
		}

		++it;
	}
}

void World::QueuePendingDependencyResolution(ComponentPtr component, const ReflectedData& reflection)
{
	m_pendingDependencies.EmplaceBack(component, reflection);
	component->m_pendingDependency = m_pendingDependencies.Last();
}

void World::RemovePendingDependencyResolutions(ComponentPtr component)
{
	if (component->m_pendingDependency != m_pendingDependencies.end())
	{
#if defined(SAILOR_ECS_TEST_HOOKS)
		++m_numRemovalVisits;
#endif
		m_pendingDependencies.Erase(component->m_pendingDependency);
		component->m_pendingDependency = {};
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
	component->m_bDependenciesResolved = component->ResolveRefs(reflection, m_objectsMap, bImmediate);
	if (!component->m_bDependenciesResolved)
	{
		QueuePendingDependencyResolution(component, reflection);
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

GameObjectPtr World::GetPrimaryEditorSelection() const
{
	GameObjectPtr primary;
	for (const auto& id : m_editorSelection)
	{
		auto object = GetObjectByInstanceId(id).DynamicCast<GameObject>();
		if (object && !object->GetComponent<EditorComponent>() &&
			(!primary || object->m_worldOrder < primary->m_worldOrder))
		{
			primary = std::move(object);
		}
	}
	return primary;
}

void World::DestroyPendingGameObjects()
{
	for (auto& object : m_pendingDestroyObjects)
	{
		if (!object)
		{
			continue;
		}

		check(object->m_bPendingDestroy);

		if (!m_objectsMap.ContainsKey(object->m_instanceId))
		{
			continue;
		}

		DestroyGameObjectHierarchy(object);
	}

	m_pendingDestroyObjects.Clear();
}

void World::DestroyGameObjectHierarchy(GameObjectPtr root)
{
	if (!root)
	{
		return;
	}

	root->SetParentInternal({}, true);
	if (!m_bIsClearing)
	{
		RemovePrefabLinksInHierarchy(root);
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

		// Cancel the owner's pending references before any EndPlay callback runs.
		for (const auto& component : go->m_components)
		{
			RemovePendingDependencyResolutions(component);
		}

		destroyingObjects.AddRange(go->GetChildren());
		m_editorSelection.Remove(go->m_instanceId);

		go->RemoveAllComponents();
		go->EndPlay();

		m_objectsMap.Remove(go->m_instanceId);
		if (!m_bIsClearing)
		{
#if defined(SAILOR_ECS_TEST_HOOKS)
			++m_numRemovalVisits;
#endif
			m_objects->Erase(go->m_worldIterator);
		}
		go.DestroyObject(m_allocator);
	}
}

GameObjectPtr World::NewGameObject(std::string_view name, const InstanceId& instanceId)
{
	auto newObject = GameObjectPtr::Make(m_allocator, this, name);

	check(newObject);
	check(instanceId);

	newObject->m_self = newObject;
	newObject->m_instanceId = instanceId;

	newObject->Initialize();

	// Selection keeps world insertion order without scanning or copying every object.
	newObject->m_worldOrder = m_nextObjectOrder++;
	m_objects->EmplaceBack(newObject);
	newObject->m_worldIterator = m_objects->Last();
	m_objectsMap[newObject->m_instanceId] = newObject;

	return newObject;
}

GameObjectPtr World::Instantiate(std::string_view name)
{
	auto newObject = NewGameObject(name, InstanceId::GenerateNewInstanceId());

	return newObject;
}

GameObjectPtr World::Instantiate(std::string_view name, const InstanceId& preferredInstanceId)
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

	DestroyGameObjectHierarchy(object);
}

TVector<GameObjectPtr> World::GetGameObjects()
{
	TVector<GameObjectPtr> objects;
	objects.Reserve(m_objects->Num());
	for (const auto& object : *m_objects) objects.Add(object);
	return objects;
}

void World::Clear()
{
	if (m_bIsClearing)
	{
		return;
	}
	m_bIsClearing = true;
	struct ClearScope
	{
		bool& m_flag;
		~ClearScope() { m_flag = false; }
	} clearScope{ m_bIsClearing };

	for (auto& pending : m_pendingDependencies)
	{
		if (pending.m_first) pending.m_first->m_pendingDependency = {};
	}
	m_pendingDependencies.Clear();

	auto objectsToDestroy = GetGameObjects();
	TVector<GameObjectPtr> roots;
	roots.Reserve(objectsToDestroy.Num());
	for (auto& go : objectsToDestroy)
	{
		if (!go)
		{
			continue;
		}

		go->m_fileId = FileId::Invalid;
		if (!go->GetParent())
		{
			roots.Add(go);
		}
	}
	m_prefabLinks.m_instances.Clear();
	m_prefabLinks.m_rootsByObject.Clear();

	for (const auto& root : roots)
	{
		DestroyGameObjectHierarchy(root);
	}

	// EndPlay can reparent descendants after their old hierarchy was queued.
	for (const auto& go : objectsToDestroy)
	{
		DestroyGameObjectHierarchy(go);
	}

	m_objects->Clear();
	m_pendingDestroyObjects.Clear();
	m_editorSelection.Clear();
	m_pDebugContext.Clear();

	for (const auto& ecs : m_ecs)
	{
		(*ecs.m_second)->EndPlay();
	}

	check(m_objectsMap.Num() == 0);
}
