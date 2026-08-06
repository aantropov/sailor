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

#include <algorithm>
#include <cmath>
#include <filesystem>
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

	struct PhysicsSceneStats final
	{
		size_t m_dynamicBodies = 0;
		size_t m_kinematicBodies = 0;
		size_t m_boxShapes = 0;
		size_t m_sphereShapes = 0;
		size_t m_capsuleShapes = 0;
		size_t m_compoundBodies = 0;
		size_t m_primitiveVisualMismatches = 0;
		float m_minDynamicScale = std::numeric_limits<float>::max();
		float m_minDynamicGravityFactor =
			std::numeric_limits<float>::max();
		float m_minDynamicFriction = std::numeric_limits<float>::max();
		float m_maxDynamicFriction = 0.0f;
		float m_minDynamicRestitution = std::numeric_limits<float>::max();
		float m_maxDynamicRestitution = 0.0f;
	};

	PhysicsSceneStats InspectPhysicsScene(
		const std::filesystem::path& path)
	{
		const YAML::Node world = YAML::LoadFile(path.string());
		Require(
			world["prefabs"].IsSequence(),
			path.filename().string() + " should contain prefabs");

		PhysicsSceneStats stats{};
		for (const YAML::Node& prefab : world["prefabs"])
		{
			const YAML::Node gameObjects = prefab["gameObjects"];
			const YAML::Node components = prefab["components"];
			Require(
				gameObjects.IsSequence() && components.IsSequence(),
				path.filename().string() +
					" should use valid prefab arrays");

			TVector<size_t> dynamicComponentIndices;
			std::string renderedModelFileId;
			std::string singleShapeType;
			size_t numShapes = 0;
			glm::vec3 singleBoxSize{};
			for (size_t componentIndex = 0;
				componentIndex < components.size();
				++componentIndex)
			{
				const YAML::Node component = components[componentIndex];
				const std::string typeName =
					component["typename"].as<std::string>();
				const YAML::Node properties = component["overrideProperties"];
				if (typeName == "Sailor::MeshRendererComponent")
				{
					const YAML::Node model = properties["model"];
					renderedModelFileId = model ?
						model["fileId"].as<std::string>() : std::string{};
				}
				else if (typeName == "Sailor::RigidBodyComponent")
				{
					const std::string motionType =
						properties["motionType"].as<std::string>();
					stats.m_kinematicBodies += motionType == "Kinematic";
					if (motionType == "Dynamic")
					{
						dynamicComponentIndices.Add(componentIndex);
						++stats.m_dynamicBodies;
						const float friction = properties["friction"] ?
							properties["friction"].as<float>() : 0.2f;
						const float restitution = properties["restitution"] ?
							properties["restitution"].as<float>() : 0.0f;
						const float gravityFactor = properties["gravityFactor"] ?
							properties["gravityFactor"].as<float>() : 1.0f;
						stats.m_minDynamicGravityFactor = std::min(
							stats.m_minDynamicGravityFactor,
							gravityFactor);
						stats.m_minDynamicFriction = std::min(
							stats.m_minDynamicFriction,
							friction);
						stats.m_maxDynamicFriction = std::max(
							stats.m_maxDynamicFriction,
							friction);
						stats.m_minDynamicRestitution = std::min(
							stats.m_minDynamicRestitution,
							restitution);
						stats.m_maxDynamicRestitution = std::max(
							stats.m_maxDynamicRestitution,
							restitution);
					}
				}
				else if (typeName == "Sailor::CollisionShapeComponent")
				{
					++numShapes;
					const std::string shapeType =
						properties["shapeType"].as<std::string>();
					singleShapeType = shapeType;
					stats.m_boxShapes += shapeType == "Box";
					stats.m_sphereShapes += shapeType == "Sphere";
					stats.m_capsuleShapes += shapeType == "Capsule";
					if (shapeType == "Box")
					{
						const YAML::Node size = properties["size"];
						singleBoxSize = glm::vec3(
							size[0].as<float>(),
							size[1].as<float>(),
							size[2].as<float>());
					}
				}
			}
			stats.m_compoundBodies += numShapes > 1;
			if (numShapes == 1)
			{
				const bool bBoxMatches = singleShapeType == "Box" &&
					renderedModelFileId.find(
						"A1450002-0000-4000-8000-000000000002") !=
						std::string::npos &&
					!glm::any(glm::greaterThan(
						glm::abs(singleBoxSize - glm::vec3(1.0f)),
						glm::vec3(0.0001f)));
				const bool bRoundShapeMatches =
					(singleShapeType == "Sphere" ||
						singleShapeType == "Capsule") &&
					renderedModelFileId.find(
						"A1450001-0000-4000-8000-000000000001") !=
						std::string::npos;
				stats.m_primitiveVisualMismatches +=
					!bBoxMatches && !bRoundShapeMatches;
			}

			for (const YAML::Node& gameObject : gameObjects)
			{
				const YAML::Node componentReferences =
					gameObject["components"];
				bool bDynamic = false;
				for (const YAML::Node& componentReference :
					componentReferences)
				{
					bDynamic |= dynamicComponentIndices.Contains(
						componentReference.as<size_t>());
				}
				if (!bDynamic)
				{
					continue;
				}
				const YAML::Node scale = gameObject["scale"];
				Require(
					scale.IsSequence() && scale.size() >= 3,
					path.filename().string() +
						" should provide a three-dimensional scale");
				stats.m_minDynamicScale = std::min(
					stats.m_minDynamicScale,
					std::min({
						std::abs(scale[0].as<float>()),
						std::abs(scale[1].as<float>()),
						std::abs(scale[2].as<float>()) }));
			}
		}
		return stats;
	}

	void TestPhysicsSceneFixtures()
	{
		const std::filesystem::path root =
			std::filesystem::path(SAILOR_TEST_SOURCE_DIR) /
			"Content/Tests/Physics";
		const PhysicsSceneStats sphereRain = InspectPhysicsScene(
			root / "PhysicsSphereRain.world");
		Require(
			sphereRain.m_dynamicBodies == 36 &&
				sphereRain.m_sphereShapes == 36 &&
				sphereRain.m_primitiveVisualMismatches == 0 &&
				sphereRain.m_minDynamicGravityFactor >= 10.0f &&
				sphereRain.m_minDynamicScale >= 20.0f,
			"sphere rain should contain 36 clearly visible dynamic balls");

		const PhysicsSceneStats mixed = InspectPhysicsScene(
			root / "PhysicsMixedShapes.world");
		Require(
			mixed.m_dynamicBodies == 21 &&
				mixed.m_boxShapes >= 9 &&
				mixed.m_sphereShapes >= 9 &&
				mixed.m_capsuleShapes == 5 &&
				mixed.m_compoundBodies == 1 &&
				mixed.m_primitiveVisualMismatches == 0 &&
				mixed.m_minDynamicGravityFactor >= 10.0f &&
				mixed.m_minDynamicScale >= 15.0f,
			"mixed scene should cover boxes, spheres, capsules, and a compound body");

		const PhysicsSceneStats stress = InspectPhysicsScene(
			root / "PhysicsStressLarge.world");
		Require(
			stress.m_dynamicBodies == 320 &&
				stress.m_boxShapes >= 164 &&
				stress.m_sphereShapes == 160 &&
				stress.m_primitiveVisualMismatches == 0 &&
				stress.m_minDynamicGravityFactor >= 10.0f &&
				stress.m_minDynamicScale >= 18.0f,
			"stress scene should keep 320 large alternating dynamic bodies");

		const PhysicsSceneStats bounce = InspectPhysicsScene(
			root / "PhysicsBounceComparison.world");
		Require(
			bounce.m_dynamicBodies == 5 &&
				bounce.m_sphereShapes == 5 &&
				bounce.m_primitiveVisualMismatches == 0 &&
				bounce.m_minDynamicGravityFactor >= 10.0f &&
				bounce.m_minDynamicRestitution == 0.0f &&
				bounce.m_maxDynamicRestitution == 1.0f &&
				bounce.m_minDynamicScale >= 22.0f,
			"bounce comparison should expose the full restitution range");

		const PhysicsSceneStats friction = InspectPhysicsScene(
			root / "PhysicsFrictionRamps.world");
		Require(
			friction.m_dynamicBodies == 5 &&
				friction.m_minDynamicFriction == 0.0f &&
				friction.m_maxDynamicFriction == 1.0f &&
				friction.m_primitiveVisualMismatches == 0 &&
				friction.m_minDynamicGravityFactor >= 10.0f &&
				friction.m_minDynamicScale >= 22.0f,
			"friction ramps should expose the full friction range");

		const PhysicsSceneStats kinematic = InspectPhysicsScene(
			root / "PhysicsKinematicPusher.world");
		Require(
			kinematic.m_dynamicBodies == 30 &&
				kinematic.m_kinematicBodies == 1 &&
				kinematic.m_sphereShapes == 30 &&
				kinematic.m_primitiveVisualMismatches == 0 &&
				kinematic.m_minDynamicGravityFactor >= 10.0f &&
				kinematic.m_minDynamicScale >= 20.0f,
			"kinematic pusher should provide a manual interaction target and ball pile");

		const PhysicsSceneStats fallingBodies = InspectPhysicsScene(
			root / "PhysicsFallingBodies.world");
		Require(
			fallingBodies.m_dynamicBodies == 4 &&
				fallingBodies.m_boxShapes == 4 &&
				fallingBodies.m_sphereShapes == 1 &&
				fallingBodies.m_capsuleShapes == 1 &&
				fallingBodies.m_primitiveVisualMismatches == 0 &&
				fallingBodies.m_minDynamicGravityFactor >= 10.0f &&
				fallingBodies.m_minDynamicScale >= 10.0f,
			"falling bodies should use large matching box and round primitives");

		const PhysicsSceneStats hierarchy = InspectPhysicsScene(
			root / "PhysicsHierarchy.world");
		Require(
			hierarchy.m_dynamicBodies == 1 &&
				hierarchy.m_kinematicBodies == 1 &&
				hierarchy.m_boxShapes == 3 &&
				hierarchy.m_primitiveVisualMismatches == 0 &&
				hierarchy.m_minDynamicGravityFactor >= 10.0f &&
				hierarchy.m_minDynamicScale >= 10.0f,
			"hierarchy scene should preserve its parent transform at the large scale");

		const std::filesystem::path sphereRoot =
			std::filesystem::path(SAILOR_TEST_SOURCE_DIR) /
			"Content/Models/PhysicsSphere";
		const YAML::Node sphereMetadata = YAML::LoadFile(
			(sphereRoot / "PhysicsSphere.gltf.asset").string());
		const YAML::Node sphereGltf = YAML::LoadFile(
			(sphereRoot / "PhysicsSphere.gltf").string());
		const YAML::Node sphereMin = sphereGltf["accessors"][0]["min"];
		const YAML::Node sphereMax = sphereGltf["accessors"][0]["max"];
		Require(
			sphereMetadata["assetInfoType"].as<std::string>() ==
				"Sailor::ModelAssetInfo" &&
				!sphereMetadata["bShouldGenerateMaterials"].as<bool>() &&
				IsNear(sphereMin[0].as<float>(), -0.5f) &&
				IsNear(sphereMin[1].as<float>(), -0.5f) &&
				IsNear(sphereMin[2].as<float>(), -0.5f) &&
				IsNear(sphereMax[0].as<float>(), 0.5f) &&
				IsNear(sphereMax[1].as<float>(), 0.5f) &&
				IsNear(sphereMax[2].as<float>(), 0.5f),
			"visible sphere radius should match the default collision radius 0.5");

		const std::filesystem::path boxRoot =
			std::filesystem::path(SAILOR_TEST_SOURCE_DIR) /
			"Content/Models/PhysicsBox";
		const YAML::Node boxMetadata = YAML::LoadFile(
			(boxRoot / "PhysicsBox.gltf.asset").string());
		const YAML::Node boxGltf = YAML::LoadFile(
			(boxRoot / "PhysicsBox.gltf").string());
		const YAML::Node boxMin = boxGltf["accessors"][2]["min"];
		const YAML::Node boxMax = boxGltf["accessors"][2]["max"];
		Require(
			boxMetadata["assetInfoType"].as<std::string>() ==
				"Sailor::ModelAssetInfo" &&
				!boxMetadata["bShouldGenerateMaterials"].as<bool>() &&
				IsNear(boxMetadata["unitScale"].as<float>(), 1.0f) &&
				IsNear(boxMin[0].as<float>(), -0.5f) &&
				IsNear(boxMin[1].as<float>(), -0.5f) &&
				IsNear(boxMin[2].as<float>(), -0.5f) &&
				IsNear(boxMax[0].as<float>(), 0.5f) &&
				IsNear(boxMax[1].as<float>(), 0.5f) &&
				IsNear(boxMax[2].as<float>(), 0.5f),
			"visible box bounds should match the default collision size 1");
	}
}

int main()
{
	Physics::JoltRuntime runtime;
	const std::pair<const char*, std::function<void()>> tests[] = {
		{ "FixedStepGravityContactsAndRaycast", TestFixedStepGravityContactsAndRaycast },
		{ "KinematicAuthority", TestKinematicAuthority },
		{ "ScaledSphereVolume", TestScaledSphereVolume },
		{ "CollisionLayersAndQueryMask", TestCollisionLayersAndQueryMask },
		{ "SensorEventsAndQueuedContactDestruction", TestSensorEventsAndQueuedContactDestruction },
		{ "LifecycleAndInputValidation", TestLifecycleAndInputValidation },
		{ "SameBuildRepeatability", TestSameBuildRepeatability },
		{ "WorldPoseToLocalForTransformedParent", TestWorldPoseToLocalForTransformedParent },
		{ "ReflectedPhysicsAuthoringContract", TestReflectedPhysicsAuthoringContract },
		{ "ComponentTeardownOrdering", TestComponentTeardownOrdering },
		{ "PhysicsSceneFixtures", TestPhysicsSceneFixtures },
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
