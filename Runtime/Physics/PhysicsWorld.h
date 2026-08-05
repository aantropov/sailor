#pragma once
#include "Core/Defines.h"
#include "Memory/UniquePtr.hpp"
#include "Physics/PhysicsTypes.h"

namespace Sailor::Physics
{
	class PhysicsWorld final
	{
	public:
		SAILOR_API PhysicsWorld();
		SAILOR_API ~PhysicsWorld();

		PhysicsWorld(const PhysicsWorld&) = delete;
		PhysicsWorld& operator=(const PhysicsWorld&) = delete;

		SAILOR_API bool CreateBody(
			const RigidBodyDesc& desc,
			uint32_t& outBodyId);
		SAILOR_API void DestroyBody(uint32_t bodyId);
		SAILOR_API bool SetBodyTransform(
			uint32_t bodyId,
			const glm::vec3& position,
			const glm::quat& rotation,
			bool bKinematic,
			float deltaTime);
		SAILOR_API bool GetBodyPose(
			uint32_t bodyId,
			PhysicsBodyPose& outPose) const;
		SAILOR_API bool SetBodyVelocity(
			uint32_t bodyId,
			const glm::vec3& linearVelocity,
			const glm::vec3& angularVelocity);
		SAILOR_API bool Step(float deltaTime);
		SAILOR_API bool Raycast(
			const glm::vec3& origin,
			const glm::vec3& direction,
			float distance,
			PhysicsRaycastHit& outHit,
			uint16_t collisionMask = 0xffffu) const;
		SAILOR_API void SetLayerCollisionEnabled(
			uint8_t firstLayer,
			uint8_t secondLayer,
			bool bEnabled);
		SAILOR_API bool IsLayerCollisionEnabled(
			uint8_t firstLayer,
			uint8_t secondLayer) const;
		SAILOR_API void DrainContactEvents(
			TVector<PhysicsContactEvent>& outEvents);
		SAILOR_API void Clear();

	private:
		class Impl;
		TUniquePtr<Impl> m_pImpl;
	};
}
