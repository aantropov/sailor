#pragma once
#include "Sailor.h"
#include "Engine/Object.h"
#include "Tasks/Scheduler.h"
#include "ECS/ECS.h"
#include "Engine/Types.h"
#include "RHI/Types.h"
#include "Components/Component.h"
#include "Memory/Memory.h"
#include "RHI/SceneView.h"
#include "RHI/Lighting.h"
#include "Raytracing/LightingModel.h"

#include <bitset>
#include <limits>

namespace Sailor
{
	class SkyComponent;
	using WorldPtr = class World*;
	using GameObjectPtr = TObjectPtr<class GameObject>;

	class LightData : public ECS::TComponent
	{

	public:

		// Point and spot lights use candela; directional lights use lux.
		glm::vec3 m_intensity{ 100.0f, 100.0f, 100.0f };
		float m_indirectLightingIntensity = 1.0f;
		ELightGlobalIlluminationMode m_globalIlluminationMode =
			ELightGlobalIlluminationMode::RealtimeAndBaked;
		// Smooth culling range for local lights, in metres.
		float m_radius = 100.0f;
		glm::vec2 m_cutOff{ 30.0f, 45.0f };
		ELightType m_type = ELightType::Point;
		RHI::EShadowType m_shadowType = RHI::EShadowType::PCF;
		ELightShadowQuality m_shadowQuality = ELightShadowQuality::Medium;
		ELightShadowFilter m_shadowFilter = ELightShadowFilter::Soft;

	protected:

		friend class LightingECS;
	};

	struct CSMLightState
	{
		uint32_t m_componentIndex = 0;
		RHI::EShadowType m_shadowType = RHI::EShadowType::None;
		glm::mat4 m_lightMatrix{};
		uint64_t m_sceneRevision = 0;
		uint64_t m_animationRevision = 0;
		uint64_t m_resourceRevision = 0;
		size_t m_lodCameraRevision = 0u;
		bool m_bContainsDynamicCasters = false;
		bool m_bContainsAnimatedCasters = false;
		bool m_bContainsCameraLodCasters = false;
		TSharedPtr<const TVector<RHI::RHISceneVersionPtr>> m_casterSceneVersions{};
		RHI::RHIRenderTargetPtr m_shadowMap{};
		RHI::RHISubmissionCompletionTokenPtr m_submissionToken{};
		RHI::RHISubmissionCompletionTokenPtr m_payloadCompletionToken{};

		SAILOR_API bool CanReuse(
			uint32_t componentIndex,
			RHI::EShadowType shadowType,
			const glm::mat4& lightMatrix,
			size_t lodCameraRevision,
			uint64_t sceneRevision,
			const TSharedPtr<const TVector<RHI::RHISceneVersionPtr>>& sceneVersions,
			const Math::Frustum& shadowFrustum,
			const RHI::RHISubmissionCompletionTokenPtr& currentSubmissionToken = {},
			uint64_t animationRevision = 0ull) const;
	};

	struct LocalLightShadowAllocation
	{
		uint32_t m_componentIndex = (std::numeric_limits<uint32_t>::max)();
		ELightType m_lightType = ELightType::Point;
		uint32_t m_resolution = 0;
		uint32_t m_requestedResolution = 0;
		uint32_t m_atlasIndex = (std::numeric_limits<uint32_t>::max)();
		uint64_t m_lastUsedFrame = 0;
		uint64_t m_revision = 0;
		TVector<uint32_t> m_slots{};
		TVector<glm::ivec4> m_tiles{};
	};

	struct LocalShadowAtlas
	{
		RHI::RHIRenderTargetPtr m_texture{};
		TVector<uint8_t> m_occupancy{};
	};

	struct LightingShadowFlightResources
	{
		TVector<RHI::RHIRenderTargetPtr> m_csmShadowMaps{};
		TVector<CSMLightState> m_csmSnapshots{};
		TVector<RHI::RHIRenderTargetPtr> m_localShadowAtlasTextures{};
		TVector<TVector<CSMLightState>> m_localShadowSnapshots{};
	};

	class LightingECS final : public ECS::TSystem<LightingECS, LightData>
	{
	public:

		// Global constants
		static constexpr uint32_t MaxShadowMapSamplers = 128;
		static constexpr uint32_t MaxShadowsInView = 2048;
		static constexpr uint32_t LightsMaxNum = 65535;
		static constexpr uint32_t InvalidShadowMapIndex = (std::numeric_limits<uint32_t>::max)();
		static constexpr uint32_t SoftShadowMapBit = 0x80000000u;
		static constexpr size_t GetGpuLightSlotsCount(size_t numComponentSlots)
		{
			return numComponentSlots < LightsMaxNum ? numComponentSlots : LightsMaxNum;
		}
		static constexpr float DefaultShadowsMemoryBudgetMb = 768.0f;
		static constexpr uint32_t LocalShadowAtlasResolution = 4096;
		static constexpr uint32_t LocalShadowMinResolution = 64;
		static constexpr uint32_t LocalShadowAtlasCellsPerAxis =
			LocalShadowAtlasResolution / LocalShadowMinResolution;
		static constexpr float LocalShadowAtlasMemoryMb =
			static_cast<float>(LocalShadowAtlasResolution * LocalShadowAtlasResolution * 2u) /
			(1024.0f * 1024.0f);
		static constexpr uint64_t LocalShadowCacheRetentionFrames = 300u;
		static constexpr uint32_t GetLocalShadowMapCount(ELightType lightType)
		{
			return lightType == ELightType::Point ? 6u :
				lightType == ELightType::Spot ? 1u : 0u;
		}
		static constexpr uint32_t GetLocalShadowResolution(ELightShadowQuality quality)
		{
			return quality == ELightShadowQuality::VeryLow ? 256u :
				quality == ELightShadowQuality::Low ? 512u :
				quality == ELightShadowQuality::High ? 2048u : 1024u;
		}

		static constexpr RHI::EFormat ShadowMapFormat = RHI::EFormat::R16_UNORM;
		static constexpr RHI::EFormat ShadowMapFormat_Evsm = RHI::EFormat::R32G32B32A32_SFLOAT;
		static constexpr RHI::EFormat GetCsmShadowMapFormat(RHI::EShadowType shadowType)
		{
			return shadowType == RHI::EShadowType::EVSM ?
				ShadowMapFormat_Evsm : ShadowMapFormat;
		}

		// CSM is based on https://learnopengl.com/Guest-Articles/2021/CSM
		// and https://learn.microsoft.com/en-us/windows/win32/dxtecharts/cascaded-shadow-maps
		// ESM is based on https://dl.acm.org/doi/pdf/10.5555/1375714.1375741
		// EVSM is based on https://www.cg.tuwien.ac.at/research/publications/2013/ADORJAN-2013-ASE/ADORJAN-2013-ASE-thesis.pdf
		// Also handy paper: https://dl.acm.org/doi/pdf/10.5555/1375714.1375739
		static constexpr uint32_t NumCascades = 4;
		static constexpr float ShadowCasterDepthExtension = 200.0f;
		static constexpr float ShadowCascadeBlendFraction = 0.1f;
		static constexpr float ShadowCascadeLevels[NumCascades] = { 0.025f, 0.075f, 0.2f, 1.0f };
		static constexpr glm::ivec2 ShadowCascadeResolutions[NumCascades] = { {4096,4096}, {2048,2048}, {1024,1024}, {1024,1024} };
		static constexpr glm::ivec2 ShadowCascadeBlur[NumCascades] = { glm::vec2(2, 2), glm::vec2(1, 1), glm::vec2(1, 1), glm::vec2(1, 1) };
		static constexpr float GetShadowCascadeLevel(
			uint32_t cascadeIndex,
			uint32_t activeCascadeCount)
		{
			const uint32_t safeCount = activeCascadeCount < 1u ? 1u :
				(activeCascadeCount > NumCascades ? NumCascades : activeCascadeCount);
			const uint32_t safeIndex = cascadeIndex < safeCount ?
				cascadeIndex : safeCount - 1u;
			return ShadowCascadeLevels[NumCascades - safeCount + safeIndex];
		}

		// TODO: Tightly pack
		using LightShaderData = RHI::RHILightShaderData;

		SAILOR_API virtual void BeginPlay() override;
		SAILOR_API virtual void Tick(float deltaTime) override;
		SAILOR_API virtual void EndPlay() override;
		SAILOR_API virtual uint32_t GetOrder() const override { return 150; }

		SAILOR_API void GetLightProxies(TVector<Raytracing::LightProxy>& outLights) const;
		// Main-thread component lifecycle. The lowest owner instance ID selects the sky.
		SAILOR_API void RegisterSky(TObjectPtr<SkyComponent> sky);
		SAILOR_API void UnregisterSky(const SkyComponent* sky);
		SAILOR_API TObjectPtr<SkyComponent> GetSky() const;
		size_t GetNumSkies() const { return m_skies.Num(); }
		uint64_t GetLightingRevision() const { return m_lightingRevision; }
		SAILOR_API void GetGlobalIlluminationBakeLightProxies(
			TVector<Raytracing::LightProxy>& outLights, const LightData* excludedLight = nullptr) const;
		SAILOR_API void FillLightingData(RHI::RHISceneViewPtr& sceneView, uint32_t shadowSlot);

		float GetShadowsOccupiedMemoryMb() const { return m_shadows.m_mapsMb; }
		float GetCsmShadowsOccupiedMemoryMb() const { return m_shadows.m_csmMapsMb; }
		float GetLocalShadowsOccupiedMemoryMb() const
		{
			return (std::max)(0.0f, m_shadows.m_mapsMb - m_shadows.m_csmMapsMb);
		}
		float GetShadowsMemoryBudgetMb() const { return m_shadowsMemoryBudgetMb; }
		void SetShadowsMemoryBudgetMb(float budgetMb)
		{
			m_shadowsMemoryBudgetMb = (std::max)(budgetMb, 0.0f);
		}

	protected:
		void CollectLightProxies(
			TVector<Raytracing::LightProxy>& outLights,
			bool bGlobalIlluminationBakeContributorsOnly, const LightData* excludedLight = nullptr) const;
		TVector<TObjectPtr<SkyComponent>> m_skies;

		SAILOR_API void PrepareCSMPasses(
			const RHI::RHISceneViewPtr& sceneView,
			const Math::Transform& cameraTransform,
			const CameraData& cameraData,
			const RHI::RHILightProxy& directionalLight,
			uint32_t flightSlot,
			LightingShadowFlightResources& flightResources,
			uint32_t& snapshotIndex,
			TVector<RHI::RHIUpdateShadowMapCommand>& outUpdateShadowMaps);

		SAILOR_API void PrepareLocalShadowPasses(
			const RHI::RHISceneViewPtr& sceneView,
			const TVector<RHI::RHILightProxy>& spotLights,
			const TVector<RHI::RHILightProxy>& pointLights,
			const Math::Transform& cameraTransform,
			const CameraData& cameraData,
			uint32_t viewportHeight,
			uint32_t flightSlot,
			LightingShadowFlightResources& flightResources,
			TVector<uint32_t>& shadowIndices,
			TVector<uint32_t>& shadowAtlasTiles,
			TVector<RHI::RHIUpdateShadowMapCommand>& outUpdateShadowMaps);
		SAILOR_API bool EnsureLocalShadowAllocation(
			uint32_t componentIndex,
			ELightType lightType,
			uint32_t desiredResolution,
			uint64_t frame);
		SAILOR_API uint32_t CalculateLocalShadowResolution(
			const LightData& light,
			float distanceToCamera,
			const CameraData& cameraData,
			uint32_t viewportHeight) const;
		SAILOR_API bool TryAllocateLocalShadowTiles(
			uint32_t count,
			uint32_t desiredResolution,
			uint32_t& outAtlasIndex,
			TVector<glm::ivec4>& outTiles);
		SAILOR_API bool TryAllocateLocalShadowTilesInAtlas(
			uint32_t atlasIndex,
			uint32_t count,
			uint32_t resolution,
			TVector<glm::ivec4>& outTiles);
		SAILOR_API bool TryCreateLocalShadowAtlas(uint32_t& outAtlasIndex);
		SAILOR_API bool EnsureWritableLocalShadowAtlas(
			uint32_t atlasIndex,
			uint32_t flightSlot,
			LightingShadowFlightResources& flightResources);
		SAILOR_API void PublishShadowMapBindings();
		SAILOR_API void ReleaseLocalShadowTiles(uint32_t atlasIndex, const TVector<glm::ivec4>& tiles);
		SAILOR_API void ReleaseUnusedLocalShadowAtlases();
		SAILOR_API void ReleaseLocalShadowAllocation(uint32_t componentIndex);
		SAILOR_API bool EvictLeastRecentlyUsedLocalShadowAllocation(
			uint32_t protectedComponentIndex,
			uint64_t frame);
		SAILOR_API void ReleaseUnusedLocalShadowAllocations(uint64_t frame);

		SAILOR_API void GetLightsInFrustum(const Math::Frustum& frustum,
			const Math::Transform& cameraTransform,
			TVector<RHI::RHILightProxy>& outDirectionalLights,
			TVector<RHI::RHILightProxy>& outSortedPointLights,
			TVector<RHI::RHILightProxy>& outSortedSpotLights);

		// Lights
		uint32_t m_numLights = 0;
		TVector<LightShaderData> m_cpuLightsData{};
		TSharedPtr<const TVector<LightShaderData>> m_publishedLightsData{};
		TVector<TSharedPtr<TVector<LightShaderData>>> m_lightsSnapshotPool{};
		uint64_t m_lightingRevision = 0ull;

		struct ShadowState
		{
			uint32_t m_directionalLightIndex = InvalidShadowMapIndex;
			// Texture template shared by immutable per-flight lighting descriptors.
			RHI::RHIShaderBindingSetPtr m_bindings;
			TVector<RHI::RHITexturePtr> m_textures;
			std::bitset<MaxShadowMapSamplers - NumCascades> m_writableLocalAtlases{};
			bool m_bBindingsDirty = false;
			TVector<RHI::RHIRenderTargetPtr> m_csmMaps;
			TVector<LightingShadowFlightResources> m_flights;
			TVector<uint32_t> m_mapOwners;
			TVector<LocalLightShadowAllocation> m_localAllocations;
			TVector<LocalShadowAtlas> m_localAtlases;
			TVector<RHI::RHILightProxy> m_directionalLightsScratch;
			TVector<RHI::RHILightProxy> m_pointLightsScratch;
			TVector<RHI::RHILightProxy> m_spotLightsScratch;
			TVector<glm::mat4> m_cascadeProjectionScratch;
			TVector<RHI::RHIVisibleShadowCaster> m_csmBroadCastersScratch;
			RHI::RHIRenderTargetPtr m_defaultMap;
			float m_mapsMb = 0.0f;
			float m_csmMapsMb = 0.0f;
			uint64_t m_localAllocationRevision = 0ull;
		};

		ShadowState m_shadows;
		float m_shadowsMemoryBudgetMb = DefaultShadowsMemoryBudgetMb;
	};

	template class ECS::TSystem<LightingECS, LightData>;
}
