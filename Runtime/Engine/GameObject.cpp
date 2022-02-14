#include "Engine/World.h"
#include "Engine/GameObject.h"
#include "Components/Component.h"

using namespace Sailor;
using namespace Sailor::JobSystem;

bool GameObject::RemoveComponent(ComponentPtr component)
{
	assert(component);

	if (m_components.RemoveFirst(component))
	{
		component->EndPlay();
		component.DestroyObject(m_world.Lock()->GetAllocator());
		return true;
	}

	return false;
}

void GameObject::RemoveAllComponents()
{
	for (auto& el : m_components)
	{
		el->EndPlay();
		el.DestroyObject(m_world.Lock()->GetAllocator());
	}

	m_components.Clear(true);
}