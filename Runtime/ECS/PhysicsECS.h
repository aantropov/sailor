#pragma once
#include "ECS/ECS.h"
#include "Physics/PhysicsTypes.h"

namespace Sailor::Physics
{
	class PhysicsWorld;
}

namespace Sailor
{
	struct RigidBodyData final : public ECS::TComponent
	{
		static constexpr uint32_t InvalidBodyId = ~0u;

		uint32_t m_bodyId = InvalidBodyId;
		Physics::ERigidBodyMotionType m_motionType =
			Physics::ERigidBodyMotionType::Dynamic;
		glm::vec3 m_bodyScale = glm::vec3(1.0f);
		Physics::PhysicsBodyPose m_previousPose{};
		Physics::PhysicsBodyPose m_currentPose{};
		size_t m_lastAppliedTransformFrame = 0;
		glm::vec3 m_pendingLinearVelocity{};
		glm::vec3 m_pendingAngularVelocity{};
		bool m_bLinearVelocityPending = false;
		bool m_bAngularVelocityPending = false;

		void ClearDirty() { m_bIsDirty = false; }

		friend class PhysicsECS;
	};

	class SAILOR_API PhysicsECS final :
		public ECS::TSystem<PhysicsECS, RigidBodyData>
	{
	public:
		PhysicsECS();
		// JoltRuntime and scheduler must outlive the system and its physics world.
		PhysicsECS(TUniquePtr<Physics::PhysicsWorld> physicsWorld, Tasks::Scheduler& scheduler);
		~PhysicsECS() override;

		Tasks::ITaskPtr Tick(float deltaTime) override;
		void EndPlay() override;
		uint32_t GetOrder() const override { return 50; }

		bool Raycast(
			const glm::vec3& origin,
			const glm::vec3& direction,
			float distance,
			Physics::PhysicsRaycastHit& outHit,
			uint16_t collisionMask = 0xffffu) const;
		void SetLayerCollisionEnabled(
			uint8_t firstLayer,
			uint8_t secondLayer,
			bool bEnabled);
		bool IsLayerCollisionEnabled(
			uint8_t firstLayer,
			uint8_t secondLayer) const;
		void DrainContactEvents(
			TVector<Physics::PhysicsContactEvent>& outEvents);
		bool AddForceAtPosition(
			size_t componentIndex,
			const glm::vec3& force,
			const glm::vec3& worldPosition);
		bool CreateStaticTriangleMesh(
			const InstanceId& instanceId,
			const TVector<glm::vec3>& vertices,
			const TVector<uint32_t>& indices,
			const glm::vec3& position,
			const glm::quat& rotation,
			const glm::vec3& scale,
			uint32_t& outBodyId);
		bool CreateStaticCompound(
			const InstanceId& instanceId,
			const TVector<Physics::CollisionShapeDesc>& shapes,
			const glm::vec3& position,
			const glm::quat& rotation,
			const glm::vec3& scale,
			uint32_t& outBodyId);
		void DestroyExternalBody(uint32_t bodyId);

		void SetFixedDeltaTime(float value);
		float GetFixedDeltaTime() const { return m_fixedDeltaTime; }
		void SetMaxSubSteps(uint32_t value);
		uint32_t GetMaxSubSteps() const { return m_maxSubSteps; }

	protected:
		void OnComponentUnregistered(
			size_t index,
			RigidBodyData& component) override;

	private:
		bool EnsurePhysicsWorld();
		bool RecreateBody(size_t index);
		bool BuildBodyDesc(
			size_t index,
			Physics::RigidBodyDesc& outDesc);
		void SyncAuthoredTransforms(float fixedDeltaTime);
		void ApplyBuoyancyForces(float sampleTime, float fixedDeltaTime);
		void ApplyDynamicTransforms(float interpolationAlpha);

		TUniquePtr<Physics::PhysicsWorld> m_physicsWorld{};
		Tasks::Scheduler* m_scheduler = nullptr;
		float m_accumulator = 0.0f;
		float m_fixedDeltaTime = 1.0f / 60.0f;
		uint32_t m_maxSubSteps = 4;
		bool m_bWasSimulationEnabled = false;
	};

	template class ECS::TSystem<PhysicsECS, RigidBodyData>;
}
