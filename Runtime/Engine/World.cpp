#include "Engine/World.h"

using namespace Sailor;

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
		el.DestroyObject();

		m_objects.RemoveFirst(el);
	}

	m_pendingDestroyObjects.Clear();
}