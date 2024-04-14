#include "ECS/TransformECS.h"
#include "Engine/GameObject.h"

using namespace Sailor;
using namespace Sailor::Tasks;

/*
void TransformComponent::SetPosition(const glm::vec4& position)
{
	if (position != m_transform.m_position)
	{
		MarkDirty();
		SetPosition(vec3(position));
	}
}*/

void TransformComponent::SetNewParent(const TransformComponent* parent)
{
	MarkDirty();
	m_newParent = parent;
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
	if (quat != m_transform.m_rotation)
	{
		MarkDirty();
		m_transform.m_rotation = quat;
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

void TransformECS::MarkDirty(TransformComponent* ptr)
{
	m_dirtyComponents.Add(TransformECS::GetComponentIndex(ptr));
}

Tasks::ITaskPtr TransformECS::PostTick()
{
	m_dirtyComponents.Clear(false);
	return nullptr;
}

Tasks::ITaskPtr TransformECS::Tick(float deltaTime)
{
	SAILOR_PROFILE_FUNCTION();

	// We guess that the amount of changed transform during frame
	// Could be much less than the whole transforms num

	const float NLogN_Algo = 2.0f * (float)m_dirtyComponents.Num() * std::max(1.0f, std::logf((float)m_dirtyComponents.Num()));
	const float N_Algo = 2.0f * (float)m_components.Num();

	if (NLogN_Algo < N_Algo)
	{
		// We should sort the dirty components to make the pass
		// more cache-friendly
		m_dirtyComponents.Sort();

		// Update only changed transforms
		for (auto& i : m_dirtyComponents)
		{
			auto& data = m_components[i];

			size_t parentId = GetComponentIndex(data.m_newParent);

			// First resolve relations
			if (parentId != data.m_parent)
			{
				if (data.m_parent != ECS::InvalidIndex)
				{
					auto& parentData = m_components[data.m_parent];
					parentData.m_children.RemoveFirst(i);
				}

				data.m_parent = parentId;

				if (data.m_parent != ECS::InvalidIndex)
				{
					auto& parentData = m_components[data.m_parent];
					parentData.m_children.Add(i);
				}
			}

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

				m_dirtyComponents.RemoveAtSwap(i);
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
				size_t parentId = GetComponentIndex(data.m_newParent);

				// First resolve relations
				if (parentId != data.m_parent)
				{
					if (data.m_parent != ECS::InvalidIndex)
					{
						auto& parentData = m_components[data.m_parent];
						parentData.m_children.RemoveFirst(i);
					}

					data.m_parent = parentId;

					if (data.m_parent != ECS::InvalidIndex)
					{
						auto& parentData = m_components[data.m_parent];
						parentData.m_children.Add(i);
					}
				}

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

	return nullptr;
}

void TransformECS::CalculateMatrices(TransformComponent& parent)
{
	const glm::mat4x4& parentMatrix = parent.GetCachedRelativeMatrix();

	if (parent.m_parent == ECS::InvalidIndex)
	{
		parent.m_cachedWorldMatrix = parent.m_cachedRelativeMatrix;
	}

	for (auto& child : parent.GetChildren())
	{
		m_components[child].m_cachedWorldMatrix = parentMatrix * m_components[child].m_cachedRelativeMatrix;

		CalculateMatrices(m_components[child]);
	}

	if (parent.m_bIsDirty)
	{
		UpdateGameObject(parent.GetOwner().StaticCast<GameObject>(), GetWorld()->GetCurrentFrame());
	}

	parent.m_bIsDirty = false;
}
