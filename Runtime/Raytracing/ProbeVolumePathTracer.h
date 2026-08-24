#pragma once

#include "AssetRegistry/GlobalIllumination/ProbeVolumeBaker.h"
#include "Raytracing/PathTracer.h"

namespace Sailor::Raytracing
{
	class SAILOR_SHARED_API ProbeVolumePathTracer final :
		public IProbeVolumeBakeRaySampler
	{
	public:
		bool Initialize(
			const TVector<PathTracer::TLASInstance>& instances,
			const TVector<MaterialPtr>& materials,
			const TVector<LightProxy>& lights,
			const ProbeVolumeBakeSettings& settings,
			const glm::vec3& fallbackEnvironment = glm::vec3(0.03f),
			const PathTracer::ScenePreparationProgressCallback& progress = {});

		void SetEnvironmentLinear(
			const TVector<glm::vec4>& image,
			const glm::uvec2& extent);

		const PathTracer::ScenePreparationStats&
			GetLastScenePreparationStats() const
		{
			return m_pathTracer.GetLastScenePreparationStats();
		}

		bool Sample(
			const glm::vec3& origin,
			const glm::vec3& direction,
			float maxDistance,
			uint32_t randomSeed,
			ProbeVolumeBakeRaySample& outSample,
			std::string& outDiagnostic) const override;

	private:
		PathTracer m_pathTracer{};
		PathTracer::Params m_params{};
		bool m_bInitialized = false;
	};
}
