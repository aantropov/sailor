#include "Engine/World.h"

using namespace Sailor;

World::World(std::string name) : m_name(std::move(name))
{
	m_allocator = Memory::ObjectAllocatorPtr::Make();
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
			el->Update(deltaTime);
		}
	}

	for (auto& el : m_pendingDestroyObjects)
	{
		assert(el->bPendingDestroy);

		el->EndPlay();
		el->RemoveAllComponents();
		el.DestroyObject(m_allocator);

		m_objects.RemoveFirst(el);
	}

	m_pendingDestroyObjects.Clear();
}