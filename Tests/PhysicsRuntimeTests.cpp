#include "Tasks/Tasks.h"
#include "Components/CollisionShapeComponent.h"
#include "Components/BuoyancyComponent.h"
#include "Components/RigidBodyComponent.h"
#include "Components/LandscapeComponent.h"
#include "Core/Reflection.h"
#include "ECS/PhysicsECS.h"
#include "ECS/TransformECS.h"
#include "Engine/GameObject.h"
#include "Engine/World.h"
#include "Physics/JoltRuntime.h"
#include "Physics/PhysicsWorld.h"
#include "Math/Transform.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <functional>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <thread>
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

		PhysicsComponentTestWorld(TUniquePtr<Physics::PhysicsWorld> physicsWorld, Tasks::Scheduler& scheduler) :
			World("PhysicsComponentTests", 0, CreateEcs(std::move(physicsWorld), &scheduler))
		{
			SetPhysicsSimulationEnabled(true);
		}

		~PhysicsComponentTestWorld() override { Clear(); }

		Tasks::ITaskPtr TickPhysics(float deltaTime)
		{
			++m_currentFrame;
			auto* transforms = GetECS<TransformECS>();
			transforms->Tick(0.0f);
			transforms->PostTick();
			auto task = GetECS<PhysicsECS>()->Tick(deltaTime);
			transforms->PostTick();
			return task;
		}

	private:
		static TVector<ECS::TBaseSystemPtr> CreateEcs(
			TUniquePtr<Physics::PhysicsWorld> physicsWorld = {}, Tasks::Scheduler* scheduler = nullptr)
		{
			TVector<ECS::TBaseSystemPtr> systems;
			systems.Add(TUniquePtr<TransformECS>::Make());
			if (scheduler)
			{
				systems.Add(TUniquePtr<PhysicsECS>::Make(std::move(physicsWorld), *scheduler));
			}
			else
			{
				systems.Add(TUniquePtr<PhysicsECS>::Make());
			}
			systems.Add(TUniquePtr<LandscapeECS>::Make());
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

	bool IsNear(const glm::vec3& lhs, const glm::vec3& rhs, float tolerance = 0.00001f)
	{
		return glm::length(lhs - rhs) <= tolerance;
	}

	bool WaitUntil(const std::function<bool()>& condition)
	{
		const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
		while (!condition())
		{
			if (std::chrono::steady_clock::now() >= deadline)
			{
				return false;
			}
			std::this_thread::yield();
		}
		return true;
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

	Physics::RigidBodyDesc MakeSphere(
		const InstanceId& instanceId,
		Physics::ERigidBodyMotionType motionType,
		const glm::vec3& position,
		float radius,
		const glm::vec3& scale = glm::vec3(1.0f))
	{
		Physics::RigidBodyDesc result{};
		result.m_instanceId = instanceId;
		result.m_motionType = motionType;
		result.m_position = position;
		result.m_scale = scale;

		Physics::CollisionShapeDesc shape{};
		shape.m_type = Physics::ECollisionShapeType::Sphere;
		shape.m_radius = radius;
		result.m_shapes.Add(shape);
		return result;
	}

	Physics::RigidBodyDesc MakeTriangleMesh(
		const InstanceId& instanceId,
		const glm::vec3& position,
		const glm::vec3& scale = glm::vec3(1.0f))
	{
		Physics::RigidBodyDesc result{};
		result.m_instanceId = instanceId;
		result.m_motionType = Physics::ERigidBodyMotionType::Static;
		result.m_position = position;
		result.m_scale = scale;

		Physics::CollisionShapeDesc shape{};
		shape.m_type = Physics::ECollisionShapeType::TriangleMesh;
		shape.m_vertices.AddRange({
			glm::vec3(-5.0f, 0.0f, -5.0f),
			glm::vec3(5.0f, 0.0f, -5.0f),
			glm::vec3(-5.0f, 0.0f, 5.0f),
			glm::vec3(5.0f, 0.0f, 5.0f) });
		shape.m_indices.AddRange({ 0u, 2u, 1u, 1u, 2u, 3u });
		result.m_shapes.Add(std::move(shape));
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

	void TestStaticTriangleMeshCollisionAndRaycast()
	{
		Physics::PhysicsWorld world;
		const InstanceId landscapeId = InstanceId::GenerateNewInstanceId();
		uint32_t landscapeBody = ~0u;
		uint32_t dynamicBody = ~0u;
		Require(
			world.CreateBody(
				MakeTriangleMesh(
					landscapeId,
					glm::vec3(0.0f, 1.25f, 0.0f),
					glm::vec3(2.0f, 1.0f, 2.0f)),
				landscapeBody),
			"static landscape triangle mesh should be created");
		Require(
			world.CreateBody(
				MakeSphere(
					InstanceId::GenerateNewInstanceId(),
					Physics::ERigidBodyMotionType::Dynamic,
					glm::vec3(0.0f, 4.0f, 0.0f),
					0.5f),
				dynamicBody),
			"dynamic sphere above landscape should be created");

		Step(world, 240u);
		Physics::PhysicsBodyPose pose{};
		Require(
			world.GetBodyPose(dynamicBody, pose),
			"dynamic sphere pose above triangle mesh should be readable");
		Require(
			pose.m_position.y > 1.70f && pose.m_position.y < 1.80f,
			"dynamic sphere should settle on the scaled landscape mesh");

		world.DestroyBody(dynamicBody);
		Physics::PhysicsRaycastHit hit{};
		Require(
			world.Raycast(
				glm::vec3(0.0f, 5.0f, 0.0f),
				glm::vec3(0.0f, -1.0f, 0.0f),
				10.0f,
				hit),
			"raycast should hit the landscape triangle mesh");
		Require(
			hit.m_instanceId == landscapeId && hit.m_normal.y > 0.99f,
			"landscape raycast should resolve its owner and upward normal");
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

	void TestScaledSphereVolume()
	{
		Physics::PhysicsWorld world;
		const InstanceId sphereId = InstanceId::GenerateNewInstanceId();
		uint32_t bodyId = ~0u;
		Require(
			world.CreateBody(
				MakeSphere(
					sphereId,
					Physics::ERigidBodyMotionType::Static,
					glm::vec3(0.0f),
					0.5f,
					glm::vec3(20.0f)),
				bodyId),
			"scaled sphere body should be created");

		Physics::PhysicsRaycastHit verticalHit{};
		Require(
			world.Raycast(
				glm::vec3(0.0f, 30.0f, 0.0f),
				glm::vec3(0.0f, -1.0f, 0.0f),
				60.0f,
				verticalHit) &&
				verticalHit.m_instanceId == sphereId &&
				IsNear(verticalHit.m_position.y, 10.0f) &&
				verticalHit.m_normal.y > 0.99f,
			"radius 0.5 sphere at scale 20 should have world radius 10 on Y");

		Physics::PhysicsRaycastHit horizontalHit{};
		Require(
			world.Raycast(
				glm::vec3(30.0f, 0.0f, 0.0f),
				glm::vec3(-1.0f, 0.0f, 0.0f),
				60.0f,
				horizontalHit) &&
				IsNear(horizontalHit.m_position.x, 10.0f) &&
				horizontalHit.m_normal.x > 0.99f,
			"scaled sphere collision volume should remain spherical on X");

		Physics::PhysicsWorld settlingWorld;
		auto ground = MakeBox(
			InstanceId::GenerateNewInstanceId(),
			Physics::ERigidBodyMotionType::Static,
			glm::vec3(0.0f, -10.0f, 0.0f),
			glm::vec3(1.0f));
		ground.m_scale = glm::vec3(100.0f, 10.0f, 100.0f);
		uint32_t groundBodyId = ~0u;
		Require(
			settlingWorld.CreateBody(ground, groundBodyId),
			"fixture-sized ground should be created");
		auto dynamicSphere = MakeSphere(
			InstanceId::GenerateNewInstanceId(),
			Physics::ERigidBodyMotionType::Dynamic,
			glm::vec3(0.0f, 50.0f, 0.0f),
			0.5f,
			glm::vec3(20.0f));
		dynamicSphere.m_gravityFactor = 10.0f;
		uint32_t dynamicSphereId = ~0u;
		Require(
			settlingWorld.CreateBody(
				dynamicSphere,
				dynamicSphereId),
			"dynamic fixture sphere should be created");
		Step(settlingWorld, 240);
		Physics::PhysicsBodyPose settledPose{};
		Require(
			settlingWorld.GetBodyPose(dynamicSphereId, settledPose) &&
				IsNear(settledPose.m_position.y, 5.0f, 0.1f),
			"radius-ten sphere should settle on the visible ground at center Y=5");
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

	void TestThinScaledBoxPreservesCollisionExtent()
	{
		Physics::PhysicsWorld world;
		for (const glm::vec3 scale : { glm::vec3(1.0f), glm::vec3(0.1f, -0.5f, 2.0f) })
		{
			auto desc = MakeBox(
				InstanceId::GenerateNewInstanceId(),
				Physics::ERigidBodyMotionType::Static,
				glm::vec3(0.0f),
				glm::vec3(2.0f, 0.01f, 3.0f));
			desc.m_scale = scale;
			uint32_t body = ~0u;
			Require(world.CreateBody(desc, body), "thin scaled boxes should be valid physics shapes");
			Physics::PhysicsRaycastHit hit{};
			Require(world.Raycast(glm::vec3(0.0f, 1.0f, 0.0f), glm::vec3(0.0f, -1.0f, 0.0f), 2.0f, hit),
				"a ray should hit the thin box");
			Require(IsNear(hit.m_position.y, 0.005f * std::abs(scale.y), 0.0001f),
				"limiting the convex radius must preserve the authored outer extent");
			world.DestroyBody(body);
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

		const TypeInfo& buoyancyType = TypeInfo::Get<BuoyancyComponent>();
		Require(
			buoyancyType.Name() == "Sailor::BuoyancyComponent" &&
				!buoyancyType.Properties()["halfExtents"].empty() &&
				buoyancyType.Properties()["waveAmplitude"] == "float",
			"buoyancy should expose Editor-compatible hull and wave fields: " +
				buoyancyType.Properties()["halfExtents"] + ", " +
				buoyancyType.Properties()["waveAmplitude"]);
	}

	void TestInitialVelocityAuthoringStaysSeparateFromRuntimeCommands()
	{
		RigidBodyComponent authoring;
		const glm::vec3 initialLinear(2.0f, 3.0f, 4.0f);
		const glm::vec3 initialAngular(0.0f, 1.0f, 2.0f);
		authoring.SetInitialLinearVelocity(initialLinear);
		authoring.SetInitialAngularVelocity(initialAngular);
		const ReflectedData before = authoring.GetReflectedData();
		Require(before.GetProperties()["linearVelocity"].as<glm::vec3>() == initialLinear &&
			before.GetProperties()["angularVelocity"].as<glm::vec3>() == initialAngular,
			"initial velocities must retain their existing reflected property names");

		authoring.SetLinearVelocity(glm::vec3(9.0f));
		authoring.SetAngularVelocity(glm::vec3(8.0f));
		Require(authoring.GetLinearVelocity() == glm::vec3(0.0f) &&
			authoring.GetAngularVelocity() == glm::vec3(0.0f) && authoring.GetReflectedData() == before,
			"unregistered runtime commands must be no-ops, not edits to initial authoring values");
		RigidBodyComponent restored;
		restored.ApplyReflection(before);
		Require(restored.GetInitialLinearVelocity() == initialLinear &&
			restored.GetInitialAngularVelocity() == initialAngular &&
			restored.GetLinearVelocity() == glm::vec3(0.0f),
			"reflection must restore initial values without fabricating a live physics velocity");
	}

	void TestPendingVelocityCommandsAndLiveBodyReconstruction()
	{
		Tasks::Scheduler scheduler;
		scheduler.Initialize();
		auto backend = TUniquePtr<Physics::PhysicsWorld>::Make(scheduler);
		auto* physicsWorld = backend.GetRawPtr();
		PhysicsComponentTestWorld world(std::move(backend), scheduler);
		auto owner = world.Instantiate("Pending velocity owner");
		auto body = owner->AddComponent<RigidBodyComponent>();
		body->SetInitialLinearVelocity(glm::vec3(2.0f, 0.0f, 0.0f));
		body->SetInitialAngularVelocity(glm::vec3(0.0f, 1.0f, 0.0f));
		const ReflectedData authored = body->GetReflectedData();
		body->SetLinearVelocity(glm::vec3(3.0f, 0.0f, 0.0f));
		body->SetLinearVelocity(glm::vec3(4.0f, 0.0f, 0.0f));
		auto* physics = world.GetECS<PhysicsECS>();
		world.TickPhysics(0.0f);
		Require(physics->GetComponentData(body->GetComponentIndex()).m_bodyId == RigidBodyData::InvalidBodyId &&
			body->GetLinearVelocity() == glm::vec3(0.0f) && body->GetAngularVelocity() == glm::vec3(0.0f),
			"commands must remain pending while a body cannot be created without a collision shape");

		auto shape = owner->AddComponent<CollisionShapeComponent>();
		world.TickPhysics(0.0f);
		auto readPose = [&]()
			{
				Physics::PhysicsBodyPose pose;
				Require(physicsWorld->GetBodyPose(physics->GetComponentData(body->GetComponentIndex()).m_bodyId, pose),
					"the component must own a live Jolt body");
				Require(IsNear(body->GetLinearVelocity(), pose.m_linearVelocity) &&
					IsNear(body->GetAngularVelocity(), pose.m_angularVelocity),
					"component velocity queries must match Jolt even when no fixed step ran");
				return pose;
			};
		auto pose = readPose();
		Require(IsNear(pose.m_linearVelocity, glm::vec3(4.0f, 0.0f, 0.0f)) &&
			IsNear(pose.m_angularVelocity, glm::vec3(0.0f, 1.0f, 0.0f)) && body->GetReflectedData() == authored,
			"creation must apply the latest command while keeping the uncommanded initial axis and serialized values");

		world.SetPhysicsSimulationEnabled(false);
		body->SetAngularVelocity(glm::vec3(0.0f, 0.0f, 3.0f));
		world.TickPhysics(0.0f);
		Require(body->GetAngularVelocity() == glm::vec3(0.0f, 1.0f, 0.0f) && body->GetReflectedData() == authored,
			"paused simulation must retain a runtime command without serializing it or reporting it as applied");
		world.SetPhysicsSimulationEnabled(true);
		world.TickPhysics(0.0f);
		pose = readPose();
		Require(IsNear(pose.m_angularVelocity, glm::vec3(0.0f, 0.0f, 3.0f)) &&
			IsNear(pose.m_linearVelocity, glm::vec3(4.0f, 0.0f, 0.0f)),
			"resuming must apply only the commanded angular velocity");

		const uint32_t originalBodyId = physics->GetComponentData(body->GetComponentIndex()).m_bodyId;
		body->SetInitialLinearVelocity(glm::vec3(9.0f, 0.0f, 0.0f));
		world.TickPhysics(0.0f);
		Require(physics->GetComponentData(body->GetComponentIndex()).m_bodyId == originalBodyId &&
			body->GetLinearVelocity() == glm::vec3(4.0f, 0.0f, 0.0f),
			"editing initial velocity must not rebuild or command the live body");
		body->SetMass(2.0f);
		world.TickPhysics(0.0f);
		const uint32_t massBodyId = physics->GetComponentData(body->GetComponentIndex()).m_bodyId;
		pose = readPose();
		Require(massBodyId != originalBodyId && IsNear(pose.m_linearVelocity, glm::vec3(4.0f, 0.0f, 0.0f)) &&
			IsNear(pose.m_angularVelocity, glm::vec3(0.0f, 0.0f, 3.0f)),
			"mass reconstruction must retain live velocities rather than resetting to initial values");
		shape->SetSize(glm::vec3(2.0f));
		body->SetLinearVelocity(glm::vec3(0.0f));
		world.TickPhysics(0.0f);
		pose = readPose();
		Require(physics->GetComponentData(body->GetComponentIndex()).m_bodyId != massBodyId &&
			IsNear(pose.m_linearVelocity, glm::vec3(0.0f)) && IsNear(pose.m_angularVelocity, glm::vec3(0.0f, 0.0f, 3.0f)),
			"shape reconstruction must overlay a pending stop command without resetting the other live axis");

		auto freshOwner = world.Instantiate("Restored initial velocity owner");
		auto freshBody = freshOwner->AddComponent<RigidBodyComponent>();
		freshBody->ApplyReflection(body->GetReflectedData());
		Require(freshBody->GetInitialLinearVelocity() == glm::vec3(9.0f, 0.0f, 0.0f) &&
			freshBody->GetInitialAngularVelocity() == glm::vec3(0.0f, 1.0f, 0.0f),
			"restoring authoring must not copy the original body's runtime velocities");
		freshBody->SetAngularVelocity(glm::vec3(0.0f));
		freshOwner->AddComponent<CollisionShapeComponent>();
		world.TickPhysics(0.0f);
		Require(IsNear(freshBody->GetLinearVelocity(), glm::vec3(9.0f, 0.0f, 0.0f)) &&
			IsNear(freshBody->GetAngularVelocity(), glm::vec3(0.0f)),
			"a pending angular command must override only that axis when a restored body is first created");
	}

	void TestVelocityQueriesAndRepeatedStopAfterAcceleration()
	{
		Tasks::Scheduler scheduler;
		scheduler.Initialize();
		auto backend = TUniquePtr<Physics::PhysicsWorld>::Make(scheduler);
		auto* physicsWorld = backend.GetRawPtr();
		PhysicsComponentTestWorld world(std::move(backend), scheduler);
		auto owner = world.Instantiate("Accelerated velocity owner");
		auto body = owner->AddComponent<RigidBodyComponent>();
		body->SetLinearDamping(0.0f);
		body->SetAngularDamping(0.0f);
		owner->AddComponent<CollisionShapeComponent>();
		world.TickPhysics(0.0f);
		const uint32_t bodyId = world.GetECS<PhysicsECS>()->GetComponentData(body->GetComponentIndex()).m_bodyId;
		const ReflectedData authored = body->GetReflectedData();
		auto readPose = [&]()
			{
				Physics::PhysicsBodyPose pose;
				Require(physicsWorld->GetBodyPose(bodyId, pose) &&
					IsNear(body->GetLinearVelocity(), pose.m_linearVelocity) &&
					IsNear(body->GetAngularVelocity(), pose.m_angularVelocity),
					"velocity getters must reflect the real body's latest synchronized state");
				return pose;
			};
		for (uint32_t repetition = 0; repetition < 2; ++repetition)
		{
			const auto step = world.TickPhysics(c_fixedDeltaTime);
			Require(step && step->IsFinished() && step->GetThreadType() == EThreadType::Physics &&
				readPose().m_linearVelocity.y < -0.1f,
				"a real fixed step must accelerate the initially stationary body under gravity");
			body->SetLinearVelocity(glm::vec3(0.0f));
			Require(!world.TickPhysics(0.0f) && IsNear(readPose().m_linearVelocity, glm::vec3(0.0f)),
				"SetLinearVelocity(0) must stop a freshly accelerated body on every call, without a simulation substep");
		}

		for (uint32_t repetition = 0; repetition < 2; ++repetition)
		{
			const auto beforeForce = readPose();
			Require(body->AddForceAtPosition(glm::vec3(12.0f, 0.0f, 0.0f),
				beforeForce.m_position + glm::vec3(0.0f, 1.0f, 0.0f)), "off-center force must reach the live body");
			world.TickPhysics(c_fixedDeltaTime);
			const auto accelerated = readPose();
			Require(accelerated.m_linearVelocity.x > 0.1f && std::abs(accelerated.m_angularVelocity.z) > 0.1f,
				"an off-center force must produce both linear and angular velocity");
			body->SetLinearVelocity(glm::vec3(0.0f));
			world.TickPhysics(0.0f);
			Require(IsNear(readPose().m_linearVelocity, glm::vec3(0.0f)) &&
				IsNear(body->GetAngularVelocity(), accelerated.m_angularVelocity),
				"a linear stop command must preserve force-generated angular velocity");
			body->SetLinearVelocity(glm::vec3(3.0f, 0.0f, 0.0f));
			world.TickPhysics(0.0f);
			body->SetAngularVelocity(glm::vec3(0.0f));
			world.TickPhysics(0.0f);
			Require(IsNear(readPose().m_angularVelocity, glm::vec3(0.0f)) &&
				IsNear(body->GetLinearVelocity(), glm::vec3(3.0f, 0.0f, 0.0f)),
				"a repeated angular stop command must preserve the uncommanded linear velocity");
		}
		Require(body->GetInitialLinearVelocity() == glm::vec3(0.0f) &&
			body->GetInitialAngularVelocity() == glm::vec3(0.0f) && body->GetReflectedData() == authored,
			"gravity, force and runtime velocity commands must never be written back into serialized authoring");
	}

	void TestKinematicVelocityQueriesFollowAuthoredTargets()
	{
		Tasks::Scheduler scheduler;
		scheduler.Initialize();
		auto backend = TUniquePtr<Physics::PhysicsWorld>::Make(scheduler);
		auto* physicsWorld = backend.GetRawPtr();
		PhysicsComponentTestWorld world(std::move(backend), scheduler);
		auto owner = world.Instantiate("Kinematic velocity owner");
		auto body = owner->AddComponent<RigidBodyComponent>();
		body->SetMotionType(Physics::ERigidBodyMotionType::Kinematic);
		owner->AddComponent<CollisionShapeComponent>();
		world.TickPhysics(0.0f);
		const auto& data = world.GetECS<PhysicsECS>()->GetComponentData(body->GetComponentIndex());
		const ReflectedData authored = body->GetReflectedData();
		auto readPose = [&]()
			{
				Physics::PhysicsBodyPose pose;
				Require(physicsWorld->GetBodyPose(data.m_bodyId, pose) &&
					IsNear(body->GetLinearVelocity(), pose.m_linearVelocity) &&
					IsNear(body->GetAngularVelocity(), pose.m_angularVelocity),
					"kinematic velocity queries must match Jolt after authored moves and fixed steps");
				Require(IsNear(data.m_currentPose.m_position, pose.m_position) &&
					std::abs(glm::dot(data.m_currentPose.m_rotation, pose.m_rotation)) > 0.99999f,
					"the synchronized kinematic pose must follow the body without dynamic interpolation");
				return pose;
			};

		const glm::vec3 targetPosition(1.0f, 0.5f, -0.25f);
		const glm::quat targetRotation = glm::angleAxis(0.25f, glm::vec3(0.0f, 1.0f, 0.0f));
		owner->GetTransformComponent().SetPosition(targetPosition);
		owner->GetTransformComponent().SetRotation(targetRotation);
		Require(!world.TickPhysics(0.0f), "an authored kinematic target must not require a fixed step to publish its velocity");
		auto pose = readPose();
		Require(glm::length(pose.m_linearVelocity) > 0.1f && glm::length(pose.m_angularVelocity) > 0.1f &&
			IsNear(pose.m_position, glm::vec3(0.0f)),
			"MoveKinematic must derive both velocities before the body advances toward its target");
		const auto step = world.TickPhysics(c_fixedDeltaTime);
		Require(step && step->IsFinished(), "the kinematic fixed step must complete");
		pose = readPose();
		Require(IsNear(pose.m_position, targetPosition, 0.001f) &&
			std::abs(glm::dot(pose.m_rotation, targetRotation)) > 0.99999f,
			"the real Jolt step must reach the authored position and rotation");

		world.TickPhysics(0.0f);
		pose = readPose();
		Require(IsNear(pose.m_linearVelocity, glm::vec3(0.0f), 0.001f) &&
			IsNear(pose.m_angularVelocity, glm::vec3(0.0f), 0.001f),
			"an unchanged reached target must publish stopped kinematic velocities without explicit commands");
		world.TickPhysics(c_fixedDeltaTime);
		pose = readPose();
		Require(IsNear(pose.m_linearVelocity, glm::vec3(0.0f), 0.001f) &&
			IsNear(pose.m_angularVelocity, glm::vec3(0.0f), 0.001f) && body->GetReflectedData() == authored,
			"a settled kinematic body must retain live query parity without rewriting initial authoring");
	}

	void TestPhysicsWorldQueuesAndDrainsJoltJobs()
	{
		std::atomic<uint32_t> workersStarted = 0;
		std::atomic<bool> releaseWorkers = false;
		Tasks::Scheduler scheduler;
		scheduler.Initialize();
		Physics::PhysicsWorld world(scheduler);
		uint32_t firstBody = ~0u;
		for (uint32_t index = 0; index < 64u; ++index)
		{
			uint32_t bodyId = ~0u;
			Require(world.CreateBody(MakeBox(InstanceId::GenerateNewInstanceId(),
				Physics::ERigidBodyMotionType::Dynamic, glm::vec3(3.0f * index, 4.0f, 0.0f), glm::vec3(1.0f)), bodyId),
				"the scheduled Jolt fixture must create real dynamic bodies");
			if (index == 0u)
			{
				firstBody = bodyId;
			}
		}

		const uint32_t workerCount = scheduler.GetNumWorkerThreads();
		TVector<Tasks::ITaskPtr> blockers;
		for (uint32_t index = 0; index < workerCount; ++index)
		{
			auto blocker = Tasks::CreateTask(scheduler, "Hold worker before Jolt step", [&]()
				{
					workersStarted.fetch_add(1, std::memory_order_release);
					releaseWorkers.wait(false, std::memory_order_acquire);
				});
			blocker->Run();
			blockers.Add(blocker);
		}
		const bool allWorkersBlocked = WaitUntil([&]() { return workersStarted.load() == workerCount; });
		const bool queueInitiallyEmpty = scheduler.GetNumTasks(EThreadType::Worker) == 0u;
		Tasks::TaskPtr<bool> step;
		bool queuedJoltJobs = false;
		bool stepWaitingForJobs = false;
		if (allWorkersBlocked)
		{
			step = Tasks::CreateTask<bool>(scheduler, "Step real Jolt world", [&]()
				{
					return world.Step(c_fixedDeltaTime);
				}, EThreadType::Physics);
			step->Run();
			queuedJoltJobs = WaitUntil([&]() { return scheduler.GetNumTasks(EThreadType::Worker) > 0u; });
			stepWaitingForJobs = !step->IsFinished();
		}

		// Release and join before assertions so a failed observation cannot strand workers.
		releaseWorkers.store(true, std::memory_order_release);
		releaseWorkers.notify_all();
		for (auto& blocker : blockers)
		{
			blocker->Wait();
		}
		if (step)
		{
			step->Wait();
		}
		Require(allWorkersBlocked && queueInitiallyEmpty && queuedJoltJobs && stepWaitingForJobs,
			"the real physics step must enqueue Jolt work and wait while its Worker queue is held");
		Require(step->GetResult() && scheduler.GetNumTasks(EThreadType::Worker) == 0u,
			"the step must finish successfully only after its queued Jolt work has drained");
		Physics::PhysicsBodyPose pose;
		Require(world.GetBodyPose(firstBody, pose) && pose.m_linearVelocity.y < -0.1f,
			"the scheduled Jolt step must actually advance the body under gravity");
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

	void TestBulkPhysicsWorldClearAndReuse()
	{
		for (uint32_t count : { 32u, 64u })
		{
			Physics::PhysicsWorld world;
			TVector<uint32_t> bodies;
			for (uint32_t index = 0; index < count; ++index)
			{
				uint32_t body = ~0u;
				Require(world.CreateBody(MakeBox(InstanceId::GenerateNewInstanceId(),
					Physics::ERigidBodyMotionType::Static, glm::vec3(3.0f * index, 0.0f, 0.0f), glm::vec3(1.0f)), body),
					"each body must be created in the actual Jolt world");
				bodies.Add(body);
				Physics::PhysicsRaycastHit hit;
				Require(world.Raycast(glm::vec3(3.0f * index, 2.0f, 0.0f), glm::vec3(0.0f, -1.0f, 0.0f), 4.0f, hit),
					"the live Jolt body must be queryable before bulk Clear");
			}
			world.Clear();
			world.Clear();
			for (uint32_t index = 0; index < count; ++index)
			{
				Physics::PhysicsBodyPose pose;
				Physics::PhysicsRaycastHit hit;
				Require(!world.GetBodyPose(bodies[index], pose) &&
					!world.Raycast(glm::vec3(3.0f * index, 2.0f, 0.0f), glm::vec3(0.0f, -1.0f, 0.0f), 4.0f, hit),
					"bulk Clear must release every body and remove all query geometry");
			}
			const InstanceId replacementId = InstanceId::GenerateNewInstanceId();
			uint32_t replacement = ~0u;
			Require(world.CreateBody(MakeBox(replacementId, Physics::ERigidBodyMotionType::Static,
				glm::vec3(0.0f), glm::vec3(1.0f)), replacement), "the cleared Jolt world must accept a new body");
			Physics::PhysicsRaycastHit hit;
			Require(world.Raycast(glm::vec3(0.0f, 2.0f, 0.0f), glm::vec3(0.0f, -1.0f, 0.0f), 4.0f, hit) &&
				hit.m_instanceId == replacementId, "reused physics state must resolve only the new owner");
		}
	}

	void TestWorldClearReleasesPhysicsAuthoringSlots()
	{
		for (uint32_t count : { 32u, 64u })
		{
			PhysicsComponentTestWorld world;
			auto* physics = world.GetECS<PhysicsECS>();
			auto* landscapes = world.GetECS<LandscapeECS>();
			TVector<GameObjectPtr> objects;
			TVector<ComponentPtr> components;
			for (uint32_t index = 0; index < count; ++index)
			{
				auto object = world.Instantiate("Physics authoring owner");
				components.Add(object->AddComponent<RigidBodyComponent>());
				components.Add(object->AddComponent<CollisionShapeComponent>());
				components.Add(object->AddComponent<LandscapeComponent>());
				objects.Add(object);
				Require(physics->IsComponentRegistered(index) && landscapes->IsComponentRegistered(index),
					"rigid body and landscape authoring must register independent ECS slots");
			}
			world.Clear();
			world.Clear();
			for (uint32_t index = 0; index < count; ++index)
			{
				Require(!objects[index] && !physics->IsComponentRegistered(index) && !landscapes->IsComponentRegistered(index),
					"world teardown must unregister every rigid body and landscape slot");
			}
			for (const auto& component : components)
			{
				Require(!component, "physics authoring component handles must not survive Clear");
			}
			auto replacement = world.Instantiate("Fresh physics authoring");
			auto body = replacement->AddComponent<RigidBodyComponent>();
			auto landscape = replacement->AddComponent<LandscapeComponent>();
			Require(body->GetComponentIndex() == 0 && landscape->GetComponentIndex() == 0 &&
				physics->GetComponentData(0).m_bodyId == RigidBodyData::InvalidBodyId &&
				landscapes->GetComponentData(0).m_physicsBodies.IsEmpty() && landscapes->GetComponentData(0).m_chunks.IsEmpty(),
				"new authoring slots must not inherit old body handles or landscape resources");
			world.Clear();
		}
	}

	void TestForceAtPositionAppliesLinearAndAngularImpulse()
	{
		Physics::PhysicsWorld world;
		uint32_t bodyId = ~0u;
		Physics::RigidBodyDesc body = MakeBox(
			InstanceId::GenerateNewInstanceId(),
			Physics::ERigidBodyMotionType::Dynamic,
			glm::vec3(0.0f),
			glm::vec3(1.0f));
		body.m_mass = 1.0f;
		Require(world.CreateBody(body, bodyId),
			"dynamic force test body should be created");
		for (uint32_t step = 0; step < 60; ++step)
		{
			Require(world.AddForceAtPosition(
				bodyId,
				glm::vec3(0.0f, 14.0f, 0.0f),
				glm::vec3(0.4f, 0.0f, 0.0f)),
				"force should be accepted for a live body");
			Require(world.Step(c_fixedDeltaTime),
				"force physics step should succeed");
		}

		Physics::PhysicsBodyPose pose{};
		Require(world.GetBodyPose(bodyId, pose),
			"force test pose should be readable");
		Require(
			pose.m_position.y > 0.5f &&
				std::abs(pose.m_angularVelocity.z) > 0.01f,
			"off-center force should create linear motion and torque");
	}

	void TestStaticTriangleMeshSupportsDynamicBodies()
	{
		Physics::PhysicsWorld world;
		Physics::RigidBodyDesc terrain{};
		terrain.m_instanceId = InstanceId::GenerateNewInstanceId();
		terrain.m_motionType = Physics::ERigidBodyMotionType::Static;

		Physics::CollisionShapeDesc mesh{};
		mesh.m_type = Physics::ECollisionShapeType::TriangleMesh;
		mesh.m_vertices = {
			glm::vec3(-10.0f, 0.0f, -10.0f),
			glm::vec3(-10.0f, 0.0f, 10.0f),
			glm::vec3(10.0f, 0.0f, -10.0f),
			glm::vec3(10.0f, 0.0f, 10.0f)
		};
		mesh.m_indices = { 0, 1, 2, 2, 1, 3 };
		terrain.m_shapes.Add(std::move(mesh));

		uint32_t terrainBody = ~0u;
		Require(world.CreateBody(terrain, terrainBody),
			"static triangle-mesh terrain should be created");

		uint32_t dynamicBody = ~0u;
		Require(world.CreateBody(
			MakeBox(
				InstanceId::GenerateNewInstanceId(),
				Physics::ERigidBodyMotionType::Dynamic,
				glm::vec3(0.0f, 4.0f, 0.0f),
				glm::vec3(1.0f)),
			dynamicBody),
			"dynamic body above triangle-mesh terrain should be created");

		Step(world, 240);
		Physics::PhysicsBodyPose pose{};
		Require(world.GetBodyPose(dynamicBody, pose),
			"dynamic body on triangle-mesh terrain should remain readable");
		Require(pose.m_position.y > 0.45f && pose.m_position.y < 0.56f,
			"dynamic body should settle on triangle-mesh terrain");
	}

}

int main()
{
	Physics::JoltRuntime runtime;
	const std::pair<const char*, std::function<void()>> tests[] = {
		{ "FixedStepGravityContactsAndRaycast", TestFixedStepGravityContactsAndRaycast },
		{ "StaticTriangleMeshCollisionAndRaycast", TestStaticTriangleMeshCollisionAndRaycast },
		{ "KinematicAuthority", TestKinematicAuthority },
		{ "ScaledSphereVolume", TestScaledSphereVolume },
		{ "CollisionLayersAndQueryMask", TestCollisionLayersAndQueryMask },
		{ "SensorEventsAndQueuedContactDestruction", TestSensorEventsAndQueuedContactDestruction },
		{ "LifecycleAndInputValidation", TestLifecycleAndInputValidation },
		{ "ThinScaledBoxPreservesCollisionExtent", TestThinScaledBoxPreservesCollisionExtent },
		{ "SameBuildRepeatability", TestSameBuildRepeatability },
		{ "WorldPoseToLocalForTransformedParent", TestWorldPoseToLocalForTransformedParent },
		{ "ReflectedPhysicsAuthoringContract", TestReflectedPhysicsAuthoringContract },
		{ "InitialVelocityAuthoringStaysSeparateFromRuntimeCommands", TestInitialVelocityAuthoringStaysSeparateFromRuntimeCommands },
		{ "PendingVelocityCommandsAndLiveBodyReconstruction", TestPendingVelocityCommandsAndLiveBodyReconstruction },
		{ "VelocityQueriesAndRepeatedStopAfterAcceleration", TestVelocityQueriesAndRepeatedStopAfterAcceleration },
		{ "KinematicVelocityQueriesFollowAuthoredTargets", TestKinematicVelocityQueriesFollowAuthoredTargets },
		{ "PhysicsWorldQueuesAndDrainsJoltJobs", TestPhysicsWorldQueuesAndDrainsJoltJobs },
		{ "ComponentTeardownOrdering", TestComponentTeardownOrdering },
		{ "BulkPhysicsWorldClearAndReuse", TestBulkPhysicsWorldClearAndReuse },
		{ "WorldClearReleasesPhysicsAuthoringSlots", TestWorldClearReleasesPhysicsAuthoringSlots },
		{ "ForceAtPositionAppliesLinearAndAngularImpulse", TestForceAtPositionAppliesLinearAndAngularImpulse },
		{ "StaticTriangleMeshSupportsDynamicBodies", TestStaticTriangleMeshSupportsDynamicBodies },
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
