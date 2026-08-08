#include "Components/RigidBodyComponent.h"
#include "ECS/PhysicsECS.h"
#include "Engine/GameObject.h"
#include "Math/Math.h"
#include <algorithm>
#include <cmath>

using namespace Sailor;

void RigidBodyComponent::Initialize()
{
	auto* ecs = GetOwner()->GetWorld()->GetECS<PhysicsECS>();
	m_handle = ecs->RegisterComponent();
	auto& data = ecs->GetComponentData(m_handle);
	data.SetOwner(GetOwner());
	data.MarkDirty();
	data.MarkVelocityDirty();
}

void RigidBodyComponent::EndPlay()
{
	if (m_handle != ECS::InvalidIndex)
	{
		GetOwner()->GetWorld()->GetECS<PhysicsECS>()
			->UnregisterComponent(m_handle);
		m_handle = ECS::InvalidIndex;
	}
}

void RigidBodyComponent::MarkPhysicsDirty()
{
	if (m_handle != ECS::InvalidIndex)
	{
		GetOwner()->GetWorld()->GetECS<PhysicsECS>()
			->GetComponentData(m_handle)
			.MarkDirty();
	}
}

void RigidBodyComponent::SetMotionType(
	Physics::ERigidBodyMotionType value)
{
	if (m_motionType != value)
	{
		m_motionType = value;
		MarkPhysicsDirty();
	}
}

void RigidBodyComponent::SetMass(float value)
{
	if (!std::isfinite(value))
	{
		return;
	}

	value = std::max(0.001f, value);
	if (m_mass != value)
	{
		m_mass = value;
		MarkPhysicsDirty();
	}
}

void RigidBodyComponent::SetFriction(float value)
{
	if (!std::isfinite(value))
	{
		return;
	}

	value = std::max(0.0f, value);
	if (m_friction != value)
	{
		m_friction = value;
		MarkPhysicsDirty();
	}
}

void RigidBodyComponent::SetRestitution(float value)
{
	if (!std::isfinite(value))
	{
		return;
	}

	value = std::clamp(value, 0.0f, 1.0f);
	if (m_restitution != value)
	{
		m_restitution = value;
		MarkPhysicsDirty();
	}
}

void RigidBodyComponent::SetLinearDamping(float value)
{
	if (!std::isfinite(value))
	{
		return;
	}

	value = std::clamp(value, 0.0f, 1.0f);
	if (m_linearDamping != value)
	{
		m_linearDamping = value;
		MarkPhysicsDirty();
	}
}

void RigidBodyComponent::SetAngularDamping(float value)
{
	if (!std::isfinite(value))
	{
		return;
	}

	value = std::clamp(value, 0.0f, 1.0f);
	if (m_angularDamping != value)
	{
		m_angularDamping = value;
		MarkPhysicsDirty();
	}
}

void RigidBodyComponent::SetGravityFactor(float value)
{
	if (!std::isfinite(value))
	{
		return;
	}

	if (m_gravityFactor != value)
	{
		m_gravityFactor = value;
		MarkPhysicsDirty();
	}
}

void RigidBodyComponent::SetCollisionLayer(uint32_t value)
{
	const uint8_t layer = static_cast<uint8_t>(std::min(value, 15u));
	if (m_collisionLayer != layer)
	{
		m_collisionLayer = layer;
		MarkPhysicsDirty();
	}
}

void RigidBodyComponent::SetSensor(bool value)
{
	if (m_bSensor != value)
	{
		m_bSensor = value;
		MarkPhysicsDirty();
	}
}

void RigidBodyComponent::SetSleepingAllowed(bool value)
{
	if (m_bAllowSleeping != value)
	{
		m_bAllowSleeping = value;
		MarkPhysicsDirty();
	}
}

void RigidBodyComponent::SetLinearVelocity(const glm::vec3& value)
{
	if (!Math::AllFinite(value))
	{
		return;
	}

	if (m_linearVelocity != value)
	{
		m_linearVelocity = value;
		if (m_handle != ECS::InvalidIndex)
		{
			GetOwner()->GetWorld()->GetECS<PhysicsECS>()
				->GetComponentData(m_handle)
				.MarkVelocityDirty();
		}
	}
}

void RigidBodyComponent::SetAngularVelocity(const glm::vec3& value)
{
	if (!Math::AllFinite(value))
	{
		return;
	}

	if (m_angularVelocity != value)
	{
		m_angularVelocity = value;
		if (m_handle != ECS::InvalidIndex)
		{
			GetOwner()->GetWorld()->GetECS<PhysicsECS>()
				->GetComponentData(m_handle)
				.MarkVelocityDirty();
		}
	}
}

bool RigidBodyComponent::AddForceAtPosition(
	const glm::vec3& force,
	const glm::vec3& worldPosition)
{
	if (m_handle == ECS::InvalidIndex ||
		!Math::AllFinite(force) || !Math::AllFinite(worldPosition))
	{
		return false;
	}

	return GetOwner()->GetWorld()->GetECS<PhysicsECS>()
		->AddForceAtPosition(m_handle, force, worldPosition);
}
