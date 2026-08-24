#include "AssetRegistry/GlobalIllumination/ProbeVolumeBinary.h"
#include "AssetRegistry/GlobalIllumination/ProbeVolumeBaker.h"
#include "AssetRegistry/GlobalIllumination/ProbeVolumeComposition.h"
#include "AssetRegistry/GlobalIllumination/ProbeVolumeSampling.h"
#include "AssetRegistry/Material/MaterialImporter.h"
#include "AssetRegistry/Texture/TextureImporter.h"
#include "Components/LightComponent.h"
#include "Components/Tests/GlobalIlluminationLandscapeTestScene.h"
#include "ECS/LightingECS.h"
#include "ECS/StaticMeshRendererECS.h"
#include "ECS/TransformECS.h"
#include "Engine/GlobalIlluminationSettings.h"
#include "Engine/GameObject.h"
#include "Engine/World.h"
#include "Editor/GlobalIlluminationBakeController.h"
#include "Memory/ObjectAllocator.hpp"
#include "RHI/GlobalIllumination.h"
#include "Raytracing/PathTracer.h"
#include "Raytracing/ProbeVolumePathTracer.h"

#include <algorithm>
#include <bit>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string>

#include <glm/gtc/packing.hpp>

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

	bool IsNear(float lhs, float rhs, float tolerance = 0.0001f)
	{
		return std::abs(lhs - rhs) <= tolerance;
	}

	template<typename TTest>
	void RunTest(const char* name, TTest&& test)
	{
		g_currentTestName = name;
		std::cout << "[ RUN      ] " << name << std::endl;
		test();
		std::cout << "[       OK ] " << name << std::endl;
	}

	ProbeVolumeData MakeVolume(float coefficient, uint64_t lightingHash)
	{
		ProbeVolumeData data;
		data.m_volumeMin = glm::vec3(0.0f);
		data.m_volumeMax = glm::vec3(1.0f);
		data.m_transportHash = 0x12345678ull;
		data.m_lightingHash = lightingHash;
		data.m_sourceWorldHash = 0xaabbccddull;
		data.m_stateName = "Test State";
		data.m_bakerVersion = "GlobalIlluminationContractTests";
		data.m_diagnostics.m_averageValidity = 1.0f;

		ProbeVolumeBrick brick;
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
					ProbeVolumeSample probe;
					probe.m_position = glm::vec3(x, y, z);
					probe.m_irradiance[0] = glm::vec3(coefficient);
					for (glm::vec2& visibility : probe.m_visibility)
					{
						visibility = glm::vec2(100.0f, 10000.0f);
					}
					data.m_probes.Add(std::move(probe));
				}
			}
		}
		data.m_layoutHash = ComputeProbeVolumeLayoutHash(data);
		data.m_representationHash = ComputeProbeVolumeRepresentationHash(
			data.m_formatVersion,
			data.m_shOrder,
			data.m_compression);
		return data;
	}

	ProbeVolumeData MakeVisualVolume(
		const char* stateName,
		const glm::vec3& irradiance,
		uint64_t lightingHash)
	{
		ProbeVolumeData data = MakeVolume(0.0f, lightingHash);
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
		data.m_layoutHash = ComputeProbeVolumeLayoutHash(data);
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

	bool IntersectsBakeTriangles(
		const TVector<Math::Triangle>& triangles,
		const Math::Ray& ray,
		float maxRayLength)
	{
		for (const Math::Triangle& triangle : triangles)
		{
			glm::vec2 barycentric{};
			float distance = 0.0f;
			if (Math::IntersectRayTriangle(
					ray.GetOrigin(),
					ray.GetDirection(),
					triangle.m_vertices[0],
					triangle.m_vertices[1],
					triangle.m_vertices[2],
					barycentric,
					distance) &&
				distance < maxRayLength &&
				distance > -0.0000001f)
			{
				return true;
			}
		}
		return false;
	}

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
		light.m_intensity = GetEveningLightIntensity();
		light.m_indirectLightingIntensity = 1.0f;
		fixture.m_lights.Add(std::move(light));
		return fixture;
	}

	ProbeVolumeData MakeEveningLandscapeBounceVolume()
	{
		EveningLandscapeRaytracingFixture fixture =
			MakeEveningLandscapeRaytracingFixture();
		ProbeVolumeBakeSettings settings;
		settings.m_raysPerProbe = 256u;
		settings.m_bounceCount = 3u;
		settings.m_randomSeed = 155u;
		settings.m_maxSubdivisionLevel = 2u;
		settings.m_minProbeSpacing = 5.0f;
		settings.m_normalBias = 0.04f;
		settings.m_viewBias = 0.02f;
		settings.m_maxRayDistance = 120.0f;
		settings.m_bIncludeSky = false;
		settings.m_bIncludeEmissive = false;
		settings.m_bIncludeDirectLighting = true;

		Raytracing::ProbeVolumePathTracer pathTracer;
		Require(pathTracer.Initialize(
				fixture.m_instances,
				fixture.m_materials,
				fixture.m_lights,
				settings,
				glm::vec3(0.0f)),
			"the evening landscape fixture must initialize the CPU path tracer");

		ProbeVolumeBakeRequest request;
		request.m_stateName = "Evening Landscape Bounce";
		request.m_bakerVersion =
			"Sailor deterministic evening-landscape visual fixture/3";
		request.m_volumeMin = glm::vec3(-22.0f, -6.0f, -18.0f);
		request.m_volumeMax = glm::vec3(22.0f, 16.0f, 18.0f);
		request.m_settings = settings;
		request.m_sceneGeometryBounds.Add(fixture.m_bounds);
		request.m_sourceWorldHash = 0x155e11e71a9d5ca3ull;
		ProbeVolumeBakeResult result = ProbeVolumeBaker::Bake(request, pathTracer);
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
			if (!ProbeVolumeBinary::SaveAtomic(
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
			const ProbeVolumeBinaryResult loaded =
				ProbeVolumeBinary::Load(outputPath);
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
		if (!ProbeVolumeBinary::SaveAtomic(
				eveningLandscapePath,
				MakeEveningLandscapeBounceVolume(),
				diagnostic))
		{
			std::cerr << "Cannot generate EveningLandscapeBounce.probes: " <<
				diagnostic << std::endl;
			return 1;
		}
		const ProbeVolumeBinaryResult eveningLandscape =
			ProbeVolumeBinary::Load(eveningLandscapePath);
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

	class ConstantBakeRaySampler final : public IProbeVolumeBakeRaySampler
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
			ProbeVolumeBakeRaySample& outSample,
			std::string&) const override
		{
			outSample.m_radiance = m_radiance;
			outSample.m_distance = maxDistance;
			outSample.m_bHit = false;
			return true;
		}

	private:
		glm::vec3 m_radiance{};
	};

	class SeedDrivenBakeRaySampler final : public IProbeVolumeBakeRaySampler
	{
	public:
		bool Sample(
			const glm::vec3&,
			const glm::vec3&,
			float maxDistance,
			uint32_t randomSeed,
			ProbeVolumeBakeRaySample& outSample,
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

	class BoundaryRelocationSampler final : public IProbeVolumeBakeRaySampler
	{
	public:
		bool Sample(
			const glm::vec3&,
			const glm::vec3& direction,
			float maxDistance,
			uint32_t,
			ProbeVolumeBakeRaySample& outSample,
			std::string&) const override
		{
			const bool bPositiveXAxis = direction.x > 0.9999f &&
				std::abs(direction.y) < 0.0001f &&
				std::abs(direction.z) < 0.0001f;
			outSample.m_radiance = glm::vec3(0.25f);
			outSample.m_distance = bPositiveXAxis ? 0.0f : maxDistance;
			outSample.m_bHit = bPositiveXAxis;
			return true;
		}
	};

	class MaterialSamplingPathTracer final : public Raytracing::PathTracer
	{
	public:
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
	};

	void TestBinaryRoundTripDeterminismAndCorruption()
	{
		const ProbeVolumeData source = MakeVolume(2.0f, 11u);
		std::string diagnostic;
		Require(source.Validate(diagnostic), "test volume must be valid: " + diagnostic);

		TVector<uint8_t> first;
		TVector<uint8_t> second;
		Require(ProbeVolumeBinary::Serialize(source, first, diagnostic),
			"valid volume should serialize: " + diagnostic);
		Require(ProbeVolumeBinary::Serialize(source, second, diagnostic) && first == second,
			"the same bake state must serialize to deterministic bytes");

		const ProbeVolumeBinaryResult roundTrip =
			ProbeVolumeBinary::Deserialize(first.GetData(), first.Num());
		Require(roundTrip.IsSuccess(), "serialized volume should round-trip: " + roundTrip.m_diagnostic);
		Require(roundTrip.m_data->m_probes.Num() == 8u &&
			roundTrip.m_data->m_bricks.Num() == 1u &&
			roundTrip.m_data->m_bricks[0].m_probeCounts == glm::uvec3(2u) &&
			roundTrip.m_data->m_lightingHash == 11u &&
			roundTrip.m_data->m_stateName == "Test State" &&
			roundTrip.m_data->m_probes[7].m_irradiance[0] == glm::vec3(2.0f),
			"round-trip must preserve settings, adaptive layout, hashes, and L2 SH");

		TVector<uint8_t> corrupted = first;
		corrupted[corrupted.Num() - 1u] ^= 0x40u;
		const ProbeVolumeBinaryResult rejected =
			ProbeVolumeBinary::Deserialize(corrupted.GetData(), corrupted.Num());
		Require(rejected.m_status == EProbeVolumeBinaryStatus::ChecksumMismatch,
			"payload corruption must be detected before publication");

		const ProbeVolumeBinaryResult truncated =
			ProbeVolumeBinary::Deserialize(first.GetData(), first.Num() - 1u);
		Require(truncated.m_status == EProbeVolumeBinaryStatus::Truncated,
			"truncated payload must be rejected without partial data");

		TVector<uint8_t> unsupportedHeader = first;
		unsupportedHeader[20u] = 1u;
		const ProbeVolumeBinaryResult unsupportedHeaderResult =
			ProbeVolumeBinary::Deserialize(
				unsupportedHeader.GetData(),
				unsupportedHeader.Num());
		Require(
			unsupportedHeaderResult.m_status ==
				EProbeVolumeBinaryStatus::InvalidPayload,
			"unknown fixed-header flags must be rejected explicitly");

		ProbeVolumeData unidentified = source;
		unidentified.m_stateName.clear();
		Require(!ProbeVolumeBinary::Serialize(
			unidentified,
			second,
			diagnostic),
			"a .probes file must identify its one baked state");
		ProbeVolumeData unhashed = source;
		unhashed.m_transportHash = 0u;
		Require(!ProbeVolumeBinary::Serialize(
			unhashed,
			second,
			diagnostic),
			"a .probes file must carry transport identity for safe composition");
		ProbeVolumeData excessiveSettings = source;
		excessiveSettings.m_bakeSettings.m_maxSubdivisionLevel =
			ProbeVolumeMaxSubdivisionLevel + 1u;
		Require(!ProbeVolumeBinary::Serialize(
			excessiveSettings,
			second,
			diagnostic),
			"a .probes file must reject unsupported bake-setting ranges");

		ProbeVolumeData overflowingGrid = source;
		overflowingGrid.m_bricks[0].m_probeCounts = glm::uvec3(
			(std::numeric_limits<uint32_t>::max)());
		Require(!overflowingGrid.Validate(diagnostic) &&
			diagnostic.find("overflows") != std::string::npos,
			"malicious brick dimensions must not wrap their probe-grid product");

		ProbeVolumeData outsideVolume = source;
		outsideVolume.m_probes[0].m_position.x = -1.0f;
		outsideVolume.m_layoutHash = ComputeProbeVolumeLayoutHash(outsideVolume);
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
		const ProbeVolumeData source = MakeVolume(1.0f, 21u);
		Require(ProbeVolumeBinary::SaveAtomic(firstPath, source, diagnostic),
			"atomic .probes save should succeed: " + diagnostic);
		const ProbeVolumeData replacement = MakeVolume(2.0f, 22u);
		Require(!ProbeVolumeBinary::SaveAtomic(
			firstPath,
			replacement,
			diagnostic,
			false),
			"Overwrite=false must atomically reject an existing .probes target");
		const ProbeVolumeBinaryResult preserved =
			ProbeVolumeBinary::Load(firstPath);
		Require(
			preserved.IsSuccess() && preserved.m_data->m_lightingHash == 21u,
			"a rejected no-overwrite save must preserve the complete existing state");
		std::filesystem::copy_file(firstPath, copyPath);
		const ProbeVolumeBinaryResult copied = ProbeVolumeBinary::Load(copyPath);
		Require(copied.IsSuccess() && copied.m_data->m_lightingHash == 21u,
			"a copied binary must remain independently loadable without an embedded FileId");
		std::error_code error;
		std::filesystem::remove_all(directory, error);
	}

	void TestBlendAndAdditiveComposition()
	{
		ProbeVolumeDataPtr day = ProbeVolumeDataPtr::Make();
		ProbeVolumeDataPtr evening = ProbeVolumeDataPtr::Make();
		ProbeVolumeDataPtr lamps = ProbeVolumeDataPtr::Make();
		*day = MakeVolume(1.0f, 1u);
		*evening = MakeVolume(3.0f, 2u);
		*lamps = MakeVolume(2.0f, 3u);

		TVector<ProbeVolumeCompositionInput> inputs;
		inputs.Add({ "Day", day, EGlobalIlluminationProbeMode::Blend, 1.0f });
		inputs.Add({ "Evening", evening, EGlobalIlluminationProbeMode::Blend, 3.0f });
		inputs.Add({ "Lamps", lamps, EGlobalIlluminationProbeMode::Additive, 0.5f });
		ProbeVolumeCompositionResult result = ProbeVolumeComposer::Compose(inputs, 3u);
		Require(result.IsSuccess(), "compatible Blend/Additive states should compose: " + result.m_diagnostic);
		Require(IsNear(result.m_data->m_probes[0].m_irradiance[0].x, 3.5f),
			"Blend must normalize to 2.5 and Additive must contribute an unnormalized 1.0");
		Require(IsNear(result.m_effectiveWeights[0], 0.25f) &&
			IsNear(result.m_effectiveWeights[1], 0.75f) &&
			IsNear(result.m_effectiveWeights[2], 0.5f),
			"snapshot must expose effective normalized Blend and raw Additive weights");

		const ProbeVolumeCompositionResult overBudget =
			ProbeVolumeComposer::Compose(inputs, 2u);
		Require(overBudget.m_status == EProbeVolumeCompositionStatus::BudgetExceeded &&
			!overBudget.m_data,
			"quality budget overflow must reject the complete mixture without dropping Additive states");

		lamps->m_transportHash += 1u;
		const ProbeVolumeCompositionResult incompatible =
			ProbeVolumeComposer::Compose(inputs, 3u);
		Require(incompatible.m_status == EProbeVolumeCompositionStatus::Incompatible,
			"different transport/visibility states must not be blended");
	}

	void TestSphericalHarmonicsAndSpatialSampling()
	{
		ProbeVolumeData data = MakeVolume(0.0f, 31u);
		for (uint32_t index = 0u; index < data.m_probes.Num(); ++index)
		{
			data.m_probes[index].m_irradiance[0] =
				glm::vec3(static_cast<float>(index + 1u));
		}
		glm::vec3 sampled{};
		ProbeVolumeSampleDebugInfo debug;
		Require(SampleProbeVolumeIrradiance(
			data,
			glm::vec3(0.5f),
			glm::vec3(0.0f, 1.0f, 0.0f),
			sampled,
			&debug),
			"center of a valid adaptive brick should sample");
		Require(IsNear(sampled.x, 4.5f * 0.2820947918f) &&
			IsNear(debug.m_totalUnnormalizedWeight, 1.0f),
			"trilinear sampling must average all eight L2 SH payloads at brick center");
		float normalizedWeight = 0.0f;
		for (float weight : debug.m_weights) normalizedWeight += weight;
		Require(IsNear(normalizedWeight, 1.0f),
			"debug visibility weights must expose the normalized interpolation used by shading");
		Require(!SampleProbeVolumeIrradiance(
			data,
			glm::vec3(2.0f),
			glm::vec3(0.0f, 1.0f, 0.0f),
			sampled),
			"positions outside all bricks must select the environment fallback");
	}

	void TestWorldBindingRoundTripAndModes()
	{
		GlobalIlluminationWorldSettings source;
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
		GlobalIlluminationWorldSettings parsed;
		std::string diagnostic;
		Require(parsed.Deserialize(root, diagnostic),
			"world GI settings should round-trip: " + diagnostic);
		Require(parsed.m_probes.Num() == 2u &&
			parsed.m_probes["Day"].m_mode == EGlobalIlluminationProbeMode::Blend &&
			parsed.m_probes["Lamps"].m_mode == EGlobalIlluminationProbeMode::Additive &&
			parsed.m_probes["Lamps"].m_bPreload,
			"each named .probes binding must retain its independent Blend/Additive role");

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

	void TestEveningLandscapeFixtureProvesSecondaryLighting()
	{
		using namespace GlobalIlluminationLandscapeTestScene;
		EveningLandscapeRaytracingFixture fixture =
			MakeEveningLandscapeRaytracingFixture();
		const glm::vec3 toLight = -GetEveningLightDirection();
		Require(IntersectsBakeTriangles(
				*fixture.m_triangles,
				Math::Ray(GetReceiverEvidencePoint(), toLight),
				120.0f),
			"the evening ridge must geometrically block direct sun at the receiver");
		Require(!IntersectsBakeTriangles(
				*fixture.m_triangles,
				Math::Ray(GetBounceCliffEvidencePoint(), toLight),
				120.0f),
			"the upper warm cliff must remain directly exposed to the evening sun");

#if defined(SAILOR_TEST_SOURCE_DIR)
		const std::filesystem::path fixturePath =
			std::filesystem::path(SAILOR_TEST_SOURCE_DIR) /
			"Content/Tests/Visual/EveningLandscapeBounce.probes";
#else
		const std::filesystem::path fixturePath =
			"Content/Tests/Visual/EveningLandscapeBounce.probes";
#endif
		const ProbeVolumeBinaryResult loaded =
			ProbeVolumeBinary::Load(fixturePath);
		Require(loaded.IsSuccess(),
			"the tracked evening landscape bake must load: " +
				loaded.m_diagnostic);
		Require(loaded.m_data->m_stateName == "Evening Landscape Bounce" &&
			loaded.m_data->m_bakeSettings.m_bounceCount >= 2u &&
			!loaded.m_data->m_bakeSettings.m_bIncludeSky &&
			loaded.m_data->m_bakeSettings.m_bIncludeDirectLighting,
			"the visual fixture must be a light-driven multi-bounce evening state, "
			"not an ambient color fill");

		glm::vec3 receiverIrradiance{};
		Require(SampleProbeVolumeIrradiance(
				*loaded.m_data,
				GetReceiverEvidencePoint(),
				glm::vec3(0.0f, 1.0f, 0.0f),
				receiverIrradiance),
			"the occluded receiver must be covered by the baked probe topology");
		const float receiverEnergy = glm::dot(
			receiverIrradiance,
			glm::vec3(0.2126f, 0.7152f, 0.0722f));
		Require(
			receiverEnergy > MinimumReceiverIrradianceEnergy,
			"an occluded receiver must retain measurable reflected irradiance");

		float minimumDcEnergy = (std::numeric_limits<float>::max)();
		float maximumDcEnergy = 0.0f;
		for (const ProbeVolumeSample& probe : loaded.m_data->m_probes)
		{
			const float energy = glm::dot(
				glm::max(probe.m_irradiance[0], glm::vec3(0.0f)),
				glm::vec3(0.2126f, 0.7152f, 0.0722f));
			minimumDcEnergy = (std::min)(minimumDcEnergy, energy);
			maximumDcEnergy = (std::max)(maximumDcEnergy, energy);
		}
		Require(maximumDcEnergy > minimumDcEnergy * 1.35f + 0.01f,
			"the baked state must contain spatially varying irradiance from scene "
			"topology instead of one uniform SH color");
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

		GlobalIlluminationWorldSettings globalIllumination;
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
				const YAML::Node intensity =
					properties["directionalLightIntensity"];
				bHasSky = name == "Evening Sky" &&
					IsNear(
						properties["sunAngle"].as<float>(),
						GlobalIlluminationLandscapeTestScene::
							EveningSunAngleDegrees) &&
					directionalLight && directionalLight.IsMap() &&
					directionalLight["instanceId"] &&
					intensity && intensity.IsSequence() &&
					intensity.size() == 3u &&
					IsNear(
						intensity[0].as<float>(),
						GlobalIlluminationLandscapeTestScene::
							GetEveningLightIntensity().x) &&
					IsNear(
						intensity[1].as<float>(),
						GlobalIlluminationLandscapeTestScene::
							GetEveningLightIntensity().y) &&
					IsNear(
						intensity[2].as<float>(),
						GlobalIlluminationLandscapeTestScene::
							GetEveningLightIntensity().z);
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

	void TestGpuPackingAndWeightOnlyUpdates()
	{
		ProbeVolumeDataPtr day = ProbeVolumeDataPtr::Make();
		ProbeVolumeDataPtr evening = ProbeVolumeDataPtr::Make();
		*day = MakeVolume(1.0f, 41u);
		*evening = MakeVolume(3.0f, 42u);

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
			(std::bit_cast<uint32_t>(layout.m_nodes[0].m_minAndLeft.w) &
				0x80000000u) != 0u,
			"single adaptive brick must produce one encoded BVH leaf");

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
				RHI::EGlobalIlluminationDebugVisualization::IndirectOnly);
		Require(header.m_counts == glm::uvec4(1u, 1u, 1u, 8u) &&
			header.m_stateAndDebug.x == 2u &&
			header.m_stateAndDebug.z == static_cast<uint32_t>(
				RHI::EGlobalIlluminationDebugVisualization::IndirectOnly),
			"GPU header must expose resident counts and the selected GI debug mode");
	}

	void TestAdaptiveBakerAndLayoutReuse()
	{
		ProbeVolumeBakeRequest request;
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
		request.m_progress = [&](const ProbeVolumeBakeProgress& progress)
		{
			finalProgress = progress.m_fraction;
		};

		const ConstantBakeRaySampler daylight(glm::vec3(1.0f));
		const ProbeVolumeBakeResult day = ProbeVolumeBaker::Bake(
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
			day.m_data->m_probes[0].m_irradiance[0].x,
			4.0f * 3.14159265358979323846f *
				0.2820947918f,
			0.001f),
			"baker must project radiance and include the white Lambertian 1 / PI normalization");

		ProbeVolumeBakeRequest reused = request;
		reused.m_stateName = "Night";
		reused.m_layoutSource = day.m_data.GetRawPtr();
		reused.m_progress = {};
		const ConstantBakeRaySampler moonlight(glm::vec3(0.2f, 0.3f, 0.5f));
		const ProbeVolumeBakeResult night = ProbeVolumeBaker::Bake(
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

		std::atomic<bool> cancel{ true };
		ProbeVolumeBakeRequest cancelled = request;
		cancelled.m_stateName = "Cancelled";
		cancelled.m_cancel = &cancel;
		const ProbeVolumeBakeResult cancelledResult = ProbeVolumeBaker::Bake(
			cancelled,
			daylight);
		Require(cancelledResult.m_status == EProbeVolumeBakeStatus::Cancelled &&
			!cancelledResult.m_data,
			"cancelled bakes must never publish partial .probes data");
	}

	void TestDeterministicBakeSeedsAndReusedLayoutValidation()
	{
		ProbeVolumeBakeRequest request;
		request.m_stateName = "Deterministic";
		request.m_volumeMin = glm::vec3(0.0f);
		request.m_volumeMax = glm::vec3(1.0f);
		request.m_settings.m_raysPerProbe = 8u;
		request.m_settings.m_bounceCount = 1u;
		request.m_settings.m_randomSeed = 1729u;
		request.m_settings.m_maxSubdivisionLevel = 0u;
		request.m_settings.m_minProbeSpacing = 1.0f;

		const SeedDrivenBakeRaySampler sampler;
		const ProbeVolumeBakeResult first = ProbeVolumeBaker::Bake(
			request,
			sampler);
		const ProbeVolumeBakeResult second = ProbeVolumeBaker::Bake(
			request,
			sampler);
		Require(first.IsSuccess() && second.IsSuccess(),
			"seed-driven test bakes should succeed");
		Require(first.m_data->m_lightingHash == second.m_data->m_lightingHash &&
			first.m_data->m_transportHash == second.m_data->m_transportHash &&
			first.m_data->m_probes[0].m_irradiance ==
				second.m_data->m_probes[0].m_irradiance,
			"the same bake randomSeed must reproduce transport and lighting output");

		ProbeVolumeBakeRequest differentSeed = request;
		differentSeed.m_settings.m_randomSeed += 1u;
		const ProbeVolumeBakeResult different = ProbeVolumeBaker::Bake(
			differentSeed,
			sampler);
		Require(different.IsSuccess() &&
			different.m_data->m_lightingHash != first.m_data->m_lightingHash,
			"a different bake randomSeed must select a different sampling stream");

		ProbeVolumeBakeRequest invalidReuse = request;
		invalidReuse.m_layoutSource = first.m_data.GetRawPtr();
		invalidReuse.m_settings.m_raysPerProbe = 0u;
		const ProbeVolumeBakeResult rejected = ProbeVolumeBaker::Bake(
			invalidReuse,
			sampler);
		Require(rejected.m_status == EProbeVolumeBakeStatus::InvalidRequest,
			"layout reuse must not bypass ray and bounce count validation");

		invalidReuse.m_settings.m_raysPerProbe =
			ProbeVolumeMaxRaysPerProbe + 1u;
		const ProbeVolumeBakeResult excessive = ProbeVolumeBaker::Bake(
			invalidReuse,
			sampler);
		Require(excessive.m_status == EProbeVolumeBakeStatus::InvalidRequest,
			"layout reuse must not bypass supported sampling limits");
	}

	void TestRelocationClampingPreservesEffectiveOffset()
	{
		ProbeVolumeBakeRequest request;
		request.m_stateName = "Relocation";
		request.m_volumeMin = glm::vec3(0.0f);
		request.m_volumeMax = glm::vec3(1.0f);
		request.m_settings.m_raysPerProbe = 8u;
		request.m_settings.m_bounceCount = 1u;
		request.m_settings.m_maxSubdivisionLevel = 0u;
		request.m_settings.m_minProbeSpacing = 1.0f;

		const BoundaryRelocationSampler sampler;
		const ProbeVolumeBakeResult result = ProbeVolumeBaker::Bake(
			request,
			sampler);
		Require(result.IsSuccess(),
			"boundary-relocation bake should succeed: " + result.m_diagnostic);

		const ProbeVolumeSample& clamped = result.m_data->m_probes[0];
		const ProbeVolumeSample& moved = result.m_data->m_probes[1];
		const uint32_t relocatedFlag = static_cast<uint32_t>(
			EProbeVolumeSampleFlag::Relocated);
		Require(clamped.m_position.x == 0.0f &&
			clamped.m_relocationOffset == glm::vec3(0.0f) &&
			(clamped.m_flags & relocatedFlag) == 0u,
			"a relocation clipped completely by the volume must record zero effective offset");
		Require(IsNear(moved.m_position.x, 0.75f) &&
			IsNear(moved.m_relocationOffset.x, -0.25f) &&
			(moved.m_flags & relocatedFlag) != 0u,
			"a moved probe must store the post-clamp offset used by debug and transport identity");
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
		ProbeVolumeBakeSettings settings;
		Raytracing::ProbeVolumePathTracer pathTracer;
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
		Raytracing::ProbeVolumePathTracer cancelledPathTracer;
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
	}

	void TestMobilityContributionPolicy()
	{
		Require(
			IsGlobalIlluminationBakeContributor(EMobilityType::Static) &&
			IsGlobalIlluminationBakeContributor(EMobilityType::Stationary),
			"static and stationary scene objects must contribute geometry and lights to baked global illumination");
		Require(
			!IsGlobalIlluminationBakeContributor(EMobilityType::Dynamic),
			"dynamic scene objects must not contribute bake geometry or lights");

		GlobalIlluminationMobilityTestWorld world;
		const auto addLight = [&world](
			const char* name,
			EMobilityType mobility,
			float intensity)
		{
			GameObjectPtr gameObject = world.Instantiate(name);
			gameObject->SetMobilityType(mobility);
			auto light = gameObject->AddComponent<LightComponent>();
			light->SetIntensity(glm::vec3(intensity));
		};
		addLight("StaticLight", EMobilityType::Static, 1.0f);
		addLight("StationaryLight", EMobilityType::Stationary, 2.0f);
		addLight("DynamicLight", EMobilityType::Dynamic, 3.0f);

		LightingECS* lighting = world.GetECS<LightingECS>();
		TVector<Raytracing::LightProxy> allLights;
		TVector<Raytracing::LightProxy> bakeLights;
		lighting->GetLightProxies(allLights);
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
			allLights.Num() == 3u && containsIntensity(allLights, 3.0f),
			"the generic path-tracer light collection must retain dynamic lights");
		Require(
			bakeLights.Num() == 2u &&
				containsIntensity(bakeLights, 1.0f) &&
				containsIntensity(bakeLights, 2.0f) &&
				!containsIntensity(bakeLights, 3.0f),
			"the GI bake light collection must include Static and Stationary lights and exclude Dynamic lights");
		world.Clear();
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
		RunTest("BinaryRoundTripDeterminismAndCorruption", TestBinaryRoundTripDeterminismAndCorruption);
		RunTest("AtomicFileAndPortableIdentityBoundary", TestAtomicFileAndPortableIdentityBoundary);
		RunTest("BlendAndAdditiveComposition", TestBlendAndAdditiveComposition);
		RunTest("SphericalHarmonicsAndSpatialSampling", TestSphericalHarmonicsAndSpatialSampling);
		RunTest("WorldBindingRoundTripAndModes", TestWorldBindingRoundTripAndModes);
		RunTest(
			"ProbeBakeSavedWorldComparisonIgnoresEditorOnlyPrefabs",
			TestProbeBakeSavedWorldComparisonIgnoresEditorOnlyPrefabs);
		RunTest(
			"EveningLandscapeFixtureProvesSecondaryLighting",
			TestEveningLandscapeFixtureProvesSecondaryLighting);
		RunTest(
			"EveningLandscapeVisualWorldIsSavedBakeableLevel",
			TestEveningLandscapeVisualWorldIsSavedBakeableLevel);
		RunTest("GpuPackingAndWeightOnlyUpdates", TestGpuPackingAndWeightOnlyUpdates);
		RunTest("AdaptiveBakerAndLayoutReuse", TestAdaptiveBakerAndLayoutReuse);
		RunTest("DeterministicBakeSeedsAndReusedLayoutValidation", TestDeterministicBakeSeedsAndReusedLayoutValidation);
		RunTest("RelocationClampingPreservesEffectiveOffset", TestRelocationClampingPreservesEffectiveOffset);
		RunTest(
			"PathTracerPreparationDeduplicationAndProgress",
			TestPathTracerPreparationDeduplicationAndProgress);
		RunTest("FloatTextureNormalizationAndLandscapeLayerSampling", TestFloatTextureNormalizationAndLandscapeLayerSampling);
		RunTest("MobilityContributionPolicy", TestMobilityContributionPolicy);
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
