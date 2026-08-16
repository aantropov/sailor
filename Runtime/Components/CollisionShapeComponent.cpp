#include "Components/CollisionShapeComponent.h"
#include "Components/RigidBodyComponent.h"
#include "ECS/TransformECS.h"
#include "Engine/GameObject.h"
#include "Math/Math.h"
#include "RHI/DebugContext.h"
#include <algorithm>
#include <cmath>

using namespace Sailor;

namespace
{
	struct WorldShape final
	{
		glm::vec3 m_center{};
		glm::quat m_rotation = glm::identity<glm::quat>();
		glm::vec3 m_scale = glm::vec3(1.0f);
	};

	WorldShape ResolveWorldShape(
		const glm::mat4& ownerWorldMatrix,
		const Physics::CollisionShapeDesc& shape)
	{
		const Math::Transform ownerTransform =
			Math::Transform::FromMatrix(ownerWorldMatrix);
		const glm::vec3 ownerScale(ownerTransform.m_scale);
		WorldShape result{};
		result.m_center = glm::vec3(ownerTransform.m_position) +
			ownerTransform.m_rotation * (shape.m_center * ownerScale);
		result.m_rotation = glm::normalize(
			ownerTransform.m_rotation * shape.m_rotation);
		result.m_scale = glm::max(
			glm::abs(ownerScale),
			glm::vec3(0.001f));
		return result;
	}

	glm::vec3 ResolveOrientedExtent(
		const glm::quat& rotation,
		const glm::vec3& halfExtent)
	{
		return glm::abs(rotation * Math::vec3_Right) * halfExtent.x +
			glm::abs(rotation * Math::vec3_Up) * halfExtent.y +
			glm::abs(rotation * Math::vec3_Forward) * halfExtent.z;
	}

	void DrawOrientedBox(
		RHI::DebugContext& debugContext,
		const glm::vec3& center,
		const glm::quat& rotation,
		const glm::vec3& halfExtent,
		const glm::vec4& color)
	{
		glm::vec3 corners[8];
		for (uint32_t i = 0; i < 8; ++i)
		{
			const glm::vec3 local(
				(i & 1u) != 0u ? halfExtent.x : -halfExtent.x,
				(i & 2u) != 0u ? halfExtent.y : -halfExtent.y,
				(i & 4u) != 0u ? halfExtent.z : -halfExtent.z);
			corners[i] = center + rotation * local;
		}

		constexpr uint32_t edges[][2] = {
			{ 0, 1 }, { 0, 2 }, { 0, 4 }, { 1, 3 }, { 1, 5 }, { 2, 3 },
			{ 2, 6 }, { 3, 7 }, { 4, 5 }, { 4, 6 }, { 5, 7 }, { 6, 7 }
		};
		for (const auto& edge : edges)
		{
			debugContext.DrawLine(corners[edge[0]], corners[edge[1]], color);
		}
	}
}

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

bool CollisionShapeComponent::TryGetWorldBounds(
	const glm::mat4& ownerWorldMatrix,
	Math::AABB& outBounds) const
{
	outBounds = {};
	const Physics::CollisionShapeDesc shape = BuildDesc();
	const WorldShape worldShape = ResolveWorldShape(ownerWorldMatrix, shape);

	switch (shape.m_type)
	{
	case Physics::ECollisionShapeType::Box:
	{
		const glm::vec3 halfExtent =
			glm::abs(shape.m_size) * worldShape.m_scale * 0.5f;
		const glm::vec3 worldExtent = ResolveOrientedExtent(
			worldShape.m_rotation,
			halfExtent);
		outBounds = Math::AABB(worldShape.m_center, worldExtent);
		break;
	}
	case Physics::ECollisionShapeType::Sphere:
	{
		const float radius = std::abs(shape.m_radius) * std::max({
			worldShape.m_scale.x,
			worldShape.m_scale.y,
			worldShape.m_scale.z });
		outBounds = Math::AABB(
			worldShape.m_center,
			glm::vec3(radius));
		break;
	}
	case Physics::ECollisionShapeType::Capsule:
	{
		const float radius = std::abs(shape.m_radius) * std::max(
			worldShape.m_scale.x,
			worldShape.m_scale.z);
		const float totalHeight = std::max(
			2.0f * radius,
			std::abs(shape.m_height) * worldShape.m_scale.y);
		const glm::vec3 axis = worldShape.m_rotation * Math::vec3_Up;
		const glm::vec3 segmentExtent =
			glm::abs(axis) * (totalHeight * 0.5f - radius);
		const glm::vec3 worldExtent = segmentExtent + glm::vec3(radius);
		outBounds = Math::AABB(worldShape.m_center, worldExtent);
		break;
	}
	case Physics::ECollisionShapeType::TriangleMesh:
	default:
		return false;
	}

	return outBounds.IsValid();
}

void CollisionShapeComponent::OnGizmo()
{
	auto owner = GetOwner();
	if (!owner || !owner->GetWorld())
	{
		return;
	}

	const glm::mat4& ownerWorldMatrix =
		owner->GetTransformComponent().GetCachedWorldMatrix();
	auto& debugContext = *owner->GetWorld()->GetDebugContext();
	const glm::vec4 color(0.2f, 1.0f, 0.35f, 1.0f);
	const Physics::CollisionShapeDesc shape = BuildDesc();
	const WorldShape worldShape = ResolveWorldShape(ownerWorldMatrix, shape);

	switch (shape.m_type)
	{
	case Physics::ECollisionShapeType::Box:
		DrawOrientedBox(
			debugContext,
			worldShape.m_center,
			worldShape.m_rotation,
			glm::abs(shape.m_size) * worldShape.m_scale * 0.5f,
			color);
		break;
	case Physics::ECollisionShapeType::Sphere:
		debugContext.DrawSphere(
			worldShape.m_center,
			std::abs(shape.m_radius) * std::max({
				worldShape.m_scale.x,
				worldShape.m_scale.y,
				worldShape.m_scale.z }),
			color);
		break;
	case Physics::ECollisionShapeType::Capsule:
	{
		const float radius = std::abs(shape.m_radius) * std::max(
			worldShape.m_scale.x,
			worldShape.m_scale.z);
		const float totalHeight = std::max(
			2.0f * radius,
			std::abs(shape.m_height) * worldShape.m_scale.y);
		const float halfSegment = totalHeight * 0.5f - radius;
		const glm::vec3 axis = worldShape.m_rotation * Math::vec3_Up;
		const glm::vec3 firstCenter = worldShape.m_center - axis * halfSegment;
		const glm::vec3 secondCenter = worldShape.m_center + axis * halfSegment;
		debugContext.DrawSphere(firstCenter, radius, color);
		debugContext.DrawSphere(secondCenter, radius, color);
		const glm::vec3 right = worldShape.m_rotation * Math::vec3_Right * radius;
		const glm::vec3 forward = worldShape.m_rotation * Math::vec3_Forward * radius;
		debugContext.DrawLine(firstCenter + right, secondCenter + right, color);
		debugContext.DrawLine(firstCenter - right, secondCenter - right, color);
		debugContext.DrawLine(firstCenter + forward, secondCenter + forward, color);
		debugContext.DrawLine(firstCenter - forward, secondCenter - forward, color);
		break;
	}
	case Physics::ECollisionShapeType::TriangleMesh:
	default:
		break;
	}
}
