#include "Raytracing/GIProbesPathTracer.h"

#include "Core/LogMacros.h"

#include <cmath>

using namespace Sailor;
using namespace Sailor::Raytracing;

namespace
{
	constexpr float ProbeBakeRayNormalBias = 0.0001f;
	constexpr float ProbeBakeRayDirectionBias = 0.0003f;
}

bool GIProbesPathTracer::Initialize(
	const TVector<PathTracer::TLASInstance>& instances,
	const TVector<MaterialPtr>& materials,
	const TVector<LightProxy>& lights,
	const GIProbesBakeSettings& settings,
	const glm::vec3& fallbackEnvironment,
	const PathTracer::ScenePreparationProgressCallback& progress,
	const PathTracer::ScenePreparationWarningCallback& warning)
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
		glm::max(fallbackEnvironment, glm::vec3(0.0f)) *
			settings.m_skyIndirectIntensity : glm::vec3(0.0f);
	// Probe interpolation biases move the runtime receiver and are authored in
	// metres. Reusing them as a path-tracing epsilon lets secondary rays
	// jump across thin walls and turns occluded baked lighting into light leaks.
	m_params.m_rayBiasBase = ProbeBakeRayNormalBias;
	m_params.m_rayBiasScale = ProbeBakeRayDirectionBias;
	m_params.m_bRunTasksInline = true;
	m_params.m_bIncludeDirectLighting = settings.m_bIncludeDirectLighting;
	m_params.m_bIncludeEnvironment = settings.m_bIncludeSky;
	m_params.m_bIncludeEmissive = settings.m_bIncludeEmissive;
	const auto reportWarning = [&warning](const std::string& diagnostic)
	{
		if (warning)
		{
			warning(diagnostic);
			return;
		}
		SAILOR_LOG("[Warning] GI bake: %s", diagnostic.c_str());
	};
	const bool bHasGeometry = m_pathTracer.InitializeScene(
		instances,
		materials,
		bakedLights,
		false,
		progress,
		true,
		reportWarning);
	m_bInitialized = bHasGeometry;
	return m_bInitialized;
}

void GIProbesPathTracer::SetEnvironmentLinear(
	const TVector<glm::vec4>& image,
	const glm::uvec2& extent)
{
	m_pathTracer.SetRuntimeEnvironmentLinear(image, extent);
}

bool GIProbesPathTracer::SamplePrimaryDirection(
	const glm::vec3& uniformDirection,
	uint32_t sampleIndex,
	uint32_t sampleCount,
	uint32_t randomSeed,
	glm::vec3& outDirection,
	float& outPdf,
	std::string& outDiagnostic) const
{
	constexpr float UniformSpherePdf =
		0.07957747154594766788f;
	outDirection = uniformDirection;
	outPdf = UniformSpherePdf;
	if (!m_pathTracer.m_bUseRuntimeEnvironmentImportance ||
		sampleCount < 2u)
	{
		outDiagnostic.clear();
		return true;
	}

	const uint32_t importanceSampleCount = sampleCount / 2u;
	const uint32_t uniformSampleCount =
		sampleCount - importanceSampleCount;
	const float importanceFraction =
		static_cast<float>(importanceSampleCount) /
		static_cast<float>(sampleCount);
	const float uniformFraction =
		static_cast<float>(uniformSampleCount) /
		static_cast<float>(sampleCount);
	float directionImportancePdf = 0.0f;
	if ((sampleIndex & 1u) != 0u)
	{
		uint32_t importanceRandomState = randomSeed;
		if (!m_pathTracer.SampleRuntimeEnvironmentImportance(
				importanceRandomState,
				outDirection,
				directionImportancePdf))
		{
			outDiagnostic =
				"GI probe path tracer could not sample its HDR environment distribution";
			return false;
		}
	}
	else
	{
		directionImportancePdf =
			m_pathTracer.RuntimeEnvironmentImportancePdf(outDirection);
	}

	outPdf = uniformFraction * UniformSpherePdf +
		importanceFraction * directionImportancePdf;
	outDiagnostic.clear();
	return std::isfinite(outPdf) && outPdf > 0.0f;
}

bool GIProbesPathTracer::Sample(
	const glm::vec3& origin,
	const glm::vec3& direction,
	float maxDistance,
	uint32_t randomSeed,
	GIProbeBakeRaySample& outSample,
	std::string& outDiagnostic) const
{
	outSample = {};
	if (!m_bInitialized)
	{
		outDiagnostic = "GI probe path tracer has no prepared scene";
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
		outDiagnostic = "GI probe path tracer rejected a bake ray";
		return false;
	}
	outSample.m_radiance = sample.m_radiance;
	outSample.m_distance = sample.m_distance;
	outSample.m_bHit = sample.m_bHit;
	outSample.m_bBackFace = sample.m_bBackFace;
	outDiagnostic.clear();
	return true;
}

bool GIProbesPathTracer::SampleVisibility(
	const glm::vec3& origin,
	const glm::vec3& direction,
	float maxDistance,
	uint32_t,
	GIProbeBakeRaySample& outSample,
	std::string& outDiagnostic) const
{
	outSample = {};
	if (!m_bInitialized)
	{
		outDiagnostic = "GI probe path tracer has no prepared scene";
		return false;
	}

	PathTracer::PreparedRaySample sample;
	if (!m_pathTracer.SamplePreparedSceneVisibility(
			origin,
			direction,
			maxDistance,
			sample))
	{
		outDiagnostic = "GI probe path tracer rejected a visibility ray";
		return false;
	}
	outSample.m_distance = sample.m_distance;
	outSample.m_bHit = sample.m_bHit;
	outSample.m_bBackFace = sample.m_bBackFace;
	outDiagnostic.clear();
	return true;
}
