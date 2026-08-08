#pragma once
#include "Components/Component.h"
#include "ECS/ECS.h"
#include "Physics/PhysicsTypes.h"

namespace Sailor
{
	class RigidBodyComponent final : public Component
	{
		SAILOR_REFLECTABLE(RigidBodyComponent)

	public:
		SAILOR_API void Initialize() override;
		SAILOR_API void EndPlay() override;

		SAILOR_API Physics::ERigidBodyMotionType GetMotionType() const { return m_motionType; }
		SAILOR_API void SetMotionType(Physics::ERigidBodyMotionType value);
		SAILOR_API float GetMass() const { return m_mass; }
		SAILOR_API void SetMass(float value);
		SAILOR_API float GetFriction() const { return m_friction; }
		SAILOR_API void SetFriction(float value);
		SAILOR_API float GetRestitution() const { return m_restitution; }
		SAILOR_API void SetRestitution(float value);
		SAILOR_API float GetLinearDamping() const { return m_linearDamping; }
		SAILOR_API void SetLinearDamping(float value);
		SAILOR_API float GetAngularDamping() const { return m_angularDamping; }
		SAILOR_API void SetAngularDamping(float value);
		SAILOR_API float GetGravityFactor() const { return m_gravityFactor; }
		SAILOR_API void SetGravityFactor(float value);
		SAILOR_API uint32_t GetCollisionLayer() const { return m_collisionLayer; }
		SAILOR_API void SetCollisionLayer(uint32_t value);
		SAILOR_API bool IsSensor() const { return m_bSensor; }
		SAILOR_API void SetSensor(bool value);
		SAILOR_API bool IsSleepingAllowed() const { return m_bAllowSleeping; }
		SAILOR_API void SetSleepingAllowed(bool value);
		SAILOR_API const glm::vec3& GetLinearVelocity() const { return m_linearVelocity; }
		SAILOR_API void SetLinearVelocity(const glm::vec3& value);
		SAILOR_API const glm::vec3& GetAngularVelocity() const { return m_angularVelocity; }
		SAILOR_API void SetAngularVelocity(const glm::vec3& value);
		SAILOR_API bool AddForceAtPosition(
			const glm::vec3& force,
			const glm::vec3& worldPosition);

		SAILOR_API void MarkPhysicsDirty();
		SAILOR_API size_t GetComponentIndex() const { return m_handle; }

	private:
		Physics::ERigidBodyMotionType m_motionType =
			Physics::ERigidBodyMotionType::Dynamic;
		glm::vec3 m_linearVelocity{};
		glm::vec3 m_angularVelocity{};
		float m_mass = 1.0f;
		float m_friction = 0.2f;
		float m_restitution = 0.0f;
		float m_linearDamping = 0.05f;
		float m_angularDamping = 0.05f;
		float m_gravityFactor = 1.0f;
		size_t m_handle = ECS::InvalidIndex;
		uint8_t m_collisionLayer = 0;
		bool m_bSensor = false;
		bool m_bAllowSleeping = true;
	};
}

using namespace Sailor::Attributes;

REFL_AUTO(
	type(Sailor::RigidBodyComponent, bases<Sailor::Component>),
	func(GetMotionType, property("motionType")),
	func(SetMotionType, property("motionType")),
	func(GetMass, property("mass")),
	func(SetMass, property("mass")),
	func(GetFriction, property("friction")),
	func(SetFriction, property("friction")),
	func(GetRestitution, property("restitution"), Range(0.0, 1.0)),
	func(SetRestitution, property("restitution")),
	func(GetLinearDamping, property("linearDamping"), Range(0.0, 1.0)),
	func(SetLinearDamping, property("linearDamping")),
	func(GetAngularDamping, property("angularDamping"), Range(0.0, 1.0)),
	func(SetAngularDamping, property("angularDamping")),
	func(GetGravityFactor, property("gravityFactor")),
	func(SetGravityFactor, property("gravityFactor")),
	func(GetCollisionLayer, property("collisionLayer"), Range(0.0, 15.0)),
	func(SetCollisionLayer, property("collisionLayer")),
	func(IsSensor, property("sensor")),
	func(SetSensor, property("sensor")),
	func(IsSleepingAllowed, property("allowSleeping")),
	func(SetSleepingAllowed, property("allowSleeping")),
	func(GetLinearVelocity, property("linearVelocity")),
	func(SetLinearVelocity, property("linearVelocity")),
	func(GetAngularVelocity, property("angularVelocity")),
	func(SetAngularVelocity, property("angularVelocity"))
)
