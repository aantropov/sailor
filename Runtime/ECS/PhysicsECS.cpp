#include "ECS/PhysicsECS.h"
#include "ECS/TransformECS.h"
#include "Physics/PhysicsWorld.h"
#include "Physics/JoltRuntime.h"
#include "Components/RigidBodyComponent.h"
#include "Components/CollisionShapeComponent.h"
#include "Engine/GameObject.h"
#include "Engine/World.h"
#include "Math/Transform.h"
#include "Tasks/Tasks.h"
#include <algorithm>
#include <cmath>

using namespace Sailor;

PhysicsECS::PhysicsECS() = default;
PhysicsECS::~PhysicsECS() = default;

bool PhysicsECS::EnsurePhysicsWorld()
{
	if (m_physicsWorld)
	{
		return true;
	}

	if (!App::GetSubmodule<Physics::JoltRuntime>())
	{
		return false;
	}

	m_physicsWorld = TUniquePtr<Physics::PhysicsWorld>::Make();
	return true;
}

void PhysicsECS::SetFixedDeltaTime(float value)
{
	m_fixedDeltaTime = std::clamp(value, 1.0f / 1000.0f, 1.0f);
	m_accumulator = std::min(m_accumulator, m_fixedDeltaTime);
}

void PhysicsECS::SetMaxSubSteps(uint32_t value)
{
	m_maxSubSteps = std::clamp(value, 1u, 16u);
}

bool PhysicsECS::BuildBodyDesc(
	size_t index,
	Physics::RigidBodyDesc& outDesc)
{
	if (!IsComponentRegistered(index))
	{
		return false;
	}

	auto& data = GetComponentData(index);
	auto gameObject = data.m_owner.StaticCast<GameObject>();
	if (!gameObject)
	{
		return false;
	}

	auto rigidBody = gameObject->GetComponent<RigidBodyComponent>();
	if (!rigidBody || rigidBody->GetComponentIndex() != index)
	{
		return false;
	}

	const Math::Transform worldTransform = Math::Transform::FromMatrix(
		gameObject->GetTransformComponent().GetCachedWorldMatrix());
	outDesc = {};
	outDesc.m_instanceId = gameObject->GetInstanceId();
	outDesc.m_motionType = rigidBody->GetMotionType();
	outDesc.m_position = glm::vec3(worldTransform.m_position);
	outDesc.m_rotation = worldTransform.m_rotation;
	outDesc.m_scale = glm::vec3(worldTransform.m_scale);
	outDesc.m_linearVelocity = rigidBody->GetLinearVelocity();
	outDesc.m_angularVelocity = rigidBody->GetAngularVelocity();
	outDesc.m_mass = rigidBody->GetMass();
	outDesc.m_friction = rigidBody->GetFriction();
	outDesc.m_restitution = rigidBody->GetRestitution();
	outDesc.m_linearDamping = rigidBody->GetLinearDamping();
	outDesc.m_angularDamping = rigidBody->GetAngularDamping();
	outDesc.m_gravityFactor = rigidBody->GetGravityFactor();
	outDesc.m_collisionLayer = static_cast<uint8_t>(
		rigidBody->GetCollisionLayer());
	outDesc.m_bSensor = rigidBody->IsSensor();
	outDesc.m_bAllowSleeping = rigidBody->IsSleepingAllowed();

	for (const auto& component : gameObject->GetComponents())
	{
		if (auto shape = component.DynamicCast<CollisionShapeComponent>())
		{
			outDesc.m_shapes.Add(shape->BuildDesc());
		}
	}
	return !outDesc.m_shapes.IsEmpty();
}

bool PhysicsECS::RecreateBody(size_t index)
{
	if (!EnsurePhysicsWorld() || !IsComponentRegistered(index))
	{
		return false;
	}

	auto& data = GetComponentData(index);
	Physics::RigidBodyDesc desc{};
	if (!BuildBodyDesc(index, desc))
	{
		if (data.m_bodyId != RigidBodyData::InvalidBodyId)
		{
			m_physicsWorld->DestroyBody(data.m_bodyId);
			data.m_bodyId = RigidBodyData::InvalidBodyId;
		}
		return false;
	}

	if (data.m_bodyId != RigidBodyData::InvalidBodyId)
	{
		Physics::PhysicsBodyPose previousPose{};
		if (!data.m_bVelocityDirty &&
			m_physicsWorld->GetBodyPose(data.m_bodyId, previousPose))
		{
			desc.m_linearVelocity = previousPose.m_linearVelocity;
			desc.m_angularVelocity = previousPose.m_angularVelocity;
		}
		m_physicsWorld->DestroyBody(data.m_bodyId);
		data.m_bodyId = RigidBodyData::InvalidBodyId;
	}

	if (!m_physicsWorld->CreateBody(desc, data.m_bodyId))
	{
		return false;
	}

	data.m_motionType = desc.m_motionType;
	data.m_bodyScale = desc.m_scale;
	data.m_bVelocityDirty = false;
	data.ClearDirty();
	data.m_lastAppliedTransformFrame = GetWorld()->GetCurrentFrame();
	if (m_physicsWorld->GetBodyPose(data.m_bodyId, data.m_currentPose))
	{
		data.m_previousPose = data.m_currentPose;
	}
	return true;
}

void PhysicsECS::SyncAuthoredTransforms(float fixedDeltaTime)
{
	for (size_t index = 0; index < m_components.Num(); ++index)
	{
		if (!IsComponentRegistered(index))
		{
			continue;
		}

		auto& data = m_components[index];
		if (data.IsDirty() || data.m_bodyId == RigidBodyData::InvalidBodyId)
		{
			if (!RecreateBody(index))
			{
				continue;
			}
		}

		auto gameObject = data.m_owner.StaticCast<GameObject>();
		if (!gameObject)
		{
			continue;
		}

		auto rigidBody = gameObject->GetComponent<RigidBodyComponent>();
		if (!rigidBody)
		{
			continue;
		}

		const auto& transform = gameObject->GetTransformComponent();
		const bool bAuthoredTransformChanged =
			transform.GetFrameLastChange() > data.m_lastAppliedTransformFrame;
		const Math::Transform worldTransform = Math::Transform::FromMatrix(
			transform.GetCachedWorldMatrix());
		if (bAuthoredTransformChanged &&
			glm::any(glm::greaterThan(
				glm::abs(glm::vec3(worldTransform.m_scale) - data.m_bodyScale),
				glm::vec3(0.000001f))))
		{
			RecreateBody(index);
			continue;
		}

		if (data.m_motionType != Physics::ERigidBodyMotionType::Dynamic ||
			bAuthoredTransformChanged)
		{
			m_physicsWorld->SetBodyTransform(
				data.m_bodyId,
				glm::vec3(worldTransform.m_position),
				worldTransform.m_rotation,
				data.m_motionType == Physics::ERigidBodyMotionType::Kinematic,
				fixedDeltaTime);
			data.m_lastAppliedTransformFrame = GetWorld()->GetCurrentFrame();
		}

		if (data.m_bVelocityDirty)
		{
			m_physicsWorld->SetBodyVelocity(
				data.m_bodyId,
				rigidBody->GetLinearVelocity(),
				rigidBody->GetAngularVelocity());
			data.m_bVelocityDirty = false;
		}
	}
}

void PhysicsECS::ApplyDynamicTransforms(float interpolationAlpha)
{
	TVector<size_t> updatedComponents;
	for (size_t index = 0; index < m_components.Num(); ++index)
	{
		if (!IsComponentRegistered(index))
		{
			continue;
		}

		auto& data = m_components[index];
		if (data.m_motionType != Physics::ERigidBodyMotionType::Dynamic ||
			data.m_bodyId == RigidBodyData::InvalidBodyId)
		{
			continue;
		}

		auto gameObject = data.m_owner.StaticCast<GameObject>();
		if (!gameObject)
		{
			continue;
		}

		const glm::vec3 worldPosition = glm::mix(
			data.m_previousPose.m_position,
			data.m_currentPose.m_position,
			interpolationAlpha);
		const glm::quat worldRotation = glm::normalize(glm::slerp(
			data.m_previousPose.m_rotation,
			data.m_currentPose.m_rotation,
			interpolationAlpha));

		glm::vec3 localPosition = worldPosition;
		glm::quat localRotation = worldRotation;
		if (auto parent = gameObject->GetParent())
		{
			if (!Physics::TryConvertWorldPoseToLocal(
					parent->GetTransformComponent().GetCachedWorldMatrix(),
					worldPosition,
					worldRotation,
					localPosition,
					localRotation))
			{
				continue;
			}
		}

		auto& transform = gameObject->GetTransformComponent();
		transform.SetPosition(localPosition);
		transform.SetRotation(localRotation);
		updatedComponents.Add(index);
	}

	if (!updatedComponents.IsEmpty())
	{
		GetWorld()->GetECS<TransformECS>()->Tick(0.0f);
		for (size_t index : updatedComponents)
		{
			m_components[index].m_lastAppliedTransformFrame =
				GetWorld()->GetCurrentFrame();
		}
	}
}

Tasks::ITaskPtr PhysicsECS::Tick(float deltaTime)
{
	if (!GetWorld()->IsPhysicsSimulationEnabled())
	{
		if (m_bWasSimulationEnabled)
		{
			m_accumulator = 0.0f;
		}
		m_bWasSimulationEnabled = false;
		return {};
	}
	m_bWasSimulationEnabled = true;

	if (!EnsurePhysicsWorld())
	{
		return {};
	}

	SyncAuthoredTransforms(m_fixedDeltaTime);
	const float maxAccumulatedTime = m_fixedDeltaTime * m_maxSubSteps;
	m_accumulator = std::min(
		m_accumulator + std::max(0.0f, deltaTime),
		maxAccumulatedTime);
	const uint32_t numSteps = std::min(
		static_cast<uint32_t>(m_accumulator / m_fixedDeltaTime),
		m_maxSubSteps);
	if (numSteps == 0)
	{
		ApplyDynamicTransforms(std::clamp(
			m_accumulator / m_fixedDeltaTime,
			0.0f,
			1.0f));
		return {};
	}
	m_accumulator -= m_fixedDeltaTime * numSteps;

	bool bStepSucceeded = true;
	auto physicsTask = Tasks::CreateTask(
		"Physics fixed step",
		[this, numSteps, &bStepSucceeded]()
		{
			for (uint32_t step = 0; step < numSteps; ++step)
			{
				for (auto& data : m_components)
				{
					if (data.m_bIsActive &&
						data.m_motionType == Physics::ERigidBodyMotionType::Dynamic &&
						data.m_bodyId != RigidBodyData::InvalidBodyId)
					{
						data.m_previousPose = data.m_currentPose;
					}
				}

				bStepSucceeded &= m_physicsWorld->Step(m_fixedDeltaTime);
				for (auto& data : m_components)
				{
					if (data.m_bIsActive &&
						data.m_motionType == Physics::ERigidBodyMotionType::Dynamic &&
						data.m_bodyId != RigidBodyData::InvalidBodyId)
					{
						m_physicsWorld->GetBodyPose(
							data.m_bodyId,
							data.m_currentPose);
					}
				}
			}
		},
		EThreadType::Physics);
	physicsTask->Run();
	physicsTask->Wait();

	if (bStepSucceeded)
	{
		ApplyDynamicTransforms(std::clamp(
			m_accumulator / m_fixedDeltaTime,
			0.0f,
			1.0f));
	}
	return physicsTask;
}

bool PhysicsECS::Raycast(
	const glm::vec3& origin,
	const glm::vec3& direction,
	float distance,
	Physics::PhysicsRaycastHit& outHit,
	uint16_t collisionMask) const
{
	return m_physicsWorld &&
		m_physicsWorld->Raycast(
			origin,
			direction,
			distance,
			outHit,
			collisionMask);
}

void PhysicsECS::SetLayerCollisionEnabled(
	uint8_t firstLayer,
	uint8_t secondLayer,
	bool bEnabled)
{
	if (EnsurePhysicsWorld())
	{
		m_physicsWorld->SetLayerCollisionEnabled(
			firstLayer,
			secondLayer,
			bEnabled);
	}
}

bool PhysicsECS::IsLayerCollisionEnabled(
	uint8_t firstLayer,
	uint8_t secondLayer) const
{
	return m_physicsWorld &&
		m_physicsWorld->IsLayerCollisionEnabled(
			firstLayer,
			secondLayer);
}

void PhysicsECS::DrainContactEvents(
	TVector<Physics::PhysicsContactEvent>& outEvents)
{
	if (m_physicsWorld)
	{
		m_physicsWorld->DrainContactEvents(outEvents);
	}
}

void PhysicsECS::OnComponentUnregistered(
	size_t,
	RigidBodyData& component)
{
	if (m_physicsWorld &&
		component.m_bodyId != RigidBodyData::InvalidBodyId)
	{
		m_physicsWorld->DestroyBody(component.m_bodyId);
	}
	component.m_bodyId = RigidBodyData::InvalidBodyId;
}

void PhysicsECS::EndPlay()
{
	if (m_physicsWorld)
	{
		m_physicsWorld->Clear();
		m_physicsWorld.Clear();
	}
	m_accumulator = 0.0f;
	m_bWasSimulationEnabled = false;
	ECS::TSystem<PhysicsECS, RigidBodyData>::EndPlay();
}
