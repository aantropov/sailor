#include "Engine/World.h"
#include "Engine/GameObject.h"
#include "Components/Component.h"
#include "ECS/TransformECS.h"

using namespace Sailor;
using namespace Sailor::Tasks;

GameObject::GameObject(WorldPtr world, const std::string& name) : m_name(name), m_pWorld(world)
{
	m_transformHandle = m_pWorld->GetECS<TransformECS>()->RegisterComponent();
}

void GameObject::BeginPlay()
{
	GetTransformComponent().SetOwner(m_self);
}

TransformComponent& GameObject::GetTransformComponent()
{
	return m_pWorld->GetECS<TransformECS>()->GetComponentData(m_transformHandle);
}

void GameObject::SetParent(GameObjectPtr parent)
{
	if (m_parent.IsValid())
	{
		m_parent->m_children.RemoveFirst(m_self);
	}

	m_parent = parent;

	if (m_parent.IsValid())
	{
		m_parent->m_children.Add(m_self);
		GetTransformComponent().SetNewParent(&parent->GetTransformComponent());
	}
	else
	{
		GetTransformComponent().SetNewParent(nullptr);
	}
}

bool GameObject::RemoveComponent(ComponentPtr component)
{
	check(component);

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
	check(m_componentsToAdd <= m_components.Num());

	for (uint32_t i = 0; i < m_components.Num() - m_componentsToAdd; i++)
	{
		auto& el = m_components[i];
		if (el->m_bBeginPlayCalled)
		{
			el->Tick(deltaTime);
		}
		else
		{
			el->BeginPlay();
			el->m_bBeginPlayCalled = true;
		}

#ifdef SAILOR_EDITOR
		el->OnGizmo();
#endif
	}

	m_componentsToAdd = 0;
}