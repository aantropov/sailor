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
#include <condition_variable>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <iostream>
#include <mutex>
#include <set>
#include <stdexcept>
#include <string>
#include <thread>

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

	class ConcurrentSeedDrivenBakeRaySampler final :
		public IProbeVolumeBakeRaySampler
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
			ProbeVolumeBakeRaySample& outSample,
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
		const ProbeVolumeSample& lhs,
		const ProbeVolumeSample& rhs)
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
		}
		return true;
	}

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
		source.m_mode = EGlobalIlluminationMode::BakedOnly;
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
		Require(parsed.m_mode == EGlobalIlluminationMode::BakedOnly &&
			parsed.m_probes.Num() == 2u &&
			parsed.m_probes["Day"].m_mode == EGlobalIlluminationProbeMode::Blend &&
			parsed.m_probes["Lamps"].m_mode == EGlobalIlluminationProbeMode::Additive &&
			parsed.m_probes["Lamps"].m_bPreload,
			"each named .probes binding must retain its independent Blend/Additive role");

		YAML::Node legacyRoot = YAML::Clone(root);
		legacyRoot["globalIllumination"].remove("mode");
		Require(parsed.Deserialize(legacyRoot, diagnostic) &&
			parsed.m_mode == EGlobalIlluminationMode::RealtimeAndBaked,
			"worlds without an explicit GI mode must retain realtime fallback compatibility");

		root["globalIllumination"]["mode"] = "ReflectionsOnly";
		Require(!parsed.Deserialize(root, diagnostic) &&
			diagnostic.find("RealtimeAndBaked") != std::string::npos,
			"unknown world GI modes must fail atomically");
		root["globalIllumination"]["mode"] = "BakedOnly";

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
		EditorProbeVolumeBakeRequest request;
		request.m_threadCount = 0u;
		std::string diagnostic;
		Require(
			!controller.Start(nullptr, request, diagnostic) &&
				diagnostic.find("between 1 and") != std::string::npos,
			"the editor bake controller must reject zero threads before capturing a scene");

		request.m_threadCount = ProbeVolumeMaxBakeThreadCount + 1u;
		diagnostic.clear();
		Require(
			!controller.Start(nullptr, request, diagnostic) &&
				diagnostic.find("between 1 and") != std::string::npos,
			"the editor bake controller must reject excessive threads before capturing a scene");
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

		const RHI::RHIGlobalIlluminationRenderStats renderStats =
			RHI::BuildGlobalIlluminationRenderStats(&snapshot);
		const uint64_t expectedCpuPayloadBytes = 2u * (
			sizeof(ProbeVolumeBrick) +
			8u * sizeof(ProbeVolumeSample));
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
				RHI::EGlobalIlluminationDebugVisualization::IndirectOnly,
				EGlobalIlluminationMode::BakedOnly,
				false);
		Require(header.m_counts == glm::uvec4(1u, 1u, 1u, 8u) &&
			header.m_stateAndDebug.x == 2u &&
			header.m_settings.x == 0u &&
			header.m_settings.y == static_cast<uint32_t>(
				EGlobalIlluminationMode::BakedOnly) &&
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

		ProbeVolumeBakeRequest parallel = request;
		parallel.m_threadCount = 4u;
		std::string parallelStage;
		parallel.m_progress = [&parallelStage](
			const ProbeVolumeBakeProgress& progress)
		{
			parallelStage = progress.m_stage;
		};
		const ConcurrentSeedDrivenBakeRaySampler parallelSampler(4u);
		const ProbeVolumeBakeResult parallelResult = ProbeVolumeBaker::Bake(
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

		ProbeVolumeBakeRequest invalidThreads = request;
		invalidThreads.m_threadCount = 0u;
		Require(
			ProbeVolumeBaker::Bake(invalidThreads, sampler).m_status ==
				EProbeVolumeBakeStatus::InvalidRequest,
			"a zero bake thread count must fail closed");
		invalidThreads.m_threadCount = ProbeVolumeMaxBakeThreadCount + 1u;
		Require(
			ProbeVolumeBaker::Bake(invalidThreads, sampler).m_status ==
				EProbeVolumeBakeStatus::InvalidRequest,
			"a bake thread count above the supported limit must fail closed");
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
				"new lights must preserve the legacy realtime plus baked behavior");
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
		pointLight->SetAttenuation(glm::vec3(1.0f, 0.0f, 0.0f));
		pointLight->SetRadius(20.0f);

		const glm::vec3 receiver(
			20.5f,
			SampleLandscapeHeight(20.5f, 20.5f),
			20.5f);
		pointLightObject->GetTransformComponent().SetPosition(
			receiver + glm::vec3(0.0f, 6.0f, 0.0f));
		world.GetECS<TransformECS>()->Tick(0.0f);

		ProbeVolumeBakeSettings settings;
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

			Raytracing::ProbeVolumePathTracer pathTracer;
			Require(pathTracer.Initialize(
					fixture.m_instances,
					fixture.m_materials,
					bakeLights,
					settings,
					glm::vec3(0.0f)),
				"the point-light GI fixture must initialize the CPU path tracer");

			ProbeVolumeBakeRaySample sample;
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

	void TestPointLightRayIntersectionsMatchRealtimeAttenuation()
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
			floor.m_colors[vertexIndex] = vertexIndex == 0u ?
				glm::vec4(1.0f, 0.0f, 0.0f, 0.0f) : glm::vec4(0.0f);
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

		Raytracing::LightProxy pointLight;
		pointLight.m_type = ELightType::Point;
		pointLight.m_worldPosition = glm::vec3(12.0f, 5.0f, 0.0f);
		pointLight.m_intensity = glm::vec3(7.0f, 5.0f, 3.0f);
		pointLight.m_attenuation = glm::vec3(1.0f, 0.5f, 0.25f);
		pointLight.m_bounds = glm::vec3(10.0f);
		TVector<Raytracing::LightProxy> lights;
		lights.Add(pointLight);

		Raytracing::PathTracer pathTracer;
		Require(pathTracer.InitializeScene(
				instances,
				materials,
				lights,
				false),
			"the Point Light ray-intersection fixture must initialize");
		Require(pathTracer.ArePreparedMaterialsFullyResolved(),
			"the Point Light ray-intersection fixture must resolve its material");

		Raytracing::PathTracer::Params params{};
		params.m_numSamples = 1u;
		params.m_numAmbientSamples = 1u;
		params.m_maxBounces = 0u;
		params.m_msaa = 1u;
		params.m_ambient = glm::vec3(0.0f);
		params.m_bIncludeDirectLighting = true;
		params.m_bIncludeEnvironment = false;
		params.m_bIncludeEmissive = false;
		params.m_bIncludePointLightRayIntersections = true;

		const auto sampleRay = [&](float perpendicularDistance,
			float maxDistance,
			uint32_t randomSeed)
		{
			Raytracing::PathTracer::PreparedRaySample sample;
			Require(pathTracer.SamplePreparedSceneRay(
					glm::vec3(-8.0f, 5.0f + perpendicularDistance, 0.0f),
					glm::vec3(1.0f, 0.0f, 0.0f),
					maxDistance,
					params,
					randomSeed,
					sample),
				"the CPU baker must accept a Point Light range test ray");
			Require(!sample.m_bHit,
				"the Point Light range test ray must not hit geometry");
			return sample.m_radiance;
		};

		const auto expectedAttenuation = [&](float distance)
		{
			const float normalizedDistance = glm::clamp(
				distance / pointLight.m_bounds.x,
				0.0f,
				1.0f);
			const float edgeProgress = glm::clamp(
				(normalizedDistance - 0.9f) / 0.1f,
				0.0f,
				1.0f);
			const float rangeWindow = 1.0f -
				edgeProgress * edgeProgress *
					(3.0f - 2.0f * edgeProgress);
			return rangeWindow / std::max(
				pointLight.m_attenuation.x +
					pointLight.m_attenuation.y * distance +
					pointLight.m_attenuation.z * distance * distance,
				0.00001f);
		};

		const glm::vec3 innerRadiance = sampleRay(1.0f, 40.0f, 11u);
		const glm::vec3 expectedInner =
			pointLight.m_intensity * expectedAttenuation(1.0f);
		Require(glm::length(innerRadiance - expectedInner) <= 0.0001f,
			"a ray crossing a Point Light range must use the realtime shader attenuation");

		const glm::vec3 edgeRadiance = sampleRay(9.5f, 40.0f, 12u);
		const glm::vec3 expectedEdge =
			pointLight.m_intensity * expectedAttenuation(9.5f);
		Require(glm::length(edgeRadiance - expectedEdge) <= 0.0001f,
			"the CPU baker must match the shader's smooth 90-100 percent range window");
		Require(glm::length(sampleRay(10.0f, 40.0f, 13u)) <= 0.000001f,
			"Point Light radiance must reach zero at the authored radius");
		Require(glm::length(sampleRay(1.0f, 5.0f, 14u)) <= 0.000001f,
			"a Point Light behind the visible ray segment must not leak into the bake");
		Require(glm::length(sampleRay(-6.0f, 40.0f, 15u)) <= 0.000001f,
			"geometry between a ray intersection and the Point Light must cast a bake shadow");

		Raytracing::PathTracer::PreparedRaySample awaySample;
		Require(pathTracer.SamplePreparedSceneRay(
				glm::vec3(11.0f, 5.0f, 0.0f),
				glm::vec3(-1.0f, 0.0f, 0.0f),
				40.0f,
				params,
				16u,
				awaySample),
			"the CPU baker must accept a ray starting inside the Point Light range");
		Require(!awaySample.m_bHit &&
			glm::length(awaySample.m_radiance) <= 0.000001f,
			"a ray pointing away from a Point Light must not create isotropic bake radiance");

		Raytracing::PathTracer::Params disabledParams = params;
		disabledParams.m_maxBounces = 1u;
		disabledParams.m_bIncludePointLightRayIntersections = false;
		Raytracing::PathTracer::Params enabledParams = disabledParams;
		enabledParams.m_bIncludePointLightRayIntersections = true;
		float strongestSecondaryContribution = 0.0f;
		float strongestDisabledContribution = 0.0f;
		for (uint32_t seed = 1u; seed <= 256u; ++seed)
		{
			Raytracing::PathTracer::PreparedRaySample disabledSample;
			Raytracing::PathTracer::PreparedRaySample enabledSample;
			Require(pathTracer.SamplePreparedSceneRay(
					glm::vec3(0.0f, 5.0f, 0.0f),
					glm::vec3(0.0f, -1.0f, 0.0f),
					10.0f,
					disabledParams,
					seed,
					disabledSample) &&
				pathTracer.SamplePreparedSceneRay(
					glm::vec3(0.0f, 5.0f, 0.0f),
					glm::vec3(0.0f, -1.0f, 0.0f),
					10.0f,
					enabledParams,
					seed,
					enabledSample),
				"the secondary Point Light fixture must sample the floor");
			Require(disabledSample.m_bHit && enabledSample.m_bHit,
				"the secondary Point Light fixture must start at the floor");
			strongestDisabledContribution = std::max(
				strongestDisabledContribution,
				glm::length(disabledSample.m_radiance));
			strongestSecondaryContribution = std::max(
				strongestSecondaryContribution,
				glm::length(enabledSample.m_radiance));
		}
		Require(strongestDisabledContribution <= 0.000001f,
			"the fixture must not receive direct or environment lighting");
		Require(strongestSecondaryContribution > 0.01f,
			"a secondary CPU bake ray crossing a Point Light range must carry its radiance");
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
			"BakeControllerRejectsInvalidThreadCountBeforeSceneCapture",
			TestBakeControllerRejectsInvalidThreadCountBeforeSceneCapture);
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
		RunTest(
			"MobilityAndLightModeContributionPolicy",
			TestMobilityAndLightModeContributionPolicy);
		RunTest(
			"PointLightModeChangesBakedRadiance",
			TestPointLightModeChangesBakedRadiance);
		RunTest(
			"PointLightRayIntersectionsMatchRealtimeAttenuation",
			TestPointLightRayIntersectionsMatchRealtimeAttenuation);
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
