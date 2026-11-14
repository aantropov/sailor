#pragma once
#include "Sailor.h"
#include "Engine/Object.h"
#include "Tasks/Scheduler.h"
#include "ECS/ECS.h"
#include "Components/Component.h"
#include "Math/Transform.h"
#include "RHI/Types.h"
#include "RHI/Texture.h"
#include "Memory/Memory.h"
#include "Engine/Types.h"
#include "AssetRegistry/Texture/TextureImporter.h"
#include "Settings/GraphicsSettings.h"

namespace Sailor
{
	class CameraData final : public ECS::TComponent
	{
	public:

		SAILOR_API __forceinline void SetProjectionMatrix(const glm::mat4& projection) { m_projectionMatrix = projection; }
		SAILOR_API __forceinline void SetViewMatrix(const glm::mat4& view) { m_viewMatrix = view; }

		SAILOR_API __forceinline const glm::mat4& GetProjectionMatrix() const { return m_projectionMatrix; }
		SAILOR_API __forceinline const glm::mat4& GetViewMatrix() const { return m_viewMatrix; }
		SAILOR_API __forceinline glm::mat4 GetInvViewMatrix() const;
		SAILOR_API __forceinline glm::mat4 GetInvViewProjection() const;
		SAILOR_API __forceinline glm::mat4 GetInvProjection() const;

		SAILOR_API float GetFov() const { return m_fovDegrees; }
		SAILOR_API float GetAspect() const { return m_aspect; }
		SAILOR_API float GetZNear() const { return m_zNear; }
		SAILOR_API float GetZFar() const { return m_zFar; }

		SAILOR_API void SetFov(float fovDegrees) { m_fovDegrees = fovDegrees; }
		SAILOR_API void SetAspect(float aspect) { m_aspect = aspect; }
		SAILOR_API void SetZNear(float zNear) { m_zNear = zNear; }
		SAILOR_API void SetZFar(float zFar) { m_zFar = zFar; }
		SAILOR_API void ResetMotionHistory() { ++m_motionHistoryRevision; }
		SAILOR_API uint64_t GetMotionHistoryRevision() const { return m_motionHistoryRevision; }

		SAILOR_API void SetRendererConfig(Sailor::FileId config) { m_rendererConfig = config; }
		SAILOR_API Sailor::FileId GetRendererConfig() const { return m_rendererConfig; }

		SAILOR_API void SetRenderTarget(TexturePtr target) { m_renderTarget = target; }
		SAILOR_API TexturePtr GetRenderTarget() const { return m_renderTarget; }

		SAILOR_API void SetQualityPreset(Settings::EGraphicsQualitySelection preset) { m_qualityPreset = preset; }
		SAILOR_API Settings::EGraphicsQualitySelection GetQualityPreset() const { return m_qualityPreset; }

		// Transient engine-assigned classification. True only for the
		// independent editor viewport camera created by engine world
		// infrastructure. Never serialized and never applied to authored
		// game cameras.
		SAILOR_API void SetEditorRole(bool value) { m_editorRole = value; }
		SAILOR_API bool GetEditorRole() const { return m_editorRole; }

		SAILOR_API void SetResolvedRenderTarget(RHI::RHITexturePtr target, uint64_t revision)
		{
			m_resolvedRenderTarget = target;
			m_resolvedRevision = revision;
		}
		SAILOR_API RHI::RHITexturePtr GetResolvedRenderTarget() const { return m_resolvedRenderTarget; }
		SAILOR_API uint64_t GetResolvedRevision() const { return m_resolvedRevision; }

		SAILOR_API void SetInstanceId(const Sailor::InstanceId& id) { m_instanceId = id; }
		SAILOR_API const Sailor::InstanceId& GetInstanceId() const { return m_instanceId; }

	protected:

		glm::mat4 m_projectionMatrix{};
		glm::mat4 m_viewMatrix{};

		float m_aspect = 0.0f;
		float m_fovDegrees = 0.0f;

		float m_zNear = 0.1f;
		float m_zFar = 3000.0f;
		uint64_t m_motionHistoryRevision = 0ull;

		Sailor::FileId m_rendererConfig = Sailor::FileId::Invalid;
		TexturePtr m_renderTarget{};
		Settings::EGraphicsQualitySelection m_qualityPreset = Settings::EGraphicsQualitySelection::ProjectDefault;
		Sailor::InstanceId m_instanceId{};
		RHI::RHITexturePtr m_resolvedRenderTarget{};
		uint64_t m_resolvedRevision = 0ull;
		bool m_editorRole = false;

		friend class CameraECS;
	};

	class SAILOR_API CameraECS final : public ECS::TSystem<CameraECS, CameraData>
	{
	public:

		virtual void Tick(float deltaTime) override;
		void CopyCameraData(RHI::RHISceneViewPtr& outCameras);
		bool TryGetActiveCamera(Math::Transform& outCameraTransform, CameraData& outCameraData);
		const TVector<Math::Transform>& GetActiveCameraTransforms() const
		{
			return m_rhiCameraTransforms;
		}
		const TVector<CameraData>& GetActiveCameras() const
		{
			return m_rhiCameras;
		}

		virtual uint32_t GetOrder() const override { return 100; }

	private:

		TVector<CameraData> m_rhiCameras;
		TVector<Math::Transform> m_rhiCameraTransforms;
	};

	template class ECS::TSystem<CameraECS, CameraData>;
}
