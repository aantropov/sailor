#include "Engine/World.h"
#include "Engine/GameObject.h"
#include "Components/Component.h"
#include "ECS/TransformECS.h"

using namespace Sailor;
using namespace Sailor::JobSystem;

GameObject::GameObject(WorldPtr world, const std::string& name) : m_name(name), m_pWorld(world)
{
	m_transformHandle = m_pWorld->GetECS<TransformECS>()->RegisterComponent();
}

void GameObject::BeginPlay()
{
	GetTransform().SetOwner(m_self);
}

Transform& GameObject::GetTransform()
{
	return m_pWorld->GetECS<TransformECS>()->GetComponentData(m_transformHandle);
}

bool GameObject::RemoveComponent(ComponentPtr component)
{
	assert(component);

	if (m_components.RemoveFirst(component))
	{
		component->EndPlay();
		component.DestroyObject(m_pWorld->GetAllocator());
		return true;
	}

	return false;
}

void GameObject::RemoveAllComponents()
{
	for (auto& el : m_components)
	{
		el->EndPlay();
		el.DestroyObject(m_pWorld->GetAllocator());
	}

	m_components.Clear(true);
}

void GameObject::EndPlay()
{
	m_pWorld->GetECS<TransformECS>()->UnregisterComponent(m_transformHandle);
}

void GameObject::Tick(float deltaTime)
{
	for (auto& el : m_components)
	{
		if (el->m_bBeginPlayCalled)
		{
			el->Tick(deltaTime);
		}
		else
		{
			el->BeginPlay();
			el->m_bBeginPlayCalled = true;
		}
	}
}