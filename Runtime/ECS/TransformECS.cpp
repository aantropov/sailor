#include "ECS/TransformECS.h"
#include "Engine/GameObject.h"

using namespace Sailor;
using namespace Sailor::Tasks;

void TransformComponent::SetNewParent(const TransformComponent* parent)
{
	MarkDirty();
	auto* transforms = GetOwner().StaticCast<GameObject>()->GetWorld()->GetECS<TransformECS>();
	transforms->RequestParent(transforms->GetComponentIndex(this), transforms->GetComponentIndex(parent));
}

void TransformComponent::SetPosition(const glm::vec3& position)
{
	if (position != vec3(m_transform.m_position))
	{
		MarkDirty();
		m_transform.m_position = vec4(position, 1);
	}
}

void TransformComponent::SetRotation(const glm::quat& quat)
{
	const auto previous = m_transform.GetRotation();
	m_transform.SetRotation(quat);
	if (previous != m_transform.GetRotation())
	{
		MarkDirty();
	}
}

void TransformComponent::SetScale(const glm::vec4& scale)
{
	if (scale != m_transform.m_scale)
	{
		MarkDirty();
		m_transform.m_scale = scale;
	}
}

void TransformComponent::MarkDirty()
{
	if (!m_bIsDirty)
	{
		GetOwner().StaticCast<GameObject>()->GetWorld()->GetECS<TransformECS>()->MarkDirty(this);
		m_bIsDirty = true;
	}

	m_frameLastChange = GetOwner().StaticCast<GameObject>()->GetWorld()->GetCurrentFrame();
}

void TransformECS::MarkDirty(TransformComponent* component)
{
	if (component->m_dirtyIndex == ECS::InvalidIndex)
	{
		component->m_dirtyIndex = m_dirtyComponents.Num();
		m_dirtyComponents.Add(GetComponentIndex(component));
	}
}

void TransformECS::RemoveDirty(TransformComponent& component)
{
	if (component.m_dirtyIndex == ECS::InvalidIndex) return;
	const size_t dirtyIndex = component.m_dirtyIndex;
	const size_t moved = *m_dirtyComponents.Last();
	m_dirtyComponents.RemoveAtSwap(dirtyIndex);
	if (dirtyIndex < m_dirtyComponents.Num()) m_components[moved].m_dirtyIndex = dirtyIndex;
	component.m_dirtyIndex = ECS::InvalidIndex;
#if defined(SAILOR_ECS_TEST_HOOKS)
	m_numRemovalVisits += 1 + (dirtyIndex < m_dirtyComponents.Num());
#endif
}

void TransformECS::RemovePendingParent(TransformComponent& component)
{
	if (component.m_pendingChildIndex == ECS::InvalidIndex) return;
	auto& children = m_pendingChildren[component.m_newParent];
	const size_t childIndex = component.m_pendingChildIndex;
	const size_t moved = *children.Last();
	children.RemoveAtSwap(childIndex);
	if (childIndex < children.Num()) m_components[moved].m_pendingChildIndex = childIndex;
	component.m_pendingChildIndex = ECS::InvalidIndex;
#if defined(SAILOR_ECS_TEST_HOOKS)
	m_numRemovalVisits += 1 + (childIndex < children.Num());
#endif
	if (children.IsEmpty()) m_pendingChildren.Remove(component.m_newParent);
}

void TransformECS::RemovePublishedParent(TransformComponent& component)
{
	if (component.m_parent == ECS::InvalidIndex) return;
	auto& children = m_components[component.m_parent].m_children;
	const size_t childIndex = component.m_parentChildIndex;
	const size_t moved = *children.Last();
	children.RemoveAtSwap(childIndex);
	if (childIndex < children.Num()) m_components[moved].m_parentChildIndex = childIndex;
	component.m_parent = ECS::InvalidIndex;
	component.m_parentChildIndex = ECS::InvalidIndex;
#if defined(SAILOR_ECS_TEST_HOOKS)
	m_numRemovalVisits += 2 + (childIndex < children.Num());
#endif
}

void TransformECS::RequestParent(size_t index, size_t parent)
{
	auto& component = m_components[index];
	if (GetWorld()->IsClearing())
	{
		// EndPlay callbacks can reparent after another transform slot was released.
		// The whole hierarchy is retiring; its reverse links are cleared in EndPlay.
		component.m_newParent = parent;
		return;
	}
	if (component.m_newParent == parent) return;
	RemovePendingParent(component);
	component.m_newParent = parent;
	if (parent != ECS::InvalidIndex && parent != component.m_parent)
	{
		auto& children = m_pendingChildren[parent];
		component.m_pendingChildIndex = children.Num();
		children.Add(index);
	}
}

void TransformECS::ApplyParent(size_t index)
{
	auto& component = m_components[index];
	if (component.m_parent == component.m_newParent) return;
	RemovePendingParent(component);
	RemovePublishedParent(component);
	component.m_parent = component.m_newParent;
	if (component.m_parent != ECS::InvalidIndex)
	{
		auto& children = m_components[component.m_parent].m_children;
		component.m_parentChildIndex = children.Num();
		children.Add(index);
	}
}

void TransformECS::OnComponentUnregistered(size_t index, TransformComponent& component)
{
	if (GetWorld() && GetWorld()->IsClearing()) return;
	RemoveDirty(component);
	RemovePendingParent(component);
	RemovePublishedParent(component);
#if defined(SAILOR_ECS_TEST_HOOKS)
	++m_numRemovalVisits;
#endif

	while (!component.m_children.IsEmpty())
	{
		auto& child = m_components[*component.m_children.Last()];
		RemovePublishedParent(child);
		// A move away from the removed parent must survive until the next Tick.
		if (child.m_newParent == index) child.m_newParent = ECS::InvalidIndex;
		child.MarkDirty();
#if defined(SAILOR_ECS_TEST_HOOKS)
		++m_numRemovalVisits;
#endif
	}

	TVector<size_t>* pending = nullptr;
	while (m_pendingChildren.Find(index, pending))
	{
		auto& child = m_components[*pending->Last()];
		RemovePendingParent(child);
		child.m_newParent = ECS::InvalidIndex;
		child.MarkDirty();
#if defined(SAILOR_ECS_TEST_HOOKS)
		++m_numRemovalVisits;
#endif
	}
}

void TransformECS::EndPlay()
{
	m_pendingChildren.Clear();
	m_dirtyComponents.Clear();
	ECS::TSystem<TransformECS, TransformComponent>::EndPlay();
}

void TransformECS::Tick(float deltaTime)
{
	SAILOR_PROFILE_FUNCTION();
	if (m_dirtyComponents.IsEmpty()) return;

	// We guess that the amount of changed transform during frame
	// Could be much less than the whole transforms num

	const float NLogN_Algo = 2.0f * (float)m_dirtyComponents.Num() * std::max(1.0f, std::logf((float)m_dirtyComponents.Num()));
	const float N_Algo = 2.0f * (float)m_components.Num();

	if (NLogN_Algo < N_Algo)
	{
		// We should sort the dirty components to make the pass
		// more cache-friendly
		m_dirtyComponents.Sort();
		for (size_t i = 0; i < m_dirtyComponents.Num(); ++i)
			m_components[m_dirtyComponents[i]].m_dirtyIndex = i;

		// Update only changed transforms
		for (auto& i : m_dirtyComponents)
		{
			auto& data = m_components[i];

			ApplyParent(i);

			if (data.m_bIsActive)
			{
				data.m_cachedRelativeMatrix = data.m_transform.Matrix();
			}
		}

		// Recalculate only root transforms
		for (int32_t i = 0; i < m_dirtyComponents.Num(); i++)
		{
			auto& data = m_components[m_dirtyComponents[i]];

			if (data.m_parent == ECS::InvalidIndex && data.m_bIsActive)
			{
				CalculateMatrices(data);

				RemoveDirty(data);
				i--;
			}
		}

		// Recalculate not calculated transforms
		for (int32_t i = 0; i < m_dirtyComponents.Num(); i++)
		{
			auto& data = m_components[m_dirtyComponents[i]];

			if (data.m_bIsDirty)
			{
				CalculateMatrices(data);
			}
		}
	}
	else
	{
		for (size_t i = 0; i < m_components.Num(); i++)
		{
			auto& data = m_components[i];
			if (data.m_bIsDirty)
			{
				ApplyParent(i);

				if (data.m_bIsActive)
				{
					data.m_cachedRelativeMatrix = data.m_transform.Matrix();
				}
			}
		}

		for (auto& data : m_components)
		{
			if (data.m_bIsDirty && data.m_bIsActive)
			{
				CalculateMatrices(data);
			}
		}
	}

	for (size_t index : m_dirtyComponents) m_components[index].m_dirtyIndex = ECS::InvalidIndex;
	m_dirtyComponents.Clear(false);
}

void TransformECS::CalculateMatrices(TransformComponent& parent)
{
	if (parent.m_parent == ECS::InvalidIndex)
	{
		parent.m_cachedWorldMatrix = parent.m_cachedRelativeMatrix;
	}
	else
	{
		parent.m_cachedWorldMatrix = m_components[parent.m_parent].GetCachedWorldMatrix() * parent.m_cachedRelativeMatrix;
	}

	const glm::mat4x4& parentMatrix = parent.GetCachedWorldMatrix();
	parent.m_frameLastChange = GetWorld()->GetCurrentFrame();

	for (auto& child : parent.GetChildren())
	{
		if (child >= m_components.Num() || !m_components[child].m_bIsActive)
		{
			continue;
		}

		m_components[child].m_cachedWorldMatrix = parentMatrix * m_components[child].m_cachedRelativeMatrix;

		CalculateMatrices(m_components[child]);
	}

	if (parent.m_bIsDirty)
	{
		UpdateGameObject(parent.GetOwner().StaticCast<GameObject>(), GetWorld()->GetCurrentFrame());
	}

	parent.m_bIsDirty = false;
}
