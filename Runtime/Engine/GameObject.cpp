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
		component.ForcelyDestroyObject();
		return true;
	}

	return false;
}

void GameObject::RemoveAllComponents()
{
	for (auto& el : m_components)
	{
		el->EndPlay();
		el.ForcelyDestroyObject();
	}

	m_components.Clear(true);
}