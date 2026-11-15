#include "ECS/CameraECS.h"
#include "ECS/TransformECS.h"
#include "Engine/GameObject.h"
#include "RHI/SceneView.h"

using namespace Sailor;
using namespace Sailor::Tasks;

void CameraECS::Tick(float deltaTime)
{
	m_rhiCameras.Clear();
	m_rhiCameraTransforms.Clear();

	for (auto& data : m_components)
	{
		if (!data.m_bIsActive)
		{
			continue;
		}

		GameObjectPtr owner = data.m_owner.StaticCast<GameObject>();
		if (!owner)
		{
			continue;
		}

		const auto& transformComponent = owner->GetTransformComponent();
		const auto& worldMatrix = transformComponent.GetCachedWorldMatrix();

		// The origin
		const mat4 origin = glm::mat4(1);// glm::rotate(glm::mat4(1), glm::radians(90.0f), Math::vec3_Up);
		data.m_viewMatrix = origin * glm::inverse(worldMatrix);

		m_rhiCameras.Add(data);
		m_rhiCameraTransforms.Add(Math::Transform::FromMatrix(worldMatrix));
	}
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

bool CameraECS::TryGetActiveCamera(Math::Transform& outCameraTransform, CameraData& outCameraData)
{
	// Issue #184: an active editor-role camera (independent viewport camera
	// created by engine world infrastructure) wins over authored cameras so
	// viewport focus/picking/navigation follow the editor view. Without one,
	// behavior stays the historical first-active-camera selection.
	CameraData* pBest = nullptr;
	for (auto& data : m_components)
	{
		if (!data.m_bIsActive)
		{
			continue;
		}

		GameObjectPtr pOwner = data.m_owner.StaticCast<GameObject>();
		if (!pOwner)
		{
			continue;
		}

		if (data.GetEditorRole())
		{
			pBest = &data;
			break;
		}

		if (!pBest)
		{
			pBest = &data;
		}
	}

	if (!pBest)
	{
		return false;
	}

	GameObjectPtr pOwner = pBest->m_owner.StaticCast<GameObject>();
	outCameraTransform = Math::Transform::FromMatrix(pOwner->GetTransformComponent().GetCachedWorldMatrix());
	outCameraData = *pBest;
	return true;
}
