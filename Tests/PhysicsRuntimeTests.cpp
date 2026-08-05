#include "Tasks/Tasks.h"
#include "Components/CollisionShapeComponent.h"
#include "Components/RigidBodyComponent.h"
#include "Core/Reflection.h"
#include "ECS/PhysicsECS.h"
#include "ECS/TransformECS.h"
#include "Engine/GameObject.h"
#include "Engine/World.h"
#include "Physics/JoltRuntime.h"
#include "Physics/PhysicsWorld.h"
#include "Math/Transform.h"

#include <cmath>
#include <functional>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <utility>

using namespace Sailor;

namespace
{
	constexpr float c_fixedDeltaTime = 1.0f / 60.0f;

	class PhysicsComponentTestWorld final : public World
	{
	public:
		PhysicsComponentTestWorld() :
			World("PhysicsComponentTests", 0, CreateEcs())
		{}

	private:
		static TVector<ECS::TBaseSystemPtr> CreateEcs()
		{
			TVector<ECS::TBaseSystemPtr> systems;
			systems.Add(TUniquePtr<TransformECS>::Make());
			systems.Add(TUniquePtr<PhysicsECS>::Make());
			return systems;
		}
	};

	void Require(bool condition, const std::string& message)
	{
		if (!condition)
		{
			throw std::runtime_error(message);
		}
	}

	bool IsNear(float lhs, float rhs, float tolerance = 0.001f)
	{
		return std::abs(lhs - rhs) <= tolerance;
	}

	Physics::RigidBodyDesc MakeBox(
		const InstanceId& instanceId,
		Physics::ERigidBodyMotionType motionType,
		const glm::vec3& position,
		const glm::vec3& size,
		uint8_t collisionLayer = 0)
	{
		Physics::RigidBodyDesc result{};
		result.m_instanceId = instanceId;
		result.m_motionType = motionType;
		result.m_position = position;
		result.m_collisionLayer = collisionLayer;

		Physics::CollisionShapeDesc shape{};
		shape.m_type = Physics::ECollisionShapeType::Box;
		shape.m_size = size;
		result.m_shapes.Add(shape);
		return result;
	}

	void Step(Physics::PhysicsWorld& world, uint32_t numSteps)
	{
		for (uint32_t step = 0; step < numSteps; ++step)
		{
			Require(
				world.Step(c_fixedDeltaTime),
				"fixed physics step should succeed");
		}
	}

	Physics::PhysicsBodyPose SimulateFallingBox()
	{
		Physics::PhysicsWorld world;
		uint32_t groundBody = ~0u;
		uint32_t dynamicBody = ~0u;
		Require(
			world.CreateBody(
				MakeBox(
					InstanceId::GenerateNewInstanceId(),
					Physics::ERigidBodyMotionType::Static,
					glm::vec3(0.0f, -0.5f, 0.0f),
					glm::vec3(20.0f, 1.0f, 20.0f)),
				groundBody),
			"static ground should be created");
		Require(
			world.CreateBody(
				MakeBox(
					InstanceId::GenerateNewInstanceId(),
					Physics::ERigidBodyMotionType::Dynamic,
					glm::vec3(0.0f, 4.0f, 0.0f),
					glm::vec3(1.0f)),
				dynamicBody),
			"dynamic box should be created");

		Step(world, 240);
		Physics::PhysicsBodyPose pose{};
		Require(
			world.GetBodyPose(dynamicBody, pose),
			"dynamic box pose should remain available");
		return pose;
	}

	void TestFixedStepGravityContactsAndRaycast()
	{
		Physics::PhysicsWorld world;
		const InstanceId groundId = InstanceId::GenerateNewInstanceId();
		const InstanceId dynamicId = InstanceId::GenerateNewInstanceId();
		uint32_t groundBody = ~0u;
		uint32_t dynamicBody = ~0u;
		Require(
			world.CreateBody(
				MakeBox(
					groundId,
					Physics::ERigidBodyMotionType::Static,
					glm::vec3(0.0f, -0.5f, 0.0f),
					glm::vec3(20.0f, 1.0f, 20.0f)),
				groundBody),
			"static ground should be created");
		Require(
			world.CreateBody(
				MakeBox(
					dynamicId,
					Physics::ERigidBodyMotionType::Dynamic,
					glm::vec3(0.0f, 4.0f, 0.0f),
					glm::vec3(1.0f)),
				dynamicBody),
			"dynamic box should be created");

		Step(world, 240);

		Physics::PhysicsBodyPose pose{};
		Require(
			world.GetBodyPose(dynamicBody, pose),
			"dynamic box pose should be readable");
		Require(
			pose.m_position.y > 0.45f && pose.m_position.y < 0.56f,
			"dynamic box should settle on top of the ground");

		TVector<Physics::PhysicsContactEvent> events;
		world.DrainContactEvents(events);
		Require(
			events.ContainsIf([&](const auto& event)
				{
					return event.m_type == Physics::EPhysicsContactType::Added &&
						((event.m_first == groundId && event.m_second == dynamicId) ||
							(event.m_first == dynamicId && event.m_second == groundId));
				}),
			"contact events should carry stable Sailor instance ids");

		Physics::PhysicsRaycastHit hit{};
		Require(
			world.Raycast(
				glm::vec3(0.0f, 5.0f, 0.0f),
				glm::vec3(0.0f, -1.0f, 0.0f),
				10.0f,
				hit),
			"raycast should hit the settled dynamic box");
		Require(
			hit.m_instanceId == dynamicId,
			"raycast should resolve the hit to its Sailor instance id");
		Require(
			hit.m_normal.y > 0.99f,
			"raycast should return a world-space surface normal");
	}

	void TestKinematicAuthority()
	{
		Physics::PhysicsWorld world;
		uint32_t bodyId = ~0u;
		Require(
			world.CreateBody(
				MakeBox(
					InstanceId::GenerateNewInstanceId(),
					Physics::ERigidBodyMotionType::Kinematic,
					glm::vec3(0.0f),
					glm::vec3(1.0f)),
				bodyId),
			"kinematic body should be created");
		Require(
			world.SetBodyTransform(
				bodyId,
				glm::vec3(3.0f, 2.0f, -1.0f),
				glm::quat(1.0f, 0.0f, 0.0f, 0.0f),
				true,
				c_fixedDeltaTime),
			"kinematic target should be accepted");
		Step(world, 1);

		Physics::PhysicsBodyPose pose{};
		Require(world.GetBodyPose(bodyId, pose), "kinematic pose should be readable");
		Require(
			IsNear(pose.m_position.x, 3.0f) &&
			IsNear(pose.m_position.y, 2.0f) &&
			IsNear(pose.m_position.z, -1.0f),
			"kinematic body should reach the authored target in one fixed step");
	}

	void TestCollisionLayersAndQueryMask()
	{
		Physics::PhysicsWorld world;
		Require(
			world.IsLayerCollisionEnabled(1, 2),
			"collision layers should interact by default");
		world.SetLayerCollisionEnabled(1, 2, false);
		Require(
			!world.IsLayerCollisionEnabled(1, 2) &&
				!world.IsLayerCollisionEnabled(2, 1),
			"collision layer matrix changes should be symmetric");

		const InstanceId firstId = InstanceId::GenerateNewInstanceId();
		const InstanceId secondId = InstanceId::GenerateNewInstanceId();
		uint32_t firstBody = ~0u;
		uint32_t secondBody = ~0u;
		Require(
			world.CreateBody(
				MakeBox(
					firstId,
					Physics::ERigidBodyMotionType::Static,
					glm::vec3(-2.0f, 0.0f, 0.0f),
					glm::vec3(1.0f),
					1),
				firstBody),
			"layer-one body should be created");
		Require(
			world.CreateBody(
				MakeBox(
					secondId,
					Physics::ERigidBodyMotionType::Static,
					glm::vec3(2.0f, 0.0f, 0.0f),
					glm::vec3(1.0f),
					2),
				secondBody),
			"layer-two body should be created");

		Physics::PhysicsRaycastHit hit{};
		Require(
			world.Raycast(
				glm::vec3(-2.0f, 3.0f, 0.0f),
				glm::vec3(0.0f, -1.0f, 0.0f),
				6.0f,
				hit,
				static_cast<uint16_t>(1u << 1)) &&
				hit.m_instanceId == firstId,
			"raycast mask should include its requested layer");
		Require(
			!world.Raycast(
				glm::vec3(-2.0f, 3.0f, 0.0f),
				glm::vec3(0.0f, -1.0f, 0.0f),
				6.0f,
				hit,
				static_cast<uint16_t>(1u << 2)),
			"raycast mask should exclude other layers");
	}

	void TestSensorEventsAndQueuedContactDestruction()
	{
		Physics::PhysicsWorld world;
		const InstanceId groundId = InstanceId::GenerateNewInstanceId();
		const InstanceId sensorId = InstanceId::GenerateNewInstanceId();
		uint32_t groundBody = ~0u;
		uint32_t sensorBody = ~0u;
		Require(
			world.CreateBody(
				MakeBox(
					groundId,
					Physics::ERigidBodyMotionType::Static,
					glm::vec3(0.0f, -0.5f, 0.0f),
					glm::vec3(10.0f, 1.0f, 10.0f)),
				groundBody),
			"sensor fixture ground should be created");
		auto sensor = MakeBox(
			sensorId,
			Physics::ERigidBodyMotionType::Dynamic,
			glm::vec3(0.0f, 2.0f, 0.0f),
			glm::vec3(1.0f));
		sensor.m_bSensor = true;
		Require(
			world.CreateBody(sensor, sensorBody),
			"sensor body should be created");

		Step(world, 120);
		world.DestroyBody(sensorBody);
		world.DestroyBody(sensorBody);

		TVector<Physics::PhysicsContactEvent> events;
		world.DrainContactEvents(events);
		Require(
			events.ContainsIf([&](const auto& event)
				{
					return event.m_bSensor &&
						event.m_type == Physics::EPhysicsContactType::Added &&
						((event.m_first == groundId && event.m_second == sensorId) ||
							(event.m_first == sensorId && event.m_second == groundId));
				}),
			"sensor overlap should be delivered as copied stable ids after the step");
		for (size_t index = 1; index < events.Num(); ++index)
		{
			const auto& previous = events[index - 1];
			const auto& current = events[index];
			const auto previousKey = std::make_pair(
				previous.m_first.ToString(),
				previous.m_second.ToString());
			const auto currentKey = std::make_pair(
				current.m_first.ToString(),
				current.m_second.ToString());
			Require(
				previousKey <= currentKey,
				"parallel contact callbacks should drain in stable pair order");
		}
	}

	void TestLifecycleAndInputValidation()
	{
		Physics::PhysicsWorld world;
		uint32_t bodyId = ~0u;
		Require(
			world.CreateBody(
				MakeBox(
					InstanceId::GenerateNewInstanceId(),
					Physics::ERigidBodyMotionType::Static,
					glm::vec3(0.0f),
					glm::vec3(1.0f)),
				bodyId),
			"body should be created for lifecycle validation");
		world.DestroyBody(bodyId);
		world.DestroyBody(bodyId);

		Physics::PhysicsBodyPose pose{};
		Require(
			!world.GetBodyPose(bodyId, pose),
			"destroyed body handle should no longer resolve");
		Require(!world.Step(0.0f), "zero-length step should be rejected");
		auto invalidBody = MakeBox(
			InstanceId::GenerateNewInstanceId(),
			Physics::ERigidBodyMotionType::Dynamic,
			glm::vec3(0.0f),
			glm::vec3(1.0f));
		invalidBody.m_gravityFactor =
			std::numeric_limits<float>::quiet_NaN();
		uint32_t invalidBodyId = ~0u;
		Require(
			!world.CreateBody(invalidBody, invalidBodyId),
			"non-finite body settings should be rejected");
		Physics::PhysicsRaycastHit hit{};
		Require(
			!world.Raycast(
				glm::vec3(0.0f),
				glm::vec3(0.0f),
				1.0f,
				hit),
			"zero-length ray direction should be rejected");
		world.Clear();
		world.Clear();
	}

	void TestSameBuildRepeatability()
	{
		const Physics::PhysicsBodyPose first = SimulateFallingBox();
		const Physics::PhysicsBodyPose second = SimulateFallingBox();
		Require(
			IsNear(first.m_position.x, second.m_position.x, 0.000001f) &&
			IsNear(first.m_position.y, second.m_position.y, 0.000001f) &&
			IsNear(first.m_position.z, second.m_position.z, 0.000001f) &&
			IsNear(first.m_linearVelocity.x, second.m_linearVelocity.x, 0.000001f) &&
			IsNear(first.m_linearVelocity.y, second.m_linearVelocity.y, 0.000001f) &&
			IsNear(first.m_linearVelocity.z, second.m_linearVelocity.z, 0.000001f),
			"identical fixed-step runs should repeat within the same build");
	}

	void TestWorldPoseToLocalForTransformedParent()
	{
		const Math::Transform parent(
			glm::vec4(4.0f, -2.0f, 7.0f, 1.0f),
			glm::angleAxis(glm::radians(35.0f), glm::normalize(glm::vec3(0.0f, 1.0f, 1.0f))),
			glm::vec4(2.0f));
		const glm::vec3 expectedLocalPosition(-3.0f, 1.5f, 2.0f);
		const glm::quat expectedLocalRotation = glm::angleAxis(
			glm::radians(-24.0f),
			glm::normalize(glm::vec3(1.0f, 0.5f, 0.25f)));
		const glm::vec3 worldPosition = glm::vec3(
			parent.Matrix() * glm::vec4(expectedLocalPosition, 1.0f));
		const glm::quat worldRotation = glm::normalize(
			parent.m_rotation * expectedLocalRotation);

		glm::vec3 localPosition{};
		glm::quat localRotation{};
		Require(
			Physics::TryConvertWorldPoseToLocal(
				parent.Matrix(),
				worldPosition,
				worldRotation,
				localPosition,
				localRotation),
			"dynamic child pose should convert through a transformed parent");
		Require(
			glm::length(localPosition - expectedLocalPosition) <= 0.00001f,
			"dynamic child local position should preserve its physics world position");
		Require(
			std::abs(glm::dot(localRotation, expectedLocalRotation)) >= 0.99999f,
			"dynamic child local rotation should preserve its physics world rotation");

		glm::mat4 singularParent = parent.Matrix();
		singularParent[0] = glm::vec4(0.0f);
		Require(
			!Physics::TryConvertWorldPoseToLocal(
				singularParent,
				worldPosition,
				worldRotation,
				localPosition,
				localRotation),
			"singular parent transforms should be rejected safely");
	}

	void TestReflectedPhysicsAuthoringContract()
	{
		const TypeInfo& rigidBodyType = TypeInfo::Get<RigidBodyComponent>();
		Require(
			rigidBodyType.Name() == "Sailor::RigidBodyComponent" &&
				rigidBodyType.Base() == "Sailor::Component",
			"rigid body should remain a reflected engine component");
		Require(
			rigidBodyType.Properties()["motionType"] ==
				"enum Sailor::ERigidBodyMotionType" &&
				rigidBodyType.Properties()["collisionLayer"] == "uint32",
			"rigid body authoring fields should export Editor-compatible types");
		Require(
			rigidBodyType.PropertyRanges()["collisionLayer"].m_min == 0.0 &&
				rigidBodyType.PropertyRanges()["collisionLayer"].m_max == 15.0,
			"collision layer should export the supported layer range");

		const TypeInfo& shapeType = TypeInfo::Get<CollisionShapeComponent>();
		Require(
			shapeType.Name() == "Sailor::CollisionShapeComponent" &&
				shapeType.Properties()["shapeType"] ==
					"enum Sailor::ECollisionShapeType" &&
				!shapeType.Properties()["center"].empty(),
			"collision shape should expose typed primitive authoring fields: " +
				shapeType.Properties()["shapeType"] + ", " +
				shapeType.Properties()["center"]);
	}

	void TestComponentTeardownOrdering()
	{
		PhysicsComponentTestWorld world;
		auto owner = world.Instantiate("Physics owner");
		owner->AddComponent<RigidBodyComponent>();
		auto shape = owner->AddComponent<CollisionShapeComponent>();
		Require(
			owner->RemoveComponent(shape),
			"an individual collision shape should be removable safely");

		owner->AddComponent<CollisionShapeComponent>();
		owner->RemoveAllComponents();
		Require(
			owner->GetComponents().IsEmpty(),
			"full object teardown should not access a destroyed rigid body");
		world.Clear();
	}
}

int main()
{
	Physics::JoltRuntime runtime;
	const std::pair<const char*, std::function<void()>> tests[] = {
		{ "FixedStepGravityContactsAndRaycast", TestFixedStepGravityContactsAndRaycast },
		{ "KinematicAuthority", TestKinematicAuthority },
		{ "CollisionLayersAndQueryMask", TestCollisionLayersAndQueryMask },
		{ "SensorEventsAndQueuedContactDestruction", TestSensorEventsAndQueuedContactDestruction },
		{ "LifecycleAndInputValidation", TestLifecycleAndInputValidation },
		{ "SameBuildRepeatability", TestSameBuildRepeatability },
		{ "WorldPoseToLocalForTransformedParent", TestWorldPoseToLocalForTransformedParent },
		{ "ReflectedPhysicsAuthoringContract", TestReflectedPhysicsAuthoringContract },
		{ "ComponentTeardownOrdering", TestComponentTeardownOrdering },
	};

	for (const auto& test : tests)
	{
		try
		{
			test.second();
			std::cout << "[PASS] " << test.first << std::endl;
		}
		catch (const std::exception& error)
		{
			std::cerr << "[FAIL] " << test.first << ": " << error.what() << std::endl;
			return 1;
		}
	}

	return 0;
}
