#include "ECS/CameraECS.h"
#include "ECS/TransformECS.h"
#include "Engine/GameObject.h"

using namespace Sailor;
using namespace Sailor::JobSystem;

JobSystem::ITaskPtr CameraECS::Tick(float deltaTime)
{
	for (auto& data : m_components)
	{
		if (data.m_bIsActive)
		{
			// The origin
			const mat4 origin = glm::rotate(glm::mat4(1), glm::radians(90.0f), Math::vec3_Up);
			data.m_viewMatrix = origin * glm::inverse(data.m_owner.StaticCast<GameObject>()->GetTransformComponent().GetCachedWorldMatrix());
		}
	}

	return nullptr;
}
