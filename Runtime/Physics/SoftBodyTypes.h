#pragma once
#include "Physics/PhysicsTypes.h"

namespace Sailor::Physics
{
	struct SoftBodyDesc final
	{
		InstanceId m_instanceId{};
		glm::vec3 m_position{};
		glm::quat m_rotation = glm::quat(1.0f, 0.0f, 0.0f, 0.0f);
		TVector<glm::vec3> m_vertices{};
		TVector<uint32_t> m_indices{};
		TVector<float> m_inverseMasses{};
		// Maximum distance from each skin target, in metres. Zero pins the vertex;
		// an empty array disables skinning.
		TVector<float> m_maxDistances{};
		float m_edgeCompliance = 0.00001f;
		float m_shearCompliance = 0.00001f;
		float m_bendCompliance = 0.001f;
		float m_vertexRadius = 0.01f;
		float m_friction = 0.3f;
		float m_restitution = 0.0f;
		float m_linearDamping = 0.1f;
		float m_maxLinearVelocity = 50.0f;
		float m_gravityFactor = 1.0f;
		uint32_t m_numIterations = 6;
		uint8_t m_collisionLayer = 0;
		bool m_bAllowSleeping = false;
	};

	struct SoftBodyVertex final
	{
		glm::vec3 m_position{};
		glm::vec3 m_velocity{};
		glm::vec3 m_normal{};
	};
}
