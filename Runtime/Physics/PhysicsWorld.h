#pragma once
#include "Core/Defines.h"
#include "Memory/UniquePtr.hpp"
#include "Physics/PhysicsTypes.h"
#include "Physics/SoftBodyTypes.h"

namespace Sailor::Tasks
{
	class Scheduler;
}

namespace Sailor::Physics
{
	class PhysicsWorld final
	{
	public:
		SAILOR_API PhysicsWorld();
		// JoltRuntime and scheduler must outlive the world.
		SAILOR_API explicit PhysicsWorld(Tasks::Scheduler& scheduler);
		SAILOR_API ~PhysicsWorld();

		PhysicsWorld(const PhysicsWorld&) = delete;
		PhysicsWorld& operator=(const PhysicsWorld&) = delete;

		SAILOR_API bool CreateBody(
			const RigidBodyDesc& desc,
			uint32_t& outBodyId);

		// Soft-body methods run between Step calls on the world's owner thread.
		SAILOR_API bool CreateSoftBody(
			const SoftBodyDesc& desc,
			uint32_t& outBodyId);
		// Targets and readback use world space and the descriptor's vertex order.
		SAILOR_API bool SetSoftBodyTargets(
			uint32_t bodyId,
			const TVector<glm::vec3>& targets,
			float maxDistanceMultiplier = 1.0f,
			bool bReset = false);
		// Refit local-space constraints without resetting particles or skin binds.
		SAILOR_API bool SetSoftBodyRestPose(
			uint32_t bodyId,
			const TVector<glm::vec3>& positions,
			bool bPreserveEdgeLengths = false);
		SAILOR_API bool ApplySoftBodyWind(
			uint32_t bodyId,
			const glm::vec3& velocity,
			float airDensity,
			float drag,
			float deltaTime);
		SAILOR_API bool GetSoftBodyVertices(
			uint32_t bodyId,
			TVector<SoftBodyVertex>& outVertices) const;

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
		SAILOR_API bool AddForceAtPosition(
			uint32_t bodyId,
			const glm::vec3& force,
			const glm::vec3& position);
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
