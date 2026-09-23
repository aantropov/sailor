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
#include <Jolt/Jolt.h>
#include <Jolt/Physics/Body/BodyID.h>

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

	void TestMirroredTriangleMeshMatchesBakedGeometry()
	{
		for (const glm::vec3 scale : { glm::vec3(2.0f, 1.25f, 1.5f),
			glm::vec3(-2.0f, 1.25f, 1.5f), glm::vec3(-2.0f, 1.25f, -1.5f),
			glm::vec3(2.0f, -1.25f, 1.5f) })
		{
			Physics::PhysicsWorld scaledWorld;
			Physics::PhysicsWorld bakedWorld;
			auto desc = MakeTriangleMesh(InstanceId::GenerateNewInstanceId(),
				glm::vec3(0.5f, 1.25f, -2.0f), scale);
			desc.m_rotation = glm::angleAxis(0.35f, glm::vec3(0.0f, 1.0f, 0.0f));
			auto& mesh = desc.m_shapes[0];
			mesh.m_center = glm::vec3(0.25f, 0.75f, -0.5f);
			mesh.m_vertices = { { 1.0f, 0.25f, 2.0f }, { 1.0f, 0.25f, 6.0f },
				{ 5.0f, 0.25f, 2.0f }, { 3.0f, 0.25f, 6.0f } };
			mesh.m_indices = { 0u, 1u, 2u, 2u, 1u, 3u };

			auto baked = desc;
			baked.m_scale = glm::vec3(1.0f);
			baked.m_shapes[0].m_center = glm::vec3(0.0f);
			for (auto& vertex : baked.m_shapes[0].m_vertices)
			{
				vertex = scale * (vertex + mesh.m_center);
			}
			const glm::vec3 normal(0.0f, scale.y < 0.0f ? -1.0f : 1.0f, 0.0f);
			const auto& vertices = baked.m_shapes[0].m_vertices;
			if (glm::dot(glm::cross(vertices[1] - vertices[0], vertices[2] - vertices[0]), normal) < 0.0f)
			{
				for (size_t index = 0u; index < baked.m_shapes[0].m_indices.Num(); index += 3u)
				{
					std::swap(baked.m_shapes[0].m_indices[index + 1u], baked.m_shapes[0].m_indices[index + 2u]);
				}
			}

			uint32_t scaledBody = ~0u;
			uint32_t bakedBody = ~0u;
			Require(scaledWorld.CreateBody(desc, scaledBody) && bakedWorld.CreateBody(baked, bakedBody),
				"scaled and explicitly baked asymmetric triangle meshes must both be valid");
			const glm::vec3 surface = desc.m_position + desc.m_rotation *
				(scale * (mesh.m_center + glm::vec3(2.0f, 0.25f, 3.0f)));
			Physics::PhysicsRaycastHit scaledHit{};
			Physics::PhysicsRaycastHit bakedHit{};
			Require(scaledWorld.Raycast(surface + normal * 4.0f, -normal, 8.0f, scaledHit) &&
				bakedWorld.Raycast(surface + normal * 4.0f, -normal, 8.0f, bakedHit),
				"raycasts must hit the mirrored location, including signed shape-center and body rotation");
			Require(scaledHit.m_instanceId == desc.m_instanceId &&
				IsNear(scaledHit.m_position, surface, 0.001f) &&
				IsNear(scaledHit.m_position, bakedHit.m_position, 0.001f) &&
				IsNear(scaledHit.m_normal, normal, 0.001f) &&
				IsNear(scaledHit.m_normal, bakedHit.m_normal, 0.001f),
				"mirroring must preserve outward winding and match explicitly transformed geometry");

			auto sphere = MakeSphere(InstanceId::GenerateNewInstanceId(),
				Physics::ERigidBodyMotionType::Dynamic, surface + normal * 2.0f, 0.25f);
			sphere.m_gravityFactor = normal.y;
			uint32_t scaledSphere = ~0u;
			uint32_t bakedSphere = ~0u;
			Require(scaledWorld.CreateBody(sphere, scaledSphere) && bakedWorld.CreateBody(sphere, bakedSphere),
				"mirrored mesh fixtures must accept matching dynamic bodies");
			Step(scaledWorld, 180u);
			Step(bakedWorld, 180u);
			Physics::PhysicsBodyPose scaledPose{};
			Physics::PhysicsBodyPose bakedPose{};
			Require(scaledWorld.GetBodyPose(scaledSphere, scaledPose) &&
				bakedWorld.GetBodyPose(bakedSphere, bakedPose) &&
				IsNear(scaledPose.m_position, surface + normal * 0.25f, 0.03f) &&
				IsNear(scaledPose.m_position, bakedPose.m_position, 0.001f),
				"collision must match the baked mesh at scale (" + std::to_string(scale.x) + ", " +
				std::to_string(scale.y) + ", " + std::to_string(scale.z) + "): surface error=" +
				std::to_string(glm::length(scaledPose.m_position - surface - normal * 0.25f)) +
				", baked error=" + std::to_string(glm::length(scaledPose.m_position - bakedPose.m_position)));
		}
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

	struct ContactPair
	{
		InstanceId m_staticId = InstanceId::GenerateNewInstanceId();
		InstanceId m_movingId = InstanceId::GenerateNewInstanceId();
		uint32_t m_staticBody = ~0u;
		uint32_t m_movingBody = ~0u;
	};

	ContactPair CreateContactPair(Physics::PhysicsWorld& world, bool bSensor)
	{
		ContactPair pair;
		Require(world.CreateBody(MakeBox(pair.m_staticId,
			Physics::ERigidBodyMotionType::Static, glm::vec3(0.0f, -0.5f, 0.0f),
			glm::vec3(10.0f, 1.0f, 10.0f)), pair.m_staticBody),
			"contact fixture ground should be created");
		auto moving = MakeBox(pair.m_movingId,
			Physics::ERigidBodyMotionType::Dynamic,
			glm::vec3(0.0f, 0.25f, 0.0f),
			glm::vec3(1.0f));
		moving.m_bSensor = bSensor;
		moving.m_gravityFactor = 0.0f;
		moving.m_bAllowSleeping = false;
		Require(world.CreateBody(moving, pair.m_movingBody),
			"overlapping dynamic body should be created");
		return pair;
	}

	void RequireContactPair(const TVector<Physics::PhysicsContactEvent>& events,
		const ContactPair& pair, bool bSensor)
	{
		const bool bStaticFirst = pair.m_staticId.ToString() < pair.m_movingId.ToString();
		const auto& first = bStaticFirst ? pair.m_staticId : pair.m_movingId;
		const auto& second = bStaticFirst ? pair.m_movingId : pair.m_staticId;
		for (const auto& event : events)
		{
			Require(event.m_first == first && event.m_second == second,
				"contact events must retain the canonical pair of instance ids");
			Require(event.m_bSensor == bSensor,
				"added, persisted and removed contacts must retain the body's sensor flag");
		}
		for (size_t index = 1; index < events.Num(); ++index)
		{
			Require(events[index - 1].m_type <= events[index].m_type,
				"contacts for the same pair must drain in stable event-type order");
		}
	}

	size_t CountContacts(const TVector<Physics::PhysicsContactEvent>& events,
		Physics::EPhysicsContactType type)
	{
		return static_cast<size_t>(std::count_if(events.begin(), events.end(),
			[type](const auto& event) { return event.m_type == type; }));
	}

	void TestSensorAndOrdinaryContactLifecycle()
	{
		for (bool bSensor : { false, true })
		{
			Physics::PhysicsWorld world;
			const auto pair = CreateContactPair(world, bSensor);
			TVector<Physics::PhysicsContactEvent> events;
			Step(world, 1);
			world.DrainContactEvents(events);
			RequireContactPair(events, pair, bSensor);
			Require(CountContacts(events, Physics::EPhysicsContactType::Added) > 0,
				"overlapping bodies must report an added contact");

			events.Clear();
			Step(world, 1);
			world.DrainContactEvents(events);
			RequireContactPair(events, pair, bSensor);
			Require(CountContacts(events, Physics::EPhysicsContactType::Persisted) > 0 &&
				CountContacts(events, Physics::EPhysicsContactType::Removed) == 0,
				"an awake overlapping pair must keep reporting persisted contacts");

			Require(world.SetBodyTransform(pair.m_movingBody, glm::vec3(0.0f, 3.0f, 0.0f),
				glm::quat(1.0f, 0.0f, 0.0f, 0.0f), false, c_fixedDeltaTime),
				"the contact body should move out of the overlap");
			events.Clear();
			Step(world, 1);
			world.DrainContactEvents(events);
			RequireContactPair(events, pair, bSensor);
			Require(CountContacts(events, Physics::EPhysicsContactType::Removed) > 0,
				"separating live bodies must report removal with the original sensor flag");
		}
	}

	void TestContactRemovalAfterBodyDestruction()
	{
		Tasks::Scheduler scheduler;
		scheduler.Initialize();
		for (uint32_t removedBodies : { 1u, 2u, 3u })
		{
			Physics::PhysicsWorld world(scheduler);
			const auto pair = CreateContactPair(world, true);
			TVector<Physics::PhysicsContactEvent> events;
			Step(world, 1);
			world.DrainContactEvents(events);
			RequireContactPair(events, pair, true);
			const size_t numAdded = CountContacts(events, Physics::EPhysicsContactType::Added);
			Require(numAdded > 0, "the pair must overlap before either body is destroyed");

			events.Clear();
			Step(world, 1);
			if ((removedBodies & 1u) != 0)
			{
				world.DestroyBody(pair.m_movingBody);
				world.DestroyBody(pair.m_movingBody);
				Physics::PhysicsBodyPose pose;
				Require(!world.GetBodyPose(pair.m_movingBody, pose) &&
					!world.SetBodyVelocity(pair.m_movingBody, glm::vec3(0.0f), glm::vec3(0.0f)),
					"retired contact metadata must not keep a destroyed body usable");
			}
			if ((removedBodies & 2u) != 0)
			{
				world.DestroyBody(pair.m_staticBody);
				world.DestroyBody(pair.m_staticBody);
			}
			world.DrainContactEvents(events);
			RequireContactPair(events, pair, true);
			Require(CountContacts(events, Physics::EPhysicsContactType::Persisted) > 0 &&
				CountContacts(events, Physics::EPhysicsContactType::Removed) == 0,
				"already queued contacts must survive destruction; exits wait for the next update");

			events.Clear();
			Require(!world.Step(0.0f) && !world.Step(-c_fixedDeltaTime),
				"invalid steps must not run Jolt or discard pending contact identities");
			world.DrainContactEvents(events);
			Require(events.IsEmpty(), "an invalid step must not manufacture contact removal");
			Step(world, 1);
			world.DrainContactEvents(events);
			RequireContactPair(events, pair, true);
			Require(CountContacts(events, Physics::EPhysicsContactType::Removed) == numAdded,
				"deleting either or both bodies must deliver each exit, including an empty world");
			events.Clear();
			Step(world, 1);
			world.DrainContactEvents(events);
			Require(events.IsEmpty(), "a removed contact must not be replayed on later steps");
		}
	}

	void TestRemovedContactsRetainBodySequence()
	{
		Physics::PhysicsWorld world;
		const auto oldPair = CreateContactPair(world, true);
		TVector<Physics::PhysicsContactEvent> events;
		Step(world, 1);
		world.DrainContactEvents(events);
		Require(CountContacts(events, Physics::EPhysicsContactType::Added) > 0,
			"the old sensor must have an active contact before its slot is reused");
		world.DestroyBody(oldPair.m_movingBody);

		ContactPair newPair = oldPair;
		newPair.m_movingId = InstanceId::GenerateNewInstanceId();
		auto replacement = MakeBox(newPair.m_movingId, Physics::ERigidBodyMotionType::Dynamic,
			glm::vec3(0.0f, 5.0f, 0.0f), glm::vec3(1.0f));
		replacement.m_gravityFactor = 0.0f;
		replacement.m_bAllowSleeping = false;
		Require(world.CreateBody(replacement, newPair.m_movingBody),
			"a non-sensor replacement body should be created before the next step");
		const JPH::BodyID oldBody(oldPair.m_movingBody);
		const JPH::BodyID newBody(newPair.m_movingBody);
		Require(oldBody.GetIndex() == newBody.GetIndex() &&
			oldBody.GetSequenceNumber() != newBody.GetSequenceNumber(),
			"the fixture must exercise actual Jolt body-slot reuse with a new sequence");

		events.Clear();
		Step(world, 1);
		world.DrainContactEvents(events);
		RequireContactPair(events, oldPair, true);
		Require(CountContacts(events, Physics::EPhysicsContactType::Removed) > 0,
			"the old exit must not borrow the replacement body's identity or sensor flag");
		Require(world.SetBodyTransform(newPair.m_movingBody, glm::vec3(0.0f, 0.25f, 0.0f),
			glm::quat(1.0f, 0.0f, 0.0f, 0.0f), false, c_fixedDeltaTime),
			"the replacement body must remain independently usable");
		events.Clear();
		Step(world, 1);
		world.DrainContactEvents(events);
		RequireContactPair(events, newPair, false);
		Require(CountContacts(events, Physics::EPhysicsContactType::Added) > 0,
			"the replacement must start its own ordinary collision lifecycle");
	}

	void TestCompoundSensorPartialExit()
	{
		Physics::PhysicsWorld world;
		ContactPair pair;
		auto compound = MakeBox(pair.m_staticId, Physics::ERigidBodyMotionType::Static,
			glm::vec3(0.0f), glm::vec3(1.0f, 4.0f, 4.0f));
		compound.m_shapes[0].m_center.x = -1.25f;
		auto secondShape = compound.m_shapes[0];
		secondShape.m_center.x = 1.25f;
		compound.m_shapes.Add(secondShape);
		Require(world.CreateBody(compound, pair.m_staticBody),
			"the compound must provide two opposing, separately contacted child faces");
		auto sensor = MakeBox(pair.m_movingId, Physics::ERigidBodyMotionType::Dynamic,
			glm::vec3(0.0f), glm::vec3(2.0f, 1.0f, 1.0f));
		sensor.m_bSensor = true;
		sensor.m_gravityFactor = 0.0f;
		sensor.m_bAllowSleeping = false;
		Require(world.CreateBody(sensor, pair.m_movingBody), "the compound sensor should be created");

		TVector<Physics::PhysicsContactEvent> events;
		Step(world, 1);
		world.DrainContactEvents(events);
		RequireContactPair(events, pair, true);
		const size_t numAdded = CountContacts(events, Physics::EPhysicsContactType::Added);
		Require(numAdded > 1, "distinct child contacts must not collapse into one body-pair event");

		Require(world.SetBodyTransform(pair.m_movingBody, glm::vec3(1.5f, 0.0f, 0.0f),
			glm::quat(1.0f, 0.0f, 0.0f, 0.0f), false, c_fixedDeltaTime),
			"the sensor should leave the left child while still overlapping the right child");
		events.Clear();
		Step(world, 1);
		world.DrainContactEvents(events);
		RequireContactPair(events, pair, true);
		const size_t numPartialRemoved = CountContacts(events, Physics::EPhysicsContactType::Removed);
		Require(numPartialRemoved > 0 && numPartialRemoved < numAdded &&
			CountContacts(events, Physics::EPhysicsContactType::Persisted) > 0 &&
			CountContacts(events, Physics::EPhysicsContactType::Added) == 0,
			"a partial compound exit must coexist with surviving per-child contacts");

		Require(world.SetBodyTransform(pair.m_movingBody, glm::vec3(5.0f, 0.0f, 0.0f),
			glm::quat(1.0f, 0.0f, 0.0f, 0.0f), false, c_fixedDeltaTime),
			"the sensor should leave the remaining child");
		events.Clear();
		Step(world, 1);
		world.DrainContactEvents(events);
		RequireContactPair(events, pair, true);
		Require(numPartialRemoved + CountContacts(events, Physics::EPhysicsContactType::Removed) == numAdded,
			"each original compound contact must produce its own exit");
	}

	void TestClearDiscardsContactHistory()
	{
		Physics::PhysicsWorld world;
		const auto oldPair = CreateContactPair(world, true);
		Step(world, 1);
		world.DestroyBody(oldPair.m_movingBody);
		world.Clear();
		world.Clear();
		TVector<Physics::PhysicsContactEvent> events;
		world.DrainContactEvents(events);
		Require(events.IsEmpty(), "Clear must discard queued contacts and pending removal metadata");

		const auto newPair = CreateContactPair(world, false);
		Step(world, 1);
		world.DrainContactEvents(events);
		RequireContactPair(events, newPair, false);
		Require(CountContacts(events, Physics::EPhysicsContactType::Added) > 0 &&
			CountContacts(events, Physics::EPhysicsContactType::Removed) == 0,
			"a reused world must report only new contacts, not delayed exits from before Clear");
		world.Clear();
		events.Clear();
		Step(world, 1);
		world.DrainContactEvents(events);
		Require(events.IsEmpty(), "stepping an empty cleared world must not replay its former contacts");
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
		{ "MirroredTriangleMeshMatchesBakedGeometry", TestMirroredTriangleMeshMatchesBakedGeometry },
		{ "KinematicAuthority", TestKinematicAuthority },
		{ "ScaledSphereVolume", TestScaledSphereVolume },
		{ "CollisionLayersAndQueryMask", TestCollisionLayersAndQueryMask },
		{ "SensorAndOrdinaryContactLifecycle", TestSensorAndOrdinaryContactLifecycle },
		{ "ContactRemovalAfterBodyDestruction", TestContactRemovalAfterBodyDestruction },
		{ "RemovedContactsRetainBodySequence", TestRemovedContactsRetainBodySequence },
		{ "CompoundSensorPartialExit", TestCompoundSensorPartialExit },
		{ "ClearDiscardsContactHistory", TestClearDiscardsContactHistory },
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
