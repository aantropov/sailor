#pragma once
#include "Sailor.h"
#include "Memory/SharedPtr.hpp"
#include "Memory/ObjectPtr.hpp"
#include "Tasks/Scheduler.h"
#include "Engine/Types.h"
#include "Engine/Object.h"
#include "Components/Component.h"
#include "ECS/CameraECS.h"
#include "AssetRegistry/Texture/TextureImporter.h"
#include "Settings/GraphicsSettings.h"

namespace Sailor
{
	class CameraComponent : public Component
	{
		SAILOR_REFLECTABLE(CameraComponent)

	public:

		SAILOR_API virtual void Initialize() override;
		SAILOR_API virtual void BeginPlay() override;
		SAILOR_API virtual void Tick(float deltaTime) override;
		SAILOR_API virtual void EditorTick(float deltaTime) override;
		SAILOR_API virtual void EndPlay() override;
		SAILOR_API __forceinline CameraData& GetData();
		SAILOR_API __forceinline const CameraData& GetData() const;

		SAILOR_API float GetFov() const { return GetData().GetFov(); }
		SAILOR_API void SetFov(float value);

		SAILOR_API float GetAspect() const { return GetData().GetAspect(); }
		SAILOR_API void SetAspect(float value);

		SAILOR_API float GetZNear() const { return GetData().GetZNear(); }
		SAILOR_API void SetZNear(float value);

		SAILOR_API float GetZFar() const { return GetData().GetZFar(); }
		SAILOR_API void SetZFar(float value);

		SAILOR_API Sailor::FileId GetRendererConfig() const { return m_rendererConfig; }
		SAILOR_API void SetRendererConfig(Sailor::FileId value);

		SAILOR_API TexturePtr GetRenderTarget() const { return m_renderTarget; }
		SAILOR_API void SetRenderTarget(TexturePtr value);

		SAILOR_API Settings::EGraphicsQualitySelection GetQualityPreset() const { return m_qualityPreset; }
		SAILOR_API void SetQualityPreset(Settings::EGraphicsQualitySelection value);

		// Call after a cinematic cut, seek, respawn or intentional teleport.
		SAILOR_API void ResetMotionHistory();

		SAILOR_API static float CalculateAspect();

	protected:

		void UpdateProjectionMatrix();
		void CaptureRenderTarget();

		size_t m_handle = (size_t)(-1);

		Sailor::FileId m_rendererConfig = Sailor::FileId::Invalid;
		TexturePtr m_renderTarget{};
		Settings::EGraphicsQualitySelection m_qualityPreset = Settings::EGraphicsQualitySelection::ProjectDefault;

	};
}

using namespace Sailor;

REFL_AUTO(
	type(Sailor::CameraComponent, bases<Component>),

	func(GetFov, property("fov"), Attributes::SkipCDO(), Attributes::Range(1.0, 179.0)),
	func(SetFov, property("fov"), Attributes::SkipCDO()),

	func(GetZNear, property("zNear"), Attributes::SkipCDO()),
	func(SetZNear, property("zNear"), Attributes::SkipCDO()),

	func(GetZFar, property("zFar"), Attributes::SkipCDO()),
	func(SetZFar, property("zFar"), Attributes::SkipCDO()),

	func(GetRendererConfig, property("rendererConfig")),
	func(SetRendererConfig, property("rendererConfig")),

	func(GetRenderTarget, property("renderTarget")),
	func(SetRenderTarget, property("renderTarget")),

	func(GetQualityPreset, property("qualityPreset")),
	func(SetQualityPreset, property("qualityPreset"))
)
