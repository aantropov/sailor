#pragma once

#include "GlobalIllumination/GIProbesBaker.h"
#include "Raytracing/PathTracer.h"

namespace Sailor::Raytracing
{
	class SAILOR_SHARED_API GIProbesPathTracer final :
		public IGIProbeBakeRaySampler
	{
	public:
		bool Initialize(
			const TVector<PathTracer::TLASInstance>& instances,
			const TVector<MaterialPtr>& materials,
			const TVector<LightProxy>& lights,
			const GIProbesBakeSettings& settings,
			const glm::vec3& fallbackEnvironment = glm::vec3(0.03f),
			const PathTracer::ScenePreparationProgressCallback& progress = {},
			const PathTracer::ScenePreparationWarningCallback& warning = {});
		bool InitializeSnapshot(
			const TVector<PathTracer::TLASInstance>& instances,
			const PathTracer::MaterialSnapshots& materials,
			const TVector<LightProxy>& lights,
			const GIProbesBakeSettings& settings,
			const glm::vec3& fallbackEnvironment = glm::vec3(0.03f),
			const PathTracer::ScenePreparationProgressCallback& progress = {},
			const PathTracer::ScenePreparationWarningCallback& warning = {});

		bool InitializeLighting(
			const GIProbesPathTracer& source,
			const PathTracer::MaterialSnapshots& materials,
			const TVector<LightProxy>& lights,
			const GIProbesBakeSettings& settings,
			const glm::vec3& fallbackEnvironment = glm::vec3(0.03f),
			const PathTracer::ScenePreparationProgressCallback& progress = {});

		void SetEnvironmentLinear(
			const TVector<glm::vec4>& image,
			const glm::uvec2& extent);
		// Cancellation invalidates this private preparation; initialize it again before retrying.
		bool SetEnvironmentLinear(const TVector<glm::vec4>& image, const glm::uvec2& extent,
			const std::function<bool()>& shouldContinue);

		const PathTracer::ScenePreparationStats&
			GetLastScenePreparationStats() const
		{
			return m_pathTracer.GetLastScenePreparationStats();
		}

		bool SamplePrimaryDirection(
			const glm::vec3& uniformDirection,
			uint32_t sampleIndex,
			uint32_t sampleCount,
			uint32_t randomSeed,
			glm::vec3& outDirection,
			float& outPdf,
			std::string& outDiagnostic) const override;
		bool Sample(
			const glm::vec3& origin,
			const glm::vec3& direction,
			float maxDistance,
			uint32_t randomSeed,
			GIProbeBakeRaySample& outSample,
			std::string& outDiagnostic) const override;
		bool SampleVisibility(
			const glm::vec3& origin,
			const glm::vec3& direction,
			float maxDistance,
			uint32_t randomSeed,
			GIProbeBakeRaySample& outSample,
			std::string& outDiagnostic) const override;

	private:
		void ConfigureParameters(const GIProbesBakeSettings& settings,
			const glm::vec3& fallbackEnvironment);

		bool InitializeInternal(
			const TVector<PathTracer::TLASInstance>& instances,
			const TVector<MaterialPtr>& runtimeMaterials,
			const PathTracer::MaterialSnapshots* snapshotMaterials,
			const TVector<LightProxy>& lights,
			const GIProbesBakeSettings& settings,
			const glm::vec3& fallbackEnvironment,
			const PathTracer::ScenePreparationProgressCallback& progress,
			const PathTracer::ScenePreparationWarningCallback& warning);

		PathTracer m_pathTracer{};
		PathTracer::Params m_params{};
		bool m_bInitialized = false;
	};
}
