#include "Engine/World.h"
#include "Engine/GameObject.h"
#include "Engine/EngineLoop.h"
#include <Components/TestComponent.h>

using namespace Sailor;

World::World(std::string name) : m_name(std::move(name)), m_bIsBeginPlayCalled(false), m_currentFrame(0)
{
	m_allocator = Memory::ObjectAllocatorPtr::Make();

	auto ecsArray = App::GetSubmodule<ECS::ECSFactory>()->CreateECS();

	for (auto& ecs : ecsArray)
	{
		m_ecs[ecs->GetComponentType()] = std::move(ecs);
	}

	m_sortedEcs.Reserve(ecsArray.Num());
	for (auto& ecs : m_ecs)
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
	m_time = frameState.GetTime();
	m_commandList = frameState.CreateCommandBuffer(0);

	RHI::Renderer::GetDriverCommands()->BeginCommandList(m_commandList, true);

	for (auto& el : m_objects)
	{
		if (!el->bBeginPlayCalled)
		{
			el->BeginPlay();
			el->bBeginPlayCalled = true;
		}
		else
		{
			el->Tick(frameState.GetDeltaTime());
		}
	}

	for (auto& ecs : m_sortedEcs)
	{
		m_ecs[ecs]->Tick(frameState.GetDeltaTime());
	}

	for (auto& el : m_pendingDestroyObjects)
	{
		assert(el->bPendingDestroy);

		el->EndPlay();
		el->RemoveAllComponents();
		el->m_self = nullptr;
		el.DestroyObject(m_allocator);

		m_objects.RemoveFirst(el);
	}

	m_pendingDestroyObjects.Clear();

	GetDebugContext()->Tick(m_commandList, frameState.GetDeltaTime());
	RHI::Renderer::GetDriverCommands()->EndCommandList(m_commandList);
}

GameObjectPtr World::Instantiate(const std::string& name)
{
	auto newObject = GameObjectPtr::Make(m_allocator, this, name);
	assert(newObject);
	newObject->m_self = newObject;

	newObject->BeginPlay();
	newObject->bBeginPlayCalled = true;

	m_objects.Add(newObject);

	return newObject;
}

void World::Destroy(GameObjectPtr object)
{
	if (object && !object->bPendingDestroy)
	{
		object->bPendingDestroy = true;
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

	for (auto& ecs : m_ecs)
	{
		ecs.m_second->EndPlay();
	}
}