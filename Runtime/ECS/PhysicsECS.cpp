#include "ECS/PhysicsECS.h"
#include "ECS/TransformECS.h"
#include "Physics/PhysicsWorld.h"
#include "Physics/JoltRuntime.h"
#include "Components/RigidBodyComponent.h"
#include "Components/CollisionShapeComponent.h"
#include "Components/BuoyancyComponent.h"
#include "Engine/GameObject.h"
#include "Engine/World.h"
#include "Math/Transform.h"
#include "Tasks/Tasks.h"
#include <algorithm>
#include <cmath>

using namespace Sailor;

namespace
{
	constexpr float c_gravity = 9.81f;
	constexpr float c_twoPi = 6.28318530718f;

	float AddOceanWave(
		const glm::vec2& position,
		glm::vec2 direction,
		float amplitude,
		float waveLength,
		float speed,
		float phaseOffset,
		float time)
	{
		direction = glm::normalize(direction);
		const float frequency = c_twoPi / std::max(waveLength, 0.01f);
		const float angularVelocity = std::sqrt(c_gravity * frequency) * speed;
		return amplitude * std::sin(
			frequency * glm::dot(direction, position) -
			angularVelocity * time + phaseOffset);
	}

	float SampleOceanHeight(
		const glm::vec2& position,
		const BuoyancyComponent& buoyancy,
		float time)
	{
		const glm::vec2 wind = glm::normalize(glm::vec2(0.94f, 0.34f));
		const glm::vec2 acrossWind(-wind.y, wind.x);
		glm::vec2 warpedPosition = position;
		warpedPosition += wind * std::sin(glm::dot(position, acrossWind) *
			0.075f + time * 0.08f) * 1.15f;
		warpedPosition += acrossWind * std::sin(glm::dot(position, wind) *
			0.052f - time * 0.055f) * 0.72f;
		const float waveGroup = 0.76f + 0.24f * std::sin(glm::dot(position,
			glm::normalize(glm::vec2(0.31f, 0.95f))) * 0.115f +
			time * 0.12f);
		const float amplitude = buoyancy.GetWaveAmplitude();
		const float waveLength = buoyancy.GetWaveLength();
		const float speed = buoyancy.GetWaveSpeed();

		float height = buoyancy.GetWaterHeight();
		height += AddOceanWave(warpedPosition, wind,
			amplitude * 0.72f * waveGroup, waveLength * 1.37f,
			speed * 0.84f, 0.37f, time);
		height += AddOceanWave(warpedPosition,
			glm::normalize(wind + acrossWind * 0.31f), amplitude * 0.38f,
			waveLength * 0.83f, speed * 0.98f, 2.11f, time);
		height += AddOceanWave(warpedPosition,
			glm::normalize(wind - acrossWind * 0.43f), amplitude * 0.27f,
			waveLength * 0.59f, speed * 1.09f, 4.73f, time);
		height += AddOceanWave(position,
			glm::normalize(wind + acrossWind * 0.72f), amplitude * 0.16f,
			waveLength * 0.41f, speed * 1.22f, 1.29f, time);
		height += AddOceanWave(position,
			glm::normalize(wind - acrossWind * 0.81f), amplitude * 0.10f,
			waveLength * 0.27f, speed * 1.38f, 5.62f, time);
		height += AddOceanWave(position, acrossWind, amplitude * 0.055f,
			waveLength * 0.18f, speed * 1.57f, 3.44f, time);
		return height;
	}
}

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

void PhysicsECS::ApplyBuoyancyForces(
	float sampleTime,
	float fixedDeltaTime)
{
	constexpr glm::vec2 sampleOffsets[] =
	{
		glm::vec2(-1.0f, -1.0f),
		glm::vec2(1.0f, -1.0f),
		glm::vec2(-1.0f, 1.0f),
		glm::vec2(1.0f, 1.0f),
		glm::vec2(0.0f, 0.0f)
	};
	constexpr float numSamples = static_cast<float>(
		std::size(sampleOffsets));

	for (auto& data : m_components)
	{
		if (!data.m_bIsActive ||
			data.m_motionType != Physics::ERigidBodyMotionType::Dynamic ||
			data.m_bodyId == RigidBodyData::InvalidBodyId)
		{
			continue;
		}

		auto gameObject = data.m_owner.StaticCast<GameObject>();
		auto buoyancy = gameObject
			? gameObject->GetComponent<BuoyancyComponent>()
			: TObjectPtr<BuoyancyComponent>{};
		auto rigidBody = gameObject
			? gameObject->GetComponent<RigidBodyComponent>()
			: TObjectPtr<RigidBodyComponent>{};
		if (!buoyancy || !rigidBody)
		{
			continue;
		}

		Physics::PhysicsBodyPose pose{};
		if (!m_physicsWorld->GetBodyPose(data.m_bodyId, pose))
		{
			continue;
		}

		const float massPerSample = rigidBody->GetMass() / numSamples;
		const glm::vec2 halfExtents = buoyancy->GetHalfExtents();
		for (const glm::vec2& offset : sampleOffsets)
		{
			const glm::vec3 localPoint(
				offset.x * halfExtents.x,
				buoyancy->GetFloatationPlane(),
				offset.y * halfExtents.y);
			const glm::vec3 worldPoint = pose.m_position +
				pose.m_rotation * localPoint;
			const glm::vec2 waterPosition(worldPoint.x, worldPoint.z);
			const float waterHeight = SampleOceanHeight(
				waterPosition,
				*buoyancy,
				sampleTime);
			const float submersion = waterHeight - worldPoint.y;
			if (submersion <= 0.0f)
			{
				continue;
			}

			const float previousWaterHeight = SampleOceanHeight(
				waterPosition,
				*buoyancy,
				sampleTime - fixedDeltaTime);
			const float waterVerticalVelocity =
				(waterHeight - previousWaterHeight) / fixedDeltaTime;
			const glm::vec3 leverArm = worldPoint - pose.m_position;
			const glm::vec3 pointVelocity = pose.m_linearVelocity +
				glm::cross(pose.m_angularVelocity, leverArm);
			const float immersion = std::clamp(submersion /
				buoyancy->GetEquilibriumDepth(), 0.0f, 2.5f);
			float lift = massPerSample * c_gravity *
				buoyancy->GetBuoyancyScale() * immersion;
			lift += massPerSample * buoyancy->GetVerticalDamping() *
				(waterVerticalVelocity - pointVelocity.y) *
				std::min(immersion, 1.0f);
			lift = std::clamp(lift, 0.0f,
				massPerSample * c_gravity * 3.0f);

			const glm::vec3 horizontalVelocity(
				pointVelocity.x,
				0.0f,
				pointVelocity.z);
			const glm::vec3 drag = -horizontalVelocity *
				massPerSample * buoyancy->GetWaterDrag() *
				std::min(immersion, 1.0f);
			m_physicsWorld->AddForceAtPosition(
				data.m_bodyId,
				drag + glm::vec3(0.0f, lift, 0.0f),
				worldPoint);
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

				const float sampleTime = GetWorld()->GetTime() -
					static_cast<float>(numSteps - step - 1) * m_fixedDeltaTime;
				ApplyBuoyancyForces(sampleTime, m_fixedDeltaTime);
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

bool PhysicsECS::AddForceAtPosition(
	size_t componentIndex,
	const glm::vec3& force,
	const glm::vec3& worldPosition)
{
	if (!m_physicsWorld || !IsComponentRegistered(componentIndex))
	{
		return false;
	}

	const auto& data = GetComponentData(componentIndex);
	return data.m_bodyId != RigidBodyData::InvalidBodyId &&
		m_physicsWorld->AddForceAtPosition(
			data.m_bodyId,
			force,
			worldPosition);
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
