#pragma once

#include "FrameGraph/EnvironmentSource.h"
#include "GlobalIllumination/GIProbesData.h"
#include "Math/Bounds.h"
#include "Raytracing/GIProbesPathTracer.h"

#include <atomic>
#include <cstdint>
#include <functional>
#include <string>

namespace Sailor
{
	class World;

	using GIProbesSceneWarningCallback =
		std::function<void(const std::string&)>;

	struct SAILOR_SHARED_API GIProbesSceneCaptureRequest final
	{
		GIProbesBakeSettings m_settings{};
		glm::vec3 m_fallbackEnvironment{ 0.03f };
		std::string m_sourceIdentity{};
	};

	struct SAILOR_SHARED_API GIProbesSceneRevision final
	{
		uint64_t m_geometry = 0u;
		uint64_t m_lighting = 0u;
		glm::vec3 m_sunDirection{};

		bool operator==(const GIProbesSceneRevision&) const noexcept = default;
		bool HasChanges(const GIProbesSceneRevision& previous, float sunAngleThresholdDegrees) const noexcept;
	};

	struct SAILOR_SHARED_API GIProbesSceneSnapshot final
	{
		TVector<Raytracing::PathTracer::TLASInstance> m_instances{};
		Raytracing::PathTracer::MaterialSnapshots m_materials{};
		TVector<Raytracing::LightProxy> m_lights{};
		TVector<Math::AABB> m_geometryBounds{};
		EnvironmentSource m_environment;
		TextureImporter::CpuTextureSnapshot m_environmentPixels;
		Math::AABB m_worldBounds{};
		uint64_t m_geometryHash = 0u;
		uint64_t m_lightingHash = 0u;
		uint64_t m_sourceWorldHash = 0u;
		GIProbesSceneRevision m_observedRevision{};
	};

	// Owner-thread validation is separate from the values consumed by background work.
	struct SAILOR_SHARED_API GIProbesSceneMaterialWatch final
	{
		Raytracing::PathTracer::MaterialSnapshotCache m_materials;
		TVector<MaterialPtr> m_slots;
		bool HasUnchangedMaterials() const noexcept;
		bool HasUnchangedSurfaces() const noexcept;
	};

	using GIProbesSceneSnapshotPtr = TSharedPtr<GIProbesSceneSnapshot>;

	struct SAILOR_SHARED_API GIProbesPreparedScene final
	{
		TSharedPtr<Raytracing::GIProbesPathTracer> m_sampler{};
		GIProbesBakeSettings m_effectiveSettings{};
		Math::AABB m_worldBounds{};
		uint64_t m_geometryHash = 0u;
		uint64_t m_lightingHash = 0u;
		GIProbesSceneRevision m_observedRevision{};
	};

	using GIProbesPreparedScenePtr = TSharedPtr<GIProbesPreparedScene>;

	SAILOR_SHARED_API bool CaptureGIProbesScene(
		World* world,
		const GIProbesSceneCaptureRequest& request,
		GIProbesSceneSnapshot& outScene,
		std::string& outDiagnostic,
		const GIProbesSceneWarningCallback& warning = {},
		GIProbesSceneMaterialWatch* materialWatch = nullptr);

	SAILOR_SHARED_API bool CaptureGIProbesSceneLighting(
		World* world,
		const GIProbesSceneCaptureRequest& request,
		GIProbesSceneSnapshot& outScene,
		std::string& outDiagnostic,
		const GIProbesSceneWarningCallback& warning = {});

	SAILOR_SHARED_API bool ObserveGIProbesSceneRevision(
		World* world,
		const GIProbesSceneCaptureRequest& request,
		GIProbesSceneRevision& outRevision,
		std::string& outDiagnostic);

	SAILOR_SHARED_API bool PrepareGIProbesScene(
		const GIProbesSceneSnapshot& scene,
		const GIProbesBakeSettings& settings,
		const std::atomic<bool>* cancel,
		GIProbesPreparedScene& outPreparedScene,
		std::string& outDiagnostic,
		const Raytracing::PathTracer::ScenePreparationProgressCallback& progress = {},
		const GIProbesSceneWarningCallback& warning = {},
		const GIProbesPreparedScene* previous = nullptr);
}
