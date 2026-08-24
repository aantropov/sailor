#include "Raytracing/ProbeVolumePathTracer.h"

#include <cmath>

using namespace Sailor;
using namespace Sailor::Raytracing;

bool ProbeVolumePathTracer::Initialize(
	const TVector<PathTracer::TLASInstance>& instances,
	const TVector<MaterialPtr>& materials,
	const TVector<LightProxy>& lights,
	const ProbeVolumeBakeSettings& settings,
	const glm::vec3& fallbackEnvironment,
	const PathTracer::ScenePreparationProgressCallback& progress)
{
	TVector<LightProxy> bakedLights;
	bakedLights.Reserve(lights.Num());
	for (const LightProxy& source : lights)
	{
		if (!std::isfinite(source.m_indirectLightingIntensity) ||
			source.m_indirectLightingIntensity <= 0.0f)
		{
			continue;
		}
		LightProxy light = source;
		light.m_intensity *= light.m_indirectLightingIntensity;
		bakedLights.Add(std::move(light));
	}

	m_params = {};
	m_params.m_maxBounces = settings.m_bounceCount;
	m_params.m_numSamples = 1u;
	m_params.m_numAmbientSamples = 1u;
	m_params.m_msaa = 1u;
	m_params.m_ambient = settings.m_bIncludeSky ?
		glm::max(fallbackEnvironment, glm::vec3(0.0f)) : glm::vec3(0.0f);
	m_params.m_rayBiasBase = settings.m_normalBias;
	m_params.m_rayBiasScale = settings.m_viewBias;
	m_params.m_bRunTasksInline = true;
	m_params.m_bIncludeDirectLighting = settings.m_bIncludeDirectLighting;
	m_params.m_bIncludeEnvironment = settings.m_bIncludeSky;
	m_params.m_bIncludeEmissive = settings.m_bIncludeEmissive;
	const bool bHasGeometry = m_pathTracer.InitializeScene(
		instances,
		materials,
		bakedLights,
		false,
		progress);
	m_bInitialized = bHasGeometry &&
		m_pathTracer.ArePreparedMaterialsFullyResolved();
	return m_bInitialized;
}

void ProbeVolumePathTracer::SetEnvironmentLinear(
	const TVector<glm::vec4>& image,
	const glm::uvec2& extent)
{
	m_pathTracer.SetRuntimeEnvironmentLinear(image, extent);
}

bool ProbeVolumePathTracer::Sample(
	const glm::vec3& origin,
	const glm::vec3& direction,
	float maxDistance,
	uint32_t randomSeed,
	ProbeVolumeBakeRaySample& outSample,
	std::string& outDiagnostic) const
{
	outSample = {};
	if (!m_bInitialized)
	{
		outDiagnostic = "probe-volume path tracer has no prepared scene";
		return false;
	}

	PathTracer::PreparedRaySample sample;
	if (!m_pathTracer.SamplePreparedSceneRay(
			origin,
			direction,
		maxDistance,
		m_params,
		randomSeed,
		sample))
	{
		outDiagnostic = "probe-volume path tracer rejected a bake ray";
		return false;
	}
	outSample.m_radiance = sample.m_radiance;
	outSample.m_distance = sample.m_distance;
	outSample.m_bHit = sample.m_bHit;
	outDiagnostic.clear();
	return true;
}
