#include "ECS/CameraECS.h"
#include "ECS/TransformECS.h"
#include "Engine/GameObject.h"
#include "RHI/SceneView.h"

using namespace Sailor;
using namespace Sailor::Tasks;

Tasks::ITaskPtr CameraECS::Tick(float deltaTime)
{
	m_rhiCameras.Clear();
	m_rhiCameraTransforms.Clear();

	for (auto& data : m_components)
	{
		if (data.m_bIsActive)
		{
			// The origin
			const mat4 origin = glm::mat4(1);// glm::rotate(glm::mat4(1), glm::radians(90.0f), Math::vec3_Up);
			data.m_viewMatrix = origin * glm::inverse(data.m_owner.StaticCast<GameObject>()->GetTransformComponent().GetCachedWorldMatrix());

			const Sailor::Math::Transform& transform = data.m_owner.StaticCast<GameObject>()->GetTransformComponent().GetTransform();
			m_rhiCameras.Add(data);
			m_rhiCameraTransforms.Add(transform);
		}
	}

	return nullptr;
}

glm::mat4 CameraData::GetInvProjection() const
{
	return glm::inverse(m_projectionMatrix);
}

glm::mat4 CameraData::GetInvViewMatrix() const
{
	return glm::inverse(m_viewMatrix);
}

glm::mat4 CameraData::GetInvViewProjection() const
{
	// We do that separately to maximize the accuracy of Math
	return GetInvProjection() * GetInvViewMatrix();
}

void CameraECS::CopyCameraData(RHI::RHISceneViewPtr& outCameras)
{
	SAILOR_PROFILE_FUNCTION();

	outCameras->m_cameras.Clear(false);
	outCameras->m_cameraTransforms.Clear(false);

	outCameras->m_cameras = std::move(m_rhiCameras);
	outCameras->m_cameraTransforms = std::move(m_rhiCameraTransforms);
}