#include "Components/CollisionShapeComponent.h"
#include "Components/RigidBodyComponent.h"
#include "Engine/GameObject.h"
#include "Math/Math.h"
#include <algorithm>

using namespace Sailor;

void CollisionShapeComponent::Initialize()
{
	NotifyRigidBody();
}

void CollisionShapeComponent::EndPlay()
{
	const auto owner = GetOwner();
	if (!owner)
	{
		return;
	}

	for (const ComponentPtr& component : owner->GetComponents())
	{
		if (component.GetRawPtr() == this)
		{
			// RemoveAllComponents keeps every component in the owner array until
			// teardown completes. The rigid body owns the native cleanup there,
			// and may already have been destroyed before this shape is visited.
			return;
		}
	}

	NotifyRigidBody();
}

void CollisionShapeComponent::NotifyRigidBody()
{
	if (auto rigidBody = GetOwner()
		? GetOwner()->GetComponent<RigidBodyComponent>()
		: TObjectPtr<RigidBodyComponent>{})
	{
		rigidBody->MarkPhysicsDirty();
	}
}

void CollisionShapeComponent::SetShapeType(
	Physics::ECollisionShapeType value)
{
	if (m_shapeType != value)
	{
		m_shapeType = value;
		NotifyRigidBody();
	}
}

void CollisionShapeComponent::SetCenter(const glm::vec3& value)
{
	if (!Math::AllFinite(value))
	{
		return;
	}

	if (m_center != value)
	{
		m_center = value;
		NotifyRigidBody();
	}
}

void CollisionShapeComponent::SetRotation(const glm::quat& value)
{
	const glm::vec4 raw(value.x, value.y, value.z, value.w);
	const glm::vec4 sanitized = Math::SafeNormalize(
		raw,
		glm::vec4(0.0f, 0.0f, 0.0f, 1.0f));
	const glm::quat normalized(
		sanitized.w,
		sanitized.x,
		sanitized.y,
		sanitized.z);
	if (m_rotation != normalized)
	{
		m_rotation = normalized;
		NotifyRigidBody();
	}
}

void CollisionShapeComponent::SetSize(const glm::vec3& value)
{
	if (!Math::AllFinite(value))
	{
		return;
	}

	const glm::vec3 sanitized = glm::max(
		glm::abs(value),
		glm::vec3(0.001f));
	if (m_size != sanitized)
	{
		m_size = sanitized;
		NotifyRigidBody();
	}
}

void CollisionShapeComponent::SetRadius(float value)
{
	if (!std::isfinite(value))
	{
		return;
	}

	value = std::max(0.001f, std::abs(value));
	if (m_radius != value)
	{
		m_radius = value;
		NotifyRigidBody();
	}
}

void CollisionShapeComponent::SetHeight(float value)
{
	if (!std::isfinite(value))
	{
		return;
	}

	value = std::max(0.002f, std::abs(value));
	if (m_height != value)
	{
		m_height = value;
		NotifyRigidBody();
	}
}

Physics::CollisionShapeDesc CollisionShapeComponent::BuildDesc() const
{
	Physics::CollisionShapeDesc result{};
	result.m_type = m_shapeType;
	result.m_center = m_center;
	result.m_rotation = m_rotation;
	result.m_size = m_size;
	result.m_radius = m_radius;
	result.m_height = m_height;
	return result;
}
