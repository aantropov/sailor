#pragma once
#include "Core/Defines.h"
#include "Engine/InstanceId.h"
#include "Containers/Vector.h"
#include <glm/mat4x4.hpp>
#include <glm/vec3.hpp>
#include <glm/gtc/quaternion.hpp>

namespace Sailor::Physics
{
	enum class ERigidBodyMotionType : uint8_t
	{
		Static = 0,
		Kinematic,
		Dynamic
	};

	enum class ECollisionShapeType : uint8_t
	{
		Box = 0,
		Sphere,
		Capsule
	};

	enum class EPhysicsContactType : uint8_t
	{
		Added = 0,
		Persisted,
		Removed
	};

	struct CollisionShapeDesc final
	{
		ECollisionShapeType m_type = ECollisionShapeType::Box;
		glm::vec3 m_center{};
		glm::quat m_rotation = glm::quat(1.0f, 0.0f, 0.0f, 0.0f);
		glm::vec3 m_size = glm::vec3(1.0f);
		float m_radius = 0.5f;
		float m_height = 1.0f;
	};

	struct RigidBodyDesc final
	{
		InstanceId m_instanceId{};
		ERigidBodyMotionType m_motionType = ERigidBodyMotionType::Dynamic;
		glm::vec3 m_position{};
		glm::quat m_rotation = glm::quat(1.0f, 0.0f, 0.0f, 0.0f);
		glm::vec3 m_scale = glm::vec3(1.0f);
		glm::vec3 m_linearVelocity{};
		glm::vec3 m_angularVelocity{};
		float m_mass = 1.0f;
		float m_friction = 0.2f;
		float m_restitution = 0.0f;
		float m_linearDamping = 0.05f;
		float m_angularDamping = 0.05f;
		float m_gravityFactor = 1.0f;
		uint8_t m_collisionLayer = 0;
		bool m_bSensor = false;
		bool m_bAllowSleeping = true;
		TVector<CollisionShapeDesc> m_shapes{};
	};

	struct PhysicsBodyPose final
	{
		glm::vec3 m_position{};
		glm::quat m_rotation = glm::quat(1.0f, 0.0f, 0.0f, 0.0f);
		glm::vec3 m_linearVelocity{};
		glm::vec3 m_angularVelocity{};
		bool m_bActive = false;
	};

	struct PhysicsContactEvent final
	{
		EPhysicsContactType m_type = EPhysicsContactType::Added;
		InstanceId m_first{};
		InstanceId m_second{};
		glm::vec3 m_position{};
		glm::vec3 m_normal{};
		bool m_bSensor = false;
	};

	struct PhysicsRaycastHit final
	{
		InstanceId m_instanceId{};
		glm::vec3 m_position{};
		glm::vec3 m_normal{};
		float m_fraction = 0.0f;
	};

	SAILOR_API bool TryConvertWorldPoseToLocal(
		const glm::mat4& parentWorldMatrix,
		const glm::vec3& worldPosition,
		const glm::quat& worldRotation,
		glm::vec3& outLocalPosition,
		glm::quat& outLocalRotation);
}
