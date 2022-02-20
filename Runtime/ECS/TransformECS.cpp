#include "ECS/TransformECS.h"

using namespace Sailor;
using namespace Sailor::JobSystem;

void Transform::SetPosition(const glm::vec4& position)
{
	m_bIsDirty = true;
	m_transform.m_position = position;
}

void Transform::SetRotation(const glm::quat& quat)
{
	m_bIsDirty = true;
	m_transform.m_rotation = quat;
}

void Transform::SetScale(const glm::vec4& scale)
{
	m_bIsDirty = true;
	m_transform.m_scale = scale;
}

void TransformECS::Tick(float deltaTime)
{
	for (auto& data : m_components)
	{
		if (data.m_bIsActive && data.m_bIsDirty)
		{
			data.m_cachedRelativeMatrix = data.m_transform.Matrix();
			data.m_bIsDirty = false;
		}
	}

	for (auto& data : m_components)
	{
		if (data.m_bIsActive && data.m_parent == ECS::InvalidIndex)
		{
			CalculateMatrices(data);
		}
	}
}

void TransformECS::CalculateMatrices(Transform& parent)
{
	const glm::mat4x4& parentMatrix = parent.GetCachedRelativeMatrix();

	for (auto& child : parent.GetChildren())
	{
		m_components[child].m_cachedWorldMatrix = parentMatrix * m_components[child].m_cachedRelativeMatrix;
		
		CalculateMatrices(m_components[child]);
	}
}
