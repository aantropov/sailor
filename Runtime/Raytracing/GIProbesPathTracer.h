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

		void SetEnvironmentLinear(
			const TVector<glm::vec4>& image,
			const glm::uvec2& extent);

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
		PathTracer m_pathTracer{};
		PathTracer::Params m_params{};
		bool m_bInitialized = false;
	};
}
