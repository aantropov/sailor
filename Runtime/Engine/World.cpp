#include "Engine/World.h"
#include "Engine/GameObject.h"

using namespace Sailor;

World::World(std::string name) : m_name(std::move(name))
{
	m_allocator = Memory::ObjectAllocatorPtr::Make();

	auto ecsArray = App::GetSubmodule<ECS::ECSFactory>()->CreateECS();

	for (auto& ecs : ecsArray)
	{
		m_ecs[ecs->GetComponentType()] = std::move(ecs);
	}
}

void World::Tick(float deltaTime)
{
	for (auto& el : m_objects)
	{
		if (!el->bBeginPlayCalled)
		{
			el->BeginPlay();
		}
		else
		{
			el->Tick(deltaTime);
		}
	}

	for (auto& ecs : m_ecs)
	{
		ecs.m_second->Tick(deltaTime);
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

	for (auto& el : m_ecs)
	{
		el.m_second->Tick(deltaTime);
	}
}

GameObjectPtr World::Instantiate(const std::string& name)
{
	auto newObject = GameObjectPtr::Make(m_allocator, this, name);
	assert(newObject);
	newObject->m_self = newObject;

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
