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
			data.m_viewMatrix = data.m_owner.StaticCast<GameObject>()->GetTransform().GetCachedWorldMatrix();
		}
	}

	return nullptr;
}
