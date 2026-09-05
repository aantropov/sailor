#include "GlobalIllumination/GIProbesBaker.h"
#include "GlobalIllumination/GIProbesBinary.h"
#include "GlobalIllumination/GIProbesComposition.h"
#include "GlobalIllumination/GIProbesSampling.h"
#include "GlobalIllumination/GIProbesTracing.h"
#include "GlobalIllumination/RuntimeGIProbesService.h"
#include "AssetRegistry/Material/MaterialImporter.h"
#include "AssetRegistry/Texture/TextureImporter.h"
#include "Components/LightComponent.h"
#include "Components/Tests/GlobalIlluminationLandscapeTestScene.h"
#include "ECS/LightingECS.h"
#include "ECS/StaticMeshRendererECS.h"
#include "ECS/TransformECS.h"
#include "GlobalIllumination/GISettings.h"
#include "Engine/GameObject.h"
#include "Engine/World.h"
#include "Editor/GlobalIlluminationBakeController.h"
#include "Memory/ObjectAllocator.hpp"
#include "RHI/GlobalIllumination.h"
#include "Raytracing/LightingModel.h"
#include "Raytracing/PathTracer.h"
#include "Raytracing/GIProbesPathTracer.h"

#include <algorithm>
#include <array>
#include <bit>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <mutex>
#include <set>
#include <stdexcept>
#include <string>
#include <thread>

#include <glm/gtc/packing.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <yaml-cpp/yaml.h>

using namespace Sailor;

namespace
{
	class GlobalIlluminationMobilityTestWorld final : public World
	{
	public:
		GlobalIlluminationMobilityTestWorld() :
			World("GlobalIlluminationMobilityContractTests", 0, CreateEcs())
		{}

	private:
		static TVector<ECS::TBaseSystemPtr> CreateEcs()
		{
			TVector<ECS::TBaseSystemPtr> systems;
			systems.Add(TUniquePtr<TransformECS>::Make());
			systems.Add(TUniquePtr<StaticMeshRendererECS>::Make());
			systems.Add(TUniquePtr<LightingECS>::Make());
			return systems;
		}
	};

	const char* g_currentTestName = "startup";

	[[noreturn]] void ReportTermination() noexcept
	{
		std::cerr << "GlobalIlluminationContractTests terminated while running: " <<
			g_currentTestName << std::endl;
		std::_Exit(2);
	}

	void Require(bool condition, const std::string& message)
	{
		if (!condition)
		{
			throw std::runtime_error(message);
		}
	}

	std::string ReadContractText(const std::filesystem::path& path)
	{
		std::ifstream input(path, std::ios::binary);
		Require(input.is_open(),
			"GI framegraph contract should be readable: " + path.generic_string());
		return std::string(
			std::istreambuf_iterator<char>(input),
			std::istreambuf_iterator<char>());
	}

	std::string GetSequenceMapping(
		const YAML::Node& sequence,
		const char* key)
	{
		if (!sequence || !sequence.IsSequence())
		{
			return {};
		}
		for (const YAML::Node& entry : sequence)
		{
			const YAML::Node value = entry[key];
			if (value && value.IsScalar())
			{
				return value.as<std::string>();
			}
		}
		return {};
	}

	void TestGlobalIlluminationFrameGraphProbeCellContract()
	{
#if defined(SAILOR_TEST_SOURCE_DIR)
		const std::filesystem::path contentRoot =
			std::filesystem::path(SAILOR_TEST_SOURCE_DIR) / "Content";
		const char* rendererPaths[] =
		{
			"DefaultRenderer.renderer",
			"EditorRenderer.renderer"
		};
		const char* probeCellTarget =
			"GlobalIlluminationProbeCellIndices";

		for (const char* rendererPath : rendererPaths)
		{
			const YAML::Node renderer = YAML::Load(
				ReadContractText(contentRoot / rendererPath));
			const YAML::Node renderTargets = renderer["renderTargets"];
			const YAML::Node frame = renderer["frame"];
			Require(renderTargets && renderTargets.IsSequence() &&
				frame && frame.IsSequence(),
				std::string(rendererPath) +
					" should expose render-target and frame sequences");

			YAML::Node target;
			for (const YAML::Node& candidate : renderTargets)
			{
				if (candidate["name"] &&
					candidate["name"].as<std::string>() == probeCellTarget)
				{
					target = candidate;
					break;
				}
			}
			Require(target &&
				target["format"].as<std::string>() ==
					"R32G32B32A32_SFLOAT" &&
				target["width"].as<std::string>() == "RenderWidth/2" &&
				target["height"].as<std::string>() == "RenderHeight/2" &&
				target["bIsCompatibleWithComputeShaders"].as<bool>(),
				std::string(rendererPath) +
					" must define the quarter-area four-subpixel probe-cell index target");
			YAML::Node depthBuffer;
			for (const YAML::Node& candidate : renderTargets)
			{
				if (candidate["name"] &&
					candidate["name"].as<std::string>() == "DepthBuffer")
				{
					depthBuffer = candidate;
					break;
				}
			}
			Require(depthBuffer &&
				depthBuffer["format"].as<std::string>() ==
					"D32_SFLOAT_S8_UINT" &&
				depthBuffer["width"].as<std::string>() == "RenderWidth" &&
				depthBuffer["height"].as<std::string>() == "RenderHeight",
				std::string(rendererPath) +
					" GI resolve must retain full-resolution scene depth");

			uint32_t resolvePasses = 0u;
			uint32_t mainConsumers = 0u;
			size_t resolvePassIndex = frame.size();
			size_t firstMainConsumerIndex = frame.size();
			for (size_t passIndex = 0u; passIndex < frame.size(); ++passIndex)
			{
				const YAML::Node pass = frame[passIndex];
				const std::string name = pass["name"] ?
					pass["name"].as<std::string>() : std::string{};
				const YAML::Node attachments = pass["renderTargets"];
				if (name == "GlobalIlluminationResolve")
				{
					++resolvePasses;
					resolvePassIndex = passIndex;
					Require(
						GetSequenceMapping(attachments, "depthSampler") == "DepthBuffer" &&
						GetSequenceMapping(attachments, "probeCellIndices") ==
							probeCellTarget,
						std::string(rendererPath) +
							" GI resolve must write the packed subpixel probe-cell indices");
					continue;
				}

				const std::string tag =
					GetSequenceMapping(pass["string"], "Tag");
				if (name != "RenderScene")
				{
					continue;
				}
				if (tag == "Opaque" || tag == "Masked")
				{
					++mainConsumers;
					firstMainConsumerIndex =
						(std::min)(firstMainConsumerIndex, passIndex);
					Require(
						GetSequenceMapping(
							attachments,
							"globalIlluminationProbeCellIndicesSampler") ==
								probeCellTarget &&
						GetSequenceMapping(
							attachments,
							"globalIlluminationDepthSampler").empty(),
						std::string(rendererPath) + " " + tag +
							" must consume the authoritative probe-cell set without a full-resolution depth binding");
				}
				else
				{
					Require(
						GetSequenceMapping(
							attachments,
							"globalIlluminationProbeCellIndicesSampler").empty(),
						std::string(rendererPath) + " " + tag +
							" must not consume the main-pass probe-cell targets");
				}
			}

			Require(resolvePasses == 1u && mainConsumers == 2u &&
				resolvePassIndex < firstMainConsumerIndex,
				std::string(rendererPath) +
					" must resolve GI once before opaque and masked consumers");
		}
#endif
	}

	void TestPhysicalHdrFrameGraphContract()
	{
#if defined(SAILOR_TEST_SOURCE_DIR)
		const std::filesystem::path contentRoot =
			std::filesystem::path(SAILOR_TEST_SOURCE_DIR) / "Content";
		const char* rendererPaths[] =
		{
			"DefaultRenderer.renderer",
			"EditorRenderer.renderer"
		};
		for (const char* rendererPath : rendererPaths)
		{
			const YAML::Node renderer = YAML::Load(
				ReadContractText(contentRoot / rendererPath));
			const YAML::Node renderTargets = renderer["renderTargets"];
			const YAML::Node frame = renderer["frame"];

			YAML::Node averageLuminance;
			for (const YAML::Node& target : renderTargets)
			{
				if (target["name"] &&
					target["name"].as<std::string>() == "AverageLuminance")
				{
					averageLuminance = target;
					break;
				}
			}
			Require(
				averageLuminance &&
					averageLuminance["format"].as<std::string>() == "R32_SFLOAT" &&
					averageLuminance["width"].as<std::string>() == "1" &&
					averageLuminance["height"].as<std::string>() == "1" &&
					averageLuminance["bIsCompatibleWithComputeShaders"].as<bool>(),
				std::string(rendererPath) +
					" must keep adapted luminance in a sampled R32 compute target");

			size_t eyeAdaptationIndex = frame.size();
			size_t bloomIndex = frame.size();
			size_t tonemappingIndex = frame.size();
			uint32_t eyeAdaptationCount = 0u;
			uint32_t bloomCount = 0u;
			uint32_t tonemappingCount = 0u;
			for (size_t passIndex = 0u; passIndex < frame.size(); ++passIndex)
			{
				const YAML::Node pass = frame[passIndex];
				const std::string name = pass["name"] ?
					pass["name"].as<std::string>() : std::string{};
				const std::string tag = pass["tag"] ?
					pass["tag"].as<std::string>() : std::string{};
				const YAML::Node attachments = pass["renderTargets"];
				if (name == "EyeAdaptation")
				{
					++eyeAdaptationCount;
					eyeAdaptationIndex = passIndex;
					Require(
						GetSequenceMapping(attachments, "hdrColor") == "Main" &&
							GetSequenceMapping(attachments, "averageLuminance") ==
								"AverageLuminance",
						std::string(rendererPath) +
							" must meter the unmodified HDR scene into AverageLuminance");
				}
				else if (name == "Bloom")
				{
					++bloomCount;
					bloomIndex = passIndex;
					Require(
						GetSequenceMapping(attachments, "bloom") == "Main" &&
							GetSequenceMapping(
								attachments,
								"averageLuminanceSampler") == "AverageLuminance",
						std::string(rendererPath) +
							" bloom must threshold in the metered scene space");
				}
				else if (name == "PostProcess" && tag == "Tonemapping")
				{
					++tonemappingCount;
					tonemappingIndex = passIndex;
					Require(
						GetSequenceMapping(pass["string"], "shader") ==
							"Shaders/Tonemapping.shader" &&
							GetSequenceMapping(attachments, "color") == "Secondary" &&
							GetSequenceMapping(attachments, "colorSampler") == "Main" &&
							GetSequenceMapping(
								attachments,
								"averageLuminanceSampler") == "AverageLuminance",
						std::string(rendererPath) +
							" tonemapping must consume the bloomed HDR scene and its meter");
				}
			}

			Require(
				eyeAdaptationCount == 1u &&
					bloomCount == 1u &&
					tonemappingCount == 1u &&
					eyeAdaptationIndex < bloomIndex &&
					bloomIndex < tonemappingIndex,
				std::string(rendererPath) +
					" must execute HDR metering, bloom, and tonemapping in that order");
		}
#endif
	}

	bool IsNear(float lhs, float rhs, float tolerance = 0.0001f)
	{
		return std::abs(lhs - rhs) <= tolerance;
	}

	void TestPhysicalDielectricTransport()
	{
		Require(IsNear(
				Raytracing::LightingModel::FresnelDielectric(
					1.0f,
					1.0f,
					1.5f),
				0.04f,
				0.00001f),
			"air-to-glass normal-incidence Fresnel reflectance must be 4 percent");
		Require(IsNear(
				Raytracing::LightingModel::FresnelDielectric(
					0.0f,
					1.0f,
					1.0f),
				0.0f),
			"an interface between matching IORs must not create a false reflection");
		Require(IsNear(
				Raytracing::LightingModel::FresnelDielectric(
					0.5f,
					1.5f,
					1.0f),
				1.0f),
				"glass-to-air transport above the critical angle must be total internal reflection");
		Require(IsNear(
				Raytracing::LightingModel::FresnelDielectric(
					1.0f,
					1.0f,
					1000000.0f),
				1.0f),
			"the glTF infinite-IOR compatibility value must reflect all incident energy");

		Raytracing::LightingModel::SampledData glass;
		glass.m_baseColor = glm::vec4(1.0f);
		glass.m_orm = glm::vec3(1.0f, 0.1f, 0.0f);
		glass.m_ior = 1.5f;
		glass.m_thicknessFactor = 1.0f;
		glass.m_transmission = 1.0f;
		const glm::vec3 normal(0.0f, 1.0f, 0.0f);
		const glm::vec3 normalView = normal;

		const auto sampleInterface = [&glass, &normal](
			const glm::vec3& viewDirection,
			float fromIor,
			float toIor,
			glm::vec2 randomSample)
		{
			struct Result
			{
				glm::vec3 m_term{};
				glm::vec3 m_direction{};
				float m_pdf = 0.0f;
				bool m_bTransmission = false;
				bool m_bSampled = false;
			};
			Result result;
			result.m_bSampled = Raytracing::LightingModel::Sample(
				glass,
				normal,
				viewDirection,
				fromIor,
				toIor,
				result.m_term,
				result.m_pdf,
				result.m_bTransmission,
				result.m_direction,
				randomSample,
				glm::vec2(0.999f, 0.5f));
			return result;
		};

		const auto entering = sampleInterface(
			normalView,
			1.0f,
			1.5f,
			glm::vec2(0.0f));
		const auto exiting = sampleInterface(
			normalView,
			1.5f,
			1.0f,
			glm::vec2(0.0f));
		Require(
			entering.m_bSampled && entering.m_bTransmission &&
			exiting.m_bSampled && exiting.m_bTransmission &&
			entering.m_pdf > 0.0f && exiting.m_pdf > 0.0f &&
			glm::dot(entering.m_direction, normal) < -0.999f &&
			glm::dot(exiting.m_direction, normal) < -0.999f,
			"a normal-incidence thick dielectric path must refract through both interfaces");
		Require(
			IsNear(entering.m_term.r, 1.0f / 2.25f, 0.001f) &&
				IsNear(exiting.m_term.r, 2.25f, 0.005f) &&
				IsNear(
					entering.m_term.r * exiting.m_term.r,
					1.0f,
					0.005f),
			"radiance-mode refraction must apply reciprocal eta-squared factors instead of unit throughput");

		glass.m_ior = 1.0f;
		const auto matchedIor = sampleInterface(
			normalView,
			1.0f,
			1.0f,
			glm::vec2(0.0f));
		Require(
			matchedIor.m_bSampled && matchedIor.m_bTransmission &&
			glm::length(matchedIor.m_direction + normalView) <= 0.0001f &&
			glm::length(matchedIor.m_term - glm::vec3(1.0f)) <= 0.0001f,
			"matching IORs must pass radiance straight through without a singular microfacet PDF");

		glass.m_ior = 1.5f;
		glass.m_orm.y = 0.6f;
		const glm::vec3 obliqueView = glm::normalize(
			glm::vec3(0.6f, 0.8f, 0.0f));
		bool bFoundFiniteTirReflection = false;
		for (uint32_t y = 1u; y < 32u && !bFoundFiniteTirReflection; ++y)
		{
			for (uint32_t x = 0u; x < 32u; ++x)
			{
				const auto tir = sampleInterface(
					obliqueView,
					1.5f,
					1.0f,
					glm::vec2(
						(static_cast<float>(x) + 0.5f) / 32.0f,
						(static_cast<float>(y) + 0.5f) / 32.0f));
				bFoundFiniteTirReflection =
					tir.m_bSampled && !tir.m_bTransmission &&
					tir.m_pdf > 0.0f && std::isfinite(tir.m_pdf) &&
					std::isfinite(tir.m_term.x) &&
					std::isfinite(tir.m_term.y) &&
					std::isfinite(tir.m_term.z) &&
					glm::dot(tir.m_direction, normal) > 0.0f;
				if (bFoundFiniteTirReflection)
				{
					break;
				}
			}
		}
		Require(bFoundFiniteTirReflection,
			"a transmission sample that reaches total internal reflection must become a finite reflected path");
	}

	void TestLayeredGltfTransport()
	{
		const glm::vec3 normal(0.0f, 1.0f, 0.0f);
		const glm::vec3 viewDirection = normal;
		const glm::vec3 lightDirection = normal;

		Raytracing::LightingModel::SampledData metallicRoughness;
		metallicRoughness.m_baseColor =
			glm::vec4(0.72f, 0.41f, 0.18f, 1.0f);
		metallicRoughness.m_orm = glm::vec3(1.0f, 0.47f, 0.0f);
		metallicRoughness.m_ior = 1.5f;
		const glm::vec3 mixedView = glm::normalize(
			glm::vec3(0.6f, 0.8f, 0.0f));
		const glm::vec3 mixedLight = glm::normalize(
			glm::vec3(-0.2f, 0.98f, 0.0f));
		const glm::vec3 dielectricResponse =
			Raytracing::LightingModel::CalculateBRDFCosineWeighted(
				mixedView,
				normal,
				mixedLight,
				metallicRoughness,
				1.0f,
				1.5f);
		metallicRoughness.m_orm.z = 1.0f;
		const glm::vec3 metalResponse =
			Raytracing::LightingModel::CalculateBRDFCosineWeighted(
				mixedView,
				normal,
				mixedLight,
				metallicRoughness,
				1.0f,
				1.5f);
		metallicRoughness.m_orm.z = 0.5f;
		const glm::vec3 mixedResponse =
			Raytracing::LightingModel::CalculateBRDFCosineWeighted(
				mixedView,
				normal,
				mixedLight,
				metallicRoughness,
				1.0f,
				1.5f);
		Require(
			glm::length(mixedResponse -
				glm::mix(dielectricResponse, metalResponse, 0.5f)) <=
				0.00001f,
			"intermediate metalness must linearly mix the complete dielectric and metallic BRDF endpoints");

		const float grazingSheenVisibility =
			Raytracing::LightingModel::VisibilitySheen(
				0.05f,
				0.05f,
				0.5f);
		Require(
			IsNear(grazingSheenVisibility, 2.3525f, 0.001f),
			"Charlie visibility must retain its finite grazing response instead of clipping the sheen BRDF");

		Raytracing::LightingModel::SampledData clearcoat;
		clearcoat.m_baseColor = glm::vec4(0.0f, 0.0f, 0.0f, 1.0f);
		clearcoat.m_orm = glm::vec3(1.0f, 0.45f, 0.0f);
		clearcoat.m_ior = 1.0f;
		clearcoat.m_clearcoatNormal = normal;
		clearcoat.m_clearcoatFactor = 1.0f;
		clearcoat.m_clearcoatRoughness = 0.35f;
		clearcoat.m_emissive = glm::vec3(10.0f, 5.0f, 2.0f);

		const glm::vec3 coatedResponse =
			Raytracing::LightingModel::CalculateBRDFCosineWeighted(
				viewDirection,
				normal,
				lightDirection,
				clearcoat,
				1.0f,
				1.0f);
		const glm::vec3 coatedEmission =
			Raytracing::LightingModel::CalculateEmittedRadiance(
				clearcoat,
				normal,
				viewDirection);
		Require(
			coatedResponse.r > 0.0f &&
			IsNear(coatedResponse.r, coatedResponse.g, 0.00001f) &&
			IsNear(coatedResponse.g, coatedResponse.b, 0.00001f),
			"a clearcoat layer over a non-reflecting base must produce a neutral microfacet reflection");
		Require(
			glm::length(coatedEmission -
				glm::vec3(9.6f, 4.8f, 1.92f)) <= 0.0001f,
			"normal-incidence clearcoat must transmit 96 percent of base emission through its Fresnel layer");

		const glm::vec3 obliqueView = glm::normalize(
			glm::vec3(0.8f, 0.6f, 0.0f));
		const glm::vec3 obliqueHalfVector = glm::normalize(
			obliqueView + lightDirection);
		const float obliqueViewCosine = glm::dot(normal, obliqueView);
		const float obliqueLightCosine = glm::dot(normal, lightDirection);
		const float obliqueViewHalfCosine = glm::abs(glm::dot(
			obliqueView,
			obliqueHalfVector));
		const float expectedClearcoatWeight =
			Raytracing::LightingModel::FresnelSchlick(
				obliqueViewHalfCosine,
				glm::vec3(0.04f)).x;
		const float expectedClearcoatResponse = expectedClearcoatWeight *
			Raytracing::LightingModel::DistributionGGX(
				normal,
				obliqueHalfVector,
				clearcoat.m_clearcoatRoughness) *
			Raytracing::LightingModel::GeometrySmithGGXCorrelated(
				obliqueLightCosine,
				obliqueViewCosine,
				clearcoat.m_clearcoatRoughness) /
			(4.0f * obliqueLightCosine * obliqueViewCosine) *
			obliqueLightCosine;
		const glm::vec3 obliqueClearcoatResponse =
			Raytracing::LightingModel::CalculateBRDFCosineWeighted(
				obliqueView,
				normal,
				lightDirection,
				clearcoat,
				1.0f,
				1.0f);
		Require(
			IsNear(
				obliqueClearcoatResponse.r,
				expectedClearcoatResponse,
				0.00001f),
			"direct clearcoat attenuation must use the per-light view-half Fresnel term");

		Raytracing::LightingModel::SampledData sheen;
		sheen.m_baseColor = glm::vec4(0.0f, 0.0f, 0.0f, 1.0f);
		sheen.m_orm = glm::vec3(1.0f, 0.7f, 0.0f);
		sheen.m_ior = 1.0f;
		sheen.m_sheenColor = glm::vec3(0.8f, 0.1f, 0.0f);
		sheen.m_sheenRoughness = 0.65f;
		const glm::vec3 grazingView = glm::normalize(
			glm::vec3(0.95f, 0.31225f, 0.0f));
		const glm::vec3 grazingLight = grazingView;
		const glm::vec3 sheenResponse =
			Raytracing::LightingModel::CalculateBRDFCosineWeighted(
				grazingView,
				normal,
				grazingLight,
				sheen,
				1.0f,
				1.0f);
		Require(
			sheenResponse.r > sheenResponse.g * 4.0f &&
			sheenResponse.g > 0.0f &&
			IsNear(sheenResponse.b, 0.0f, 0.000001f),
			"Charlie sheen must preserve its authored color in the grazing reflection lobe");

		Raytracing::LightingModel::SampledData layered;
		layered.m_baseColor = glm::vec4(0.45f, 0.32f, 0.18f, 1.0f);
		layered.m_orm = glm::vec3(1.0f, 0.55f, 0.0f);
		layered.m_ior = 1.5f;
		layered.m_sheenColor = glm::vec3(0.3f, 0.12f, 0.04f);
		layered.m_sheenRoughness = 0.7f;
		layered.m_clearcoatNormal = normal;
		layered.m_clearcoatFactor = 0.65f;
		layered.m_clearcoatRoughness = 0.4f;
		uint32_t finiteSamples = 0u;
		glm::vec3 accumulatedThroughput(0.0f);
		for (uint32_t sampleIndex = 0u; sampleIndex < 2048u;
			++sampleIndex)
		{
			const glm::vec2 randomSample(
				(static_cast<float>(sampleIndex % 64u) + 0.5f) / 64.0f,
				(static_cast<float>(sampleIndex / 64u) + 0.5f) / 32.0f);
			const glm::vec2 selectionSample(
				(static_cast<float>(sampleIndex) + 0.5f) / 2048.0f,
				0.5f);
			glm::vec3 term{};
			glm::vec3 direction{};
			float pdf = 0.0f;
			bool bTransmission = false;
			if (!Raytracing::LightingModel::Sample(
					layered,
					normal,
					viewDirection,
					1.0f,
					1.5f,
					term,
					pdf,
					bTransmission,
					direction,
					randomSample,
					selectionSample))
			{
				continue;
			}
			const float evaluatedPdf =
				Raytracing::LightingModel::ReflectionPdf(
					layered,
					normal,
					viewDirection,
					direction);
			Require(
				!bTransmission && pdf > 0.0f &&
				std::isfinite(pdf) &&
				IsNear(pdf, evaluatedPdf, 0.00001f) &&
				std::isfinite(term.x) &&
				std::isfinite(term.y) &&
				std::isfinite(term.z) &&
				glm::all(glm::greaterThanEqual(
					term,
					glm::vec3(0.0f))),
				"layered clearcoat and sheen samples must report the finite mixture PDF used by their throughput");
			accumulatedThroughput += term;
			++finiteSamples;
		}
		Require(
			finiteSamples > 1500u &&
			std::isfinite(accumulatedThroughput.x) &&
			std::isfinite(accumulatedThroughput.y) &&
			std::isfinite(accumulatedThroughput.z) &&
			glm::dot(accumulatedThroughput, glm::vec3(1.0f)) > 0.0f,
			"the layered glTF BSDF must sample all reflective energy without collapsing into rejected or invalid paths");
	}

	template<typename TTest>
	void RunTest(const char* name, TTest&& test)
	{
		g_currentTestName = name;
		std::cout << "[ RUN      ] " << name << std::endl;
		test();
		std::cout << "[       OK ] " << name << std::endl;
	}

	GIProbesData MakeVolume(float coefficient, uint64_t lightingHash)
	{
		GIProbesData data;
		data.m_volumeMin = glm::vec3(0.0f);
		data.m_volumeMax = glm::vec3(1.0f);
		data.m_transportHash = 0x12345678ull;
		data.m_lightingHash = lightingHash;
		data.m_sourceWorldHash = 0xaabbccddull;
		data.m_stateName = "Test State";
		data.m_bakerVersion = "GlobalIlluminationContractTests";
		data.m_diagnostics.m_averageValidity = 1.0f;

		GIProbeBrick brick;
		brick.m_min = data.m_volumeMin;
		brick.m_max = data.m_volumeMax;
		brick.m_probeCounts = glm::uvec3(2u);
		brick.m_probeCount = 8u;
		data.m_bricks.Add(brick);

		for (uint32_t z = 0u; z < 2u; ++z)
		{
			for (uint32_t y = 0u; y < 2u; ++y)
			{
				for (uint32_t x = 0u; x < 2u; ++x)
				{
					GIProbe probe;
					probe.m_position = glm::vec3(x, y, z);
					probe.m_irradiance[0] = glm::vec3(coefficient);
					for (glm::vec2& visibility : probe.m_visibility)
					{
						visibility = glm::vec2(100.0f, 10000.0f);
					}
					for (uint32_t directionIndex = 0u;
						directionIndex < GIProbeVisibilityDirectionCount;
						++directionIndex)
					{
						probe.m_environmentVisibility[directionIndex] =
							0.1f * static_cast<float>(directionIndex + 1u);
					}
					data.m_probes.Add(std::move(probe));
				}
			}
		}
		data.m_layoutHash = ComputeGIProbesLayoutHash(data);
		data.m_representationHash = ComputeGIProbesRepresentationHash(
			data.m_formatVersion,
			data.m_shOrder,
			data.m_compression);
		return data;
	}

	GIProbesData MakeVisualVolume(
		const char* stateName,
		const glm::vec3& irradiance,
		uint64_t lightingHash)
	{
		GIProbesData data = MakeVolume(0.0f, lightingHash);
		data.m_stateName = stateName;
		data.m_bakerVersion = "Sailor visual-test fixture generator";
		data.m_sourceWorldHash = 0x155155155ull;
		data.m_volumeMin = glm::vec3(-90.0f, -15.0f, -90.0f);
		data.m_volumeMax = glm::vec3(90.0f, 90.0f, 90.0f);
		data.m_bakeSettings.m_raysPerProbe = 64u;
		data.m_bakeSettings.m_bounceCount = 2u;
		data.m_bakeSettings.m_randomSeed = 155u;
		data.m_bakeSettings.m_maxSubdivisionLevel = 0u;
		data.m_bakeSettings.m_minProbeSpacing = 90.0f;
		data.m_bakeSettings.m_maxRayDistance = 500.0f;
		data.m_bricks[0].m_min = data.m_volumeMin;
		data.m_bricks[0].m_max = data.m_volumeMax;
		constexpr float ShConstant = 0.2820947918f;
		for (uint32_t z = 0u; z < 2u; ++z)
		{
			for (uint32_t y = 0u; y < 2u; ++y)
			{
				for (uint32_t x = 0u; x < 2u; ++x)
				{
					const uint32_t index = x + 2u * (y + 2u * z);
					auto& probe = data.m_probes[index];
					probe.m_position = glm::mix(
						data.m_volumeMin,
						data.m_volumeMax,
						glm::vec3(x, y, z));
					probe.m_irradiance = {};
					probe.m_irradiance[0] = irradiance / ShConstant;
					for (glm::vec2& visibility : probe.m_visibility)
					{
						visibility = glm::vec2(500.0f, 250000.0f);
					}
				}
			}
		}
		data.m_layoutHash = ComputeGIProbesLayoutHash(data);
		return data;
	}

	MaterialPtr MakeDiffuseFixtureMaterial(
		const Memory::ObjectAllocatorPtr& allocator,
		const glm::vec3& color)
	{
		MaterialPtr material = MaterialPtr::Make(allocator, FileId::Invalid);
		material->SetUniform(
			"material.baseColorFactor",
			glm::vec4(color, 1.0f));
		material->SetUniform("material.metallicFactor", 0.0f);
		material->SetUniform("material.roughnessFactor", 1.0f);
		return material;
	}

	class CpuTextureFixture final : public Texture
	{
	public:
		explicit CpuTextureFixture(FileId fileId) : Texture(fileId) {}

		void SetPixel(const glm::u8vec4& pixel)
		{
			m_width = 1;
			m_height = 1;
			m_mipLevels = 1u;
			m_decodedData.Resize(4u);
			for (glm::length_t component = 0; component < 4; ++component)
			{
				m_decodedData[component] = pixel[component];
			}
		}
	};

	struct EveningLandscapeRaytracingFixture final
	{
		Memory::ObjectAllocatorPtr m_allocator{};
		TVector<MaterialPtr> m_materials{};
		TVector<Raytracing::PathTracer::TLASInstance> m_instances{};
		TVector<Raytracing::LightProxy> m_lights{};
		TSharedPtr<TVector<Math::Triangle>> m_triangles{};
		TSharedPtr<Raytracing::BVH> m_blas{};
		Math::AABB m_bounds{};
	};

	EveningLandscapeRaytracingFixture
	MakeEveningLandscapeRaytracingFixture()
	{
		using namespace GlobalIlluminationLandscapeTestScene;
		EveningLandscapeRaytracingFixture fixture;
		fixture.m_allocator = Memory::ObjectAllocatorPtr::Make(
			Memory::EAllocationPolicy::SharedMemory_MultiThreaded);
		fixture.m_materials.Add(MakeDiffuseFixtureMaterial(
			fixture.m_allocator,
			glm::vec3(0.34f, 0.40f, 0.24f)));
		fixture.m_materials.Add(MakeDiffuseFixtureMaterial(
			fixture.m_allocator,
			glm::vec3(0.10f, 0.12f, 0.16f)));
		fixture.m_materials.Add(MakeDiffuseFixtureMaterial(
			fixture.m_allocator,
			glm::vec3(0.92f, 0.24f, 0.055f)));
		fixture.m_materials.Add(MakeDiffuseFixtureMaterial(
			fixture.m_allocator,
			glm::vec3(0.56f, 0.66f, 0.78f)));

		TVector<Math::Triangle> triangles;
		BuildBakeTriangles(triangles, fixture.m_bounds);
		fixture.m_triangles =
			TSharedPtr<TVector<Math::Triangle>>::Make(std::move(triangles));
		fixture.m_blas = TSharedPtr<Raytracing::BVH>::Make(
			static_cast<uint32_t>(fixture.m_triangles->Num()));
		BuildBakeBlas(*fixture.m_blas, *fixture.m_triangles);

		Raytracing::PathTracer::TLASInstance instance;
		instance.m_triangles = fixture.m_triangles;
		instance.m_blas = fixture.m_blas;
		instance.m_worldBounds = fixture.m_bounds;
		instance.m_worldMatrix = glm::mat4(1.0f);
		instance.m_inverseWorldMatrix = glm::mat4(1.0f);
		instance.m_materialBaseOffset = 0;
		fixture.m_instances.Add(std::move(instance));

		Raytracing::LightProxy light;
		light.m_type = ELightType::Directional;
		light.m_direction = GetEveningLightDirection();
		light.m_intensity = GetEveningSunIlluminance();
		light.m_indirectLightingIntensity = 1.0f;
		fixture.m_lights.Add(std::move(light));
		return fixture;
	}

	GIProbesData MakeEveningLandscapeBounceVolume()
	{
		EveningLandscapeRaytracingFixture fixture =
			MakeEveningLandscapeRaytracingFixture();
		GIProbesBakeSettings settings;
		settings.m_raysPerProbe = 256u;
		settings.m_bounceCount = 3u;
		settings.m_randomSeed = 155u;
		settings.m_maxSubdivisionLevel = 3u;
		settings.m_minProbeSpacing = 5.0f;
		settings.m_normalBias = 0.04f;
		settings.m_viewBias = 0.02f;
		settings.m_maxRayDistance = 120.0f;
		settings.m_bIncludeSky = false;
		settings.m_bIncludeEmissive = false;
		settings.m_bIncludeDirectLighting = true;

		Raytracing::GIProbesPathTracer pathTracer;
		Require(pathTracer.Initialize(
				fixture.m_instances,
				fixture.m_materials,
				fixture.m_lights,
				settings,
				glm::vec3(0.0f)),
			"the evening landscape fixture must initialize the CPU path tracer");

		GIProbesBakeRequest request;
		request.m_stateName = "Evening Landscape Bounce";
		request.m_bakerVersion =
			"Sailor deterministic evening-landscape visual fixture/1";
		request.m_volumeMin = glm::vec3(-22.0f, -6.0f, -18.0f);
		request.m_volumeMax = glm::vec3(22.0f, 16.0f, 18.0f);
		request.m_settings = settings;
		request.m_sceneGeometryBounds.Add(fixture.m_bounds);
		request.m_sourceWorldHash = 0x155e11e71a9d5ca3ull;
		GIProbesBakeResult result = GIProbesBaker::Bake(request, pathTracer);
		Require(result.IsSuccess(),
			"the evening landscape visual fixture must bake: " +
				result.m_diagnostic);
		result.m_data->m_diagnostics.m_message =
			"Evening landscape: the receiver's sun ray is occluded by the ridge; "
			"stored irradiance comes from reflected light, including the sunlit cliff.";
		result.m_data->m_diagnostics.m_bakeDurationSeconds = 0.0f;
		std::string diagnostic;
		Require(result.m_data->Validate(diagnostic),
			"the generated evening landscape state must validate: " + diagnostic);
		return std::move(*result.m_data);
	}

	int GenerateVisualAssets(const std::filesystem::path& outputDirectory)
	{
		std::error_code error;
		std::filesystem::create_directories(outputDirectory, error);
		if (error)
		{
			std::cerr << "Cannot create visual fixture directory: " <<
				error.message() << std::endl;
			return 1;
		}
		struct Fixture final
		{
			const char* m_filename = nullptr;
			const char* m_stateName = nullptr;
			glm::vec3 m_irradiance{};
			uint64_t m_lightingHash = 0u;
		};
		const Fixture fixtures[] = {
			{ "Day.probes", "Day", glm::vec3(1.15f, 0.95f, 0.72f), 0x1001u },
			{ "Evening.probes", "Evening", glm::vec3(1.0f, 0.30f, 0.10f), 0x1002u },
			{ "Night.probes", "Night", glm::vec3(0.08f, 0.16f, 0.52f), 0x1003u },
			{ "Lamps.probes", "Lamps", glm::vec3(0.28f, 0.12f, 0.025f), 0x1004u }
		};
		for (const Fixture& fixture : fixtures)
		{
			std::string diagnostic;
			const std::filesystem::path outputPath =
				outputDirectory / fixture.m_filename;
			if (!GIProbesBinary::SaveAtomic(
					outputPath,
					MakeVisualVolume(
						fixture.m_stateName,
						fixture.m_irradiance,
						fixture.m_lightingHash),
					diagnostic))
			{
				std::cerr << "Cannot generate " << fixture.m_filename << ": " <<
					diagnostic << std::endl;
				return 1;
			}
			const GIProbesBinaryResult loaded =
				GIProbesBinary::Load(outputPath);
			if (!loaded.IsSuccess() ||
				loaded.m_data->m_stateName != fixture.m_stateName ||
				loaded.m_data->m_lightingHash != fixture.m_lightingHash)
			{
				std::cerr << "Cannot verify " << fixture.m_filename << ": " <<
					loaded.m_diagnostic << std::endl;
				return 1;
			}
		}

		const std::filesystem::path eveningLandscapePath =
			outputDirectory / "EveningLandscapeBounce.probes";
		std::string diagnostic;
		if (!GIProbesBinary::SaveAtomic(
				eveningLandscapePath,
				MakeEveningLandscapeBounceVolume(),
				diagnostic))
		{
			std::cerr << "Cannot generate EveningLandscapeBounce.probes: " <<
				diagnostic << std::endl;
			return 1;
		}
		const GIProbesBinaryResult eveningLandscape =
			GIProbesBinary::Load(eveningLandscapePath);
		if (!eveningLandscape.IsSuccess() ||
			eveningLandscape.m_data->m_stateName !=
				"Evening Landscape Bounce" ||
			eveningLandscape.m_data->m_probes.Num() < 64u)
		{
			std::cerr << "Cannot verify EveningLandscapeBounce.probes: " <<
				eveningLandscape.m_diagnostic << std::endl;
			return 1;
		}
		std::cout << "Generated deterministic GI visual fixtures in " <<
			outputDirectory << std::endl;
		return 0;
	}

	FileId ParseFileId(const char* value)
	{
		FileId result;
		result.Deserialize(YAML::Node(value));
		return result;
	}

	class ConstantBakeRaySampler final : public IGIProbeBakeRaySampler
	{
	public:
		explicit ConstantBakeRaySampler(glm::vec3 radiance) :
			m_radiance(radiance)
		{}

		bool Sample(
			const glm::vec3&,
			const glm::vec3&,
			float maxDistance,
			uint32_t,
			GIProbeBakeRaySample& outSample,
			std::string&) const override
		{
			m_irradianceSampleCount.fetch_add(1u, std::memory_order_relaxed);
			m_lastMaxDistance.store(maxDistance, std::memory_order_relaxed);
			outSample.m_radiance = m_radiance;
			outSample.m_distance = maxDistance;
			outSample.m_bHit = false;
			return true;
		}

		bool SampleVisibility(
			const glm::vec3&,
			const glm::vec3&,
			float maxDistance,
			uint32_t,
			GIProbeBakeRaySample& outSample,
			std::string&) const override
		{
			m_visibilitySampleCount.fetch_add(1u, std::memory_order_relaxed);
			m_lastMaxDistance.store(maxDistance, std::memory_order_relaxed);
			outSample.m_radiance = m_radiance;
			outSample.m_distance = maxDistance;
			outSample.m_bHit = false;
			return true;
		}

		float GetLastMaxDistance() const noexcept
		{
			return m_lastMaxDistance.load(std::memory_order_relaxed);
		}

		uint64_t GetIrradianceSampleCount() const noexcept
		{
			return m_irradianceSampleCount.load(std::memory_order_relaxed);
		}

		uint64_t GetVisibilitySampleCount() const noexcept
		{
			return m_visibilitySampleCount.load(std::memory_order_relaxed);
		}

	private:
		glm::vec3 m_radiance{};
		mutable std::atomic<float> m_lastMaxDistance{ 0.0f };
		mutable std::atomic<uint64_t> m_irradianceSampleCount{ 0u };
		mutable std::atomic<uint64_t> m_visibilitySampleCount{ 0u };
	};

	class SeedDrivenBakeRaySampler final : public IGIProbeBakeRaySampler
	{
	public:
		bool Sample(
			const glm::vec3&,
			const glm::vec3&,
			float maxDistance,
			uint32_t randomSeed,
			GIProbeBakeRaySample& outSample,
			std::string&) const override
		{
			const float value = static_cast<float>(randomSeed & 0xffffu) /
				65535.0f;
			outSample.m_radiance = glm::vec3(
				value,
				value * value,
				1.0f - value);
			outSample.m_distance = maxDistance;
			outSample.m_bHit = false;
			return true;
		}
	};

	class FailingBakeRaySampler final : public IGIProbeBakeRaySampler
	{
	public:
		bool Sample(
			const glm::vec3&,
			const glm::vec3&,
			float,
			uint32_t,
			GIProbeBakeRaySample&,
			std::string& outDiagnostic) const override
		{
			outDiagnostic = "intentional runtime GI sampler failure";
			return false;
		}
	};

	class CancellingBakeRaySampler final : public IGIProbeBakeRaySampler
	{
	public:
		explicit CancellingBakeRaySampler(std::atomic<bool>& cancel) :
			m_cancel(cancel)
		{}

		bool Sample(
			const glm::vec3&,
			const glm::vec3&,
			float maxDistance,
			uint32_t,
			GIProbeBakeRaySample& outSample,
			std::string& outDiagnostic) const override
		{
			outSample.m_radiance = glm::vec3(1.0f);
			outSample.m_distance = maxDistance;
			outSample.m_bHit = false;
			outDiagnostic.clear();
			if (++m_sampleCount == 2u)
			{
				m_cancel.store(true, std::memory_order_release);
			}
			return true;
		}

	private:
		std::atomic<bool>& m_cancel;
		mutable uint32_t m_sampleCount = 0u;
	};

	class PdfWeightedPrimaryDirectionSampler final :
		public IGIProbeBakeRaySampler
	{
	public:
		bool SamplePrimaryDirection(
			const glm::vec3&,
			uint32_t sampleIndex,
			uint32_t,
			uint32_t,
			glm::vec3& outDirection,
			float& outPdf,
			std::string& outDiagnostic) const override
		{
			const bool bUpper = (sampleIndex & 1u) == 0u;
			outDirection = bUpper ?
				glm::vec3(0.0f, 1.0f, 0.0f) :
				glm::vec3(0.0f, -1.0f, 0.0f);
			outPdf = bUpper ? UpperPdf : LowerPdf;
			outDiagnostic.clear();
			return true;
		}

		bool Sample(
			const glm::vec3&,
			const glm::vec3& direction,
			float maxDistance,
			uint32_t,
			GIProbeBakeRaySample& outSample,
			std::string& outDiagnostic) const override
		{
			const float pdf = direction.y >= 0.0f ? UpperPdf : LowerPdf;
			outSample.m_radiance = glm::vec3(pdf * RadianceOverPdf);
			outSample.m_distance = maxDistance;
			outSample.m_bHit = false;
			outDiagnostic.clear();
			return true;
		}

		static constexpr float RadianceOverPdf = 4.0f;

	private:
		static constexpr float UpperPdf =
			1.0f / (16.0f * 3.14159265358979323846f);
		static constexpr float LowerPdf =
			1.0f / (8.0f * 3.14159265358979323846f);
	};

	class ConcurrentSeedDrivenBakeRaySampler final :
		public IGIProbeBakeRaySampler
	{
	public:
		explicit ConcurrentSeedDrivenBakeRaySampler(uint32_t expectedThreads) :
			m_expectedThreads(expectedThreads)
		{}

		bool Sample(
			const glm::vec3&,
			const glm::vec3&,
			float maxDistance,
			uint32_t randomSeed,
			GIProbeBakeRaySample& outSample,
			std::string& outDiagnostic) const override
		{
			{
				std::unique_lock<std::mutex> lock(m_mutex);
				m_threads.insert(std::this_thread::get_id());
				if (m_threads.size() >= m_expectedThreads)
				{
					m_released = true;
					m_condition.notify_all();
				}
				else if (!m_condition.wait_for(
						lock,
						std::chrono::seconds(2),
						[this]() { return m_released; }))
				{
					outDiagnostic =
						"the configured bake threads did not execute concurrently";
					return false;
				}
			}

			const float value = static_cast<float>(randomSeed & 0xffffu) /
				65535.0f;
			outSample.m_radiance = glm::vec3(
				value,
				value * value,
				1.0f - value);
			outSample.m_distance = maxDistance;
			outSample.m_bHit = false;
			outDiagnostic.clear();
			return true;
		}

		size_t GetObservedThreadCount() const
		{
			const std::lock_guard<std::mutex> lock(m_mutex);
			return m_threads.size();
		}

	private:
		uint32_t m_expectedThreads = 1u;
		mutable std::mutex m_mutex;
		mutable std::condition_variable m_condition;
		mutable std::set<std::thread::id> m_threads;
		mutable bool m_released = false;
	};

	bool HasSameFloatBits(float lhs, float rhs)
	{
		return std::bit_cast<uint32_t>(lhs) == std::bit_cast<uint32_t>(rhs);
	}

	bool HasSameVectorBits(const glm::vec2& lhs, const glm::vec2& rhs)
	{
		return HasSameFloatBits(lhs.x, rhs.x) &&
			HasSameFloatBits(lhs.y, rhs.y);
	}

	bool HasSameVectorBits(const glm::vec3& lhs, const glm::vec3& rhs)
	{
		return HasSameFloatBits(lhs.x, rhs.x) &&
			HasSameFloatBits(lhs.y, rhs.y) &&
			HasSameFloatBits(lhs.z, rhs.z);
	}

	bool HasSameProbeBits(
		const GIProbe& lhs,
		const GIProbe& rhs)
	{
		if (!HasSameVectorBits(lhs.m_position, rhs.m_position) ||
			!HasSameVectorBits(lhs.m_relocationOffset, rhs.m_relocationOffset) ||
			!HasSameFloatBits(lhs.m_validity, rhs.m_validity) ||
			lhs.m_flags != rhs.m_flags)
		{
			return false;
		}
		for (size_t index = 0u; index < lhs.m_irradiance.size(); ++index)
		{
			if (!HasSameVectorBits(
					lhs.m_irradiance[index],
					rhs.m_irradiance[index]))
			{
				return false;
			}
		}
		for (size_t index = 0u; index < lhs.m_visibility.size(); ++index)
		{
			if (!HasSameVectorBits(
					lhs.m_visibility[index],
					rhs.m_visibility[index]))
			{
				return false;
			}
			if (!HasSameFloatBits(
					lhs.m_environmentVisibility[index],
					rhs.m_environmentVisibility[index]))
			{
				return false;
			}
		}
		return true;
	}

	bool HasSameIrradianceBits(
		const GIProbe& lhs,
		const GIProbe& rhs)
	{
		for (size_t index = 0u; index < lhs.m_irradiance.size(); ++index)
		{
			if (!HasSameVectorBits(
					lhs.m_irradiance[index],
					rhs.m_irradiance[index]))
			{
				return false;
			}
		}
		return true;
	}

	bool HasSameTransportBits(
		const GIProbe& lhs,
		const GIProbe& rhs)
	{
		if (!HasSameVectorBits(lhs.m_position, rhs.m_position) ||
			!HasSameVectorBits(lhs.m_relocationOffset, rhs.m_relocationOffset) ||
			!HasSameFloatBits(lhs.m_validity, rhs.m_validity) ||
			lhs.m_flags != rhs.m_flags)
		{
			return false;
		}
		for (size_t index = 0u; index < lhs.m_visibility.size(); ++index)
		{
			if (!HasSameVectorBits(lhs.m_visibility[index], rhs.m_visibility[index]) ||
				!HasSameFloatBits(
					lhs.m_environmentVisibility[index],
					rhs.m_environmentVisibility[index]))
			{
				return false;
			}
		}
		return true;
	}

	class BoundaryRelocationSampler final : public IGIProbeBakeRaySampler
	{
	public:
		bool Sample(
			const glm::vec3&,
			const glm::vec3& direction,
			float maxDistance,
			uint32_t,
			GIProbeBakeRaySample& outSample,
			std::string&) const override
		{
			m_sampleCount.fetch_add(1u, std::memory_order_relaxed);
			const bool bPositiveXAxis = direction.x > 0.9999f &&
				std::abs(direction.y) < 0.0001f &&
				std::abs(direction.z) < 0.0001f;
			outSample.m_radiance = glm::vec3(0.25f);
			outSample.m_distance = bPositiveXAxis ? 0.0f : maxDistance;
			outSample.m_bHit = bPositiveXAxis;
			return true;
		}

		uint64_t GetSampleCount() const noexcept
		{
			return m_sampleCount.load(std::memory_order_relaxed);
		}

	private:
		mutable std::atomic<uint64_t> m_sampleCount{ 0u };
	};

	class ObliqueNearWallBakeRaySampler final :
		public IGIProbeBakeRaySampler
	{
	public:
		bool Sample(
			const glm::vec3& origin,
			const glm::vec3& direction,
			float maxDistance,
			uint32_t randomSeed,
			GIProbeBakeRaySample& outSample,
			std::string& outDiagnostic) const override
		{
			if (!SampleVisibility(
					origin,
					direction,
					maxDistance,
					randomSeed,
					outSample,
					outDiagnostic))
			{
				return false;
			}
			outSample.m_radiance = glm::vec3(0.25f);
			return true;
		}

		bool SampleVisibility(
			const glm::vec3& origin,
			const glm::vec3& direction,
			float maxDistance,
			uint32_t,
			GIProbeBakeRaySample& outSample,
			std::string& outDiagnostic) const override
		{
			outSample = {};
			outSample.m_distance = maxDistance;
			const glm::vec3 wallNormal = glm::normalize(
				glm::vec3(1.0f, 0.0f, 1.0f));
			const float alignment = glm::dot(direction, -wallNormal);
			// Model a small diagonal wall patch. None of the six signed-axis rays
			// can see it, while the fixed spherical transport directions can.
			if (alignment > 0.8f)
			{
				const float signedDistance =
					0.05f + glm::dot(origin, wallNormal);
				const float hitDistance = signedDistance / alignment;
				if (hitDistance >= 0.0f && hitDistance <= maxDistance)
				{
					outSample.m_bHit = true;
					outSample.m_distance = hitDistance;
				}
			}
			outDiagnostic.clear();
			return true;
		}
	};

	class HalfSpaceBakeRaySampler final : public IGIProbeBakeRaySampler
	{
	public:
		bool Sample(
			const glm::vec3& origin,
			const glm::vec3& direction,
			float maxDistance,
			uint32_t randomSeed,
			GIProbeBakeRaySample& outSample,
			std::string& outDiagnostic) const override
		{
			if (!SampleVisibility(
					origin,
					direction,
					maxDistance,
					randomSeed,
					outSample,
					outDiagnostic))
			{
				return false;
			}
			outSample.m_radiance = glm::vec3(0.25f);
			return true;
		}

		bool SampleVisibility(
			const glm::vec3& origin,
			const glm::vec3& direction,
			float maxDistance,
			uint32_t,
			GIProbeBakeRaySample& outSample,
			std::string& outDiagnostic) const override
		{
			outSample = {};
			outSample.m_distance = maxDistance;
			const bool bFromNegativeSide = origin.x < -0.0001f &&
				direction.x > 0.0001f;
			const bool bFromPositiveSide = origin.x > 0.0001f &&
				direction.x < -0.0001f;
			if (bFromNegativeSide || bFromPositiveSide)
			{
				const float distance = -origin.x / direction.x;
				if (distance >= 0.0f && distance <= maxDistance)
				{
					outSample.m_bHit = true;
					outSample.m_distance = distance;
					outSample.m_bBackFace = bFromNegativeSide;
				}
			}
			outDiagnostic.clear();
			return true;
		}
	};

	class FullyEmbeddedBakeRaySampler final : public IGIProbeBakeRaySampler
	{
	public:
		bool Sample(
			const glm::vec3& origin,
			const glm::vec3& direction,
			float maxDistance,
			uint32_t randomSeed,
			GIProbeBakeRaySample& outSample,
			std::string& outDiagnostic) const override
		{
			if (!SampleVisibility(
					origin,
					direction,
					maxDistance,
					randomSeed,
					outSample,
					outDiagnostic))
			{
				return false;
			}
			outSample.m_radiance = glm::vec3(100.0f);
			return true;
		}

		bool SampleVisibility(
			const glm::vec3&,
			const glm::vec3&,
			float maxDistance,
			uint32_t,
			GIProbeBakeRaySample& outSample,
			std::string& outDiagnostic) const override
		{
			outSample = {};
			outSample.m_bHit = true;
			outSample.m_bBackFace = true;
			outSample.m_distance = (std::min)(0.1f, maxDistance);
			outDiagnostic.clear();
			return true;
		}
	};

	class DirectionalDistanceBakeRaySampler final :
		public IGIProbeBakeRaySampler
	{
	public:
		bool Sample(
			const glm::vec3&,
			const glm::vec3& direction,
			float maxDistance,
			uint32_t,
			GIProbeBakeRaySample& outSample,
			std::string& outDiagnostic) const override
		{
			outSample = {};
			outSample.m_radiance = glm::vec3(0.25f);
			outSample.m_bHit = true;
			outSample.m_distance = (std::min)(
				maxDistance,
				1.25f + direction.x * 0.25f + direction.z * 0.125f);
			outDiagnostic.clear();
			return true;
		}
	};

	class InspectablePathTracer final : public Raytracing::PathTracer
	{
	public:
		float TraceDirectTransmittance(
			const Math::Ray& ray,
			float maxDistance) const
		{
			return TraceDirectLightTransmittance(
				ray,
				maxDistance);
		}

		bool IntersectIgnoring(
			const Math::Ray& ray,
			uint32_t ignoreInstance,
			uint32_t ignoreTriangle,
			uint32_t& outTriangle,
			float& outDistance) const
		{
			TLASHit hit;
			if (!IntersectScene(
					ray,
					hit,
					100.0f,
					ignoreInstance,
					ignoreTriangle))
			{
				return false;
			}
			outTriangle = hit.m_triangleIndex;
			outDistance = hit.m_hit.m_rayLenght;
			return true;
		}

		bool UsesEnvironmentImportance() const
		{
			return m_bUseRuntimeEnvironmentImportance;
		}

		bool SampleEnvironment(
			const glm::vec3& worldNormal,
			uint32_t& randomState,
			glm::vec3& outDirection,
			float& outPdf) const
		{
			return SampleDirectEnvironment(
				worldNormal,
				randomState,
				outDirection,
				outPdf);
		}

		float EnvironmentPdf(
			const glm::vec3& worldNormal,
			const glm::vec3& direction) const
		{
			return DirectEnvironmentPdf(worldNormal, direction);
		}

		glm::vec3 DirectEnvironmentRadiance(
			const glm::vec3& direction) const
		{
			return SampleRuntimeDirectEnvironment(direction);
		}
	};

	class MaterialSamplingPathTracer final : public Raytracing::PathTracer
	{
	public:
		Raytracing::LightingModel::SampledData SamplePreparedMaterial(
			size_t materialIndex,
			glm::vec2 uv = glm::vec2(0.5f)) const
		{
			return GetMaterialData(materialIndex, uv);
		}

		Raytracing::LightingModel::SampledData SampleLayeredMaterial(
			const glm::vec4& first,
			const glm::vec4& second,
			const glm::vec4& weights)
		{
			m_materials.Clear();
			m_textures.Clear();

			Raytracing::Material material;
			material.m_baseColorFactor = glm::vec4(0.5f, 1.0f, 1.0f, 1.0f);
			material.m_layerColorIndices[0] = 0u;
			material.m_layerColorIndices[1] = 1u;
			m_materials.Add(material);

			auto addTexture = [this](const glm::vec4& color)
			{
				TSharedPtr<Raytracing::CombinedSampler2D> texture =
					TSharedPtr<Raytracing::CombinedSampler2D>::Make();
				texture->Initialize<glm::vec4>(1u, 1u, 4u);
				texture->SetPixel(0u, 0u, color);
				m_textures.Add(std::move(texture));
			};
			addTexture(first);
			addTexture(second);

			return GetMaterialData(0u, glm::vec2(0.0f), weights);
		}

		glm::vec3 SampleScaledNormal(
			const glm::vec3& normal,
			float normalScale)
		{
			m_materials.Clear();
			m_textures.Clear();

			Raytracing::Material material;
			material.m_normalIndex = 0u;
			material.m_normalScale = normalScale;
			m_materials.Add(material);

			TSharedPtr<Raytracing::CombinedSampler2D> texture =
				TSharedPtr<Raytracing::CombinedSampler2D>::Make();
			texture->Initialize<glm::vec3>(1u, 1u, 3u);
			texture->SetPixel(0u, 0u, normal);
			m_textures.Add(std::move(texture));

			return GetMaterialData(0u, glm::vec2(0.0f)).m_normal;
		}

		glm::vec4 SampleVertexTint(
			const glm::vec4& baseColor,
			const glm::vec4& vertexColor)
		{
			m_materials.Clear();
			m_textures.Clear();

			Raytracing::Material material;
			material.m_baseColorFactor = baseColor;
			m_materials.Add(material);
			return GetMaterialData(
				0u,
				glm::vec2(0.0f),
				vertexColor).m_baseColor;
		}
	};

	void TestBinaryRoundTripDeterminismAndCorruption()
	{
		GIProbesData source = MakeVolume(2.0f, 11u);
		source.m_bakeSettings.m_skyIndirectIntensity = 1.75f;
		source.m_probes[7].m_flags |= GIProbeBlockedDirectionBit(5u);
		std::string diagnostic;
		Require(source.Validate(diagnostic), "test volume must be valid: " + diagnostic);

		TVector<uint8_t> first;
		TVector<uint8_t> second;
		Require(GIProbesBinary::Serialize(source, first, diagnostic),
			"valid volume should serialize: " + diagnostic);
		Require(GIProbesBinary::Serialize(source, second, diagnostic) && first == second,
			"the same bake state must serialize to deterministic bytes");

		const GIProbesBinaryResult roundTrip =
			GIProbesBinary::Deserialize(first.GetData(), first.Num());
		Require(roundTrip.IsSuccess(), "serialized volume should round-trip: " + roundTrip.m_diagnostic);
		Require(roundTrip.m_data->m_probes.Num() == 8u &&
			roundTrip.m_data->m_bricks.Num() == 1u &&
			roundTrip.m_data->m_bricks[0].m_probeCounts == glm::uvec3(2u) &&
			roundTrip.m_data->m_lightingHash == 11u &&
			roundTrip.m_data->m_stateName == "Test State" &&
			IsNear(roundTrip.m_data->m_bakeSettings.m_skyIndirectIntensity, 1.75f) &&
			roundTrip.m_data->m_probes[7].m_irradiance[0] == glm::vec3(2.0f) &&
			HasSameProbeBits(
				source.m_probes[7],
				roundTrip.m_data->m_probes[7]),
			"round-trip must preserve settings, adaptive layout, hashes, and L2 SH");

		TVector<uint8_t> corrupted = first;
		corrupted[corrupted.Num() - 1u] ^= 0x40u;
		const GIProbesBinaryResult rejected =
			GIProbesBinary::Deserialize(corrupted.GetData(), corrupted.Num());
		Require(rejected.m_status == EGIProbesBinaryStatus::ChecksumMismatch,
			"payload corruption must be detected before publication");

		const GIProbesBinaryResult truncated =
			GIProbesBinary::Deserialize(first.GetData(), first.Num() - 1u);
		Require(truncated.m_status == EGIProbesBinaryStatus::Truncated,
			"truncated payload must be rejected without partial data");

		TVector<uint8_t> unsupportedHeader = first;
		unsupportedHeader[20u] = 1u;
		const GIProbesBinaryResult unsupportedHeaderResult =
			GIProbesBinary::Deserialize(
				unsupportedHeader.GetData(),
				unsupportedHeader.Num());
		Require(
			unsupportedHeaderResult.m_status ==
				EGIProbesBinaryStatus::InvalidPayload,
			"unknown fixed-header flags must be rejected explicitly");

		GIProbesData unidentified = source;
		unidentified.m_stateName.clear();
		Require(!GIProbesBinary::Serialize(
			unidentified,
			second,
			diagnostic),
			"a .probes file must identify its one baked state");
		GIProbesData unhashed = source;
		unhashed.m_transportHash = 0u;
		Require(!GIProbesBinary::Serialize(
			unhashed,
			second,
			diagnostic),
			"a .probes file must carry transport identity for safe composition");
		GIProbesData excessiveSettings = source;
		excessiveSettings.m_bakeSettings.m_maxSubdivisionLevel =
			GIProbesMaxSubdivisionLevel + 1u;
		Require(!GIProbesBinary::Serialize(
			excessiveSettings,
			second,
			diagnostic),
			"a .probes file must reject unsupported bake-setting ranges");
		GIProbesData invalidSkyIndirectIntensity = source;
		invalidSkyIndirectIntensity.m_bakeSettings.m_skyIndirectIntensity = -0.01f;
		Require(!GIProbesBinary::Serialize(
			invalidSkyIndirectIntensity,
			second,
			diagnostic),
			"a .probes file must reject a negative sky intensity");

		GIProbesData overflowingGrid = source;
		overflowingGrid.m_bricks[0].m_probeCounts = glm::uvec3(
			(std::numeric_limits<uint32_t>::max)());
		Require(!overflowingGrid.Validate(diagnostic) &&
			diagnostic.find("overflows") != std::string::npos,
			"malicious brick dimensions must not wrap their probe-grid product");

		GIProbesData outsideVolume = source;
		outsideVolume.m_probes[0].m_position.x = -1.0f;
		outsideVolume.m_layoutHash = ComputeGIProbesLayoutHash(outsideVolume);
		Require(!outsideVolume.Validate(diagnostic),
			"a .probes payload must reject samples outside its declared volume");
	}

	void TestAtomicFileAndPortableIdentityBoundary()
	{
		const auto id = std::chrono::steady_clock::now().time_since_epoch().count();
		const std::filesystem::path directory =
			std::filesystem::temp_directory_path() /
			("sailor-probes-" + std::to_string(id));
		const std::filesystem::path firstPath = directory / "Day.probes";
		const std::filesystem::path copyPath = directory / "CopiedDay.probes";
		std::string diagnostic;
		const GIProbesData source = MakeVolume(1.0f, 21u);
		Require(GIProbesBinary::SaveAtomic(firstPath, source, diagnostic),
			"atomic .probes save should succeed: " + diagnostic);
		const GIProbesData replacement = MakeVolume(2.0f, 22u);
		Require(!GIProbesBinary::SaveAtomic(
			firstPath,
			replacement,
			diagnostic,
			false),
			"Overwrite=false must atomically reject an existing .probes target");
		const GIProbesBinaryResult preserved =
			GIProbesBinary::Load(firstPath);
		Require(
			preserved.IsSuccess() && preserved.m_data->m_lightingHash == 21u,
			"a rejected no-overwrite save must preserve the complete existing state");
		std::filesystem::copy_file(firstPath, copyPath);
		const GIProbesBinaryResult copied = GIProbesBinary::Load(copyPath);
		Require(copied.IsSuccess() && copied.m_data->m_lightingHash == 21u,
			"a copied binary must remain independently loadable without an embedded FileId");
		std::error_code error;
		std::filesystem::remove_all(directory, error);
	}

	void TestBlendAndAdditiveComposition()
	{
		GIProbesDataPtr day = GIProbesDataPtr::Make();
		GIProbesDataPtr evening = GIProbesDataPtr::Make();
		GIProbesDataPtr lamps = GIProbesDataPtr::Make();
		*day = MakeVolume(1.0f, 1u);
		*evening = MakeVolume(3.0f, 2u);
		*lamps = MakeVolume(2.0f, 3u);

		TVector<GIProbesCompositionInput> inputs;
		inputs.Add({ "Day", day, EGlobalIlluminationProbeMode::Blend, 1.0f });
		inputs.Add({ "Evening", evening, EGlobalIlluminationProbeMode::Blend, 3.0f });
		inputs.Add({ "Lamps", lamps, EGlobalIlluminationProbeMode::Additive, 0.5f });
		GIProbesCompositionResult result = GIProbesComposer::Compose(inputs, 3u);
		Require(result.IsSuccess(), "compatible Blend/Additive states should compose: " + result.m_diagnostic);
		Require(IsNear(result.m_data->m_probes[0].m_irradiance[0].x, 3.5f),
			"Blend must normalize to 2.5 and Additive must contribute an unnormalized 1.0");
		Require(IsNear(result.m_effectiveWeights[0], 0.25f) &&
			IsNear(result.m_effectiveWeights[1], 0.75f) &&
			IsNear(result.m_effectiveWeights[2], 0.5f),
			"snapshot must expose effective normalized Blend and raw Additive weights");

		const GIProbesCompositionResult overBudget =
			GIProbesComposer::Compose(inputs, 2u);
		Require(overBudget.m_status == EGIProbesCompositionStatus::BudgetExceeded &&
			!overBudget.m_data,
			"quality budget overflow must reject the complete mixture without dropping Additive states");

		lamps->m_transportHash += 1u;
		const GIProbesCompositionResult incompatible =
			GIProbesComposer::Compose(inputs, 3u);
		Require(incompatible.m_status == EGIProbesCompositionStatus::Incompatible,
			"different transport/visibility states must not be blended");
	}

	void TestSphericalHarmonicsAndSpatialSampling()
	{
		GIProbesData data = MakeVolume(0.0f, 31u);
		for (uint32_t index = 0u; index < data.m_probes.Num(); ++index)
		{
			data.m_probes[index].m_irradiance[0] =
				glm::vec3(static_cast<float>(index + 1u));
		}
		glm::vec3 sampled{};
		GIProbeDebugInfo debug;
		Require(SampleGIProbesIrradiance(
			data,
			glm::vec3(0.5f),
			glm::vec3(0.0f),
			sampled,
			&debug),
			"center of a valid adaptive brick should sample");
		Require(IsNear(sampled.x, 4.5f * 0.2820947918f) &&
			IsNear(debug.m_totalUnnormalizedWeight, 1.0f),
			"trilinear sampling must average all eight L2 SH payloads at brick center");
		float normalizedWeight = 0.0f;
		for (float weight : debug.m_weights) normalizedWeight += weight;
		Require(IsNear(normalizedWeight, 1.0f),
			"debug weights must expose the normalized receiver-side interpolation coefficients used by shading");

		const float unoccludedIrradiance = sampled.x;
		for (GIProbe& probe : data.m_probes)
		{
			for (glm::vec2& visibility : probe.m_visibility)
			{
				visibility = glm::vec2(0.5f, 0.3839746f);
			}
		}
		Require(SampleGIProbesIrradiance(
			data,
			glm::vec3(0.5f),
			glm::vec3(0.0f),
			sampled,
			&debug) &&
			IsNear(sampled.x, unoccludedIrradiance),
			"clearance moments without blocker metadata must not alter receiver-side interpolation");
		float attenuatedWeight = 0.0f;
		for (float weight : debug.m_weights) attenuatedWeight += weight;
		Require(IsNear(attenuatedWeight, 1.0f),
			"selected receiver-side probe weights must normalize before shading");

		for (GIProbe& probe : data.m_probes)
		{
			for (glm::vec2& visibility : probe.m_visibility)
			{
				visibility = glm::vec2(0.05f, 0.002501f);
			}
			probe.m_flags |= GIProbeBlockedDirectionMask;
		}
		Require(!SampleGIProbesIrradiance(
			data,
			glm::vec3(0.5f),
			glm::vec3(0.0f),
			sampled,
			&debug),
			"near-zero local distance moments must reject a receiver beyond nearby geometry");

		GIProbesData directional;
		directional.m_volumeMin = glm::vec3(-2.0f);
		directional.m_volumeMax = glm::vec3(2.0f);
		GIProbeBrick directionalBrick;
		directionalBrick.m_min = directional.m_volumeMin;
		directionalBrick.m_max = directional.m_volumeMax;
		directionalBrick.m_probeCounts = glm::uvec3(2u, 1u, 1u);
		directionalBrick.m_probeCount = 2u;
		directional.m_bricks.Add(directionalBrick);
		GIProbe directionalProbe;
		directionalProbe.m_irradiance[0] = glm::vec3(1.0f);
		for (glm::vec2& visibility : directionalProbe.m_visibility)
		{
			visibility = glm::vec2(2.0f, 4.0f);
		}
		directionalProbe.m_visibility[2u] = glm::vec2(0.1f, 0.01001f);
		directionalProbe.m_flags |= GIProbeBlockedDirectionBit(2u);
		directional.m_probes.Add(directionalProbe);
		GIProbe referenceProbe;
		referenceProbe.m_position = glm::vec3(2.0f, 0.0f, 0.0f);
		referenceProbe.m_irradiance[0] = glm::vec3(1.0f);
		for (glm::vec2& visibility : referenceProbe.m_visibility)
		{
			visibility = glm::vec2(2.0f, 4.0f);
		}
		directional.m_probes.Add(referenceProbe);
		glm::vec3 xDominant{};
		glm::vec3 yDominant{};
		GIProbeDebugInfo xDominantDebug;
		GIProbeDebugInfo yDominantDebug;
		Require(SampleGIProbesIrradiance(
			directional,
			glm::vec3(1.0f, 0.99f, 0.0f),
			glm::vec3(0.0f),
			xDominant,
			&xDominantDebug) &&
			SampleGIProbesIrradiance(
				directional,
				glm::vec3(0.99f, 1.0f, 0.0f),
				glm::vec3(0.0f),
				yDominant,
				&yDominantDebug),
			"directional visibility fixtures should sample inside their brick");
		const float fullDirectionalIrradiance = 0.2820947918f;
		Require(
			IsNear(xDominant.x, fullDirectionalIrradiance) &&
			IsNear(yDominant.x, fullDirectionalIrradiance) &&
			xDominantDebug.m_totalUnnormalizedWeight < 1.0f &&
			yDominantDebug.m_totalUnnormalizedWeight < 1.0f &&
			std::abs(
				xDominantDebug.m_totalUnnormalizedWeight -
				yDominantDebug.m_totalUnnormalizedWeight) < 0.02f,
			"broad visibility moments must vary smoothly across signed-axis lobe boundaries");
		Require(!SampleGIProbesIrradiance(
			data,
			glm::vec3(2.0f),
			glm::vec3(0.0f, 1.0f, 0.0f),
			sampled),
			"positions outside all bricks must select the environment fallback");
	}

	void TestConstantIrradianceReceiverSelection()
	{
		for (const float spacing : { 0.5f, 1.5f, 6.0f })
		{
			auto data = MakeVolume(1.0f, 32u);
			data.m_volumeMax = glm::vec3(spacing);
			data.m_bricks[0].m_max = data.m_volumeMax;
			data.m_bakeSettings.m_normalBias = 0.04f;
			data.m_bakeSettings.m_minProbeSpacing = spacing;
			for (auto& probe : data.m_probes) probe.m_position *= spacing;
			for (int latitude = 1; latitude < 32; ++latitude)
			{
				for (int longitude = 0; longitude < 64; ++longitude)
				{
					const float polar = glm::pi<float>() * latitude / 32.0f;
					const float azimuth = glm::two_pi<float>() * longitude / 64.0f;
					const glm::vec3 normal(std::sin(polar) * std::cos(azimuth),
						std::cos(polar), std::sin(polar) * std::sin(azimuth));
					const auto position = glm::vec3(spacing * 0.5f) + normal * spacing * 0.31f;
					glm::vec3 irradiance;
					Require(SampleGIProbesIrradiance(data, position, normal, irradiance) &&
						glm::length(irradiance - glm::vec3(0.2820947918f)) < 0.0001f,
						"a constant unoccluded field must not acquire normal-dependent bands");
				}
			}
			for (auto& probe : data.m_probes)
				probe.m_irradiance[0] = glm::vec3(probe.m_position.x == 0.0f ? 100.0f : 1.0f);
			glm::vec3 previous;
			for (int degree = 0; degree <= 360; ++degree)
			{
				const float angle = glm::radians(static_cast<float>(degree));
				glm::vec3 current;
				Require(SampleGIProbesIrradiance(data, glm::vec3(spacing * 0.5f),
					glm::vec3(std::cos(angle), std::sin(angle), 0), current),
					"the normal-rotation fixture must retain probe support");
				if (degree != 0)
					Require(std::abs(current.x - previous.x) < 0.04f * 99.0f * 0.2820947918f,
						"one-degree normal changes must not abruptly swap contrasting probes");
				previous = current;
			}
			for (auto& probe : data.m_probes)
			{
				probe.m_irradiance[0] = glm::vec3(1.0f);
				probe.m_flags |= GIProbeBlockedDirectionMask;
				for (auto& moment : probe.m_visibility)
					moment = glm::vec2(0.1f * spacing, 0.01001f * spacing * spacing);
			}
			glm::vec3 occluded;
			Require(!SampleGIProbesIrradiance(data, glm::vec3(spacing * 0.5f),
				glm::vec3(0, 1, 0), occluded) || glm::length(occluded) < 0.01f,
				"normalization must not restore irradiance rejected by blocker moments");
		}
	}

	void TestReceiverPlaneProbeRejection()
	{
		GIProbesData data;
		data.m_volumeMin = glm::vec3(-1.0f);
		data.m_volumeMax = glm::vec3(1.0f);
		GIProbeBrick brick;
		brick.m_min = data.m_volumeMin;
		brick.m_max = data.m_volumeMax;
		brick.m_probeCounts = glm::uvec3(1u, 1u, 2u);
		brick.m_probeCount = 2u;
		data.m_bricks.Add(brick);

		GIProbe leakingProbe;
		leakingProbe.m_position = glm::vec3(0.0f, 0.0f, -1.0f);
		leakingProbe.m_irradiance[0] = glm::vec3(100.0f);
		for (glm::vec2& visibility : leakingProbe.m_visibility)
		{
			visibility = glm::vec2(2.0f, 4.0f);
		}
		leakingProbe.m_visibility[4u] = glm::vec2(0.25f, 0.0625f);
		leakingProbe.m_flags |= GIProbeBlockedDirectionBit(4u);
		data.m_probes.Add(leakingProbe);

		GIProbe referenceProbe;
		referenceProbe.m_position = glm::vec3(0.0f, 0.0f, 1.0f);
		referenceProbe.m_irradiance[0] = glm::vec3(1.0f);
		for (glm::vec2& visibility : referenceProbe.m_visibility)
		{
			visibility = glm::vec2(2.0f, 4.0f);
		}
		data.m_probes.Add(referenceProbe);

		glm::vec3 sampled{};
		GIProbeDebugInfo debug;
		const glm::vec3 obliqueSurface(0.75f, 0.0f, 0.0f);
		Require(SampleGIProbesIrradiance(
			data,
			obliqueSurface,
			glm::vec3(0.0f),
			sampled,
			&debug),
			"a surface between two valid probes should sample");
		const float obliqueIrradiance = sampled.x;
		glm::vec3 nearbySample{};
		Require(
			obliqueIrradiance > 10.0f * 0.2820947918f &&
			obliqueIrradiance < 50.5f * 0.2820947918f &&
			SampleGIProbesIrradiance(
				data,
				glm::vec3(0.74f, 0.0f, 0.0f),
				glm::vec3(0.0f),
				nearbySample) &&
			std::abs(nearbySample.x - obliqueIrradiance) <
				obliqueIrradiance * 0.05f,
			"broad distance moments must attenuate cross-wall leakage without an "
				"infinite hard rejection plane");

		data.m_probes[0].m_flags &= ~GIProbeBlockedDirectionMask;
		data.m_probes[0].m_visibility[4u] = glm::vec2(2.0f, 4.0f);
		Require(SampleGIProbesIrradiance(
			data,
			glm::vec3(0.0f),
			glm::vec3(0.0f, 0.0f, 1.0f),
			sampled,
			&debug),
			"normal-side selection fixture should sample");
		Require(IsNear(sampled.x, 0.2820947918f, 0.0001f),
			"a bright probe behind the shaded surface must not win over a "
			"same-side reference probe");

		Require(SampleGIProbesIrradiance(
			data,
			glm::vec3(0.0f, 0.0f, -0.8f),
			glm::vec3(0.0f, 0.0f, 1.0f),
			sampled,
			&debug),
			"low receiver coverage should retain its same-side probe support");
		Require(IsNear(sampled.x, 0.2820947918f, 0.0001f),
			"normal-facing selection must not dim an unoccluded same-side field");
		float lowCoverageWeight = 0.0f;
		for (float weight : debug.m_weights) lowCoverageWeight += weight;
		Require(IsNear(lowCoverageWeight, 1.0f, 0.0001f),
			"debug weights must normalize unoccluded normal-facing support");

		data.m_probes[0].m_visibility[4u] = glm::vec2(0.25f, 0.0625f);
		data.m_probes[0].m_flags |= GIProbeBlockedDirectionBit(4u);
		Require(SampleGIProbesIrradiance(
			data,
			glm::vec3(0.0f),
			glm::vec3(0.0f, 0.0f, 1.0f),
			sampled,
			&debug),
			"surface-facing rescue should handle a fully blocked interpolation cell");
		Require(IsNear(sampled.x, 0.2820947918f, 0.0001f),
			"the fully blocked rescue path must still reject a bright probe "
			"behind the shaded surface");
		Require(!SampleGIProbesIrradiance(
			data,
			glm::vec3(0.75f, 0.0f, 0.0f),
			glm::vec3(1.0f, 0.0f, 0.0f),
			sampled,
			&debug),
			"a cell without any surface-facing probe must request the "
			"environment fallback instead of sampling behind the surface");
	}

	void TestWorldBindingRoundTripAndModes()
	{
		GISettings source;
		source.m_mode = EGlobalIlluminationMode::Baked;
		source.m_runtimeProbes.m_bounceCount = 2u;
		source.m_runtimeProbes.m_minProbeSpacing = 2.5f;
		source.m_runtimeProbes.m_normalBias = 0.075f;
		source.m_runtimeProbes.m_viewBias = 0.125f;
		source.m_runtimeProbes.m_maxRayDistance = 750.0f;
		source.m_runtimeProbes.m_bIncludeSky = false;
		GlobalIlluminationProbeBinding day;
		day.m_asset = ParseFileId("11111111-1111-1111-1111-111111111111");
		day.m_mode = EGlobalIlluminationProbeMode::Blend;
		day.m_initialWeight = 0.75f;
		source.m_probes.Insert("Day", day);
		GlobalIlluminationProbeBinding lamps;
		lamps.m_asset = ParseFileId("22222222-2222-2222-2222-222222222222");
		lamps.m_mode = EGlobalIlluminationProbeMode::Additive;
		lamps.m_initialWeight = 0.25f;
		lamps.m_bPreload = true;
		source.m_probes.Insert("Lamps", lamps);

		YAML::Node root;
		root["globalIllumination"] = source.Serialize();
		GISettings parsed;
		std::string diagnostic;
		Require(parsed.Deserialize(root, diagnostic),
			"world GI settings should round-trip: " + diagnostic);
		Require(parsed.m_mode == EGlobalIlluminationMode::Baked &&
			parsed.m_runtimeProbes.m_version == 1u &&
			parsed.m_runtimeProbes.m_bounceCount == 2u &&
			IsNear(parsed.m_runtimeProbes.m_minProbeSpacing, 2.5f) &&
			IsNear(parsed.m_runtimeProbes.m_normalBias, 0.075f) &&
			IsNear(parsed.m_runtimeProbes.m_viewBias, 0.125f) &&
			IsNear(parsed.m_runtimeProbes.m_maxRayDistance, 750.0f) &&
			!parsed.m_runtimeProbes.m_bIncludeSky &&
			parsed.m_probes.Num() == 2u &&
			parsed.m_probes["Day"].m_mode == EGlobalIlluminationProbeMode::Blend &&
			parsed.m_probes["Lamps"].m_mode == EGlobalIlluminationProbeMode::Additive &&
			parsed.m_probes["Lamps"].m_bPreload,
			"each named .probes binding must retain its independent Blend/Additive role");

		YAML::Node incompleteRoot = YAML::Clone(root);
		incompleteRoot["globalIllumination"].remove("mode");
		Require(!parsed.Deserialize(incompleteRoot, diagnostic) &&
			diagnostic.find("mode is required") != std::string::npos &&
			parsed.m_mode == EGlobalIlluminationMode::Baked &&
			parsed.m_probes.Num() == 2u,
			"invalid world GI settings must preserve the previous complete value");

		root["globalIllumination"]["mode"] = "ReflectionsOnly";
		Require(!parsed.Deserialize(root, diagnostic) &&
			diagnostic.find("Runtime") != std::string::npos &&
			parsed.m_mode == EGlobalIlluminationMode::Baked,
			"unknown world GI modes must fail atomically");
		root["globalIllumination"]["mode"] = "Baked";

		root["globalIllumination"]["probes"]["Lamps"]["mode"] = "Multiply";
		Require(!parsed.Deserialize(root, diagnostic) &&
			diagnostic.find("Blend or Additive") != std::string::npos,
			"unknown world composition modes must fail atomically");
	}

	void TestProbeBakeSavedWorldComparisonIgnoresEditorOnlyPrefabs()
	{
		const YAML::Node saved = YAML::Load(R"yaml(
name: BakeWorld
prefabs:
  - gameObjects:
      - name: Static Geometry
        components: [0]
    components:
      - typename: Sailor::MeshRendererComponent
        overrideProperties:
          meshIndex: 0
)yaml");
		YAML::Node current = YAML::Clone(saved);
		current["prefabs"].push_back(YAML::Load(R"yaml(
gameObjects:
  - name: Editor Camera
    position: [10, 20, 30, 1]
    components: [0, 1]
components:
  - typename: Sailor::CameraComponent
    overrideProperties:
      fov: 90
  - typename: Sailor::EditorComponent
    overrideProperties: {}
)yaml"));

		std::string diagnostic;
		Require(AreWorldDocumentsEquivalentForProbeBake(
				saved,
				current,
				diagnostic),
			"an injected editor camera must not make a saved level look dirty: " +
				diagnostic);

		current["prefabs"][0]["components"][0]["overrideProperties"]
			["meshIndex"] = 1;
		Require(!AreWorldDocumentsEquivalentForProbeBake(
				saved,
				current,
				diagnostic),
			"a real level edit must still fail the saved-world bake preflight");
	}

	void TestBakeControllerRejectsInvalidThreadCountBeforeSceneCapture()
	{
		GlobalIlluminationBakeController controller;
		EditorGIProbesBakeRequest request;
		request.m_threadCount = 0u;
		std::string diagnostic;
		Require(
			!controller.Start(nullptr, request, diagnostic) &&
				diagnostic.find("between 1 and") != std::string::npos,
			"the editor bake controller must reject zero threads before capturing a scene");

		request.m_threadCount = GIProbesMaxBakeThreadCount + 1u;
		diagnostic.clear();
		Require(
			!controller.Start(nullptr, request, diagnostic) &&
				diagnostic.find("between 1 and") != std::string::npos,
			"the editor bake controller must reject excessive threads before capturing a scene");
	}

	void TestEveningLandscapeVisualWorldIsSavedBakeableLevel()
	{
#if defined(SAILOR_TEST_SOURCE_DIR)
		const std::filesystem::path worldPath =
			std::filesystem::path(SAILOR_TEST_SOURCE_DIR) /
			"Content/Tests/Visual/GlobalIlluminationLandscapeEveningBounce.world";
#else
		const std::filesystem::path worldPath =
			"Content/Tests/Visual/GlobalIlluminationLandscapeEveningBounce.world";
#endif
		const YAML::Node world = YAML::LoadFile(worldPath.string());
		Require(world["name"].as<std::string>() ==
			"GlobalIlluminationLandscapeEveningBounce",
			"the visual fixture must be a named, reusable level asset");

		GISettings globalIllumination;
		std::string diagnostic;
		Require(globalIllumination.Deserialize(world, diagnostic),
			"the evening landscape world GI map must deserialize: " + diagnostic);
		const GlobalIlluminationProbeBinding* binding = nullptr;
		Require(globalIllumination.m_probes.Find(
				"Evening Landscape Bounce",
				binding) &&
			binding &&
			binding->m_asset == ParseFileId(
				"15500000-0000-4000-8000-000000000005") &&
			binding->m_mode == EGlobalIlluminationProbeMode::Blend &&
			IsNear(binding->m_initialWeight, 1.0f) &&
			binding->m_bPreload,
			"the saved level must bind its one baked evening state at full weight");

		bool bHasLandscape = false;
		bool bHasVisualTest = false;
		bool bHasEveningLight = false;
		bool bHasSky = false;
		bool bHasCamera = false;
		std::string eveningLightComponentId;
		std::string skyDirectionalLightComponentId;
		uint32_t savedTopologyCount = 0u;
		for (const YAML::Node& prefab : world["prefabs"])
		{
			const YAML::Node gameObjects = prefab["gameObjects"];
			const YAML::Node components = prefab["components"];
			if (!gameObjects || gameObjects.size() == 0u ||
				!components || components.size() == 0u)
			{
				continue;
			}
			const std::string name = gameObjects[0]["name"].as<std::string>();
			const std::string typeName =
				components[0]["typename"].as<std::string>();
			const YAML::Node properties = components[0]["overrideProperties"];
			bHasVisualTest |= typeName ==
				"Sailor::GlobalIlluminationLandscapeVisualTestComponent";
			bHasCamera |= typeName == "Sailor::CameraComponent";
			if (typeName == "Sailor::LandscapeComponent")
			{
				bHasLandscape =
					gameObjects[0]["position"][1].as<float>() ==
						GlobalIlluminationLandscapeTestScene::LandscapeWorldY &&
					properties["chunksX"].as<uint32_t>() ==
						GlobalIlluminationLandscapeTestScene::LandscapeChunksX &&
					properties["chunksZ"].as<uint32_t>() ==
						GlobalIlluminationLandscapeTestScene::LandscapeChunksZ &&
					properties["sculptStamps"].size() ==
						GlobalIlluminationLandscapeTestScene::
							GetLandscapeSculptStamps().Num();
			}
			if (typeName == "Sailor::LightComponent")
			{
				bHasEveningLight = name == "Evening Sun" &&
					IsNear(
						properties["indirectLightingIntensity"].as<float>(),
						1.0f);
				if (bHasEveningLight)
				{
					eveningLightComponentId =
						properties["instanceId"].as<std::string>();
				}
			}
			if (typeName == "Sailor::SkyComponent")
			{
				const YAML::Node directionalLight =
					properties["m_directionalLight"];
				const YAML::Node illuminance =
					properties["sunIlluminance"];
				bHasSky = name == "Evening Sky" &&
					IsNear(
						properties["sunAngle"].as<float>(),
						GlobalIlluminationLandscapeTestScene::
							EveningSunAngleDegrees) &&
					directionalLight && directionalLight.IsMap() &&
					directionalLight["instanceId"] &&
					illuminance && illuminance.IsSequence() &&
					illuminance.size() == 3u &&
					IsNear(
						illuminance[0].as<float>(),
						GlobalIlluminationLandscapeTestScene::
							GetEveningSunIlluminance().x) &&
					IsNear(
						illuminance[1].as<float>(),
						GlobalIlluminationLandscapeTestScene::
							GetEveningSunIlluminance().y) &&
					IsNear(
						illuminance[2].as<float>(),
						GlobalIlluminationLandscapeTestScene::
							GetEveningSunIlluminance().z);
				if (bHasSky)
				{
					skyDirectionalLightComponentId =
						directionalLight["instanceId"].as<std::string>();
				}
			}
			if (typeName == "Sailor::MeshRendererComponent")
			{
				for (const auto& expected :
					GlobalIlluminationLandscapeTestScene::GetBoxes())
				{
					savedTopologyCount += name == expected.m_name ? 1u : 0u;
				}
			}
		}
		Require(bHasLandscape && bHasVisualTest && bHasEveningLight &&
			bHasSky && !eveningLightComponentId.empty() &&
			eveningLightComponentId == skyDirectionalLightComponentId &&
			bHasCamera && savedTopologyCount ==
				GlobalIlluminationLandscapeTestScene::GetBoxes().Num(),
			"the visual world must save the landscape, an evening light linked to "
			"the SkyComponent, camera, validation component, and all "
			"occlusion/bounce topology; runtime-only spawned geometry is not accepted");
	}

	void TestGIBakeQualityLabCoversCanonicalCases()
	{
#if defined(SAILOR_TEST_SOURCE_DIR)
		const std::filesystem::path sourceRoot = SAILOR_TEST_SOURCE_DIR;
#else
		const std::filesystem::path sourceRoot = ".";
#endif
		const YAML::Node world = YAML::LoadFile((sourceRoot /
			"Content/Tests/Visual/GIBakeQualityLab.world").string());
		Require(world["name"].as<std::string>() == "GIBakeQualityLab",
			"the GI quality lab must remain a named reusable visual-test level");

		GISettings globalIllumination;
		std::string diagnostic;
		Require(globalIllumination.Deserialize(world, diagnostic) &&
			globalIllumination.m_mode ==
				EGlobalIlluminationMode::Baked &&
			globalIllumination.m_probes.IsEmpty(),
			"the reusable GI quality lab must not check in local baked states: " +
				diagnostic);

		std::set<std::string> names;
		std::set<std::string> materialIds;
		uint32_t colorBleedObjects = 0u;
		uint32_t corridorObjects = 0u;
		uint32_t lightTrapObjects = 0u;
		uint32_t skyTubeObjects = 0u;
		uint32_t colorBleedLights = 0u;
		uint32_t corridorLights = 0u;
		uint32_t enclosedLights = 0u;
		bool bHasCloudlessSky = false;
		bool bSkyReferencesDirectionalLight = false;
		for (const YAML::Node& prefab : world["prefabs"])
		{
			const YAML::Node gameObjects = prefab["gameObjects"];
			if (!gameObjects || gameObjects.size() == 0u)
			{
				continue;
			}
			const std::string name = gameObjects[0]["name"].as<std::string>();
			names.insert(name);
			const bool bColorBleed = name.starts_with("A_ColorBleed_");
			const bool bCorridor = name.starts_with("B_Corridor_");
			const bool bLightTrap = name.starts_with("C_LightTrap_");
			const bool bSkyTube = name.starts_with("D_SkyTube_");
			colorBleedObjects += bColorBleed ? 1u : 0u;
			corridorObjects += bCorridor ? 1u : 0u;
			lightTrapObjects += bLightTrap ? 1u : 0u;
			skyTubeObjects += bSkyTube ? 1u : 0u;

			for (const YAML::Node& component : prefab["components"])
			{
				const std::string typeName =
					component["typename"].as<std::string>();
				const YAML::Node properties = component["overrideProperties"];
				if (typeName == "Sailor::LightComponent")
				{
					colorBleedLights += bColorBleed ? 1u : 0u;
					corridorLights += bCorridor ? 1u : 0u;
					enclosedLights += (bLightTrap || bSkyTube) ? 1u : 0u;
				}
				else if (typeName == "Sailor::SkyComponent")
				{
					bHasCloudlessSky =
						IsNear(properties["cloudsDensity"].as<float>(), 0.0f) &&
						IsNear(properties["cloudsCoverage"].as<float>(), 0.0f) &&
						properties["giIndirectIntensity"].as<float>() > 0.0f;
					const YAML::Node directionalLight =
						properties["m_directionalLight"];
					bSkyReferencesDirectionalLight = directionalLight &&
						directionalLight["instanceId"] &&
						!directionalLight["instanceId"].as<std::string>().empty();
				}
				else if (typeName == "Sailor::MeshRendererComponent")
				{
					Require(properties["model"]["fileId"].as<std::string>() ==
						"2387902B-538E-4191-93D6-53503B5571B1",
						"every GI quality-lab case must use the same Box primitive");
					const YAML::Node overrides = properties["overrideMaterials"];
					Require(overrides && overrides.size() == 1u,
						"every GI quality-lab primitive must name one controlled material");
					materialIds.insert(overrides[0].as<std::string>());
				}
			}
		}

		const std::array<const char*, 12> requiredObjects =
		{
			"A_ColorBleed_RedLeftWall",
			"A_ColorBleed_WhiteReferenceBlock",
			"A_ColorBleed_NeutralPoint",
			"B_Corridor_HalfWidthBaffle",
			"B_Corridor_WarmPointAtEnd",
			"C_LightTrap_FrontWallLeft",
			"C_LightTrap_FrontWallRight",
			"C_LightTrap_TransverseBaffle",
			"C_LightTrap_DiffuseReference",
			"C_LightTrap_GlossyReference",
			"D_SkyTube_ApertureLeftStrip",
			"D_SkyTube_ApertureReferenceBlock"
		};
		for (const char* requiredObject : requiredObjects)
		{
			Require(names.contains(requiredObject),
				std::string("GI quality lab is missing case geometry: ") +
					requiredObject);
		}
		Require(colorBleedObjects >= 8u && corridorObjects >= 8u &&
			lightTrapObjects >= 11u && skyTubeObjects >= 9u &&
			colorBleedLights == 1u && corridorLights == 1u &&
			enclosedLights == 0u && bHasCloudlessSky &&
			bSkyReferencesDirectionalLight,
			"the lab must retain color bleed, end-lit corridor, indirect-only light trap, and sky-aperture cases");

		const std::array<const char*, 4> materialPaths =
		{
			"Content/Tests/Visual/GIBakeQualityLab/MatteWhite.mat",
			"Content/Tests/Visual/GIBakeQualityLab/MatteRed.mat",
			"Content/Tests/Visual/GIBakeQualityLab/GlossyWhite.mat",
			"Content/Tests/Visual/GIBakeQualityLab/WarmEmitter.mat"
		};
		for (const char* relativePath : materialPaths)
		{
			const YAML::Node material = YAML::LoadFile(
				(sourceRoot / relativePath).string());
			Require(material["shaderUid"].as<std::string>().find(
					"1A4BA353-FDA4-4F65-941F-D9FFEE4630A0") !=
					std::string::npos &&
				material["samplers"].IsMap() &&
				material["samplers"].size() == 0u &&
				material["uniformsVec4"]["material.baseColorFactor"] &&
				material["uniformsVec4"]["material.emissiveFactor"] &&
				material["uniformsFloat"]["material.roughnessFactor"] &&
				material["uniformsFloat"]["material.metallicFactor"] &&
				material["uniformsFloat"]["material.normalScale"] &&
				material["uniformsFloat"]["material.occlusionStrength"] &&
				!material["uniformsVec4"]["material.albedo"] &&
				!material["uniformsFloat"]["material.roughness"],
				std::string("GI quality-lab textureless material must use the canonical Standard_glTF schema: ") +
					relativePath);
		}
		Require(materialIds == std::set<std::string>{
				"D1A60000-0000-4000-8000-000000000101",
				"D1A60000-0000-4000-8000-000000000102",
				"D1A60000-0000-4000-8000-000000000103",
				"D1A60000-0000-4000-8000-000000000104" },
			"the visual lab must use only its four controlled PBR reference materials");
	}

	void TestGpuPackingAndWeightOnlyUpdates()
	{
		GIProbesDataPtr day = GIProbesDataPtr::Make();
		GIProbesDataPtr evening = GIProbesDataPtr::Make();
		*day = MakeVolume(1.0f, 41u);
		*evening = MakeVolume(3.0f, 42u);
		const uint32_t blockedDirection = GIProbeBlockedDirectionBit(5u);
		day->m_probes[0].m_flags |= blockedDirection;

		RHI::RHIGlobalIlluminationSnapshot snapshot;
		snapshot.m_generation = 7u;
		snapshot.m_lightingHash = 71u;
		snapshot.m_layout = day;
		snapshot.m_qualityBudget = 2u;
		RHI::RHIGlobalIlluminationState dayState;
		dayState.m_name = "Day";
		dayState.m_data = day;
		dayState.m_effectiveWeight = 0.75f;
		dayState.m_mode = EGlobalIlluminationProbeMode::Blend;
		snapshot.m_states.Add(dayState);
		RHI::RHIGlobalIlluminationState eveningState;
		eveningState.m_name = "Evening";
		eveningState.m_data = evening;
		eveningState.m_effectiveWeight = 0.25f;
		eveningState.m_mode = EGlobalIlluminationProbeMode::Blend;
		snapshot.m_states.Add(eveningState);

		const RHI::RHIGlobalIlluminationRenderStats renderStats =
			RHI::BuildGlobalIlluminationRenderStats(&snapshot);
		const uint64_t expectedCpuPayloadBytes = 2u * (
			sizeof(GIProbeBrick) +
			8u * sizeof(GIProbe));
		Require(renderStats.m_bActive &&
			renderStats.m_activeRevision == 7u &&
			renderStats.m_loadedBricks == 1u &&
			renderStats.m_totalBricks == 1u &&
			renderStats.m_probeCount == 8u &&
			renderStats.m_stateCount == 2u &&
			renderStats.m_qualityBudget == 2u &&
			renderStats.m_cpuPayloadBytes == expectedCpuPayloadBytes &&
			renderStats.m_gpuAllocatedBytes == 0u &&
			renderStats.m_uploadedGpuBytes == 0u,
			"render stats must report unique immutable CPU payloads and active GI counts");

		std::string diagnostic;
		RHI::RHIGlobalIlluminationGpuLayout layout;
		Require(RHI::BuildGlobalIlluminationGpuLayout(
			*day,
			layout,
			diagnostic),
			"valid adaptive layout should pack for GPU: " + diagnostic);
		Require(layout.m_nodes.Num() == 1u &&
			layout.m_bricks.Num() == 1u &&
			layout.m_probes.Num() == 8u &&
			layout.m_bricks[0].m_probeCountsAndValidCount ==
				glm::uvec4(2u, 2u, 2u, 8u) &&
			(std::bit_cast<uint32_t>(
				layout.m_bricks[0].m_minAndSubdivision.w) &
				RHI::GlobalIlluminationBrickFullyValidBit) != 0u &&
			(std::bit_cast<uint32_t>(layout.m_nodes[0].m_minAndLeft.w) &
				0x80000000u) != 0u,
			"single adaptive brick must produce one encoded BVH leaf and expose "
			"its valid probe count");
		GIProbesData partiallyInvalid = *day;
		partiallyInvalid.m_probes[
			partiallyInvalid.m_probes.Num() - 1u].m_validity = 0.0f;
		RHI::RHIGlobalIlluminationGpuLayout partiallyInvalidLayout;
		Require(RHI::BuildGlobalIlluminationGpuLayout(
				partiallyInvalid,
				partiallyInvalidLayout,
				diagnostic) &&
			partiallyInvalidLayout.m_bricks[0].m_probeCountsAndValidCount.w == 7u &&
			(std::bit_cast<uint32_t>(partiallyInvalidLayout.m_bricks[0]
				.m_minAndSubdivision.w) &
				RHI::GlobalIlluminationBrickFullyValidBit) == 0u,
			"GPU brick metadata must exclude invalid probes so selection can "
			"fall back to neighboring bricks");

		GIProbesData adaptiveNeighbors = MakeVolume(1.0f, 43u);
		adaptiveNeighbors.m_volumeMax = glm::vec3(2.0f, 1.0f, 1.0f);
		adaptiveNeighbors.m_bricks.Clear();
		adaptiveNeighbors.m_probes.Clear();
		auto appendBrick = [&](const glm::vec3& brickMin,
			const glm::vec3& brickMax,
			uint32_t subdivision)
			{
				GIProbeBrick brick;
				brick.m_min = brickMin;
				brick.m_max = brickMax;
				brick.m_subdivisionLevel = subdivision;
				brick.m_firstProbeIndex = static_cast<uint32_t>(
					adaptiveNeighbors.m_probes.Num());
				brick.m_probeCount = 8u;
				brick.m_probeCounts = glm::uvec3(2u);
				adaptiveNeighbors.m_bricks.Add(brick);
				for (uint32_t z = 0u; z < 2u; ++z)
				{
					for (uint32_t y = 0u; y < 2u; ++y)
					{
						for (uint32_t x = 0u; x < 2u; ++x)
						{
							GIProbe probe;
							probe.m_position = glm::vec3(
								x != 0u ? brickMax.x : brickMin.x,
								y != 0u ? brickMax.y : brickMin.y,
								z != 0u ? brickMax.z : brickMin.z);
							probe.m_irradiance[0] = glm::vec3(1.0f);
							adaptiveNeighbors.m_probes.Add(std::move(probe));
						}
					}
				}
			};
		appendBrick(glm::vec3(0.0f), glm::vec3(1.0f), 0u);
		appendBrick(
			glm::vec3(1.0f, 0.0f, 0.0f),
			glm::vec3(2.0f, 0.4f, 0.4f),
			1u);
		adaptiveNeighbors.m_layoutHash =
			ComputeGIProbesLayoutHash(adaptiveNeighbors);
		RHI::RHIGlobalIlluminationGpuLayout adaptiveLayout;
		Require(RHI::BuildGlobalIlluminationGpuLayout(
				adaptiveNeighbors,
				adaptiveLayout,
				diagnostic) &&
			adaptiveLayout.m_bricks.Num() == 2u,
			"adaptive neighbor metadata should pack: " + diagnostic);
		const auto adaptiveFaceMask = [&](size_t brickIndex)
			{
				const uint32_t metadata = std::bit_cast<uint32_t>(
					adaptiveLayout.m_bricks[brickIndex]
						.m_minAndSubdivision.w);
				return (metadata &
					RHI::GlobalIlluminationBrickAdaptiveFaceMask) >>
					RHI::GlobalIlluminationBrickAdaptiveFaceShift;
			};
		Require(
			adaptiveFaceMask(0u) == (1u << 1u) &&
			adaptiveFaceMask(1u) == (1u << 0u),
			"GPU brick metadata must mark both sides of a partial adaptive "
			"X-face even when the neighbor does not cover the face center");
		const float visibilityMaxDistance =
			CalculateGIProbeVisibilityMaxDistance(
				*day,
				day->m_bricks[0]);
		const glm::vec2 packedVisibilityMoments = glm::unpackHalf2x16(
			layout.m_probes[0].m_visibilityMoments0123.x);
		Require(
			layout.m_probes[0].m_environmentVisibility0123 ==
				glm::vec4(0.1f, 0.2f, 0.3f, 0.4f) &&
			layout.m_probes[0].m_environmentVisibility45.x == 0.5f &&
			layout.m_probes[0].m_environmentVisibility45.y == 0.6f &&
			IsNear(
				layout.m_probes[0].m_environmentVisibility45.z,
				visibilityMaxDistance) &&
			std::bit_cast<uint32_t>(
				layout.m_probes[0].m_environmentVisibility45.w) ==
				blockedDirection &&
			IsNear(packedVisibilityMoments.x, 1.0f, 0.001f) &&
			IsNear(packedVisibilityMoments.y, 1.0f, 0.001f),
			"GPU probe layout must preserve environment lobes, local support, "
				"blocker bits, and normalized distance moments");

		TVector<RHI::RHIGlobalIlluminationGpuCoefficients> coefficients;
		Require(RHI::BuildGlobalIlluminationGpuCoefficients(
			snapshot,
			coefficients,
			diagnostic),
			"resident baked states should pack SH for GPU: " + diagnostic);
		Require(coefficients.Num() == 16u,
			"GPU SH storage must contain stateCount times probeCount records");
		const glm::vec2 firstTwo = glm::unpackHalf2x16(
			coefficients[0].m_packed[0].x);
		const glm::vec2 nextTwo = glm::unpackHalf2x16(
			coefficients[0].m_packed[0].y);
		Require(IsNear(firstTwo.x, 1.0f, 0.001f) &&
			IsNear(firstTwo.y, 1.0f, 0.001f) &&
			IsNear(nextTwo.x, 1.0f, 0.001f),
			"signed RGB SH components must retain their sequential FP16 representation");

		GIProbesDataPtr hdr = GIProbesDataPtr::Make();
		*hdr = MakeVolume(0.0f, 44u);
		hdr->m_probes[0].m_irradiance[0] =
			glm::vec3(180000.0f, -90000.0f, 45000.0f);
		hdr->m_probes[0].m_irradiance[8].z = -120000.0f;
		RHI::RHIGlobalIlluminationSnapshot hdrSnapshot;
		hdrSnapshot.m_layout = hdr;
		hdrSnapshot.m_qualityBudget = 1u;
		RHI::RHIGlobalIlluminationState hdrState;
		hdrState.m_name = "HDR";
		hdrState.m_data = hdr;
		hdrState.m_effectiveWeight = 1.0f;
		hdrSnapshot.m_states.Add(std::move(hdrState));
		TVector<RHI::RHIGlobalIlluminationGpuCoefficients> hdrCoefficients;
		Require(RHI::BuildGlobalIlluminationGpuCoefficients(
				hdrSnapshot,
				hdrCoefficients,
				diagnostic) &&
			hdrCoefficients.Num() == hdr->m_probes.Num(),
			"physical HDR SH coefficients should pack without FP16 clipping: " +
				diagnostic);
		const glm::vec2 hdrFirstTwo = glm::unpackHalf2x16(
			hdrCoefficients[0].m_packed[0].x);
		const glm::vec2 hdrNextTwo = glm::unpackHalf2x16(
			hdrCoefficients[0].m_packed[0].y);
		const glm::vec2 hdrLastPair = glm::unpackHalf2x16(
			hdrCoefficients[0].m_packed[3].y);
		const float hdrScale = hdrLastPair.y;
		Require(hdrScale > 1.0f &&
			IsNear(hdrFirstTwo.x * hdrScale, 180000.0f, 100.0f) &&
			IsNear(hdrFirstTwo.y * hdrScale, -90000.0f, 100.0f) &&
			IsNear(hdrNextTwo.x * hdrScale, 45000.0f, 100.0f) &&
			IsNear(hdrLastPair.x * hdrScale, -120000.0f, 100.0f),
			"the shared FP16 HDR scale must preserve bright signed SH values");

		TVector<RHI::RHIGlobalIlluminationGpuState> states;
		Require(RHI::BuildGlobalIlluminationGpuStates(
			snapshot,
			states,
			diagnostic) &&
			states.Num() == 2u &&
			IsNear(states[0].m_parameters.x, 0.75f) &&
			IsNear(states[1].m_parameters.x, 0.25f),
			"GPU state metadata must retain effective snapshot weights");

		const uint64_t layoutSignature =
			RHI::ComputeGlobalIlluminationLayoutSignature(snapshot);
		const uint64_t coefficientSignature =
			RHI::ComputeGlobalIlluminationCoefficientSignature(snapshot);
		const uint64_t stateSignature =
			RHI::ComputeGlobalIlluminationStateSignature(snapshot);
		snapshot.m_generation += 1u;
		snapshot.m_states[0].m_effectiveWeight = 0.5f;
		snapshot.m_states[1].m_effectiveWeight = 0.5f;
		Require(
			RHI::ComputeGlobalIlluminationLayoutSignature(snapshot) ==
				layoutSignature &&
			RHI::ComputeGlobalIlluminationCoefficientSignature(snapshot) ==
				coefficientSignature &&
			RHI::ComputeGlobalIlluminationStateSignature(snapshot) !=
				stateSignature,
			"weight-only interpolation must upload state metadata without repacking layout or SH");

		const RHI::RHIGlobalIlluminationGpuHeader header =
			RHI::BuildGlobalIlluminationGpuHeader(
				&snapshot,
				RHI::EGlobalIlluminationDebugVisualization::IndirectOnly,
				EGlobalIlluminationMode::Baked,
				false);
		Require(header.m_counts == glm::uvec4(1u, 1u, 1u, 8u) &&
			header.m_stateAndDebug.x == 2u &&
			header.m_settings.x == 0u &&
			IsNear(
				std::bit_cast<float>(header.m_settings.z),
				day->m_bakeSettings.m_minProbeSpacing) &&
			IsNear(header.m_volumeMin.w, day->m_bakeSettings.m_normalBias) &&
			IsNear(header.m_volumeMax.w, day->m_bakeSettings.m_viewBias) &&
			header.m_settings.y == static_cast<uint32_t>(
				EGlobalIlluminationMode::Baked) &&
			header.m_stateAndDebug.z == static_cast<uint32_t>(
				RHI::EGlobalIlluminationDebugVisualization::IndirectOnly),
			"GPU header must expose resident counts and the selected GI debug mode");
	}

	void TestAdaptiveBakerAndLayoutReuse()
	{
		GIProbesBakeRequest request;
		request.m_stateName = "Day";
		request.m_volumeMin = glm::vec3(0.0f);
		request.m_volumeMax = glm::vec3(4.0f);
		request.m_settings.m_raysPerProbe = 16u;
		request.m_settings.m_bounceCount = 2u;
		request.m_settings.m_maxSubdivisionLevel = 2u;
		request.m_settings.m_minProbeSpacing = 1.0f;
		request.m_sourceWorldHash = 101u;
		Math::AABB geometry;
		geometry.Extend(glm::vec3(0.1f));
		geometry.Extend(glm::vec3(0.5f));
		request.m_sceneGeometryBounds.Add(geometry);
		float finalProgress = 0.0f;
		request.m_progress = [&](const GIProbesBakeProgress& progress)
		{
			finalProgress = progress.m_fraction;
		};

		const ConstantBakeRaySampler daylight(glm::vec3(1.0f));
		const GIProbesBakeResult day = GIProbesBaker::Bake(
			request,
			daylight);
		Require(day.IsSuccess(),
			"adaptive constant-radiance bake should succeed: " +
			day.m_diagnostic);
		Require(day.m_data->m_bricks.Num() == 15u &&
			day.m_data->m_probes.Num() == 120u &&
			IsNear(finalProgress, 1.0f),
			"geometry-local refinement must replace one level-one brick with eight level-two bricks");
		Require(IsNear(
				daylight.GetLastMaxDistance(),
				request.m_settings.m_maxRayDistance),
			"radiance rays must retain the scene-scale maxRayDistance");
		float smallestVisibilitySupport =
			(std::numeric_limits<float>::max)();
		float largestVisibilitySupport = 0.0f;
		for (const GIProbeBrick& brick : day.m_data->m_bricks)
		{
			const glm::uvec3 cellCounts = glm::max(
				brick.m_probeCounts,
				glm::uvec3(2u)) - glm::uvec3(1u);
			const float expectedVisibilityMaxDistance = glm::length(
				(brick.m_max - brick.m_min) / glm::vec3(cellCounts)) +
				request.m_settings.m_minProbeSpacing * 0.45f +
				request.m_settings.m_normalBias +
				request.m_settings.m_viewBias;
			smallestVisibilitySupport = (std::min)(
				smallestVisibilitySupport,
				expectedVisibilityMaxDistance);
			largestVisibilitySupport = (std::max)(
				largestVisibilitySupport,
				expectedVisibilityMaxDistance);
			for (uint32_t probeOffset = 0u;
				probeOffset < brick.m_probeCount;
				++probeOffset)
			{
				const GIProbe& probe = day.m_data->m_probes[
					brick.m_firstProbeIndex + probeOffset];
				for (const glm::vec2& visibility : probe.m_visibility)
				{
					Require(
						IsNear(
							visibility.x,
							expectedVisibilityMaxDistance,
							0.001f) &&
						IsNear(
							visibility.y,
							expectedVisibilityMaxDistance *
								expectedVisibilityMaxDistance,
							0.001f),
						"visibility misses must be clamped to their owning "
						"brick interpolation support");
				}
				for (const float environmentVisibility :
					probe.m_environmentVisibility)
				{
					Require(IsNear(environmentVisibility, 1.0f),
						"transport rays that miss geometry must preserve the full sky-specular path");
				}
			}
		}
		Require(largestVisibilitySupport > smallestVisibilitySupport * 1.5f,
			"coarse adaptive bricks must retain a larger visibility support "
			"than fine bricks");
		const auto findSubdivisionLevel = [&day](size_t probeIndex)
		{
			for (const GIProbeBrick& brick : day.m_data->m_bricks)
			{
				if (probeIndex >= brick.m_firstProbeIndex &&
					probeIndex < brick.m_firstProbeIndex + brick.m_probeCount)
				{
					return brick.m_subdivisionLevel;
				}
			}
			return (std::numeric_limits<uint32_t>::max)();
		};
		bool bFoundSharedProbe = false;
		bool bFoundCrossLevelSharedProbe = false;
		for (size_t lhsIndex = 0u;
			lhsIndex < day.m_data->m_probes.Num();
			++lhsIndex)
		{
			for (size_t rhsIndex = lhsIndex + 1u;
				rhsIndex < day.m_data->m_probes.Num();
				++rhsIndex)
			{
				if (HasSameVectorBits(
						day.m_data->m_probes[lhsIndex].m_position,
						day.m_data->m_probes[rhsIndex].m_position))
				{
					bFoundSharedProbe = true;
					const uint32_t lhsLevel = findSubdivisionLevel(lhsIndex);
					const uint32_t rhsLevel = findSubdivisionLevel(rhsIndex);
					Require(HasSameIrradianceBits(
							day.m_data->m_probes[lhsIndex],
							day.m_data->m_probes[rhsIndex]),
						"bricks sharing a corner must use one canonical radiance sample");
					if (lhsLevel == rhsLevel)
					{
						Require(HasSameProbeBits(
								day.m_data->m_probes[lhsIndex],
								day.m_data->m_probes[rhsIndex]),
							"same-level bricks sharing a corner must use one "
							"canonical transport sample");
					}
					else
					{
						bFoundCrossLevelSharedProbe = true;
						Require(!HasSameVectorBits(
								day.m_data->m_probes[lhsIndex].m_visibility[0u],
								day.m_data->m_probes[rhsIndex].m_visibility[0u]),
							"cross-level shared corners must preserve their "
							"different local visibility support");
					}
				}
			}
		}
		Require(bFoundSharedProbe && bFoundCrossLevelSharedProbe,
			"adaptive fixture must contain same-position brick corners across "
			"adaptive levels");
		Require(IsNear(
			day.m_data->m_probes[0].m_irradiance[0].x,
			4.0f * 3.14159265358979323846f *
				0.2820947918f,
			0.001f),
			"baker must project radiance and include the white Lambertian 1 / PI normalization");

		GIProbesBakeRequest reused = request;
		reused.m_stateName = "Night";
		reused.m_layoutSource = day.m_data.GetRawPtr();
		reused.m_progress = {};
		const ConstantBakeRaySampler moonlight(glm::vec3(0.2f, 0.3f, 0.5f));
		const GIProbesBakeResult night = GIProbesBaker::Bake(
			reused,
			moonlight);
		Require(night.IsSuccess(),
			"lighting-only bake with reused layout should succeed: " +
			night.m_diagnostic);
		std::string compatibilityDiagnostic;
		Require(day.m_data->IsCompositionCompatibleWith(
				*night.m_data,
				compatibilityDiagnostic) &&
			day.m_data->m_lightingHash != night.m_data->m_lightingHash,
			"reused layout/transport must stay Blend-compatible while lighting changes");

		GIProbesBakeRequest dimSky = request;
		dimSky.m_stateName = "Dim Sky";
		dimSky.m_layoutSource = day.m_data.GetRawPtr();
		dimSky.m_settings.m_skyIndirectIntensity = 0.25f;
		const GIProbesBakeResult dimSkyResult = GIProbesBaker::Bake(
			dimSky,
			daylight);
		Require(dimSkyResult.IsSuccess() &&
			dimSkyResult.m_data->m_layoutHash == day.m_data->m_layoutHash &&
			dimSkyResult.m_data->m_transportHash == day.m_data->m_transportHash &&
			dimSkyResult.m_data->m_lightingHash != day.m_data->m_lightingHash,
			"Sky GI indirect intensity must change lighting identity without invalidating reusable geometry transport");

		std::atomic<bool> cancel{ true };
		GIProbesBakeRequest cancelled = request;
		cancelled.m_stateName = "Cancelled";
		cancelled.m_cancel = &cancel;
		const GIProbesBakeResult cancelledResult = GIProbesBaker::Bake(
			cancelled,
			daylight);
		Require(cancelledResult.m_status == EGIProbesBakeStatus::Cancelled &&
			!cancelledResult.m_data,
			"cancelled bakes must never publish partial .probes data");
	}

	void TestAnisotropicAdaptiveSubdivisionHonorsSpacing()
	{
		GIProbesBakeRequest request;
		request.m_stateName = "Anisotropic";
		request.m_volumeMin = glm::vec3(0.0f);
		request.m_volumeMax = glm::vec3(16.0f, 1.0f, 16.0f);
		request.m_settings.m_raysPerProbe = 1u;
		request.m_settings.m_bounceCount = 1u;
		request.m_settings.m_maxSubdivisionLevel = 4u;
		request.m_settings.m_minProbeSpacing = 1.0f;
		Math::AABB geometry;
		geometry.Extend(glm::vec3(2.1f, 0.2f, 2.1f));
		geometry.Extend(glm::vec3(2.2f, 0.8f, 2.2f));
		request.m_sceneGeometryBounds.Add(geometry);

		GIProbesBakeRequest tooCoarse = request;
		tooCoarse.m_settings.m_maxSubdivisionLevel = 3u;
		const ConstantBakeRaySampler daylight(glm::vec3(1.0f));
		const GIProbesBakeResult rejected = GIProbesBaker::Bake(
			tooCoarse,
			daylight);
		Require(
			rejected.m_status == EGIProbesBakeStatus::InvalidRequest &&
				rejected.m_diagnostic.find("use at least level 4") !=
					std::string::npos,
			"a subdivision cap must not silently turn one-metre spacing into "
			"multi-metre probes");

		const GIProbesBakeResult baked = GIProbesBaker::Bake(
			request,
			daylight);
		Require(baked.IsSuccess(),
			"anisotropic adaptive bake should succeed: " + baked.m_diagnostic);
		bool bFoundFinestBrick = false;
		for (const GIProbeBrick& brick : baked.m_data->m_bricks)
		{
			const glm::vec3 extent = brick.m_max - brick.m_min;
			Require(IsNear(extent.y, 1.0f),
				"an unsplit short axis must retain the complete volume extent");
			if (brick.m_subdivisionLevel == 4u)
			{
				bFoundFinestBrick = true;
				Require(
					extent.x <= 1.0001f && extent.z <= 1.0001f,
					"a short Y axis must not stop X/Z refinement at eight metres");
			}
		}
		Require(bFoundFinestBrick,
			"geometry-local anisotropic refinement should reach requested spacing");
	}

	void TestGeometryNeighborhoodRefinementAndWallRepulsion()
	{
		GIProbesBakeRequest adaptiveRequest;
		adaptiveRequest.m_stateName = "Geometry Neighborhood";
		adaptiveRequest.m_volumeMin = glm::vec3(0.0f);
		adaptiveRequest.m_volumeMax = glm::vec3(4.0f);
		adaptiveRequest.m_settings.m_raysPerProbe = 1u;
		adaptiveRequest.m_settings.m_bounceCount = 1u;
		adaptiveRequest.m_settings.m_maxSubdivisionLevel = 2u;
		adaptiveRequest.m_settings.m_minProbeSpacing = 1.0f;
		Math::AABB nearbyGeometry;
		nearbyGeometry.Extend(glm::vec3(-0.75f, 1.8f, 1.8f));
		nearbyGeometry.Extend(glm::vec3(-0.5f, 2.2f, 2.2f));
		adaptiveRequest.m_sceneGeometryBounds.Add(nearbyGeometry);

		const ConstantBakeRaySampler daylight(glm::vec3(1.0f));
		const GIProbesBakeResult adaptive = GIProbesBaker::Bake(
			adaptiveRequest,
			daylight);
		Require(adaptive.IsSuccess(),
			"geometry-neighborhood bake should succeed: " +
				adaptive.m_diagnostic);
		bool bFoundFinestBrick = false;
		for (const GIProbeBrick& brick : adaptive.m_data->m_bricks)
		{
			bFoundFinestBrick |= brick.m_subdivisionLevel == 2u;
		}
		Require(
			bFoundFinestBrick &&
			adaptive.m_data->m_bricks.Num() > 1u &&
			adaptive.m_data->m_bricks.Num() < 64u,
			"adaptive layout must refine the probe shell next to geometry without "
			"uniformly refining the whole volume");

		GIProbesBakeRequest relocationRequest;
		relocationRequest.m_stateName = "Oblique Wall";
		relocationRequest.m_volumeMin = glm::vec3(0.0f);
		relocationRequest.m_volumeMax = glm::vec3(1.0f);
		relocationRequest.m_settings.m_raysPerProbe = 1u;
		relocationRequest.m_settings.m_bounceCount = 1u;
		relocationRequest.m_settings.m_maxSubdivisionLevel = 0u;
		relocationRequest.m_settings.m_minProbeSpacing = 1.0f;
		const ObliqueNearWallBakeRaySampler wallSampler;
		const GIProbesBakeResult relocated = GIProbesBaker::Bake(
			relocationRequest,
			wallSampler);
		Require(relocated.IsSuccess(),
			"oblique-wall relocation bake should succeed: " +
				relocated.m_diagnostic);

		const uint32_t relocatedFlag = static_cast<uint32_t>(
			EGIProbeFlag::Relocated);
		bool bMovedAwayFromObliqueWall = false;
		for (const GIProbe& probe : relocated.m_data->m_probes)
		{
			const glm::vec3 nominalPosition =
				probe.m_position - probe.m_relocationOffset;
			if (IsNear(nominalPosition.x, 0.0f) &&
				IsNear(nominalPosition.z, 0.0f) &&
				probe.m_position.x > 0.05f &&
				probe.m_position.z > 0.05f &&
				(probe.m_flags & relocatedFlag) != 0u)
			{
				bMovedAwayFromObliqueWall = true;
			}
			Require(glm::length(probe.m_relocationOffset) <= 0.4501f,
				"geometry-aware wall repulsion must preserve the relocation cap");
		}
		Require(bMovedAwayFromObliqueWall,
			"a nearby oblique wall missed by signed-axis rays must push its probes "
			"into free space");
	}

	void TestPrimaryDirectionPdfWeighting()
	{
		GIProbesBakeRequest request;
		request.m_stateName = "Primary Direction PDF";
		request.m_volumeMin = glm::vec3(0.0f);
		request.m_volumeMax = glm::vec3(1.0f);
		request.m_settings.m_raysPerProbe = 8u;
		request.m_settings.m_bounceCount = 1u;
		request.m_settings.m_maxSubdivisionLevel = 0u;
		request.m_settings.m_minProbeSpacing = 1.0f;

		const PdfWeightedPrimaryDirectionSampler sampler;
		const GIProbesBakeResult result = GIProbesBaker::Bake(
			request,
			sampler);
		Require(result.IsSuccess(),
			"PDF-weighted primary-direction bake should succeed: " +
			result.m_diagnostic);
		const float expectedL0 =
			PdfWeightedPrimaryDirectionSampler::RadianceOverPdf *
			0.2820947918f;
		for (const GIProbe& probe : result.m_data->m_probes)
		{
			Require(IsNear(
					probe.m_irradiance[0].x,
					expectedL0,
					0.0001f),
				"primary SH projection must divide each radiance sample by "
				"its direction PDF");
		}
	}

	void TestIncrementalProbeIrradianceAccumulation()
	{
		GIProbeTraceRequest request;
		request.m_settings.m_raysPerProbe = 64u;
		request.m_settings.m_bounceCount = 1u;
		request.m_settings.m_randomSeed = 1729u;
		request.m_settings.m_maxRayDistance = 100.0f;
		const SeedDrivenBakeRaySampler sampler;
		const glm::vec3 position(2.0f, 3.0f, 4.0f);
		constexpr uint32_t ProbeSeed = 91u;
		constexpr uint32_t SequenceSamples = 64u;
		std::string diagnostic;

		GIProbeIrradianceAccumulator oneShot;
		Require(AccumulateGIProbeIrradianceRange(
				request,
				sampler,
				position,
				ProbeSeed,
				0u,
				SequenceSamples,
				SequenceSamples,
				oneShot,
				diagnostic),
			"one-shot probe irradiance accumulation should succeed: " +
				diagnostic);
		GIProbe oneShotProbe;
		Require(ResolveGIProbeIrradiance(
				oneShot,
				oneShotProbe,
				diagnostic),
			"one-shot probe irradiance should resolve: " + diagnostic);

		GIProbeIrradianceAccumulator progressive;
		Require(AccumulateGIProbeIrradianceRange(
				request,
				sampler,
				position,
				ProbeSeed,
				0u,
				16u,
				SequenceSamples,
				progressive,
				diagnostic),
			"the first runtime probe sample range should succeed: " +
				diagnostic);
		GIProbe warmProbe;
		Require(ResolveGIProbeIrradiance(
				progressive,
				warmProbe,
				diagnostic) &&
			progressive.m_sampleCount == 16u &&
			std::isfinite(warmProbe.m_irradiance[0].x),
			"a partially warmed runtime probe must resolve finite energy");

		Require(AccumulateGIProbeIrradianceRange(
				request,
				sampler,
				position,
				ProbeSeed,
				16u,
				16u,
				SequenceSamples,
				progressive,
				diagnostic) &&
			AccumulateGIProbeIrradianceRange(
				request,
				sampler,
				position,
				ProbeSeed,
				32u,
				32u,
				SequenceSamples,
				progressive,
				diagnostic),
			"sequential runtime probe ranges should refine without restarting: " +
				diagnostic);
		GIProbe progressiveProbe;
		Require(ResolveGIProbeIrradiance(
				progressive,
				progressiveProbe,
				diagnostic),
			"fully refined runtime probe irradiance should resolve: " +
				diagnostic);
		for (uint32_t coefficientIndex = 0u;
			coefficientIndex < GIProbeSphericalHarmonicsCoefficientCount;
			++coefficientIndex)
		{
			for (uint32_t component = 0u; component < 3u; ++component)
			{
				Require(IsNear(
						progressiveProbe.m_irradiance[coefficientIndex][component],
						oneShotProbe.m_irradiance[coefficientIndex][component],
						0.000001f),
					"incremental probe ranges must match one-shot irradiance");
			}
		}

		const uint32_t completedSamples = progressive.m_sampleCount;
		Require(!AccumulateGIProbeIrradianceRange(
				request,
				sampler,
				position,
				ProbeSeed,
				16u,
				1u,
				SequenceSamples,
				progressive,
				diagnostic) &&
			progressive.m_sampleCount == completedSamples,
			"overlapping or repeated sample ranges must be rejected without mutation");

		std::atomic<bool> cancel{ true };
		request.m_cancel = &cancel;
		GIProbeIrradianceAccumulator cancelled;
		Require(!AccumulateGIProbeIrradianceRange(
				request,
				sampler,
				position,
				ProbeSeed,
				0u,
				16u,
				SequenceSamples,
				cancelled,
				diagnostic) &&
			cancelled.m_sampleCount == 0u,
			"cancelled runtime probe tracing must not publish samples");

		cancel.store(false, std::memory_order_release);
		const CancellingBakeRaySampler cancellingSampler(cancel);
		GIProbeIrradianceAccumulator interrupted;
		Require(!AccumulateGIProbeIrradianceRange(
				request,
				cancellingSampler,
				position,
				ProbeSeed,
				0u,
				16u,
				SequenceSamples,
				interrupted,
				diagnostic) &&
			interrupted.m_sampleCount == 0u &&
			interrupted.m_sequenceSampleCount == 0u,
			"cancellation during a runtime batch must discard the entire batch");
	}

	void TestReusableProbeTransportTracing()
	{
		GIProbesBakeRequest bakeRequest;
		bakeRequest.m_stateName = "Reusable Transport";
		bakeRequest.m_volumeMin = glm::vec3(0.0f);
		bakeRequest.m_volumeMax = glm::vec3(1.0f);
		bakeRequest.m_settings.m_raysPerProbe = 8u;
		bakeRequest.m_settings.m_bounceCount = 1u;
		bakeRequest.m_settings.m_maxSubdivisionLevel = 0u;
		bakeRequest.m_settings.m_minProbeSpacing = 1.0f;
		bakeRequest.m_settings.m_maxRayDistance = 10.0f;

		const DirectionalDistanceBakeRaySampler sampler;
		const GIProbesBakeResult baked = GIProbesBaker::Bake(
			bakeRequest,
			sampler);
		Require(baked.IsSuccess(),
			"reference transport bake should succeed: " + baked.m_diagnostic);
		const GIProbe& bakedProbe = baked.m_data->m_probes[0];
		GIProbe tracedProbe;
		tracedProbe.m_position =
			bakedProbe.m_position - bakedProbe.m_relocationOffset;

		GIProbeTraceRequest traceRequest;
		traceRequest.m_settings = bakeRequest.m_settings;
		traceRequest.m_volumeMin = bakeRequest.m_volumeMin;
		traceRequest.m_volumeMax = bakeRequest.m_volumeMax;
		const float visibilityMaxDistance =
			CalculateGIProbeVisibilityMaxDistance(
				*baked.m_data,
				baked.m_data->m_bricks[0]);
		std::string diagnostic;
		Require(TraceGIProbeTransport(
				traceRequest,
				sampler,
				0u,
				visibilityMaxDistance,
				tracedProbe,
				diagnostic),
			"standalone runtime transport tracing should succeed: " +
				diagnostic);
		Require(HasSameTransportBits(tracedProbe, bakedProbe),
			"standalone runtime transport must match the established bake path");

		std::atomic<bool> cancel{ true };
		traceRequest.m_cancel = &cancel;
		GIProbe untouched = tracedProbe;
		Require(!TraceGIProbeTransport(
				traceRequest,
				sampler,
				0u,
				visibilityMaxDistance,
				untouched,
				diagnostic) &&
			HasSameProbeBits(untouched, tracedProbe),
			"cancelled standalone transport must preserve the last probe state");
	}

	RuntimeGIProbesStartRequest MakeRuntimeGIProbesRequest(
		const TSharedPtr<IGIProbeBakeRaySampler>& sampler,
		uint64_t geometryGeneration,
		uint64_t lightingGeneration)
	{
		RuntimeGIProbesStartRequest request;
		request.m_sampler = sampler;
		request.m_priorityPosition = glm::vec3(0.0f);
		request.m_geometryBounds.m_min = glm::vec3(-0.1f, -0.1f, -2.1f);
		request.m_geometryBounds.m_max = glm::vec3(0.1f, 0.1f, -1.9f);
		request.m_geometryGeneration = geometryGeneration;
		request.m_lightingGeneration = lightingGeneration;
		request.m_randomSeed = 9187u;
		request.m_worldSettings.m_bounceCount = 1u;
		request.m_worldSettings.m_minProbeSpacing = 1.0f;
		request.m_worldSettings.m_maxRayDistance = 10.0f;
		request.m_qualitySettings.m_bEnabled = true;
		request.m_qualitySettings.m_maxActiveProbes = 8u;
		request.m_qualitySettings.m_initialSamplesPerProbe = 16u;
		request.m_qualitySettings.m_targetSamplesPerProbe = 32u;
		request.m_qualitySettings.m_workerCount = 1u;
		request.m_qualitySettings.m_cpuDutyFraction = 1.0f;
		request.m_qualitySettings.m_cpuBudgetMilliseconds = 4.0f;
		request.m_qualitySettings.m_maxPublicationsPerSecond = 60.0f;
		request.m_qualitySettings.m_initialPublicationCoverage = 0.125f;
		return request;
	}

	template<typename TPredicate>
	bool WaitForRuntimeGIProbes(
		RuntimeGIProbesService& service,
		TPredicate&& predicate,
		std::chrono::milliseconds timeout = std::chrono::seconds(2))
	{
		const auto deadline = std::chrono::steady_clock::now() + timeout;
		do
		{
			service.Tick();
			if (predicate(service.GetStatus()))
			{
				return true;
			}
			std::this_thread::yield();
		}
		while (std::chrono::steady_clock::now() < deadline);
		service.Tick();
		return predicate(service.GetStatus());
	}

	void TestRuntimeGIProbesServiceLifecycleAndPublication()
	{
		RuntimeGIProbesService service;
		service.SetWorkAllowed(false);
		auto constantSampler =
			TSharedPtr<ConstantBakeRaySampler>::Make(glm::vec3(0.5f));
		TSharedPtr<IGIProbeBakeRaySampler> sampler = constantSampler;
		RuntimeGIProbesStartRequest request = MakeRuntimeGIProbesRequest(
			sampler,
			11u,
			21u);
		std::string diagnostic;
		RuntimeGIProbesQualitySettings workerLimit =
			request.m_qualitySettings;
		workerLimit.m_workerCount = 16u;
		Require(workerLimit.Validate(diagnostic),
			"runtime GI must accept up to sixteen low-priority workers: " +
				diagnostic);
		workerLimit.m_workerCount = 17u;
		Require(!workerLimit.Validate(diagnostic),
			"runtime GI must reject worker counts above sixteen");
		RuntimeGIProbesStartRequest missingGeometryRequest = request;
		missingGeometryRequest.m_geometryBounds = {};
		Require(!service.Start(missingGeometryRequest, diagnostic) &&
			diagnostic.find("prepared-scene geometry bounds") !=
				std::string::npos,
			"runtime placement must reject a request without prepared geometry bounds");
		Require(service.Start(request, diagnostic),
			"the bounded runtime GI service should accept a prepared sampler: " +
				diagnostic);
		Require(
			service.GetStatus().m_lifecycle ==
				ERuntimeGIProbesLifecycle::Throttled &&
			service.GetStatus().m_readyProbeCount == 0u &&
			!service.GetPublishedData(),
			"runtime tracing must remain idle while frame headroom disallows work");

		service.SetPaused(true);
		service.SetWorkAllowed(true);
		std::this_thread::sleep_for(std::chrono::milliseconds(5));
		Require(
			service.GetStatus().m_lifecycle ==
				ERuntimeGIProbesLifecycle::Paused &&
			service.GetStatus().m_readyProbeCount == 0u &&
			!service.GetPublishedData(),
			"pause must prevent queued probes from entering placement or tracing");

		service.SetPaused(false);
		Require(WaitForRuntimeGIProbes(
				service,
				[&service](const RuntimeGIProbesStatus& status)
				{
					return status.m_readyProbeCount > 0u &&
						service.GetPublishedData();
				}),
			"runtime probes should reach initial coverage and publish within the test budget");

		const RuntimeGIProbesStatus publishedStatus = service.GetStatus();
		GIProbesDataPtr firstPublished = service.GetPublishedData();
		Require(
			firstPublished &&
			publishedStatus.m_activeProbeCount == 8u &&
			publishedStatus.m_activeProbeCount <= publishedStatus.m_capacity &&
			publishedStatus.m_publishedRevision > 0u &&
			publishedStatus.m_publishedBytes > 0u,
			"runtime publication must remain inside the configured capacity and expose telemetry");
		Require(firstPublished->Validate(diagnostic),
			"a runtime publication must satisfy the existing probe payload contract: " +
				diagnostic);
		uint32_t usableProbeCount = 0u;
		for (const GIProbe& probe : firstPublished->m_probes)
		{
			if ((probe.m_flags & static_cast<uint32_t>(EGIProbeFlag::Valid)) == 0u)
			{
				continue;
			}
			++usableProbeCount;
			Require(
				std::isfinite(probe.m_irradiance[0].x) &&
				probe.m_irradiance[0].x > 0.0f,
				"a ready runtime probe must never publish cleared or non-finite irradiance");
		}
		Require(usableProbeCount > 0u,
			"initial runtime coverage must contain at least one usable probe");

		auto failingSampler = TSharedPtr<FailingBakeRaySampler>::Make();
		TSharedPtr<IGIProbeBakeRaySampler> failingSamplerBase = failingSampler;
		RuntimeGIProbesStartRequest failingRequest = MakeRuntimeGIProbesRequest(
			failingSamplerBase,
			12u,
			22u);
		Require(service.Start(failingRequest, diagnostic),
			"a new prepared generation should start before its trace failure is observed");
		Require(
			service.GetPublishedData().GetRawPtr() == firstPublished.GetRawPtr(),
			"starting a replacement generation must retain the last immutable publication");
		Require(WaitForRuntimeGIProbes(
				service,
				[](const RuntimeGIProbesStatus& status)
				{
					return status.m_lifecycle ==
						ERuntimeGIProbesLifecycle::Failed;
				}),
			"the service must expose a failed replacement generation");
		Require(
			service.GetPublishedData().GetRawPtr() == firstPublished.GetRawPtr() &&
			service.GetStatus().m_diagnostic.find("intentional") !=
				std::string::npos,
			"a failed generation must preserve the previous GI snapshot and diagnostic");

		RuntimeGIProbesStartRequest replacementRequest = MakeRuntimeGIProbesRequest(
			sampler,
			13u,
			23u);
		Require(service.Start(replacementRequest, diagnostic),
			"the solver should restart after a failed generation: " + diagnostic);
		const uint64_t firstRevision = publishedStatus.m_publishedRevision;
		Require(WaitForRuntimeGIProbes(
				service,
				[&service, firstRevision](const RuntimeGIProbesStatus& status)
				{
					return status.m_publishedRevision > firstRevision &&
						service.GetPublishedData();
				}),
			"a later valid generation should replace the retained snapshot");
		Require(
			service.GetPublishedData().GetRawPtr() != firstPublished.GetRawPtr() &&
			service.GetStatus().m_sceneGeneration == 13u &&
			service.GetStatus().m_lightingGeneration == 23u,
			"only the current scene and lighting generation may become visible");

		service.Disable();
		Require(
			service.GetStatus().m_lifecycle ==
				ERuntimeGIProbesLifecycle::Disabled &&
			!service.GetPublishedData(),
			"disabling the experimental provider must stop publishing transient data");
	}

	void TestRuntimeGIProbesFixedGridReuseAndBudgets()
	{
		RuntimeGIProbesService service;
		auto constantSampler =
			TSharedPtr<ConstantBakeRaySampler>::Make(glm::vec3(0.25f));
		TSharedPtr<IGIProbeBakeRaySampler> sampler = constantSampler;
		RuntimeGIProbesStartRequest request = MakeRuntimeGIProbesRequest(
			sampler,
			31u,
			41u);
		request.m_qualitySettings.m_maxActiveProbes = 64u;
		request.m_qualitySettings.m_initialSamplesPerProbe = 16u;
		request.m_qualitySettings.m_targetSamplesPerProbe = 32u;
		request.m_qualitySettings.m_maxDirtyUploadBytesPerFrame = 64u * 1024u;
		request.m_geometryBounds.m_min = glm::vec3(-0.1f, -0.1f, -9.0f);
		request.m_geometryBounds.m_max = glm::vec3(0.6f, 0.1f, -2.0f);
		std::string diagnostic;
		Require(service.Start(request, diagnostic),
			"the runtime scene grid should start: " + diagnostic);
		Require(WaitForRuntimeGIProbes(
				service,
				[](const RuntimeGIProbesStatus& status)
				{
					return status.m_lifecycle ==
						ERuntimeGIProbesLifecycle::Ready &&
						status.m_publishedRevision > 0u;
				}),
			"the runtime scene grid should converge within the test budget");

		const GIProbesDataPtr firstPublished = service.GetPublishedData();
		Require(firstPublished && firstPublished->Validate(diagnostic),
			"the converged runtime grid must satisfy the probe payload contract: " +
				diagnostic);
		Require(firstPublished->m_bricks.Num() == 1u &&
			firstPublished->m_bakeSettings.m_maxSubdivisionLevel == 0u &&
			firstPublished->m_bricks[0].m_subdivisionLevel == 0u &&
			firstPublished->m_probes.Num() <=
				request.m_qualitySettings.m_maxActiveProbes,
			"runtime GI must use one fixed scene-wide grid within its capacity");
		const GIProbeBrick& fixedGrid = firstPublished->m_bricks[0];
		Require(glm::all(glm::lessThanEqual(
				fixedGrid.m_min,
				request.m_geometryBounds.m_min)) &&
			glm::all(glm::greaterThanEqual(
				fixedGrid.m_max,
				request.m_geometryBounds.m_max)),
			"the automatically sized runtime grid must cover all prepared geometry bounds");

		const uint64_t visibilityBeforeMove =
			constantSampler->GetVisibilitySampleCount();
		service.SetWorkAllowed(false);
		RuntimeGIProbesStartRequest movedRequest = request;
		movedRequest.m_priorityPosition.x += 12.25f;
		movedRequest.m_qualitySettings.m_cpuBudgetMilliseconds = 0.000001f;
		Require(service.Start(movedRequest, diagnostic),
			"changing the priority camera should start a compatible generation: " +
				diagnostic);
		const RuntimeGIProbesStatus movedStatus = service.GetStatus();
		Require(movedStatus.m_readyProbeCount == movedStatus.m_activeProbeCount &&
			IsNear(movedStatus.m_refinement, 1.0f),
			"camera motion must preserve and reuse every fixed-grid probe");
		service.SetWorkAllowed(true);
		service.Tick(0.0f);
		Require(WaitForRuntimeGIProbes(
				service,
				[](const RuntimeGIProbesStatus& status)
				{
					return status.m_lifecycle ==
						ERuntimeGIProbesLifecycle::Ready;
				}),
			"the camera-independent runtime grid should remain converged");
		const uint64_t visibilityAfterMove =
			constantSampler->GetVisibilitySampleCount();
		Require(visibilityAfterMove == visibilityBeforeMove,
			"camera movement must not retrace a stable geometry-clipped layout");

		service.SetWorkAllowed(false);
		const uint64_t visibilityBeforeLightingChange =
			constantSampler->GetVisibilitySampleCount();
		const uint64_t irradianceBeforeLightingChange =
			constantSampler->GetIrradianceSampleCount();
		RuntimeGIProbesStartRequest relitRequest = movedRequest;
		++relitRequest.m_lightingGeneration;
		relitRequest.m_qualitySettings.m_cpuBudgetMilliseconds =
			request.m_qualitySettings.m_cpuBudgetMilliseconds;
		Require(service.Start(relitRequest, diagnostic),
			"a lighting-only generation should restart irradiance: " + diagnostic);
		Require(service.GetStatus().m_readyProbeCount == 0u,
			"valid probes must warm new irradiance before a lighting generation publishes");
		service.SetWorkAllowed(true);
		Require(WaitForRuntimeGIProbes(
				service,
				[](const RuntimeGIProbesStatus& status)
				{
					return status.m_lifecycle ==
						ERuntimeGIProbesLifecycle::Ready;
				}),
			"the lighting-only generation should converge");
		Require(constantSampler->GetVisibilitySampleCount() ==
				visibilityBeforeLightingChange &&
			constantSampler->GetIrradianceSampleCount() >
				irradianceBeforeLightingChange,
			"lighting-only invalidation must reuse placement and visibility while retracing irradiance");

		service.Disable();
		RuntimeGIProbesStartRequest boundedRequest = request;
		boundedRequest.m_qualitySettings.m_maxActiveProbes =
			RuntimeGIProbesHardMaxActiveProbes;
		boundedRequest.m_qualitySettings.m_maxDirtyUploadBytesPerFrame =
			16u * 1024u;
		Require(service.Start(boundedRequest, diagnostic),
			"a small publication budget should reduce effective runtime capacity: " +
				diagnostic);
		Require(service.GetStatus().m_capacity <
				RuntimeGIProbesHardMaxActiveProbes &&
			service.GetStatus().m_activeProbeCount <=
				service.GetStatus().m_capacity,
			"the automatically sized grid must remain inside both probe and publication limits");
		Require(WaitForRuntimeGIProbes(
				service,
				[](const RuntimeGIProbesStatus& status)
				{
					return status.m_lifecycle ==
						ERuntimeGIProbesLifecycle::Ready &&
						status.m_publishedRevision > 0u;
				}),
			"the publication-limited grid should still converge");
		Require(service.GetStatus().m_publishedBytes <=
				boundedRequest.m_qualitySettings.m_maxDirtyUploadBytesPerFrame,
			"a runtime snapshot must never exceed its configured publication budget");

		service.Disable();
		boundedRequest.m_qualitySettings.m_maxDirtyUploadBytesPerFrame = 1u;
		Require(!service.Start(boundedRequest, diagnostic) &&
			diagnostic.find("minimum eight-probe grid") != std::string::npos,
			"an impossible publication budget must be rejected before workers start");
	}

	void TestRuntimeGIProbesIgnoreCameraMotion()
	{
		RuntimeGIProbesService service;
		auto constantSampler =
			TSharedPtr<ConstantBakeRaySampler>::Make(glm::vec3(0.25f));
		TSharedPtr<IGIProbeBakeRaySampler> sampler = constantSampler;
		RuntimeGIProbesStartRequest initialRequest = MakeRuntimeGIProbesRequest(
			sampler,
			51u,
			61u);
		initialRequest.m_qualitySettings.m_maxActiveProbes = 64u;
		initialRequest.m_qualitySettings.m_initialSamplesPerProbe = 16u;
		initialRequest.m_qualitySettings.m_targetSamplesPerProbe = 16u;
		initialRequest.m_qualitySettings.m_maxDirtyUploadBytesPerFrame =
			64u * 1024u;
		initialRequest.m_geometryBounds.m_min =
			glm::vec3(-0.1f, -0.1f, -64.0f);
		initialRequest.m_geometryBounds.m_max =
			glm::vec3(0.1f, 0.1f, 576.0f);
		std::string diagnostic;
		Require(service.Start(initialRequest, diagnostic),
			"the initial runtime priority should start: " + diagnostic);
		Require(WaitForRuntimeGIProbes(
				service,
				[](const RuntimeGIProbesStatus& status)
				{
					return status.m_lifecycle ==
						ERuntimeGIProbesLifecycle::Ready;
				}),
			"the initial runtime grid should converge");
		const RuntimeGIProbesStatus initialStatus = service.GetStatus();
		const GIProbesDataPtr initialData = service.GetPublishedData();
		Require(initialData && initialData->m_bricks.Num() == 1u &&
			initialStatus.m_readyProbeCount == initialStatus.m_activeProbeCount,
			"the fixed runtime grid must converge before camera motion");

		RuntimeGIProbesStartRequest movedRequest = initialRequest;
		movedRequest.m_priorityPosition.z += 512.0f;
		const uint64_t visibilityBeforeMove =
			constantSampler->GetVisibilitySampleCount();
		const uint64_t irradianceBeforeMove =
			constantSampler->GetIrradianceSampleCount();
		Require(service.Start(movedRequest, diagnostic),
			"the moved runtime priority should start: " + diagnostic);
		const RuntimeGIProbesStatus movedStatus = service.GetStatus();
		Require(movedStatus.m_readyProbeCount ==
				movedStatus.m_activeProbeCount &&
			IsNear(movedStatus.m_refinement, 1.0f) &&
			constantSampler->GetVisibilitySampleCount() == visibilityBeforeMove &&
			constantSampler->GetIrradianceSampleCount() == irradianceBeforeMove,
			"moving the camera must neither replace nor retrace the scene-wide grid");

		service.SetWorkAllowed(false);
		const uint64_t visibilityBeforeReturn =
			constantSampler->GetVisibilitySampleCount();
		const uint64_t irradianceBeforeReturn =
			constantSampler->GetIrradianceSampleCount();
		Require(service.Start(initialRequest, diagnostic),
			"returning to the initial runtime priority should start: " + diagnostic);
		const RuntimeGIProbesStatus returnedStatus = service.GetStatus();
		Require(returnedStatus.m_readyProbeCount ==
				returnedStatus.m_activeProbeCount &&
			IsNear(returnedStatus.m_refinement, 1.0f) &&
			constantSampler->GetVisibilitySampleCount() ==
				visibilityBeforeReturn &&
			constantSampler->GetIrradianceSampleCount() ==
				irradianceBeforeReturn,
			"camera motion must preserve every converged probe without retracing");
		service.Disable();
	}

	void TestRuntimeGIProbesProgressiveSamplingPublication()
	{
		RuntimeGIProbesService service;
		auto constantSampler =
			TSharedPtr<ConstantBakeRaySampler>::Make(glm::vec3(0.25f));
		TSharedPtr<IGIProbeBakeRaySampler> sampler = constantSampler;
		RuntimeGIProbesStartRequest request = MakeRuntimeGIProbesRequest(
			sampler,
			111u,
			121u);
		request.m_qualitySettings.m_maxActiveProbes = 64u;
		request.m_qualitySettings.m_initialSamplesPerProbe = 16u;
		request.m_qualitySettings.m_targetSamplesPerProbe = 32u;
		request.m_qualitySettings.m_workerCount = 1u;
		request.m_qualitySettings.m_cpuBudgetMilliseconds = 4.0f;
		request.m_qualitySettings.m_maxPublicationsPerSecond = 60.0f;
		request.m_qualitySettings.m_initialPublicationCoverage = 0.125f;
		request.m_qualitySettings.m_maxDirtyUploadBytesPerFrame = 64u * 1024u;
		request.m_geometryBounds.m_min = glm::vec3(-0.1f, -0.1f, -9.0f);
		request.m_geometryBounds.m_max = glm::vec3(0.6f, 0.1f, -2.0f);
		std::string diagnostic;
		Require(service.Start(request, diagnostic),
			"the progressive runtime GI fixture should start: " + diagnostic);
		Require(WaitForRuntimeGIProbes(
				service,
				[](const RuntimeGIProbesStatus& status)
				{
					return status.m_publishedRevision > 0u;
				}),
			"runtime GI should publish probes after their initial sample pass");

		const RuntimeGIProbesStatus initialStatus = service.GetStatus();
		const GIProbesDataPtr initialPublication = service.GetPublishedData();
		Require(initialPublication && initialPublication->m_bricks.Num() == 1u &&
			initialStatus.m_readyProbeCount < initialStatus.m_activeProbeCount,
			"the fixed grid may publish completed 16-sample probes before all probes warm");
		bool bHasReadyProbe = false;
		bool bHasPendingProbe = false;
		for (const GIProbe& probe : initialPublication->m_probes)
		{
			const bool bValid = (probe.m_flags &
				static_cast<uint32_t>(EGIProbeFlag::Valid)) != 0u;
			bHasReadyProbe |= bValid;
			bHasPendingProbe |= !bValid;
		}
		Require(bHasReadyProbe && bHasPendingProbe,
			"initial publication must preserve both completed probes and fallback cells");

		const uint64_t initialRevision = initialStatus.m_publishedRevision;
		Require(WaitForRuntimeGIProbes(
				service,
				[initialRevision](const RuntimeGIProbesStatus& status)
				{
					return status.m_lifecycle ==
						ERuntimeGIProbesLifecycle::Ready &&
						status.m_publishedRevision > initialRevision;
				}),
			"runtime probes should refine from 16 samples to the target without changing layout");
		service.Disable();
	}

	void TestRuntimeGIProbesRetainRelocationAcrossCameraMotion()
	{
		RuntimeGIProbesService service;
		auto boundarySampler = TSharedPtr<BoundaryRelocationSampler>::Make();
		TSharedPtr<IGIProbeBakeRaySampler> sampler = boundarySampler;
		RuntimeGIProbesStartRequest request = MakeRuntimeGIProbesRequest(
			sampler,
			91u,
			101u);
		request.m_geometryBounds.m_min = glm::vec3(-10.0f, -0.1f, -8.0f);
		request.m_geometryBounds.m_max = glm::vec3(10.0f, 0.1f, -1.0f);
		request.m_qualitySettings.m_maxActiveProbes = 256u;
		request.m_qualitySettings.m_initialSamplesPerProbe = 16u;
		request.m_qualitySettings.m_targetSamplesPerProbe = 16u;
		request.m_qualitySettings.m_maxDirtyUploadBytesPerFrame =
			128u * 1024u;
		std::string diagnostic;
		Require(service.Start(request, diagnostic),
			"the relocation-boundary runtime view should start: " + diagnostic);
		Require(WaitForRuntimeGIProbes(
				service,
				[](const RuntimeGIProbesStatus& status)
				{
					return status.m_lifecycle ==
						ERuntimeGIProbesLifecycle::Ready &&
						status.m_publishedRevision > 0u;
				}),
			"the relocation-boundary runtime view should publish");

		const GIProbesDataPtr firstPublished = service.GetPublishedData();
		Require(firstPublished && firstPublished->m_bricks.Num() == 1u,
			"the relocation-boundary fixture should publish one runtime brick");
		bool bFoundRelocation = false;
		for (const GIProbe& probe : firstPublished->m_probes)
		{
			bFoundRelocation |= (probe.m_flags & static_cast<uint32_t>(
				EGIProbeFlag::Relocated)) != 0u;
			Require(glm::all(glm::greaterThanEqual(
					probe.m_position,
					firstPublished->m_volumeMin)) &&
				glm::all(glm::lessThanEqual(
					probe.m_position,
					firstPublished->m_volumeMax)),
				"relocated runtime probes must stay inside the fixed grid");
		}
		Require(bFoundRelocation,
			"the fixture must produce at least one relocated runtime probe");

		const uint64_t firstRevision =
			service.GetStatus().m_publishedRevision;
		const uint64_t samplesBeforeMove = boundarySampler->GetSampleCount();
		RuntimeGIProbesStartRequest movedRequest = request;
		movedRequest.m_priorityPosition.x += 1.0f;
		Require(service.Start(movedRequest, diagnostic),
			"the moved relocation-boundary view should start: " + diagnostic);
		Require(WaitForRuntimeGIProbes(
				service,
				[firstRevision](const RuntimeGIProbesStatus& status)
				{
					return status.m_lifecycle ==
						ERuntimeGIProbesLifecycle::Failed ||
						status.m_publishedRevision > firstRevision;
				}),
			"the moved relocation-boundary view should finish its replacement");
		const RuntimeGIProbesStatus movedStatus = service.GetStatus();
		const GIProbesDataPtr movedPublished = service.GetPublishedData();
		Require(movedStatus.m_lifecycle !=
				ERuntimeGIProbesLifecycle::Failed &&
			movedStatus.m_publishedRevision > firstRevision &&
			movedPublished && movedPublished->Validate(diagnostic) &&
			boundarySampler->GetSampleCount() == samplesBeforeMove,
			"camera motion must retain relocated probes without retracing: " +
				movedStatus.m_diagnostic + " " + diagnostic);
		service.Disable();
	}

	void TestRuntimeGIProbesKeepOneGridAcrossManyViews()
	{
		RuntimeGIProbesService service;
		auto constantSampler =
			TSharedPtr<ConstantBakeRaySampler>::Make(glm::vec3(0.25f));
		TSharedPtr<IGIProbeBakeRaySampler> sampler = constantSampler;
		RuntimeGIProbesStartRequest request = MakeRuntimeGIProbesRequest(
			sampler,
			71u,
			81u);
		request.m_qualitySettings.m_initialSamplesPerProbe = 16u;
		request.m_qualitySettings.m_targetSamplesPerProbe = 16u;
		request.m_qualitySettings.m_maxDirtyUploadBytesPerFrame =
			64u * 1024u;
		request.m_geometryBounds.m_min =
			glm::vec3(4000.0f, -0.1f, -64.0f);
		request.m_geometryBounds.m_max =
			glm::vec3(66000.0f, 0.1f, -1.0f);
		std::string diagnostic;
		const auto convergeAt = [&service, &request, &diagnostic](float x)
		{
			request.m_priorityPosition.x = x;
			Require(service.Start(request, diagnostic),
				"a runtime priority view should start: " + diagnostic);
			Require(WaitForRuntimeGIProbes(
					service,
					[](const RuntimeGIProbesStatus& status)
					{
						return status.m_lifecycle ==
							ERuntimeGIProbesLifecycle::Ready;
					}),
				"the fixed runtime grid should stay converged");
		};

		convergeAt(4096.0f);
		const RuntimeGIProbesStartRequest oldestRequest = request;
		const uint64_t visibilityAfterInitialGrid =
			constantSampler->GetVisibilitySampleCount();
		const uint64_t irradianceAfterInitialGrid =
			constantSampler->GetIrradianceSampleCount();
		for (uint32_t viewIndex = 1u; viewIndex < 16u; ++viewIndex)
		{
			convergeAt(4096.0f * static_cast<float>(viewIndex + 1u));
		}
		const RuntimeGIProbesStatus filledStatus = service.GetStatus();
		Require(filledStatus.m_readyProbeCount ==
				filledStatus.m_activeProbeCount &&
			constantSampler->GetVisibilitySampleCount() ==
				visibilityAfterInitialGrid &&
			constantSampler->GetIrradianceSampleCount() ==
				irradianceAfterInitialGrid,
			"many camera positions must keep the same converged scene-wide grid");

		service.SetWorkAllowed(false);
		const uint64_t visibilityBeforeReturn =
			constantSampler->GetVisibilitySampleCount();
		const uint64_t irradianceBeforeReturn =
			constantSampler->GetIrradianceSampleCount();
		Require(service.Start(oldestRequest, diagnostic),
			"the original priority view should restart: " + diagnostic);
		const RuntimeGIProbesStatus returnedStatus = service.GetStatus();
		Require(returnedStatus.m_readyProbeCount ==
				returnedStatus.m_activeProbeCount &&
			IsNear(returnedStatus.m_refinement, 1.0f) &&
			constantSampler->GetVisibilitySampleCount() ==
				visibilityBeforeReturn &&
			constantSampler->GetIrradianceSampleCount() ==
				irradianceBeforeReturn,
			"returning to an earlier view must not retrace the fixed grid");
		service.Disable();
	}

	void TestDeterministicBakeSeedsAndReusedLayoutValidation()
	{
		GIProbesBakeRequest request;
		request.m_stateName = "Deterministic";
		request.m_volumeMin = glm::vec3(0.0f);
		request.m_volumeMax = glm::vec3(1.0f);
		request.m_settings.m_raysPerProbe = 8u;
		request.m_settings.m_bounceCount = 1u;
		request.m_settings.m_randomSeed = 1729u;
		request.m_settings.m_maxSubdivisionLevel = 0u;
		request.m_settings.m_minProbeSpacing = 1.0f;

		const SeedDrivenBakeRaySampler sampler;
		const GIProbesBakeResult first = GIProbesBaker::Bake(
			request,
			sampler);
		const GIProbesBakeResult second = GIProbesBaker::Bake(
			request,
			sampler);
		Require(first.IsSuccess() && second.IsSuccess(),
			"seed-driven test bakes should succeed");
		Require(first.m_data->m_lightingHash == second.m_data->m_lightingHash &&
			first.m_data->m_transportHash == second.m_data->m_transportHash &&
			first.m_data->m_probes[0].m_irradiance ==
				second.m_data->m_probes[0].m_irradiance,
			"the same bake randomSeed must reproduce transport and lighting output");

		GIProbesBakeRequest parallel = request;
		parallel.m_threadCount = 4u;
		std::string parallelStage;
		parallel.m_progress = [&parallelStage](
			const GIProbesBakeProgress& progress)
		{
			parallelStage = progress.m_stage;
		};
		const ConcurrentSeedDrivenBakeRaySampler parallelSampler(4u);
		const GIProbesBakeResult parallelResult = GIProbesBaker::Bake(
			parallel,
			parallelSampler);
		Require(parallelResult.IsSuccess(),
			"four-thread seed-driven bake should succeed: " +
				parallelResult.m_diagnostic);
		bool bSameProbeBits = first.m_data->m_probes.Num() ==
			parallelResult.m_data->m_probes.Num();
		for (size_t probeIndex = 0u;
			bSameProbeBits && probeIndex < first.m_data->m_probes.Num();
			++probeIndex)
		{
			bSameProbeBits = HasSameProbeBits(
				first.m_data->m_probes[probeIndex],
				parallelResult.m_data->m_probes[probeIndex]);
		}
		Require(
			parallelSampler.GetObservedThreadCount() == 4u &&
			parallelStage.find("(4 threads)") != std::string::npos &&
			first.m_data->m_layoutHash == parallelResult.m_data->m_layoutHash &&
			first.m_data->m_transportHash ==
				parallelResult.m_data->m_transportHash &&
			first.m_data->m_lightingHash ==
				parallelResult.m_data->m_lightingHash &&
			bSameProbeBits,
			"configured bake threads must execute concurrently without changing deterministic output");

		GIProbesBakeRequest differentSeed = request;
		differentSeed.m_settings.m_randomSeed += 1u;
		const GIProbesBakeResult different = GIProbesBaker::Bake(
			differentSeed,
			sampler);
		Require(different.IsSuccess() &&
			different.m_data->m_lightingHash != first.m_data->m_lightingHash,
			"a different bake randomSeed must select a different sampling stream");

		GIProbesBakeRequest invalidReuse = request;
		invalidReuse.m_layoutSource = first.m_data.GetRawPtr();
		invalidReuse.m_settings.m_raysPerProbe = 0u;
		const GIProbesBakeResult rejected = GIProbesBaker::Bake(
			invalidReuse,
			sampler);
		Require(rejected.m_status == EGIProbesBakeStatus::InvalidRequest,
			"layout reuse must not bypass ray and bounce count validation");

		invalidReuse.m_settings.m_raysPerProbe =
			GIProbesMaxRaysPerProbe + 1u;
		const GIProbesBakeResult excessive = GIProbesBaker::Bake(
			invalidReuse,
			sampler);
		Require(excessive.m_status == EGIProbesBakeStatus::InvalidRequest,
			"layout reuse must not bypass supported sampling limits");

		invalidReuse.m_settings.m_raysPerProbe = request.m_settings.m_raysPerProbe;
		invalidReuse.m_settings.m_skyIndirectIntensity = -1.0f;
		Require(
			GIProbesBaker::Bake(invalidReuse, sampler).m_status ==
				EGIProbesBakeStatus::InvalidRequest,
			"layout reuse must not bypass sky-intensity validation");

		GIProbesBakeRequest invalidThreads = request;
		invalidThreads.m_threadCount = 0u;
		Require(
			GIProbesBaker::Bake(invalidThreads, sampler).m_status ==
				EGIProbesBakeStatus::InvalidRequest,
			"a zero bake thread count must fail closed");
		invalidThreads.m_threadCount = GIProbesMaxBakeThreadCount + 1u;
		Require(
			GIProbesBaker::Bake(invalidThreads, sampler).m_status ==
				EGIProbesBakeStatus::InvalidRequest,
			"a bake thread count above the supported limit must fail closed");
	}

	void TestRelocationClampingPreservesEffectiveOffset()
	{
		GIProbesBakeRequest request;
		request.m_stateName = "Relocation";
		request.m_volumeMin = glm::vec3(0.0f);
		request.m_volumeMax = glm::vec3(1.0f);
		request.m_settings.m_raysPerProbe = 8u;
		request.m_settings.m_bounceCount = 1u;
		request.m_settings.m_maxSubdivisionLevel = 0u;
		request.m_settings.m_minProbeSpacing = 1.0f;

		const BoundaryRelocationSampler sampler;
		const GIProbesBakeResult result = GIProbesBaker::Bake(
			request,
			sampler);
		Require(result.IsSuccess(),
			"boundary-relocation bake should succeed: " + result.m_diagnostic);

		const GIProbe& clamped = result.m_data->m_probes[0];
		const GIProbe& moved = result.m_data->m_probes[1];
		const uint32_t relocatedFlag = static_cast<uint32_t>(
			EGIProbeFlag::Relocated);
		Require(clamped.m_position.x == 0.0f &&
			clamped.m_relocationOffset == glm::vec3(0.0f) &&
			(clamped.m_flags & relocatedFlag) == 0u,
			"a relocation clipped completely by the volume must record zero effective offset");
		Require(IsNear(moved.m_position.x, 0.75f) &&
			IsNear(moved.m_relocationOffset.x, -0.25f) &&
			(moved.m_flags & relocatedFlag) != 0u,
			"a moved probe must store the post-clamp offset used by debug and transport identity");
	}

	void TestEmbeddedProbeClassificationAndStableTransport()
	{
		GIProbesBakeRequest request;
		request.m_stateName = "Half Space";
		request.m_volumeMin = glm::vec3(-0.1f, 0.0f, 0.0f);
		request.m_volumeMax = glm::vec3(0.9f, 1.0f, 1.0f);
		request.m_settings.m_raysPerProbe = 8u;
		request.m_settings.m_bounceCount = 1u;
		request.m_settings.m_maxSubdivisionLevel = 0u;
		request.m_settings.m_minProbeSpacing = 1.0f;
		request.m_settings.m_maxRayDistance = 10.0f;

		const HalfSpaceBakeRaySampler halfSpaceSampler;
		const GIProbesBakeResult halfSpace = GIProbesBaker::Bake(
			request,
			halfSpaceSampler);
		Require(halfSpace.IsSuccess(),
			"half-space relocation bake should succeed: " +
				halfSpace.m_diagnostic);
		const uint32_t relocatedFlag = static_cast<uint32_t>(
			EGIProbeFlag::Relocated);
		for (uint32_t probeIndex = 0u;
			probeIndex < halfSpace.m_data->m_probes.Num();
			++probeIndex)
		{
			const GIProbe& probe =
				halfSpace.m_data->m_probes[probeIndex];
			Require(probe.m_validity > 0.99f,
				"probes that can be moved outside a back-facing half-space must remain valid");
			if ((probeIndex & 1u) == 0u)
			{
				Require(
					probe.m_position.x > 0.0f &&
					probe.m_relocationOffset.x > 0.1f &&
					(probe.m_flags & relocatedFlag) != 0u,
					"an embedded probe must cross its closest back-facing surface "
					"instead of remaining inside geometry");
			}
		}

		GIProbesBakeRequest embeddedRequest = request;
		embeddedRequest.m_stateName = "Embedded";
		embeddedRequest.m_volumeMin = glm::vec3(0.0f);
		embeddedRequest.m_volumeMax = glm::vec3(1.0f);
		const FullyEmbeddedBakeRaySampler embeddedSampler;
		const GIProbesBakeResult embedded = GIProbesBaker::Bake(
			embeddedRequest,
			embeddedSampler);
		Require(embedded.IsSuccess(),
			"fully embedded classification bake should succeed: " +
				embedded.m_diagnostic);
		for (const GIProbe& probe : embedded.m_data->m_probes)
		{
			float irradianceEnergy = 0.0f;
			for (const glm::vec3& coefficient : probe.m_irradiance)
			{
				irradianceEnergy += glm::length(coefficient);
			}
			Require(
				probe.m_validity == 0.0f &&
				(probe.m_flags & static_cast<uint32_t>(
					EGIProbeFlag::Valid)) == 0u &&
				irradianceEnergy <= 0.000001f,
				"a probe still surrounded by back faces must be invalid and must "
				"not publish a black or bright irradiance sample");
		}

		GIProbesBakeRequest stableRequest = embeddedRequest;
		stableRequest.m_stateName = "Stable Transport";
		const DirectionalDistanceBakeRaySampler directionalSampler;
		const GIProbesBakeResult stable = GIProbesBaker::Bake(
			stableRequest,
			directionalSampler);
		Require(stable.IsSuccess(),
			"directional transport bake should succeed: " +
				stable.m_diagnostic);
		for (size_t probeIndex = 1u;
			probeIndex < stable.m_data->m_probes.Num();
			++probeIndex)
		{
			for (uint32_t directionIndex = 0u;
				directionIndex < GIProbeVisibilityDirectionCount;
				++directionIndex)
			{
				Require(HasSameVectorBits(
						stable.m_data->m_probes[0].m_visibility[directionIndex],
						stable.m_data->m_probes[probeIndex].m_visibility[directionIndex]),
					"visibility transport must use the same fixed directional estimator "
					"at neighbouring probes instead of random per-probe lobes");
				Require(
					IsNear(
						stable.m_data->m_probes[0].m_environmentVisibility[directionIndex],
						0.0f) &&
					HasSameFloatBits(
						stable.m_data->m_probes[0].m_environmentVisibility[directionIndex],
						stable.m_data->m_probes[probeIndex].m_environmentVisibility[directionIndex]),
					"geometry hits must close the corresponding deterministic sky-specular transport lobe");
			}
		}
		const float visibilityMaxDistance =
			CalculateGIProbeVisibilityMaxDistance(
				*stable.m_data,
				stable.m_data->m_bricks[0]);
		for (const GIProbe& probe : stable.m_data->m_probes)
		{
			for (uint32_t directionIndex = 0u;
				directionIndex < GIProbeVisibilityDirectionCount;
				++directionIndex)
			{
				const glm::vec2 moments =
					probe.m_visibility[directionIndex];
				Require(
					moments.x > 0.0f &&
					moments.x < visibilityMaxDistance &&
					moments.y >= moments.x * moments.x &&
					moments.y <= visibilityMaxDistance * visibilityMaxDistance,
					"local blocker transport must store bounded first and second "
						"distance moments for every broad signed-axis lobe");
			}
			Require(
				probe.m_visibility[0u].x > probe.m_visibility[1u].x &&
				probe.m_visibility[4u].x > probe.m_visibility[5u].x,
				"broad lobe moments must preserve the sampler's positive X and Z distance gradients");
			Require(
				(probe.m_flags & GIProbeBlockedDirectionMask) ==
					GIProbeBlockedDirectionMask,
				"a deterministic local hit in every signed-axis lobe must bake "
				"all six blocking-direction bits");
		}
	}

	void TestPathTracerBackfacesAndIgnoredTriangleTraversal()
	{
		Memory::ObjectAllocatorPtr allocator =
			Memory::ObjectAllocatorPtr::Make(
				Memory::EAllocationPolicy::SharedMemory_MultiThreaded);
		TVector<MaterialPtr> materials;
		materials.Add(MakeDiffuseFixtureMaterial(
			allocator,
			glm::vec3(0.8f)));

		const auto makeTriangle = [](float height)
		{
			Math::Triangle triangle{};
			triangle.m_vertices[0] = glm::vec3(-1.0f, height, -1.0f);
			triangle.m_vertices[1] = glm::vec3(0.0f, height, 1.0f);
			triangle.m_vertices[2] = glm::vec3(1.0f, height, -1.0f);
			triangle.m_centroid =
				(triangle.m_vertices[0] + triangle.m_vertices[1] +
					triangle.m_vertices[2]) / 3.0f;
			for (uint32_t vertexIndex = 0u; vertexIndex < 3u; ++vertexIndex)
			{
				triangle.m_normals[vertexIndex] = glm::vec3(0.0f, 1.0f, 0.0f);
				triangle.m_tangent[vertexIndex] = glm::vec3(1.0f, 0.0f, 0.0f);
				triangle.m_bitangent[vertexIndex] = glm::vec3(0.0f, 0.0f, -1.0f);
				triangle.m_colors[vertexIndex] = glm::vec4(1.0f);
			}
			return triangle;
		};

		auto triangles = TSharedPtr<TVector<Math::Triangle>>::Make();
		triangles->Add(makeTriangle(1.0f));
		triangles->Add(makeTriangle(0.0f));
		auto blas = TSharedPtr<Raytracing::BVH>::Make(2u);
		GlobalIlluminationLandscapeTestScene::BuildBakeBlas(
			*blas,
			*triangles);
		Math::AABB bounds;
		for (const Math::Triangle& triangle : *triangles)
		{
			for (const glm::vec3& vertex : triangle.m_vertices)
			{
				bounds.Extend(vertex);
			}
		}

		Raytracing::PathTracer::TLASInstance instance;
		instance.m_triangles = triangles;
		instance.m_blas = blas;
		instance.m_worldBounds = bounds;
		instance.m_worldMatrix = glm::mat4(1.0f);
		instance.m_inverseWorldMatrix = glm::mat4(1.0f);
		instance.m_materialBaseOffset = 0;
		TVector<Raytracing::PathTracer::TLASInstance> instances;
		instances.Add(std::move(instance));

		InspectablePathTracer pathTracer;
		Require(pathTracer.InitializeScene(
				instances,
				materials,
				{},
				false),
			"the backface traversal fixture must initialize");
		Raytracing::PathTracer::PreparedRaySample front;
		Raytracing::PathTracer::PreparedRaySample back;
		Require(
			pathTracer.SamplePreparedSceneVisibility(
				glm::vec3(0.0f, 2.0f, 0.0f),
				glm::vec3(0.0f, -1.0f, 0.0f),
				4.0f,
				front) &&
			front.m_bHit &&
			!front.m_bBackFace &&
			pathTracer.SamplePreparedSceneVisibility(
				glm::vec3(0.0f, -1.0f, 0.0f),
				glm::vec3(0.0f, 1.0f, 0.0f),
				4.0f,
				back) &&
			back.m_bHit &&
			back.m_bBackFace,
			"visibility rays must distinguish front-facing geometry from probes "
			"tracing outward through back faces");

		uint32_t triangleIndex = (std::numeric_limits<uint32_t>::max)();
		float distance = 0.0f;
		Require(
			pathTracer.IntersectIgnoring(
				Math::Ray(
					glm::vec3(0.0f, 2.0f, 0.0f),
					glm::vec3(0.0f, -1.0f, 0.0f)),
				0u,
				0u,
				triangleIndex,
				distance) &&
			triangleIndex == 1u &&
			IsNear(distance, 2.0f),
			"ignoring the source triangle must continue through the same BLAS "
			"instead of making its entire mesh transparent");
	}

	void TestPathTracerPreparationDeduplicationAndProgress()
	{
		EveningLandscapeRaytracingFixture fixture =
			MakeEveningLandscapeRaytracingFixture();
		fixture.m_instances[0].m_blas.Clear();
		fixture.m_instances.Add(fixture.m_instances[0]);
		auto cpuTextureFixture = TObjectPtr<CpuTextureFixture>::Make(
			fixture.m_allocator,
			FileId::Invalid);
		cpuTextureFixture->SetPixel(glm::u8vec4(64u, 128u, 192u, 255u));
		const TexturePtr sharedTexture = cpuTextureFixture;
		fixture.m_materials[0]->SetSampler(
			"baseColorSampler",
			sharedTexture);
		fixture.m_materials[1]->SetSampler(
			"baseColorSampler",
			sharedTexture);

		TVector<MaterialPtr> duplicatedMaterials;
		for (const MaterialPtr& material : fixture.m_materials)
		{
			duplicatedMaterials.Add(material);
			duplicatedMaterials.Add(material);
		}

		bool bSawGeometryStart = false;
		bool bSawGeometryComplete = false;
		bool bSawMaterialsStart = false;
		bool bSawMaterialsComplete = false;
		GIProbesBakeSettings settings;
		Raytracing::GIProbesPathTracer pathTracer;
		Require(
			pathTracer.Initialize(
				fixture.m_instances,
				duplicatedMaterials,
				fixture.m_lights,
				settings,
				glm::vec3(0.0f),
				[&](
					const Raytracing::PathTracer::ScenePreparationProgress&
						progress)
				{
					if (progress.m_stage == Raytracing::PathTracer::
						EScenePreparationStage::Geometry)
					{
						bSawGeometryStart |= progress.m_completed == 0u;
						bSawGeometryComplete |=
							progress.m_completed == fixture.m_instances.Num() &&
							progress.m_total == fixture.m_instances.Num();
					}
					else
					{
						bSawMaterialsStart |= progress.m_completed == 0u;
						bSawMaterialsComplete |=
							progress.m_completed == duplicatedMaterials.Num() &&
							progress.m_total == duplicatedMaterials.Num();
					}
					return true;
				}),
			"path-tracer preparation must accept immutable geometry without prebuilt BLAS instances");
		Require(
			bSawGeometryStart && bSawGeometryComplete &&
			bSawMaterialsStart && bSawMaterialsComplete,
			"path-tracer preparation must report live geometry and material progress");

		const auto& stats = pathTracer.GetLastScenePreparationStats();
		Require(
			stats.m_instanceCount == 2u &&
			stats.m_geometryInstanceCount == 2u &&
			stats.m_builtBlasCount == 1u &&
			stats.m_reusedBlasCount == 1u,
			"identical vegetation geometry snapshots must build one shared CPU BLAS");
		Require(
			stats.m_materialSlotCount == duplicatedMaterials.Num() &&
			stats.m_uniqueMaterialCount == fixture.m_materials.Num() &&
			stats.m_reusedMaterialCount == fixture.m_materials.Num(),
			"repeated material slots must reuse one prepared material per loaded object");
		Require(
			stats.m_textureReferenceCount == 2u &&
			stats.m_uniqueTextureCount == 1u &&
			stats.m_decodedTextureCount == 0u,
			"distinct materials sharing one resident texture must snapshot its CPU pixels once");

		bool bCancellationRequested = false;
		Raytracing::GIProbesPathTracer cancelledPathTracer;
		Require(
			!cancelledPathTracer.Initialize(
				fixture.m_instances,
				duplicatedMaterials,
				fixture.m_lights,
				settings,
				glm::vec3(0.0f),
				[&](const Raytracing::PathTracer::ScenePreparationProgress&)
				{
					bCancellationRequested = true;
					return false;
				}) &&
			bCancellationRequested,
			"path-tracer preparation must stop when its progress callback requests cancellation");
	}

	void TestProbeBakeSkipsUnavailableMeshAndMaterialInstances()
	{
		Memory::ObjectAllocatorPtr allocator =
			Memory::ObjectAllocatorPtr::Make(
				Memory::EAllocationPolicy::SharedMemory_MultiThreaded);
		MaterialPtr validMaterial = MakeDiffuseFixtureMaterial(
			allocator,
			glm::vec3(0.8f));
		MaterialPtr unresolvedMaterial = MakeDiffuseFixtureMaterial(
			allocator,
			glm::vec3(0.8f));
		unresolvedMaterial->SetSampler(
			"baseColorSampler",
			TObjectPtr<CpuTextureFixture>::Make(
				allocator,
				FileId::Invalid));
		TVector<MaterialPtr> materials;
		materials.Add(validMaterial);
		materials.Add(unresolvedMaterial);

		Math::Triangle triangle{};
		triangle.m_vertices[0] = glm::vec3(-0.75f, 0.0f, -0.75f);
		triangle.m_vertices[1] = glm::vec3(0.0f, 0.0f, 0.75f);
		triangle.m_vertices[2] = glm::vec3(0.75f, 0.0f, -0.75f);
		triangle.m_centroid =
			(triangle.m_vertices[0] + triangle.m_vertices[1] +
				triangle.m_vertices[2]) / 3.0f;
		for (uint32_t vertexIndex = 0u; vertexIndex < 3u; ++vertexIndex)
		{
			triangle.m_normals[vertexIndex] = glm::vec3(0.0f, 1.0f, 0.0f);
			triangle.m_tangent[vertexIndex] = glm::vec3(1.0f, 0.0f, 0.0f);
			triangle.m_bitangent[vertexIndex] = glm::vec3(0.0f, 0.0f, -1.0f);
			triangle.m_colors[vertexIndex] = glm::vec4(1.0f);
		}
		auto triangles = TSharedPtr<TVector<Math::Triangle>>::Make();
		triangles->Add(triangle);
		auto blas = TSharedPtr<Raytracing::BVH>::Make(1u);
		GlobalIlluminationLandscapeTestScene::BuildBakeBlas(
			*blas,
			*triangles);
		Math::AABB localBounds;
		for (const glm::vec3& vertex : triangle.m_vertices)
		{
			localBounds.Extend(vertex);
		}

		const auto makeInstance = [&](float x,
			int32_t materialBaseOffset,
			const std::string& debugName)
		{
			Raytracing::PathTracer::TLASInstance instance;
			instance.m_triangles = triangles;
			instance.m_blas = blas;
			instance.m_worldMatrix = glm::translate(
				glm::mat4(1.0f),
				glm::vec3(x, 0.0f, 0.0f));
			instance.m_inverseWorldMatrix = glm::inverse(
				instance.m_worldMatrix);
			instance.m_worldBounds = localBounds;
			instance.m_worldBounds.Apply(instance.m_worldMatrix);
			instance.m_materialBaseOffset = materialBaseOffset;
			instance.m_debugName = debugName;
			return instance;
		};
		TVector<Raytracing::PathTracer::TLASInstance> instances;
		instances.Add(makeInstance(-2.0f, 0, "valid mesh 'Receiver'"));
		instances.Add(makeInstance(2.0f, 1, "static mesh 'Broken Mesh'"));
		auto unavailableMesh = makeInstance(
			6.0f,
			0,
			"static mesh 'Unavailable Mesh'");
		unavailableMesh.m_blas.Clear();
		unavailableMesh.m_triangles.Clear();
		instances.Add(std::move(unavailableMesh));

		Raytracing::LightProxy light;
		light.m_type = ELightType::Directional;
		light.m_direction = glm::vec3(0.0f, -1.0f, 0.0f);
		light.m_intensity = glm::vec3(2.0f);
		TVector<Raytracing::LightProxy> lights;
		lights.Add(light);

		TVector<std::string> warnings;
		GIProbesBakeSettings settings;
		settings.m_bIncludeSky = false;
		settings.m_bIncludeEmissive = false;
		settings.m_bIncludeDirectLighting = true;
		settings.m_bounceCount = 0u;
		Raytracing::GIProbesPathTracer pathTracer;
		Require(pathTracer.Initialize(
				instances,
				materials,
				lights,
				settings,
				glm::vec3(0.0f),
				{},
				[&warnings](const std::string& warning)
				{
					warnings.Add(warning);
				}),
			"probe bake must continue when another mesh has an unresolved material texture");

		const auto& stats = pathTracer.GetLastScenePreparationStats();
		Require(
			stats.m_instanceCount == 3u &&
			stats.m_geometryInstanceCount == 1u &&
			stats.m_skippedInstanceCount == 2u,
			"only valid mesh instances must remain in the bake scene");
		bool bNamedSamplerWarning = false;
		bool bNamedMeshWarning = false;
		bool bNamedUnavailableMeshWarning = false;
		for (const std::string& warning : warnings)
		{
			bNamedSamplerWarning |=
				warning.find("baseColorSampler") != std::string::npos;
			bNamedMeshWarning |=
				warning.find("Broken Mesh") != std::string::npos;
			bNamedUnavailableMeshWarning |=
				warning.find("Unavailable Mesh") != std::string::npos;
		}
		Require(
			bNamedSamplerWarning && bNamedMeshWarning &&
			bNamedUnavailableMeshWarning,
			"warnings must identify failed sampler, material mesh, and unavailable mesh");

		GIProbeBakeRaySample validSample;
		GIProbeBakeRaySample skippedSample;
		std::string diagnostic;
		Require(pathTracer.Sample(
				glm::vec3(-2.0f, 2.0f, 0.0f),
				glm::vec3(0.0f, -1.0f, 0.0f),
				4.0f,
				1u,
				validSample,
				diagnostic) &&
			validSample.m_bHit,
			"valid geometry must remain available to the continuing bake");
		Require(pathTracer.Sample(
				glm::vec3(2.0f, 2.0f, 0.0f),
				glm::vec3(0.0f, -1.0f, 0.0f),
				4.0f,
				2u,
				skippedSample,
				diagnostic) &&
			!skippedSample.m_bHit &&
			glm::length(skippedSample.m_radiance) <= 0.000001f,
			"the mesh using the unresolved material must not leak fallback shading into the bake");
	}

	void TestFloatTextureNormalizationAndLandscapeLayerSampling()
	{
		Raytracing::CombinedSampler2D floatTexture;
		floatTexture.m_width = 1;
		floatTexture.m_height = 1;
		const glm::vec4 source(0.25f, 0.5f, 0.75f, 1.0f);
		floatTexture.Initialize<glm::vec4, glm::vec4>(
			&source,
			false);
		Require(floatTexture.Sample<glm::vec4>(glm::vec2(0.5f)) == source,
			"decoded floating-point textures must not be normalized as byte data");

		MaterialSamplingPathTracer pathTracer;
		const Raytracing::LightingModel::SampledData sampled =
			pathTracer.SampleLayeredMaterial(
				glm::vec4(1.0f, 0.0f, 0.0f, 1.0f),
				glm::vec4(0.0f, 0.0f, 1.0f, 1.0f),
				glm::vec4(1.0f, 3.0f, 0.0f, 0.0f));
		Require(
			IsNear(sampled.m_baseColor.r, 0.125f) &&
			IsNear(sampled.m_baseColor.g, 0.0f) &&
			IsNear(sampled.m_baseColor.b, 0.75f) &&
			IsNear(sampled.m_baseColor.a, 1.0f),
			"the bake path tracer must normalize vertex layer weights and sample landscape layers");

		const glm::vec3 scaledNormal = pathTracer.SampleScaledNormal(
			glm::vec3(0.8f, -0.6f, 0.4f),
			0.25f);
		Require(
			IsNear(scaledNormal.x, 0.2f) &&
			IsNear(scaledNormal.y, -0.15f) &&
			IsNear(scaledNormal.z, 0.4f),
			"the bake path tracer must apply the material normal scale to tangent-space XY");

		const glm::vec4 vertexTinted = pathTracer.SampleVertexTint(
			glm::vec4(0.8f, 0.6f, 0.4f, 0.5f),
			glm::vec4(0.25f, 0.5f, 0.75f, 0.4f));
		Require(
			IsNear(vertexTinted.r, 0.2f) &&
			IsNear(vertexTinted.g, 0.3f) &&
			IsNear(vertexTinted.b, 0.3f) &&
			IsNear(vertexTinted.a, 0.2f),
			"ordinary bake materials must apply vertex color and alpha like the raster shader");

		EveningLandscapeRaytracingFixture fixture =
			MakeEveningLandscapeRaytracingFixture();
		MaterialPtr layeredMaterial = fixture.m_materials[0];
		layeredMaterial->SetUniform("material.clearcoatFactor", 0.8f);
		layeredMaterial->SetUniform(
			"material.clearcoatRoughnessFactor",
			0.6f);
		layeredMaterial->SetUniform(
			"material.clearcoatNormalScale",
			0.5f);
		layeredMaterial->SetUniform(
			"material.sheenColorFactor",
			glm::vec4(0.75f, 0.5f, 0.25f, 0.0f));
		layeredMaterial->SetUniform(
			"material.sheenRoughnessFactor",
			0.9f);
		auto layeredTexture = TObjectPtr<CpuTextureFixture>::Make(
			fixture.m_allocator,
			FileId::Invalid);
		layeredTexture->SetPixel(glm::u8vec4(128u, 64u, 192u, 32u));
		const TexturePtr texture = layeredTexture;
		layeredMaterial->SetSampler("clearcoatSampler", texture);
		layeredMaterial->SetSampler(
			"clearcoatRoughnessSampler",
			texture);
		layeredMaterial->SetSampler("clearcoatNormalSampler", texture);
		layeredMaterial->SetSampler("sheenColorSampler", texture);
		layeredMaterial->SetSampler("sheenRoughnessSampler", texture);

		MaterialSamplingPathTracer preparedPathTracer;
		Require(preparedPathTracer.InitializeScene(
				fixture.m_instances,
				fixture.m_materials,
				fixture.m_lights,
				false),
			"the layered material conversion fixture must initialize");
		const Raytracing::LightingModel::SampledData layeredSample =
			preparedPathTracer.SamplePreparedMaterial(0u);
		const auto srgbToLinear = [](float value)
		{
			return value <= 0.04045f ?
				value / 12.92f :
				std::pow((value + 0.055f) / 1.055f, 2.4f);
		};
		const float red = 128.0f / 255.0f;
		const float green = 64.0f / 255.0f;
		const float blue = 192.0f / 255.0f;
		const float alpha = 32.0f / 255.0f;
		Require(
			IsNear(layeredSample.m_clearcoatFactor, 0.8f * red) &&
			IsNear(
				layeredSample.m_clearcoatRoughness,
				0.6f * green) &&
			IsNear(
				layeredSample.m_clearcoatNormal.x,
				(2.0f * red - 1.0f) * 0.5f) &&
			IsNear(
				layeredSample.m_clearcoatNormal.y,
				(2.0f * green - 1.0f) * 0.5f) &&
			IsNear(
				layeredSample.m_clearcoatNormal.z,
				2.0f * blue - 1.0f),
			"the bake material converter must preserve clearcoat factor, G-channel roughness, and its independently scaled normal map");
		Require(
			IsNear(
				layeredSample.m_sheenColor.r,
				0.75f * srgbToLinear(red)) &&
			IsNear(
				layeredSample.m_sheenColor.g,
				0.5f * srgbToLinear(green)) &&
			IsNear(
				layeredSample.m_sheenColor.b,
				0.25f * srgbToLinear(blue)) &&
			IsNear(layeredSample.m_sheenRoughness, 0.9f * alpha),
			"the bake material converter must linearize sheen color and read sheen roughness from alpha");
	}

	void TestMobilityAndLightModeContributionPolicy()
	{
		Require(
			IsGlobalIlluminationBakeContributor(EMobilityType::Static) &&
			IsGlobalIlluminationBakeContributor(EMobilityType::Stationary),
			"static and stationary scene objects must contribute geometry and lights to baked global illumination");
		Require(
			!IsGlobalIlluminationBakeContributor(EMobilityType::Dynamic),
			"dynamic scene objects must not contribute bake geometry or lights");
		Require(
			ContributesToRealtimeLighting(
				ELightGlobalIlluminationMode::Realtime) &&
			!ContributesToBakedGlobalIllumination(
				ELightGlobalIlluminationMode::Realtime) &&
			ContributesToRealtimeLighting(
				ELightGlobalIlluminationMode::RealtimeAndBaked) &&
			ContributesToBakedGlobalIllumination(
				ELightGlobalIlluminationMode::RealtimeAndBaked) &&
			!ContributesToRealtimeLighting(
				ELightGlobalIlluminationMode::BakedOnly) &&
			ContributesToBakedGlobalIllumination(
				ELightGlobalIlluminationMode::BakedOnly),
			"per-light GI modes must map to the expected realtime and baked paths");

		const auto& properties =
			LightComponent::GetStaticTypeInfo().Properties();
		Require(
			properties.ContainsKey("globalIlluminationMode") &&
			properties["globalIlluminationMode"] ==
				"enum Sailor::ELightGlobalIlluminationMode",
			"the light GI mode must be exported as an Editor-compatible enum");

		GlobalIlluminationMobilityTestWorld world;
		const auto addLight = [&world](
			const char* name,
			EMobilityType mobility,
			float intensity,
			ELightGlobalIlluminationMode mode)
		{
			GameObjectPtr gameObject = world.Instantiate(name);
			gameObject->SetMobilityType(mobility);
			auto light = gameObject->AddComponent<LightComponent>();
			Require(
				light->GetGlobalIlluminationMode() ==
					ELightGlobalIlluminationMode::RealtimeAndBaked,
				"new lights must default to realtime plus baked behavior");
			light->SetIntensity(glm::vec3(intensity));
			light->SetGlobalIlluminationMode(mode);
			return light;
		};
		auto realtimeLight = addLight(
			"RealtimeLight",
			EMobilityType::Stationary,
			1.0f,
			ELightGlobalIlluminationMode::Realtime);
		addLight(
			"RealtimeAndBakedLight",
			EMobilityType::Stationary,
			2.0f,
			ELightGlobalIlluminationMode::RealtimeAndBaked);
		addLight(
			"BakedOnlyLight",
			EMobilityType::Stationary,
			3.0f,
			ELightGlobalIlluminationMode::BakedOnly);
		addLight(
			"DynamicRealtimeAndBakedLight",
			EMobilityType::Dynamic,
			4.0f,
			ELightGlobalIlluminationMode::RealtimeAndBaked);

		const ReflectedData reflectedRealtimeLight =
			realtimeLight->GetReflectedData();
		Require(
			reflectedRealtimeLight.GetProperties().ContainsKey(
				"globalIlluminationMode") &&
			reflectedRealtimeLight.GetProperties()[
				"globalIlluminationMode"].as<std::string>() == "Realtime",
			"the selected per-light GI mode must serialize into the world override");

		LightingECS* lighting = world.GetECS<LightingECS>();
		TVector<Raytracing::LightProxy> realtimeLights;
		TVector<Raytracing::LightProxy> bakeLights;
		lighting->GetLightProxies(realtimeLights);
		lighting->GetGlobalIlluminationBakeLightProxies(bakeLights);
		const auto containsIntensity = [](const auto& lights, float intensity)
		{
			return std::any_of(
				lights.begin(),
				lights.end(),
				[intensity](const Raytracing::LightProxy& light)
				{
					return light.m_intensity == glm::vec3(intensity);
				});
		};
		Require(
			realtimeLights.Num() == 3u &&
				containsIntensity(realtimeLights, 1.0f) &&
				containsIntensity(realtimeLights, 2.0f) &&
				!containsIntensity(realtimeLights, 3.0f) &&
				containsIntensity(realtimeLights, 4.0f),
			"realtime lighting must exclude Baked Only while retaining dynamic realtime lights");
		Require(
			bakeLights.Num() == 2u &&
				containsIntensity(bakeLights, 2.0f) &&
				containsIntensity(bakeLights, 3.0f) &&
				!containsIntensity(bakeLights, 1.0f) &&
				!containsIntensity(bakeLights, 4.0f),
			"GI bake lighting must honor per-light mode and still exclude Dynamic lights");

		realtimeLight->SetGlobalIlluminationMode(
			ELightGlobalIlluminationMode::BakedOnly);
		lighting->GetLightProxies(realtimeLights);
		lighting->GetGlobalIlluminationBakeLightProxies(bakeLights);
		Require(
			realtimeLights.Num() == 2u &&
				!containsIntensity(realtimeLights, 1.0f) &&
				bakeLights.Num() == 3u &&
				containsIntensity(bakeLights, 1.0f),
			"changing a light GI mode in the Editor must affect both paths immediately");
		world.Clear();
	}

	void TestPointLightModeChangesBakedRadiance()
	{
		using namespace GlobalIlluminationLandscapeTestScene;

		EveningLandscapeRaytracingFixture fixture =
			MakeEveningLandscapeRaytracingFixture();
		fixture.m_lights.Clear();

		GlobalIlluminationMobilityTestWorld world;
		GameObjectPtr pointLightObject = world.Instantiate("PointLight");
		pointLightObject->SetMobilityType(EMobilityType::Stationary);
		auto pointLight = pointLightObject->AddComponent<LightComponent>();
		pointLight->SetLightType(ELightType::Point);
		pointLight->SetIntensity(glm::vec3(40.0f));
		pointLight->SetIndirectLightingIntensity(1.0f);
		pointLight->SetRadius(20.0f);
		pointLight->SetShadowType(RHI::EShadowType::None);

		const glm::vec3 receiver(
			20.5f,
			SampleLandscapeHeight(20.5f, 20.5f),
			20.5f);
		pointLightObject->GetTransformComponent().SetPosition(
			receiver + glm::vec3(0.0f, 6.0f, 0.0f));
		world.GetECS<TransformECS>()->Tick(0.0f);

		GIProbesBakeSettings settings;
		settings.m_bounceCount = 0u;
		settings.m_normalBias = 0.001f;
		settings.m_viewBias = 0.0f;
		settings.m_bIncludeSky = false;
		settings.m_bIncludeEmissive = false;
		settings.m_bIncludeDirectLighting = true;

		const auto sampleMode = [&](ELightGlobalIlluminationMode mode)
		{
			pointLight->SetGlobalIlluminationMode(mode);
			TVector<Raytracing::LightProxy> bakeLights;
			world.GetECS<LightingECS>()
				->GetGlobalIlluminationBakeLightProxies(bakeLights);
			Require(
				bakeLights.IsEmpty() || !bakeLights[0].m_bCastShadows,
				"GI bake proxies must preserve a light's disabled shadow state");

			Raytracing::GIProbesPathTracer pathTracer;
			Require(pathTracer.Initialize(
					fixture.m_instances,
					fixture.m_materials,
					bakeLights,
					settings,
					glm::vec3(0.0f)),
				"the point-light GI fixture must initialize the CPU path tracer");

			GIProbeBakeRaySample sample;
			std::string diagnostic;
			Require(pathTracer.Sample(
					receiver + glm::vec3(0.0f, 3.0f, 0.0f),
					glm::vec3(0.0f, -1.0f, 0.0f),
					20.0f,
					155u,
					sample,
					diagnostic),
				"the point-light GI fixture must sample the landscape: " +
					diagnostic);
			Require(sample.m_bHit,
				"the point-light GI fixture ray must hit the landscape");
			return std::pair<size_t, glm::vec3>(
				bakeLights.Num(),
				sample.m_radiance);
		};

		const auto [realtimeLightCount, realtimeRadiance] = sampleMode(
			ELightGlobalIlluminationMode::Realtime);
		const auto [bakedLightCount, bakedRadiance] = sampleMode(
			ELightGlobalIlluminationMode::RealtimeAndBaked);
		Require(
			realtimeLightCount == 0u &&
				glm::length(realtimeRadiance) <= 0.000001f,
			"Realtime point lights must not contribute radiance to a GI bake");
		Require(
			bakedLightCount == 1u &&
				std::isfinite(bakedRadiance.x) &&
				std::isfinite(bakedRadiance.y) &&
				std::isfinite(bakedRadiance.z) &&
				glm::length(bakedRadiance) > 0.01f,
			"Realtime + Baked point lights must contribute actual baked radiance");

		world.Clear();
	}

	void TestPointLightRadiusAttenuationAndSecondaryBounce()
	{
		Memory::ObjectAllocatorPtr allocator =
			Memory::ObjectAllocatorPtr::Make(
				Memory::EAllocationPolicy::SharedMemory_MultiThreaded);
		TVector<MaterialPtr> materials;
		materials.Add(MakeDiffuseFixtureMaterial(
			allocator,
			glm::vec3(0.8f)));

		Math::Triangle floor{};
		floor.m_vertices[0] = glm::vec3(-100.0f, 0.0f, -100.0f);
		floor.m_vertices[1] = glm::vec3(0.0f, 0.0f, 100.0f);
		floor.m_vertices[2] = glm::vec3(100.0f, 0.0f, -100.0f);
		floor.m_centroid =
			(floor.m_vertices[0] + floor.m_vertices[1] +
				floor.m_vertices[2]) / 3.0f;
		for (uint32_t vertexIndex = 0u; vertexIndex < 3u; ++vertexIndex)
		{
			floor.m_normals[vertexIndex] = glm::vec3(0.0f, 1.0f, 0.0f);
			floor.m_tangent[vertexIndex] = glm::vec3(1.0f, 0.0f, 0.0f);
			floor.m_bitangent[vertexIndex] = glm::vec3(0.0f, 0.0f, -1.0f);
			floor.m_colors[vertexIndex] = glm::vec4(1.0f);
		}

		auto triangles = TSharedPtr<TVector<Math::Triangle>>::Make();
		triangles->Add(floor);
		auto blas = TSharedPtr<Raytracing::BVH>::Make(1u);
		GlobalIlluminationLandscapeTestScene::BuildBakeBlas(
			*blas,
			*triangles);
		Math::AABB floorBounds;
		for (const glm::vec3& vertex : floor.m_vertices)
		{
			floorBounds.Extend(vertex);
		}

		Raytracing::PathTracer::TLASInstance floorInstance;
		floorInstance.m_triangles = triangles;
		floorInstance.m_blas = blas;
		floorInstance.m_worldBounds = floorBounds;
		floorInstance.m_worldMatrix = glm::mat4(1.0f);
		floorInstance.m_inverseWorldMatrix = glm::mat4(1.0f);
		floorInstance.m_materialBaseOffset = 0;
		TVector<Raytracing::PathTracer::TLASInstance> instances;
		instances.Add(std::move(floorInstance));

		Raytracing::PathTracer::Params params{};
		params.m_numSamples = 1u;
		params.m_numAmbientSamples = 1u;
		params.m_maxBounces = 0u;
		params.m_msaa = 1u;
		params.m_ambient = glm::vec3(0.0f);
		params.m_bIncludeDirectLighting = true;
		params.m_bIncludeEnvironment = false;
		params.m_bIncludeEmissive = false;

		Raytracing::LightProxy pointLight;
		pointLight.m_type = ELightType::Point;
		pointLight.m_intensity = glm::vec3(7.0f, 5.0f, 3.0f);

		const auto expectedAttenuation = [&](float distance, float radius)
		{
			const float normalizedDistance = glm::clamp(
				distance / radius,
				0.0f,
				1.0f);
			const float normalizedDistanceSquared =
				normalizedDistance * normalizedDistance;
			const float rangeBase = glm::clamp(
				1.0f - normalizedDistanceSquared *
					normalizedDistanceSquared,
				0.0f,
				1.0f);
			const float rangeWindow = rangeBase * rangeBase;
			const float safeDistance = std::max(distance, 0.01f);
			return rangeWindow / (safeDistance * safeDistance);
		};

		const auto sampleDirect = [&](const Raytracing::LightProxy& sourceLight,
			float distance,
			float radius)
		{
			Raytracing::LightProxy light = sourceLight;
			light.m_worldPosition = glm::vec3(0.0f, distance, 0.0f);
			light.m_bounds = glm::vec3(radius);
			TVector<Raytracing::LightProxy> lights;
			lights.Add(light);

			Raytracing::PathTracer pathTracer;
			Require(pathTracer.InitializeScene(
					instances,
					materials,
					lights,
					false),
				"the Point Light attenuation fixture must initialize");
			Raytracing::PathTracer::PreparedRaySample sample;
			Require(pathTracer.SamplePreparedSceneRay(
					glm::vec3(0.0f, 2.0f, 0.0f),
					glm::vec3(0.0f, -1.0f, 0.0f),
					4.0f,
					params,
					11u,
					sample) && sample.m_bHit,
				"the Point Light attenuation fixture must hit the receiver");
			return sample.m_radiance;
		};
		const auto samplePoint = [&](float distance, float radius)
		{
			return sampleDirect(pointLight, distance, radius);
		};

		// The fixture uses a rough, non-metallic 0.8-gray surface with aligned
		// normal, view, and light directions. Evaluate that closed-form BRDF in
		// the test instead of reaching through the Windows DLL boundary to the
		// intentionally internal LightingModel implementation.
		constexpr float Pi = 3.14159265358979323846f;
		const glm::vec3 directBrdf(
			0.96f * 0.8f / Pi + 0.04f / (Pi * 4.001f));
		const auto expectedDirect = [&](float distance, float radius)
		{
			return directBrdf * pointLight.m_intensity *
				expectedAttenuation(distance, radius);
		};
		const auto requireNear = [](const glm::vec3& actual,
			const glm::vec3& expected,
			const std::string& message)
		{
			Require(
				glm::length(actual - expected) <=
					0.0001f * std::max(1.0f, glm::length(expected)),
				message);
		};

		const glm::vec3 middleRadiance = samplePoint(5.0f, 10.0f);
		requireNear(
			middleRadiance,
			expectedDirect(5.0f, 10.0f),
			"CPU Point Light attenuation must use the surface-to-light distance");
		requireNear(
			samplePoint(8.0f, 10.0f),
			expectedDirect(8.0f, 10.0f),
			"CPU Point Light inverse-square attenuation must match the realtime shader");
		requireNear(
			samplePoint(9.5f, 10.0f),
			expectedDirect(9.5f, 10.0f),
			"CPU Point Light falloff must match the KHR range window");
		const glm::vec3 narrowRadiusRadiance = samplePoint(5.0f, 6.0f);
		requireNear(
			narrowRadiusRadiance,
			expectedDirect(5.0f, 6.0f),
			"changing radius must evaluate the same smooth KHR range window");
		Require(
			glm::length(narrowRadiusRadiance) < glm::length(middleRadiance),
			"a narrower Point Light range must reduce radiance at a fixed distance");

		Raytracing::LightProxy spotLight = pointLight;
		spotLight.m_type = ELightType::Spot;
		spotLight.m_cutOff = glm::vec2(0.9f, 0.5f);
		const float middleConeCosine =
			(spotLight.m_cutOff.x + spotLight.m_cutOff.y) * 0.5f;
		spotLight.m_direction = glm::normalize(glm::vec3(
			sqrt(1.0f - middleConeCosine * middleConeCosine),
			-middleConeCosine,
			0.0f));
		requireNear(
			sampleDirect(spotLight, 5.0f, 10.0f),
			expectedDirect(5.0f, 10.0f) * 0.25f,
			"CPU Spot Light angular falloff must square the normalized cone weight");

		auto blockedTriangles =
			TSharedPtr<TVector<Math::Triangle>>::Make();
		blockedTriangles->Add(floor);
		Math::Triangle blocker{};
		blocker.m_vertices[0] = glm::vec3(-2.0f, 3.0f, -2.0f);
		blocker.m_vertices[1] = glm::vec3(2.0f, 3.0f, -2.0f);
		blocker.m_vertices[2] = glm::vec3(0.0f, 3.0f, 2.0f);
		blocker.m_centroid =
			(blocker.m_vertices[0] + blocker.m_vertices[1] +
				blocker.m_vertices[2]) / 3.0f;
		for (uint32_t vertexIndex = 0u; vertexIndex < 3u; ++vertexIndex)
		{
			blocker.m_normals[vertexIndex] = glm::vec3(0.0f, -1.0f, 0.0f);
			blocker.m_tangent[vertexIndex] = glm::vec3(1.0f, 0.0f, 0.0f);
			blocker.m_bitangent[vertexIndex] = glm::vec3(0.0f, 0.0f, 1.0f);
		}
		blockedTriangles->Add(blocker);
		auto blockedBlas = TSharedPtr<Raytracing::BVH>::Make(
			static_cast<uint32_t>(blockedTriangles->Num()));
		GlobalIlluminationLandscapeTestScene::BuildBakeBlas(
			*blockedBlas,
			*blockedTriangles);
		Math::AABB blockedBounds;
		for (const Math::Triangle& triangle : *blockedTriangles)
		{
			for (const glm::vec3& vertex : triangle.m_vertices)
			{
				blockedBounds.Extend(vertex);
			}
		}
		Raytracing::PathTracer::TLASInstance blockedInstance;
		blockedInstance.m_triangles = blockedTriangles;
		blockedInstance.m_blas = blockedBlas;
		blockedInstance.m_worldBounds = blockedBounds;
		blockedInstance.m_worldMatrix = glm::mat4(1.0f);
		blockedInstance.m_inverseWorldMatrix = glm::mat4(1.0f);
		blockedInstance.m_materialBaseOffset = 0;
		TVector<Raytracing::PathTracer::TLASInstance> blockedInstances;
		blockedInstances.Add(std::move(blockedInstance));
		const auto sampleBehindBlocker = [&](bool bCastShadows)
		{
			Raytracing::LightProxy light = pointLight;
			light.m_worldPosition = glm::vec3(0.0f, 5.0f, 0.0f);
			light.m_bounds = glm::vec3(10.0f);
			light.m_bCastShadows = bCastShadows;
			TVector<Raytracing::LightProxy> lights;
			lights.Add(light);
			Raytracing::PathTracer pathTracer;
			Require(pathTracer.InitializeScene(
					blockedInstances,
					materials,
					lights,
					false),
				"the Point Light blocker fixture must initialize");
			Raytracing::PathTracer::PreparedRaySample sample;
			Require(pathTracer.SamplePreparedSceneRay(
					glm::vec3(0.0f, 2.0f, 0.0f),
					glm::vec3(0.0f, -1.0f, 0.0f),
					4.0f,
					params,
					13u,
					sample) && sample.m_bHit,
				"the Point Light blocker fixture must hit the receiver");
			return sample.m_radiance;
		};
		Require(
			glm::length(sampleBehindBlocker(true)) <= 0.000001f,
			"a shadowed Point Light must be occluded during GI baking");
		requireNear(
			sampleBehindBlocker(false),
			expectedDirect(5.0f, 10.0f),
			"a Point Light with shadows disabled must illuminate GI receivers through blockers");

		auto behindLightTriangles =
			TSharedPtr<TVector<Math::Triangle>>::Make();
		behindLightTriangles->Add(floor);
		Math::Triangle behindLightBlocker = blocker;
		for (glm::vec3& vertex : behindLightBlocker.m_vertices)
		{
			vertex.y = 5.05f;
		}
		behindLightBlocker.m_centroid =
			(behindLightBlocker.m_vertices[0] +
				behindLightBlocker.m_vertices[1] +
				behindLightBlocker.m_vertices[2]) / 3.0f;
		behindLightTriangles->Add(behindLightBlocker);
		auto behindLightBlas = TSharedPtr<Raytracing::BVH>::Make(
			static_cast<uint32_t>(behindLightTriangles->Num()));
		GlobalIlluminationLandscapeTestScene::BuildBakeBlas(
			*behindLightBlas,
			*behindLightTriangles);
		Math::AABB behindLightBounds;
		for (const Math::Triangle& triangle : *behindLightTriangles)
		{
			for (const glm::vec3& vertex : triangle.m_vertices)
			{
				behindLightBounds.Extend(vertex);
			}
		}
		Raytracing::PathTracer::TLASInstance behindLightInstance;
		behindLightInstance.m_triangles = behindLightTriangles;
		behindLightInstance.m_blas = behindLightBlas;
		behindLightInstance.m_worldBounds = behindLightBounds;
		behindLightInstance.m_worldMatrix = glm::mat4(1.0f);
		behindLightInstance.m_inverseWorldMatrix = glm::mat4(1.0f);
		behindLightInstance.m_materialBaseOffset = 0;
		TVector<Raytracing::PathTracer::TLASInstance> behindLightInstances;
		behindLightInstances.Add(std::move(behindLightInstance));
		Raytracing::LightProxy endpointLight = pointLight;
		endpointLight.m_worldPosition = glm::vec3(0.0f, 5.0f, 0.0f);
		endpointLight.m_bounds = glm::vec3(10.0f);
		TVector<Raytracing::LightProxy> endpointLights;
		endpointLights.Add(endpointLight);
		Raytracing::PathTracer endpointPathTracer;
		Require(endpointPathTracer.InitializeScene(
				behindLightInstances,
				materials,
				endpointLights,
				false),
			"the local-light endpoint fixture must initialize");
		Raytracing::PathTracer::Params endpointParams = params;
		endpointParams.m_rayBiasBase = 0.05f;
		endpointParams.m_rayBiasScale = 0.05f;
		Raytracing::PathTracer::PreparedRaySample endpointSample;
		Require(endpointPathTracer.SamplePreparedSceneRay(
				glm::vec3(0.0f, 2.0f, 0.0f),
				glm::vec3(0.0f, -1.0f, 0.0f),
				4.0f,
				endpointParams,
				19u,
				endpointSample) && endpointSample.m_bHit,
			"the local-light endpoint fixture must hit its receiver");
		requireNear(
			endpointSample.m_radiance,
			expectedDirect(5.0f, 10.0f),
			"geometry behind a local light must not shadow its GI contribution");

		auto thinBlockerTriangles =
			TSharedPtr<TVector<Math::Triangle>>::Make();
		thinBlockerTriangles->Add(floor);
		const glm::vec3 thinBlockerDirection = glm::normalize(
			glm::vec3(1.0f, 1.0f, 0.0f));
		const glm::vec3 thinBlockerTangent = glm::normalize(
			glm::vec3(-1.0f, 1.0f, 0.0f));
		const glm::vec3 thinBlockerCenter = thinBlockerDirection * 0.06f;
		Math::Triangle thinBlocker{};
		thinBlocker.m_vertices[0] = thinBlockerCenter +
			thinBlockerTangent * 0.012f + glm::vec3(0.0f, 0.0f, 0.012f);
		thinBlocker.m_vertices[1] = thinBlockerCenter -
			thinBlockerTangent * 0.012f + glm::vec3(0.0f, 0.0f, 0.012f);
		thinBlocker.m_vertices[2] = thinBlockerCenter -
			glm::vec3(0.0f, 0.0f, 0.024f);
		thinBlocker.m_centroid =
			(thinBlocker.m_vertices[0] + thinBlocker.m_vertices[1] +
				thinBlocker.m_vertices[2]) / 3.0f;
		for (uint32_t vertexIndex = 0u; vertexIndex < 3u; ++vertexIndex)
		{
			thinBlocker.m_normals[vertexIndex] = -thinBlockerDirection;
			thinBlocker.m_tangent[vertexIndex] = thinBlockerTangent;
			thinBlocker.m_bitangent[vertexIndex] = glm::vec3(0.0f, 0.0f, 1.0f);
		}
		thinBlockerTriangles->Add(thinBlocker);
		auto thinBlockerBlas = TSharedPtr<Raytracing::BVH>::Make(
			static_cast<uint32_t>(thinBlockerTriangles->Num()));
		GlobalIlluminationLandscapeTestScene::BuildBakeBlas(
			*thinBlockerBlas,
			*thinBlockerTriangles);
		Math::AABB thinBlockerBounds;
		for (const Math::Triangle& triangle : *thinBlockerTriangles)
		{
			for (const glm::vec3& vertex : triangle.m_vertices)
			{
				thinBlockerBounds.Extend(vertex);
			}
		}
		Raytracing::PathTracer::TLASInstance thinBlockerInstance;
		thinBlockerInstance.m_triangles = thinBlockerTriangles;
		thinBlockerInstance.m_blas = thinBlockerBlas;
		thinBlockerInstance.m_worldBounds = thinBlockerBounds;
		thinBlockerInstance.m_worldMatrix = glm::mat4(1.0f);
		thinBlockerInstance.m_inverseWorldMatrix = glm::mat4(1.0f);
		thinBlockerInstance.m_materialBaseOffset = 0;
		TVector<Raytracing::PathTracer::TLASInstance> thinBlockerInstances;
		thinBlockerInstances.Add(std::move(thinBlockerInstance));

		Raytracing::LightProxy thinBlockerLight = pointLight;
		thinBlockerLight.m_worldPosition = glm::vec3(1.0f, 1.0f, 0.0f);
		thinBlockerLight.m_bounds = glm::vec3(3.0f);
		TVector<Raytracing::LightProxy> thinBlockerLights;
		thinBlockerLights.Add(thinBlockerLight);
		GIProbesBakeSettings thinBlockerSettings;
		thinBlockerSettings.m_bounceCount = 1u;
		thinBlockerSettings.m_normalBias = 0.05f;
		thinBlockerSettings.m_viewBias = 0.05f;
		thinBlockerSettings.m_bIncludeSky = false;
		thinBlockerSettings.m_bIncludeEmissive = false;
		thinBlockerSettings.m_bIncludeDirectLighting = true;
		Raytracing::GIProbesPathTracer thinBlockerPathTracer;
		Require(thinBlockerPathTracer.Initialize(
				thinBlockerInstances,
				materials,
				thinBlockerLights,
				thinBlockerSettings,
				glm::vec3(0.0f)),
			"the thin GI shadow blocker fixture must initialize");
		GIProbeBakeRaySample thinBlockerSample;
		std::string thinBlockerDiagnostic;
		Require(thinBlockerPathTracer.Sample(
				glm::vec3(0.0f, 2.0f, 0.0f),
				glm::vec3(0.0f, -1.0f, 0.0f),
				4.0f,
				17u,
				thinBlockerSample,
				thinBlockerDiagnostic) &&
			thinBlockerSample.m_bHit,
			"the thin GI shadow blocker fixture must hit its receiver: " +
				thinBlockerDiagnostic);
		Require(
			glm::length(thinBlockerSample.m_radiance) <= 0.000001f,
			"runtime probe interpolation biases must not make bake rays jump "
			"across thin shadow blockers");

		const glm::vec3 largeWorldOffset(8000.0f, 0.0f, -8000.0f);
		Math::AABB translatedBounds;
		for (const Math::Triangle& triangle : *blockedTriangles)
		{
			for (const glm::vec3& vertex : triangle.m_vertices)
			{
				translatedBounds.Extend(vertex + largeWorldOffset);
			}
		}
		Raytracing::PathTracer::TLASInstance translatedInstance;
		translatedInstance.m_triangles = blockedTriangles;
		translatedInstance.m_blas = blockedBlas;
		translatedInstance.m_worldBounds = translatedBounds;
		translatedInstance.m_worldMatrix = glm::translate(
			glm::mat4(1.0f),
			largeWorldOffset);
		translatedInstance.m_inverseWorldMatrix = glm::translate(
			glm::mat4(1.0f),
			-largeWorldOffset);
		translatedInstance.m_materialBaseOffset = 0;
		TVector<Raytracing::PathTracer::TLASInstance> translatedInstances;
		translatedInstances.Add(std::move(translatedInstance));
		const auto sampleLargeWorldBlocker = [&](bool bCastShadows)
		{
			Raytracing::LightProxy light = pointLight;
			light.m_worldPosition =
				largeWorldOffset + glm::vec3(0.0f, 5.0f, 0.0f);
			light.m_bounds = glm::vec3(10.0f);
			light.m_bCastShadows = bCastShadows;
			TVector<Raytracing::LightProxy> lights;
			lights.Add(light);
			Raytracing::PathTracer pathTracer;
			Require(pathTracer.InitializeScene(
					translatedInstances,
					materials,
					lights,
					false),
				"the large-world Point Light blocker fixture must initialize");
			Raytracing::PathTracer::Params largeWorldParams = params;
			largeWorldParams.m_rayBiasBase = 0.05f;
			largeWorldParams.m_rayBiasScale = 0.05f;
			Raytracing::PathTracer::PreparedRaySample sample;
			Require(pathTracer.SamplePreparedSceneRay(
					largeWorldOffset + glm::vec3(0.0f, 2.0f, 0.0f),
					glm::vec3(0.0f, -1.0f, 0.0f),
					4.0f,
					largeWorldParams,
					13u,
					sample) && sample.m_bHit,
				"the large-world Point Light fixture must hit the receiver");
			return sample.m_radiance;
		};
		Require(
			glm::length(sampleLargeWorldBlocker(true)) <= 0.000001f,
			"GI shadow bias must not grow with absolute world coordinates and jump past blockers");
		requireNear(
			sampleLargeWorldBlocker(false),
			expectedDirect(5.0f, 10.0f),
			"large-world GI lighting must remain unchanged when shadow traversal is disabled");
		Require(
			glm::length(samplePoint(5.0f, 5.0f)) <= 0.000001f &&
				glm::length(samplePoint(5.0f, 4.0f)) <= 0.000001f,
			"Point Light radiance must be zero at and beyond the authored radius");

		Raytracing::LightProxy crossingLight = pointLight;
		crossingLight.m_worldPosition = glm::vec3(12.0f, 5.0f, 0.0f);
		crossingLight.m_bounds = glm::vec3(10.0f);
		TVector<Raytracing::LightProxy> crossingLights;
		crossingLights.Add(crossingLight);
		Raytracing::PathTracer crossingPathTracer;
		Require(crossingPathTracer.InitializeScene(
				instances,
				materials,
				crossingLights,
				false),
			"the Point Light vacuum fixture must initialize");
		Raytracing::PathTracer::PreparedRaySample crossingSample;
		Require(crossingPathTracer.SamplePreparedSceneRay(
				glm::vec3(-8.0f, 5.0f, 0.0f),
				glm::vec3(1.0f, 0.0f, 0.0f),
				40.0f,
				params,
				12u,
				crossingSample),
			"the CPU baker must accept a ray crossing a Point Light range");
		Require(
			!crossingSample.m_bHit &&
				glm::length(crossingSample.m_radiance) <= 0.000001f,
			"crossing a Point Light range in vacuum must not emit volumetric radiance");

		auto bounceTriangles = TSharedPtr<TVector<Math::Triangle>>::Make();
		bounceTriangles->Add(floor);
		const auto addWallTriangle = [&](const glm::vec3& a,
			const glm::vec3& b,
			const glm::vec3& c)
		{
			Math::Triangle wall{};
			wall.m_vertices[0] = a;
			wall.m_vertices[1] = b;
			wall.m_vertices[2] = c;
			wall.m_centroid = (a + b + c) / 3.0f;
			for (uint32_t vertexIndex = 0u; vertexIndex < 3u; ++vertexIndex)
			{
				wall.m_normals[vertexIndex] = glm::vec3(-1.0f, 0.0f, 0.0f);
				wall.m_tangent[vertexIndex] = glm::vec3(0.0f, 0.0f, 1.0f);
				wall.m_bitangent[vertexIndex] = glm::vec3(0.0f, 1.0f, 0.0f);
				wall.m_colors[vertexIndex] = glm::vec4(1.0f);
			}
			bounceTriangles->Add(std::move(wall));
		};
		addWallTriangle(
			glm::vec3(4.0f, 0.0f, -12.0f),
			glm::vec3(4.0f, 12.0f, -12.0f),
			glm::vec3(4.0f, 12.0f, 12.0f));
		addWallTriangle(
			glm::vec3(4.0f, 0.0f, -12.0f),
			glm::vec3(4.0f, 12.0f, 12.0f),
			glm::vec3(4.0f, 0.0f, 12.0f));

		auto bounceBlas = TSharedPtr<Raytracing::BVH>::Make(
			static_cast<uint32_t>(bounceTriangles->Num()));
		GlobalIlluminationLandscapeTestScene::BuildBakeBlas(
			*bounceBlas,
			*bounceTriangles);
		Math::AABB bounceBounds;
		for (const Math::Triangle& triangle : *bounceTriangles)
		{
			for (const glm::vec3& vertex : triangle.m_vertices)
			{
				bounceBounds.Extend(vertex);
			}
		}
		Raytracing::PathTracer::TLASInstance bounceInstance;
		bounceInstance.m_triangles = bounceTriangles;
		bounceInstance.m_blas = bounceBlas;
		bounceInstance.m_worldBounds = bounceBounds;
		bounceInstance.m_worldMatrix = glm::mat4(1.0f);
		bounceInstance.m_inverseWorldMatrix = glm::mat4(1.0f);
		bounceInstance.m_materialBaseOffset = 0;
		TVector<Raytracing::PathTracer::TLASInstance> bounceInstances;
		bounceInstances.Add(std::move(bounceInstance));

		Raytracing::LightProxy bounceLight = pointLight;
		bounceLight.m_worldPosition = glm::vec3(3.0f, 4.0f, 0.0f);
		bounceLight.m_intensity = glm::vec3(4000.0f);
		bounceLight.m_bounds = glm::vec3(4.5f);
		TVector<Raytracing::LightProxy> bounceLights;
		bounceLights.Add(bounceLight);
		Raytracing::PathTracer bouncePathTracer;
		Require(bouncePathTracer.InitializeScene(
				bounceInstances,
				materials,
				bounceLights,
				false),
			"the secondary Point Light fixture must initialize");

		Raytracing::PathTracer::Params directOnlyParams = params;
		Raytracing::PathTracer::PreparedRaySample directOnlySample;
		Require(bouncePathTracer.SamplePreparedSceneRay(
				glm::vec3(0.0f, 5.0f, 0.0f),
				glm::vec3(0.0f, -1.0f, 0.0f),
				10.0f,
				directOnlyParams,
				1u,
				directOnlySample) && directOnlySample.m_bHit,
			"the secondary Point Light fixture must hit its first receiver");
		Require(
			glm::length(directOnlySample.m_radiance) <= 0.000001f,
			"the first receiver outside Point Light radius must remain dark");

		Raytracing::PathTracer::Params bounceParams = params;
		bounceParams.m_maxBounces = 1u;
		bounceParams.m_rayBiasBase = 0.001f;
		float strongestSecondaryContribution = 0.0f;
		for (uint32_t seed = 1u; seed <= 2048u; ++seed)
		{
			Raytracing::PathTracer::PreparedRaySample sample;
			Require(bouncePathTracer.SamplePreparedSceneRay(
					glm::vec3(0.0f, 5.0f, 0.0f),
					glm::vec3(0.0f, -1.0f, 0.0f),
					10.0f,
					bounceParams,
					seed,
					sample) && sample.m_bHit,
				"the secondary Point Light fixture must sample the floor");
			strongestSecondaryContribution = std::max(
				strongestSecondaryContribution,
				glm::length(sample.m_radiance));
			if (strongestSecondaryContribution > 0.01f)
			{
				break;
			}
		}
		Require(strongestSecondaryContribution > 0.01f,
			"a real secondary surface inside Point Light radius must carry baked radiance");

		TVector<MaterialPtr> lowThroughputMaterials;
		MaterialPtr lowThroughputMaterial = MakeDiffuseFixtureMaterial(
			allocator,
			glm::vec3(0.005f));
		lowThroughputMaterial->SetUniform(
			"material.indexOfRefraction",
			1.0f);
		lowThroughputMaterials.Add(lowThroughputMaterial);
		Raytracing::PathTracer lowThroughputPathTracer;
		Require(lowThroughputPathTracer.InitializeScene(
				bounceInstances,
				lowThroughputMaterials,
				bounceLights,
				false),
			"the low-throughput secondary-light fixture must initialize");
		float strongestLowThroughputContribution = 0.0f;
		for (uint32_t seed = 1u; seed <= 2048u; ++seed)
		{
			Raytracing::PathTracer::PreparedRaySample sample;
			Require(lowThroughputPathTracer.SamplePreparedSceneRay(
					glm::vec3(0.0f, 5.0f, 0.0f),
					glm::vec3(0.0f, -1.0f, 0.0f),
					10.0f,
					bounceParams,
					seed,
					sample) && sample.m_bHit,
				"the low-throughput fixture must sample the floor");
			const float contribution = glm::length(sample.m_radiance);
			Require(std::isfinite(contribution),
				"low-throughput secondary radiance must remain finite");
			strongestLowThroughputContribution = std::max(
				strongestLowThroughputContribution,
				contribution);
			if (strongestLowThroughputContribution > 0.000001f)
			{
				break;
			}
		}
		Require(strongestLowThroughputContribution > 0.000001f,
			"finite-bounce GI must preserve weak indirect paths instead of "
			"discarding them with a throughput threshold");
	}

	void TestHdrEnvironmentImportanceDistribution()
	{
		constexpr uint32_t Width = 64u;
		constexpr uint32_t Height = 32u;
		constexpr uint32_t BrightX = 32u;
		constexpr uint32_t BrightY = 8u;
		TVector<glm::vec4> environment;
		environment.Resize(Width * Height);
		for (glm::vec4& pixel : environment)
		{
			pixel = glm::vec4(1.0f);
		}
		environment[BrightX + BrightY * Width] =
			glm::vec4(65504.0f, 65504.0f, 65504.0f, 1.0f);

		InspectablePathTracer pathTracer;
		pathTracer.SetRuntimeEnvironmentLinear(
			environment,
			glm::uvec2(Width, Height));
		Require(pathTracer.UsesEnvironmentImportance(),
			"a concentrated HDR environment must use a luminance-weighted "
			"direct-light distribution");

		TVector<glm::vec4> diffuseEnvironment;
		diffuseEnvironment.Resize(Width * Height);
		for (glm::vec4& pixel : diffuseEnvironment)
		{
			pixel = glm::vec4(0.0f, 16.0f, 0.0f, 1.0f);
		}
		pathTracer.SetRuntimeDiffuseEnvironmentLinear(
			diffuseEnvironment,
			glm::uvec2(Width, Height));
		Require(pathTracer.UsesEnvironmentImportance(),
			"a preconvolved diffuse environment must not replace the raw HDR "
			"importance distribution");
		const glm::vec3 directEnvironment =
			pathTracer.DirectEnvironmentRadiance(glm::vec3(1.0f, 0.0f, 0.0f));
		Require(glm::length(directEnvironment - glm::vec3(1.0f)) <= 0.0001f,
			"direct environment lighting must evaluate raw radiance instead of "
			"the preconvolved irradiance map");

		const glm::vec3 worldNormal(0.0f, 1.0f, 0.0f);
		double integratedPdf = 0.0;
		for (uint32_t y = 0u; y < Height; ++y)
		{
			const double theta0 = glm::pi<double>() *
				static_cast<double>(y) / static_cast<double>(Height);
			const double theta1 = glm::pi<double>() *
				static_cast<double>(y + 1u) / static_cast<double>(Height);
			const double pixelSolidAngle =
				2.0 * glm::pi<double>() / static_cast<double>(Width) *
				(std::cos(theta0) - std::cos(theta1));
			const float theta = glm::pi<float>() *
				(static_cast<float>(y) + 0.5f) /
				static_cast<float>(Height);
			for (uint32_t x = 0u; x < Width; ++x)
			{
				const float phi = -glm::pi<float>() +
					2.0f * glm::pi<float>() *
					(static_cast<float>(x) + 0.5f) /
					static_cast<float>(Width);
				const glm::vec3 direction(
					std::cos(phi) * std::sin(theta),
					std::cos(theta),
					std::sin(phi) * std::sin(theta));
				integratedPdf += pathTracer.EnvironmentPdf(
					worldNormal,
					direction) * pixelSolidAngle;
			}
		}
		Require(std::abs(integratedPdf - 1.0) <= 0.001,
			"the mixed HDR environment PDF must integrate to one; got " +
				std::to_string(integratedPdf));

		constexpr uint32_t SampleCount = 4096u;
		uint32_t randomState = 0x155155u;
		uint32_t brightSamples = 0u;
		for (uint32_t sampleIndex = 0u;
			sampleIndex < SampleCount;
			++sampleIndex)
		{
			glm::vec3 direction{};
			float pdf = 0.0f;
			Require(pathTracer.SampleEnvironment(
					worldNormal,
					randomState,
					direction,
					pdf) &&
				std::isfinite(pdf) && pdf > 0.0f,
				"HDR environment importance samples must have finite positive PDFs");
			const float queriedPdf = pathTracer.EnvironmentPdf(
				worldNormal,
				direction);
			Require(std::abs(pdf - queriedPdf) <=
					0.00001f * (std::max)(pdf, 1.0f),
				"sampled and queried HDR environment PDFs must agree");

			const float phi = std::atan2(direction.z, direction.x);
			const float theta = std::acos(glm::clamp(
				direction.y,
				-1.0f,
				1.0f));
			const uint32_t x = (std::min)(
				static_cast<uint32_t>(
					(phi + glm::pi<float>()) /
					(2.0f * glm::pi<float>()) * Width),
				Width - 1u);
			const uint32_t y = (std::min)(
				static_cast<uint32_t>(
					theta / glm::pi<float>() * Height),
				Height - 1u);
			brightSamples += x == BrightX && y == BrightY ? 1u : 0u;
		}
		Require(brightSamples > SampleCount / 3u,
			"HDR environment sampling must concentrate work on the bright texel "
			"instead of producing rare uniform-sampling fireflies; got " +
				std::to_string(brightSamples) + " of " +
				std::to_string(SampleCount));
	}

	void TestEnvironmentMissIsCountedOnce()
	{
		Memory::ObjectAllocatorPtr allocator =
			Memory::ObjectAllocatorPtr::Make(
				Memory::EAllocationPolicy::SharedMemory_MultiThreaded);
		const glm::vec3 baseColor(0.8f, 0.6f, 0.4f);
		TVector<MaterialPtr> materials;
		auto mirrorMaterial = MakeDiffuseFixtureMaterial(allocator, baseColor);
		mirrorMaterial->SetUniform("material.metallicFactor", 1.0f);
		mirrorMaterial->SetUniform("material.roughnessFactor", 0.0f);
		materials.Add(mirrorMaterial);

		Math::Triangle floor{};
		floor.m_vertices[0] = glm::vec3(-100.0f, 0.0f, -100.0f);
		floor.m_vertices[1] = glm::vec3(0.0f, 0.0f, 100.0f);
		floor.m_vertices[2] = glm::vec3(100.0f, 0.0f, -100.0f);
		floor.m_centroid =
			(floor.m_vertices[0] + floor.m_vertices[1] +
				floor.m_vertices[2]) / 3.0f;
		for (uint32_t vertexIndex = 0u; vertexIndex < 3u; ++vertexIndex)
		{
			floor.m_normals[vertexIndex] = glm::vec3(0.0f, 1.0f, 0.0f);
			floor.m_tangent[vertexIndex] = glm::vec3(1.0f, 0.0f, 0.0f);
			floor.m_bitangent[vertexIndex] = glm::vec3(0.0f, 0.0f, -1.0f);
			floor.m_colors[vertexIndex] = glm::vec4(1.0f);
		}

		auto triangles = TSharedPtr<TVector<Math::Triangle>>::Make();
		triangles->Add(floor);
		auto blas = TSharedPtr<Raytracing::BVH>::Make(1u);
		GlobalIlluminationLandscapeTestScene::BuildBakeBlas(
			*blas,
			*triangles);
		Math::AABB floorBounds;
		for (const glm::vec3& vertex : floor.m_vertices)
		{
			floorBounds.Extend(vertex);
		}

		Raytracing::PathTracer::TLASInstance floorInstance;
		floorInstance.m_triangles = triangles;
		floorInstance.m_blas = blas;
		floorInstance.m_worldBounds = floorBounds;
		floorInstance.m_worldMatrix = glm::mat4(1.0f);
		floorInstance.m_inverseWorldMatrix = glm::mat4(1.0f);
		floorInstance.m_materialBaseOffset = 0;
		TVector<Raytracing::PathTracer::TLASInstance> instances;
		instances.Add(std::move(floorInstance));

		Raytracing::PathTracer pathTracer;
		Require(
			pathTracer.InitializeScene(
				instances,
				materials,
				{},
				false),
			"the constant-environment fixture should initialize");
		const glm::vec3 environmentRadiance(0.25f, 0.20f, 0.15f);
		TVector<glm::vec4> environment;
		environment.Add(glm::vec4(environmentRadiance, 1.0f));
		pathTracer.SetRuntimeEnvironmentLinear(environment, glm::uvec2(1u));

		Raytracing::PathTracer::Params params{};
		params.m_numSamples = 256u;
		params.m_numAmbientSamples = 4096u;
		params.m_maxBounces = 0u;
		params.m_msaa = 1u;
		params.m_ambient = glm::vec3(0.0f);
		params.m_bRunTasksInline = true;
		params.m_bIncludeDirectLighting = false;
		params.m_bIncludeEnvironment = true;
		params.m_bIncludeEmissive = false;

		Raytracing::PathTracer::PreparedRaySample sample;
		Require(
			pathTracer.SamplePreparedSceneRay(
				glm::vec3(0.0f, 2.0f, 0.0f),
				glm::vec3(0.0f, -1.0f, 0.0f),
				4.0f,
				params,
				155u,
				sample) &&
			sample.m_bHit,
			"the constant-environment fixture ray should hit its receiver");

		Require(
			std::isfinite(sample.m_radiance.x) &&
			std::isfinite(sample.m_radiance.y) &&
			std::isfinite(sample.m_radiance.z) &&
			glm::all(glm::greaterThanEqual(
				sample.m_radiance,
				glm::vec3(0.0f))),
			"a sampled constant environment should stay finite and non-negative");
		Require(
			glm::length(sample.m_radiance) > 0.05f,
			"the mirror receiver should retain a visible environment contribution");

		// A passive perfect conductor under a constant environment cannot return
		// more radiance than that environment. Adding the importance-sampled miss
		// to both the MIS ambient and recursive estimators violates this bound.
		const glm::vec3 energyCeiling = environmentRadiance * 1.05f;
		Require(
			glm::all(glm::lessThanEqual(
				sample.m_radiance,
				energyCeiling)),
			"an environment miss must enter only the MIS estimator; radiance " +
				std::to_string(sample.m_radiance.x) + ", " +
				std::to_string(sample.m_radiance.y) + ", " +
				std::to_string(sample.m_radiance.z) + " exceeds ceiling " +
				std::to_string(energyCeiling.x) + ", " +
				std::to_string(energyCeiling.y) + ", " +
				std::to_string(energyCeiling.z));

		constexpr float HdrScale = 128.0f;
		TVector<glm::vec4> hdrEnvironment;
		hdrEnvironment.Add(glm::vec4(
			environmentRadiance * HdrScale,
			1.0f));
		pathTracer.SetRuntimeEnvironmentLinear(
			hdrEnvironment,
			glm::uvec2(1u));
		Raytracing::PathTracer::PreparedRaySample hdrSample;
		Require(
			pathTracer.SamplePreparedSceneRay(
				glm::vec3(0.0f, 2.0f, 0.0f),
				glm::vec3(0.0f, -1.0f, 0.0f),
				4.0f,
				params,
				155u,
				hdrSample) &&
			hdrSample.m_bHit,
			"the HDR constant-environment fixture ray should hit its receiver");
		const glm::vec3 expectedHdrRadiance = sample.m_radiance * HdrScale;
		Require(
			glm::length(hdrSample.m_radiance - expectedHdrRadiance) <=
				0.001f * (std::max)(
					glm::length(expectedHdrRadiance),
					1.0f) &&
			(std::max)({
				hdrSample.m_radiance.x,
				hdrSample.m_radiance.y,
				hdrSample.m_radiance.z }) > 10.0f,
			"environment transport must stay linear above the legacy LDR clamp; "
			"expected " +
				std::to_string(expectedHdrRadiance.x) + ", " +
				std::to_string(expectedHdrRadiance.y) + ", " +
				std::to_string(expectedHdrRadiance.z) + " but got " +
				std::to_string(hdrSample.m_radiance.x) + ", " +
				std::to_string(hdrSample.m_radiance.y) + ", " +
				std::to_string(hdrSample.m_radiance.z));

		TVector<MaterialPtr> diffuseMaterials;
		diffuseMaterials.Add(MakeDiffuseFixtureMaterial(
			allocator,
			glm::vec3(1.0f)));
		Raytracing::PathTracer openSkyPathTracer;
		Require(
			openSkyPathTracer.InitializeScene(
				instances,
				diffuseMaterials,
				{},
				false),
			"the open-sky diffuse fixture should initialize");
		openSkyPathTracer.SetRuntimeEnvironmentLinear(
			environment,
			glm::uvec2(1u));

		auto coveredTriangles =
			TSharedPtr<TVector<Math::Triangle>>::Make();
		coveredTriangles->Add(floor);
		const glm::vec3 roofVertices[] = {
			glm::vec3(-1000.0f, 1.0f, -1000.0f),
			glm::vec3(1000.0f, 1.0f, -1000.0f),
			glm::vec3(1000.0f, 1.0f, 1000.0f),
			glm::vec3(-1000.0f, 1.0f, 1000.0f)
		};
		for (uint32_t triangleIndex = 0u;
			triangleIndex < 2u;
			++triangleIndex)
		{
			Math::Triangle roof{};
			const uint32_t indices[2][3] = {
				{ 0u, 1u, 2u },
				{ 0u, 2u, 3u }
			};
			for (uint32_t vertexIndex = 0u;
				vertexIndex < 3u;
				++vertexIndex)
			{
				roof.m_vertices[vertexIndex] =
					roofVertices[indices[triangleIndex][vertexIndex]];
				roof.m_normals[vertexIndex] =
					glm::vec3(0.0f, -1.0f, 0.0f);
				roof.m_tangent[vertexIndex] =
					glm::vec3(1.0f, 0.0f, 0.0f);
				roof.m_bitangent[vertexIndex] =
					glm::vec3(0.0f, 0.0f, 1.0f);
			}
			roof.m_centroid =
				(roof.m_vertices[0] + roof.m_vertices[1] +
					roof.m_vertices[2]) / 3.0f;
			coveredTriangles->Add(std::move(roof));
		}
		auto coveredBlas = TSharedPtr<Raytracing::BVH>::Make(
			static_cast<uint32_t>(coveredTriangles->Num()));
		GlobalIlluminationLandscapeTestScene::BuildBakeBlas(
			*coveredBlas,
			*coveredTriangles);
		Math::AABB coveredBounds;
		for (const Math::Triangle& triangle : *coveredTriangles)
		{
			for (const glm::vec3& vertex : triangle.m_vertices)
			{
				coveredBounds.Extend(vertex);
			}
		}
		Raytracing::PathTracer::TLASInstance coveredInstance;
		coveredInstance.m_triangles = coveredTriangles;
		coveredInstance.m_blas = coveredBlas;
		coveredInstance.m_worldBounds = coveredBounds;
		coveredInstance.m_worldMatrix = glm::mat4(1.0f);
		coveredInstance.m_inverseWorldMatrix = glm::mat4(1.0f);
		coveredInstance.m_materialBaseOffset = 0;
		TVector<Raytracing::PathTracer::TLASInstance> coveredInstances;
		coveredInstances.Add(std::move(coveredInstance));
		Raytracing::PathTracer coveredSkyPathTracer;
		Require(
			coveredSkyPathTracer.InitializeScene(
				coveredInstances,
				diffuseMaterials,
				{},
				false),
			"the covered-sky diffuse fixture should initialize");
		coveredSkyPathTracer.SetRuntimeEnvironmentLinear(
			environment,
			glm::uvec2(1u));

		Raytracing::PathTracer::Params skyShadowParams = params;
		skyShadowParams.m_numSamples = 256u;
		skyShadowParams.m_numAmbientSamples = 4096u;
		const auto sampleSkyReceiver = [&skyShadowParams](
			const Raytracing::PathTracer& tracer)
		{
			Raytracing::PathTracer::PreparedRaySample result;
			Require(
				tracer.SamplePreparedSceneRay(
					glm::vec3(0.0f, 0.5f, 0.0f),
					glm::vec3(0.0f, -1.0f, 0.0f),
					2.0f,
					skyShadowParams,
					0x5a17u,
					result) &&
				result.m_bHit,
				"the sky-shadow fixture ray should hit its receiver");
			return result.m_radiance;
		};
		const glm::vec3 openSkyRadiance =
			sampleSkyReceiver(openSkyPathTracer);
		const glm::vec3 coveredSkyRadiance =
			sampleSkyReceiver(coveredSkyPathTracer);
		const glm::vec3 roughDiffuseEnergyCeiling =
			environmentRadiance * 1.05f;
		Require(
			glm::all(glm::lessThanEqual(
				openSkyRadiance,
				roughDiffuseEnergyCeiling)),
			"a passive rough dielectric under a constant environment must not "
			"gain energy from rejected BSDF directions; radiance " +
				std::to_string(openSkyRadiance.x) + ", " +
				std::to_string(openSkyRadiance.y) + ", " +
				std::to_string(openSkyRadiance.z) + " exceeds ceiling " +
				std::to_string(roughDiffuseEnergyCeiling.x) + ", " +
				std::to_string(roughDiffuseEnergyCeiling.y) + ", " +
				std::to_string(roughDiffuseEnergyCeiling.z));
		Require(
			glm::length(openSkyRadiance) > 0.01f &&
			glm::length(coveredSkyRadiance) <=
				glm::length(openSkyRadiance) * 0.001f,
			"bake geometry must occlude the environment instead of leaking "
			"through the sky estimator");

		TVector<MaterialPtr> seededMaterials;
		seededMaterials.Add(MakeDiffuseFixtureMaterial(allocator, baseColor));
		Raytracing::PathTracer seededPathTracer;
		Require(
			seededPathTracer.InitializeScene(
				instances,
				seededMaterials,
				{},
				false),
			"the seeded bake-ray fixture should initialize");
		seededPathTracer.SetRuntimeEnvironmentLinear(
			environment,
			glm::uvec2(1u));
		Raytracing::PathTracer::Params seededParams = params;
		seededParams.m_numSamples = 8u;
		seededParams.m_numAmbientSamples = 8u;
		Raytracing::PathTracer::PreparedRaySample seededReference;
		Require(
			seededPathTracer.SamplePreparedSceneRay(
				glm::vec3(0.0f, 2.0f, 0.0f),
				glm::vec3(0.0f, -1.0f, 0.0f),
				4.0f,
				seededParams,
				0x12345678u,
				seededReference) &&
			seededReference.m_bHit,
			"the seeded bake ray should hit its receiver");
		for (uint32_t repetition = 0u; repetition < 16u; ++repetition)
		{
			Raytracing::PathTracer::PreparedRaySample repeated;
			Require(
				seededPathTracer.SamplePreparedSceneRay(
					glm::vec3(0.0f, 2.0f, 0.0f),
					glm::vec3(0.0f, -1.0f, 0.0f),
					4.0f,
					seededParams,
					0x12345678u,
					repeated) &&
				HasSameVectorBits(
					repeated.m_radiance,
					seededReference.m_radiance),
				"a prepared GI bake ray must derive every material-lobe decision from its explicit random seed");
		}
	}

	void TestEmissiveTrianglesIlluminateProbeReceivers()
	{
		Memory::ObjectAllocatorPtr allocator =
			Memory::ObjectAllocatorPtr::Make(
				Memory::EAllocationPolicy::SharedMemory_MultiThreaded);
		TVector<MaterialPtr> materials;
		materials.Add(MakeDiffuseFixtureMaterial(
			allocator,
			glm::vec3(1.0f)));
		auto emitterMaterial = MakeDiffuseFixtureMaterial(
			allocator,
			glm::vec3(1.0f));
		emitterMaterial->SetUniform(
			"material.emissiveFactor",
			glm::vec4(20.0f, 4.0f, 1.0f, 0.0f));
		materials.Add(std::move(emitterMaterial));

		auto triangles = TSharedPtr<TVector<Math::Triangle>>::Make();
		Math::Triangle receiver{};
		receiver.m_vertices[0] = glm::vec3(-5.0f, 0.0f, -5.0f);
		receiver.m_vertices[1] = glm::vec3(0.0f, 0.0f, 5.0f);
		receiver.m_vertices[2] = glm::vec3(5.0f, 0.0f, -5.0f);
		receiver.m_centroid =
			(receiver.m_vertices[0] + receiver.m_vertices[1] +
				receiver.m_vertices[2]) / 3.0f;
		for (uint32_t vertexIndex = 0u; vertexIndex < 3u; ++vertexIndex)
		{
			receiver.m_normals[vertexIndex] = glm::vec3(0.0f, 1.0f, 0.0f);
			receiver.m_tangent[vertexIndex] = glm::vec3(1.0f, 0.0f, 0.0f);
			receiver.m_bitangent[vertexIndex] = glm::vec3(0.0f, 0.0f, 1.0f);
			receiver.m_colors[vertexIndex] = glm::vec4(1.0f);
		}
		triangles->Add(receiver);

		Math::Triangle emitter{};
		emitter.m_vertices[0] = glm::vec3(0.5f, 2.0f, -0.5f);
		emitter.m_vertices[1] = glm::vec3(1.5f, 2.0f, -0.5f);
		emitter.m_vertices[2] = glm::vec3(1.0f, 2.0f, 0.5f);
		emitter.m_centroid =
			(emitter.m_vertices[0] + emitter.m_vertices[1] +
				emitter.m_vertices[2]) / 3.0f;
		emitter.m_materialIndex = 1u;
		for (uint32_t vertexIndex = 0u; vertexIndex < 3u; ++vertexIndex)
		{
			emitter.m_normals[vertexIndex] = glm::vec3(0.0f, -1.0f, 0.0f);
			emitter.m_tangent[vertexIndex] = glm::vec3(1.0f, 0.0f, 0.0f);
			emitter.m_bitangent[vertexIndex] = glm::vec3(0.0f, 0.0f, -1.0f);
			emitter.m_colors[vertexIndex] = glm::vec4(1.0f);
		}
		triangles->Add(emitter);

		auto blas = TSharedPtr<Raytracing::BVH>::Make(
			static_cast<uint32_t>(triangles->Num()));
		GlobalIlluminationLandscapeTestScene::BuildBakeBlas(
			*blas,
			*triangles);
		Math::AABB bounds;
		for (const Math::Triangle& triangle : *triangles)
		{
			for (const glm::vec3& vertex : triangle.m_vertices)
			{
				bounds.Extend(vertex);
			}
		}
		Raytracing::PathTracer::TLASInstance instance;
		instance.m_triangles = triangles;
		instance.m_blas = blas;
		instance.m_worldBounds = bounds;
		instance.m_worldMatrix = glm::mat4(1.0f);
		instance.m_inverseWorldMatrix = glm::mat4(1.0f);
		instance.m_materialBaseOffset = 0;
		TVector<Raytracing::PathTracer::TLASInstance> instances;
		instances.Add(std::move(instance));

		Raytracing::PathTracer pathTracer;
		Require(pathTracer.InitializeScene(
				instances,
				materials,
				{},
				false),
			"the emissive area-light fixture must initialize");
		const auto& preparationStats =
			pathTracer.GetLastScenePreparationStats();
		Require(
			preparationStats.m_triangleCount == 2u &&
			preparationStats.m_emissiveTriangleCount == 1u &&
			preparationStats.m_emissiveSamplingWeight > 0.0,
			"scene preparation must register the authored emissive triangle "
			"as an importance-sampled area light");

		Raytracing::PathTracer::Params params{};
		params.m_numSamples = 1u;
		params.m_numAmbientSamples = 1u;
		params.m_maxBounces = 0u;
		params.m_msaa = 1u;
		params.m_bRunTasksInline = true;
		params.m_bIncludeDirectLighting = false;
		params.m_bIncludeEnvironment = false;
		params.m_bIncludeEmissive = false;
		Raytracing::PathTracer::PreparedRaySample disabled;
		Require(pathTracer.SamplePreparedSceneRay(
				glm::vec3(0.0f, 1.0f, 0.0f),
				glm::vec3(0.0f, -1.0f, 0.0f),
				10.0f,
				params,
				155u,
				disabled) && disabled.m_bHit,
			"the emissive area-light fixture must hit its receiver");
		Require(glm::length(disabled.m_radiance) <= 0.000001f,
			"disabled emissive baking must leave the receiver dark");

		params.m_bIncludeEmissive = true;
		Raytracing::PathTracer::PreparedRaySample enabled;
		Require(pathTracer.SamplePreparedSceneRay(
				glm::vec3(0.0f, 1.0f, 0.0f),
				glm::vec3(0.0f, -1.0f, 0.0f),
				10.0f,
				params,
				155u,
				enabled) && enabled.m_bHit,
			"the emissive area-light fixture must resample its receiver");
		Require(
			enabled.m_radiance.r > 0.1f &&
				enabled.m_radiance.r > enabled.m_radiance.g &&
				enabled.m_radiance.g > enabled.m_radiance.b,
			"a small emissive triangle must illuminate a diffuse GI receiver with authored HDR color");
	}

	void TestAlphaCutoutTraversalMatchesRasterVisibility()
	{
		Memory::ObjectAllocatorPtr allocator =
			Memory::ObjectAllocatorPtr::Make(
				Memory::EAllocationPolicy::SharedMemory_MultiThreaded);
		auto cutoutTexture = TObjectPtr<CpuTextureFixture>::Make(
			allocator,
			FileId::Invalid);
		cutoutTexture->SetPixel(glm::u8vec4(255u, 255u, 255u, 0u));

		auto cutoutMaterial = MakeDiffuseFixtureMaterial(
			allocator,
			glm::vec3(1.0f));
		cutoutMaterial->SetSampler("baseColorSampler", cutoutTexture);
		cutoutMaterial->SetUniform("material.alphaCutoff", 0.5f);
		cutoutMaterial->SetRenderState(RHI::RenderState(
			true,
			true,
			0.0f,
			true,
			RHI::ECullMode::Back,
			RHI::EBlendMode::None));

		TVector<MaterialPtr> materials;
		materials.Add(std::move(cutoutMaterial));
		materials.Add(MakeDiffuseFixtureMaterial(
			allocator,
			glm::vec3(1.0f)));

		const auto makeTriangle = [](float y, uint8_t materialIndex)
		{
			Math::Triangle triangle{};
			triangle.m_vertices[0] = glm::vec3(-2.0f, y, -2.0f);
			triangle.m_vertices[1] = glm::vec3(0.0f, y, 2.0f);
			triangle.m_vertices[2] = glm::vec3(2.0f, y, -2.0f);
			triangle.m_centroid =
				(triangle.m_vertices[0] + triangle.m_vertices[1] +
					triangle.m_vertices[2]) / 3.0f;
			triangle.m_materialIndex = materialIndex;
			for (uint32_t vertexIndex = 0u; vertexIndex < 3u;
				++vertexIndex)
			{
				triangle.m_normals[vertexIndex] =
					glm::vec3(0.0f, 1.0f, 0.0f);
				triangle.m_tangent[vertexIndex] =
					glm::vec3(1.0f, 0.0f, 0.0f);
				triangle.m_bitangent[vertexIndex] =
					glm::vec3(0.0f, 0.0f, 1.0f);
				triangle.m_colors[vertexIndex] = glm::vec4(1.0f);
			}
			return triangle;
		};

		auto triangles = TSharedPtr<TVector<Math::Triangle>>::Make();
		triangles->Add(makeTriangle(1.0f, 0u));
		triangles->Add(makeTriangle(0.0f, 1u));
		auto blas = TSharedPtr<Raytracing::BVH>::Make(2u);
		GlobalIlluminationLandscapeTestScene::BuildBakeBlas(
			*blas,
			*triangles);
		Math::AABB bounds;
		for (const Math::Triangle& triangle : *triangles)
		{
			for (const glm::vec3& vertex : triangle.m_vertices)
			{
				bounds.Extend(vertex);
			}
		}

		Raytracing::PathTracer::TLASInstance instance;
		instance.m_triangles = triangles;
		instance.m_blas = blas;
		instance.m_worldBounds = bounds;
		instance.m_worldMatrix = glm::mat4(1.0f);
		instance.m_inverseWorldMatrix = glm::mat4(1.0f);
		instance.m_materialBaseOffset = 0;
		TVector<Raytracing::PathTracer::TLASInstance> instances;
		instances.Add(std::move(instance));

		Raytracing::PathTracer pathTracer;
		Require(pathTracer.InitializeScene(
				instances,
				materials,
				{},
				false),
			"the alpha-cutout traversal fixture must initialize");
		Raytracing::PathTracer::PreparedRaySample visibility;
		Require(
			pathTracer.SamplePreparedSceneVisibility(
				glm::vec3(0.0f, 2.0f, 0.0f),
				glm::vec3(0.0f, -1.0f, 0.0f),
				4.0f,
				visibility) &&
			visibility.m_bHit &&
			IsNear(visibility.m_distance, 2.0f, 0.0002f),
			"a transparent masked texel must not occlude bake, shadow, or "
			"probe-visibility rays before the opaque surface behind it");
	}

	void TestAlphaBlendDirectLightTransmittance()
	{
		Memory::ObjectAllocatorPtr allocator =
			Memory::ObjectAllocatorPtr::Make(
				Memory::EAllocationPolicy::SharedMemory_MultiThreaded);
		TVector<MaterialPtr> materials;
		for (const float alpha : { 0.25f, 0.5f })
		{
			auto material = MakeDiffuseFixtureMaterial(
				allocator,
				glm::vec3(1.0f));
			material->SetUniform(
				"material.baseColorFactor",
				glm::vec4(1.0f, 1.0f, 1.0f, alpha));
			material->SetRenderState(RHI::RenderState(
				true,
				false,
				0.0f,
				false,
				RHI::ECullMode::Back,
				RHI::EBlendMode::AlphaBlending));
			materials.Add(std::move(material));
		}
		materials.Add(MakeDiffuseFixtureMaterial(
			allocator,
			glm::vec3(1.0f)));

		const auto makeTriangle = [](float y, uint8_t materialIndex)
		{
			Math::Triangle triangle{};
			triangle.m_vertices[0] = glm::vec3(-2.0f, y, -2.0f);
			triangle.m_vertices[1] = glm::vec3(0.0f, y, 2.0f);
			triangle.m_vertices[2] = glm::vec3(2.0f, y, -2.0f);
			triangle.m_centroid =
				(triangle.m_vertices[0] + triangle.m_vertices[1] +
					triangle.m_vertices[2]) / 3.0f;
			triangle.m_materialIndex = materialIndex;
			for (uint32_t vertexIndex = 0u; vertexIndex < 3u;
				++vertexIndex)
			{
				triangle.m_normals[vertexIndex] =
					glm::vec3(0.0f, 1.0f, 0.0f);
				triangle.m_tangent[vertexIndex] =
					glm::vec3(1.0f, 0.0f, 0.0f);
				triangle.m_bitangent[vertexIndex] =
					glm::vec3(0.0f, 0.0f, 1.0f);
				triangle.m_colors[vertexIndex] = glm::vec4(1.0f);
			}
			return triangle;
		};

		auto triangles = TSharedPtr<TVector<Math::Triangle>>::Make();
		triangles->Add(makeTriangle(1.0f, 0u));
		triangles->Add(makeTriangle(0.5f, 1u));
		triangles->Add(makeTriangle(0.0f, 2u));
		auto blas = TSharedPtr<Raytracing::BVH>::Make(
			static_cast<uint32_t>(triangles->Num()));
		GlobalIlluminationLandscapeTestScene::BuildBakeBlas(
			*blas,
			*triangles);
		Math::AABB bounds;
		for (const Math::Triangle& triangle : *triangles)
		{
			for (const glm::vec3& vertex : triangle.m_vertices)
			{
				bounds.Extend(vertex);
			}
		}

		Raytracing::PathTracer::TLASInstance instance;
		instance.m_triangles = triangles;
		instance.m_blas = blas;
		instance.m_worldBounds = bounds;
		instance.m_worldMatrix = glm::mat4(1.0f);
		instance.m_inverseWorldMatrix = glm::mat4(1.0f);
		instance.m_materialBaseOffset = 0;
		TVector<Raytracing::PathTracer::TLASInstance> instances;
		instances.Add(std::move(instance));

		InspectablePathTracer pathTracer;
		Require(pathTracer.InitializeScene(
				instances,
				materials,
				{},
				false),
			"the alpha-blend shadow fixture must initialize");
		const Math::Ray ray(
			glm::vec3(0.0f, 2.0f, 0.0f),
			glm::vec3(0.0f, -1.0f, 0.0f));
		Require(IsNear(
				pathTracer.TraceDirectTransmittance(ray, 1.75f),
				0.375f,
				0.0001f),
			"two alpha-blended layers must attenuate direct light by the "
			"product of their uncovered fractions");
		Require(IsNear(
				pathTracer.TraceDirectTransmittance(ray, 3.0f),
				0.0f),
			"an opaque surface behind alpha-blended layers must still block "
			"the remaining direct light");
	}
}

int main(int argc, char** argv)
{
	std::set_terminate(ReportTermination);
	if (argc == 3 && std::string(argv[1]) == "--generate-visual-assets")
	{
		return GenerateVisualAssets(argv[2]);
	}
	try
	{
		RunTest(
			"GlobalIlluminationFrameGraphProbeCellContract",
			TestGlobalIlluminationFrameGraphProbeCellContract);
		RunTest(
			"PhysicalHdrFrameGraphContract",
			TestPhysicalHdrFrameGraphContract);
		RunTest("BinaryRoundTripDeterminismAndCorruption", TestBinaryRoundTripDeterminismAndCorruption);
		RunTest("AtomicFileAndPortableIdentityBoundary", TestAtomicFileAndPortableIdentityBoundary);
		RunTest("BlendAndAdditiveComposition", TestBlendAndAdditiveComposition);
		RunTest("SphericalHarmonicsAndSpatialSampling", TestSphericalHarmonicsAndSpatialSampling);
		RunTest("ConstantIrradianceReceiverSelection", TestConstantIrradianceReceiverSelection);
		RunTest(
			"ReceiverPlaneProbeRejection",
			TestReceiverPlaneProbeRejection);
		RunTest("WorldBindingRoundTripAndModes", TestWorldBindingRoundTripAndModes);
		RunTest(
			"ProbeBakeSavedWorldComparisonIgnoresEditorOnlyPrefabs",
			TestProbeBakeSavedWorldComparisonIgnoresEditorOnlyPrefabs);
		RunTest(
			"BakeControllerRejectsInvalidThreadCountBeforeSceneCapture",
			TestBakeControllerRejectsInvalidThreadCountBeforeSceneCapture);
		RunTest(
			"EveningLandscapeVisualWorldIsSavedBakeableLevel",
			TestEveningLandscapeVisualWorldIsSavedBakeableLevel);
		RunTest(
			"GIBakeQualityLabCoversCanonicalCases",
			TestGIBakeQualityLabCoversCanonicalCases);
		RunTest("GpuPackingAndWeightOnlyUpdates", TestGpuPackingAndWeightOnlyUpdates);
		RunTest("AdaptiveBakerAndLayoutReuse", TestAdaptiveBakerAndLayoutReuse);
		RunTest(
			"PrimaryDirectionPdfWeighting",
			TestPrimaryDirectionPdfWeighting);
		RunTest(
			"IncrementalProbeIrradianceAccumulation",
			TestIncrementalProbeIrradianceAccumulation);
		RunTest(
			"ReusableProbeTransportTracing",
			TestReusableProbeTransportTracing);
		RunTest(
			"RuntimeGIProbesServiceLifecycleAndPublication",
			TestRuntimeGIProbesServiceLifecycleAndPublication);
		RunTest(
			"RuntimeGIProbesFixedGridReuseAndBudgets",
			TestRuntimeGIProbesFixedGridReuseAndBudgets);
		RunTest(
			"RuntimeGIProbesIgnoreCameraMotion",
			TestRuntimeGIProbesIgnoreCameraMotion);
		RunTest(
			"RuntimeGIProbesProgressiveSamplingPublication",
			TestRuntimeGIProbesProgressiveSamplingPublication);
		RunTest(
			"RuntimeGIProbesRetainRelocationAcrossCameraMotion",
			TestRuntimeGIProbesRetainRelocationAcrossCameraMotion);
		RunTest(
			"RuntimeGIProbesKeepOneGridAcrossManyViews",
			TestRuntimeGIProbesKeepOneGridAcrossManyViews);
		RunTest(
			"AnisotropicAdaptiveSubdivisionHonorsSpacing",
			TestAnisotropicAdaptiveSubdivisionHonorsSpacing);
		RunTest(
			"GeometryNeighborhoodRefinementAndWallRepulsion",
			TestGeometryNeighborhoodRefinementAndWallRepulsion);
		RunTest("DeterministicBakeSeedsAndReusedLayoutValidation", TestDeterministicBakeSeedsAndReusedLayoutValidation);
		RunTest("RelocationClampingPreservesEffectiveOffset", TestRelocationClampingPreservesEffectiveOffset);
		RunTest(
			"EmbeddedProbeClassificationAndStableTransport",
			TestEmbeddedProbeClassificationAndStableTransport);
		RunTest(
			"PathTracerBackfacesAndIgnoredTriangleTraversal",
			TestPathTracerBackfacesAndIgnoredTriangleTraversal);
		RunTest(
			"PhysicalDielectricTransport",
			TestPhysicalDielectricTransport);
		RunTest(
			"LayeredGltfTransport",
			TestLayeredGltfTransport);
		RunTest(
			"PathTracerPreparationDeduplicationAndProgress",
			TestPathTracerPreparationDeduplicationAndProgress);
		RunTest(
			"ProbeBakeSkipsUnavailableMeshAndMaterialInstances",
			TestProbeBakeSkipsUnavailableMeshAndMaterialInstances);
		RunTest("FloatTextureNormalizationAndLandscapeLayerSampling", TestFloatTextureNormalizationAndLandscapeLayerSampling);
		RunTest(
			"MobilityAndLightModeContributionPolicy",
			TestMobilityAndLightModeContributionPolicy);
		RunTest(
			"PointLightModeChangesBakedRadiance",
			TestPointLightModeChangesBakedRadiance);
		RunTest(
			"PointLightRadiusAttenuationAndSecondaryBounce",
			TestPointLightRadiusAttenuationAndSecondaryBounce);
		RunTest(
			"HdrEnvironmentImportanceDistribution",
			TestHdrEnvironmentImportanceDistribution);
		RunTest(
			"EnvironmentMissIsCountedOnce",
			TestEnvironmentMissIsCountedOnce);
		RunTest(
			"EmissiveTrianglesIlluminateProbeReceivers",
			TestEmissiveTrianglesIlluminateProbeReceivers);
		RunTest(
			"AlphaCutoutTraversalMatchesRasterVisibility",
			TestAlphaCutoutTraversalMatchesRasterVisibility);
		RunTest(
			"AlphaBlendDirectLightTransmittance",
			TestAlphaBlendDirectLightTransmittance);
	}
	catch (const std::exception& exception)
	{
		std::cerr << "GlobalIlluminationContractTests failed: " <<
			exception.what() << std::endl;
		return 1;
	}
	std::cout << "GlobalIlluminationContractTests passed." << std::endl;
	return 0;
}
