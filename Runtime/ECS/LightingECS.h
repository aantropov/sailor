#pragma once
#include "Sailor.h"
#include "Engine/Object.h"
#include "Tasks/Scheduler.h"
#include "ECS/ECS.h"
#include "Engine/Types.h"
#include "RHI/Types.h"
#include "Containers/Pair.h"
#include "Components/Component.h"
#include "Memory/Memory.h"
#include "RHI/SceneView.h"

namespace Sailor
{
	using WorldPtr = class World*;
	using GameObjectPtr = TObjectPtr<class GameObject>;

	class LightData : public ECS::TComponent
	{

	public:

		glm::vec3 m_intensity{ 100.0f, 100.0f, 100.0f };
		glm::vec3 m_attenuation{ 1.0f, 0.022f, 0.0019f };
		glm::vec3 m_bounds{ 100.0f, 100.0f, 100.0f };
		glm::vec2 m_cutOff{ 30.0f, 45.0f };
                ELightType m_type = ELightType::Point;
                RHI::EShadowType m_shadowType = RHI::EShadowType::PCF;
                float m_evsmBlurScale{ 1.0f };

	protected:

		friend class LightingECS;
	};

	struct CSMLightState
	{
		uint32_t m_componentIndex = 0;
		glm::mat4 m_lightMatrix{};
		Math::Transform m_cameraTransform{};
		Math::Transform m_lightTransform{};

		// <Mesh ECS Index, LastFrameChanged>
		TVector<TPair<size_t, size_t>> m_snapshot{};

		bool Equals(const CSMLightState& rhs) const;
	};

	class LightingECS final : public ECS::TSystem<LightingECS, LightData>
	{
	public:

		// Global constants
		static constexpr uint32_t MaxShadowsInView = 1024;
		const uint32_t LightsMaxNum = 65535;
		static constexpr float ShadowsMemoryBudgetMb = 350.0f;

		const RHI::EFormat ShadowMapFormat = RHI::EFormat::R16_SFLOAT;
		const RHI::EFormat ShadowMapFormat_Evsm = RHI::EFormat::R32G32B32A32_SFLOAT;

		// CSM is based on https://learnopengl.com/Guest-Articles/2021/CSM
		// and https://learn.microsoft.com/en-us/windows/win32/dxtecharts/cascaded-shadow-maps
		// ESM is based on https://dl.acm.org/doi/pdf/10.5555/1375714.1375741
		// EVSM is based on https://www.cg.tuwien.ac.at/research/publications/2013/ADORJAN-2013-ASE/ADORJAN-2013-ASE-thesis.pdf
		// Also handy paper: https://dl.acm.org/doi/pdf/10.5555/1375714.1375739
		static constexpr uint32_t NumCascades = 4;
		static constexpr float ShadowCascadeLevels[NumCascades] = { 1.0f / 20.0f, 1.0f / 10.0f, 1.0f / 3.0f, 1.0f / 2.0f };
                static constexpr glm::ivec2 ShadowCascadeResolutions[NumCascades] = { {4096,4096}, {4096,4096}, {4096,4096}, {4096,4096} };
                static constexpr glm::vec2 ShadowCascadeBlur[NumCascades] = { {2.0f, 5.0f}, {1.0f, 4.0f}, {1.0f, 3.0f}, {1.0f, 2.0f} };

		// TODO: Tightly pack
		struct LightShaderData
		{
			uint32_t m_type;
			uint32_t m_shadowType;
			alignas(16) glm::vec3 m_worldPosition;
			alignas(16) glm::vec3 m_direction;
			alignas(16) glm::vec3 m_intensity;
			alignas(16) glm::vec3 m_attenuation;
			alignas(16) glm::vec2 m_cutOff;
			alignas(16) glm::vec3 m_bounds;
		};

		SAILOR_API virtual void BeginPlay() override;
		SAILOR_API virtual Tasks::ITaskPtr Tick(float deltaTime) override;
		SAILOR_API virtual void EndPlay() override;
		SAILOR_API virtual uint32_t GetOrder() const override { return 150; }

		void FillLightingData(RHI::RHISceneViewPtr& sceneView);

		float GetShadowsOccupiedMemoryMb() const { return m_shadowMapsMb; }

	protected:

		SAILOR_API TVector<RHI::RHIUpdateShadowMapCommand> PrepareCSMPasses(
			const RHI::RHISceneViewPtr& sceneView,
			const Math::Transform& cameraTransform,
			const CameraData& cameraData,
			const TVector<RHI::RHILightProxy>& directionalLights);

		SAILOR_API void GetLightsInFrustum(const Math::Frustum& frustum,
			const Math::Transform& cameraTransform,
			TVector<RHI::RHILightProxy>& outDirectionalLights,
			TVector<RHI::RHILightProxy>& outSortedPointLights,
			TVector<RHI::RHILightProxy>& outSortedSpotLights);

		// Lights
		TVector<TPair<uint32_t, uint32_t>> m_skipList;
		RHI::RHIShaderBindingSetPtr m_lightsData;

		// Shadows
		// Light matrices and shadowMaps
		RHI::RHIShaderBindingPtr m_shadowMaps;
		RHI::RHIShaderBindingPtr m_lightMatrices;
		RHI::RHIShaderBindingPtr m_shadowIndices;

		TVector<RHI::RHIRenderTargetPtr> m_csmShadowMaps;
		TVector<CSMLightState> m_csmSnapshots;

		RHI::RHIRenderTargetPtr m_defaultShadowMap;

		float m_shadowMapsMb = 0;
	};

	template class ECS::TSystem<LightingECS, LightData>;
}
