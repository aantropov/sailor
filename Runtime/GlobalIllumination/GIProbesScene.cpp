#include "GlobalIllumination/GIProbesScene.h"

#include "AssetRegistry/Material/MaterialImporter.h"
#include "AssetRegistry/FrameGraph/FrameGraphImporter.h"
#include "Components/MeshRendererComponent.h"
#include "Components/SkyComponent.h"
#include "Containers/Hash.h"
#include "ECS/LandscapeECS.h"
#include "ECS/LightingECS.h"
#include "ECS/StaticMeshRendererECS.h"
#include "ECS/TransformECS.h"
#include "Engine/GameObject.h"
#include "Engine/World.h"
#include "GlobalIllumination/GISettings.h"
#include "Math/Math.h"
#include "Raytracing/SkyEnvironmentGenerator.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>

using namespace Sailor;

namespace
{
	struct MeshCandidate final
	{
		std::string m_instanceId{};
		GameObjectPtr m_gameObject{};
		MeshRendererComponentPtr m_renderer{};
	};

	struct FrozenModelGeometry final
	{
		TSharedPtr<const Model::BLASGeometry> m_modelGeometry{};
		TSharedPtr<TVector<Math::Triangle>> m_triangles{};
		Math::AABB m_localBounds{};
		uint64_t m_contentHash = 0u;
	};

	struct ObservedSky final
	{
		SkyParameters m_parameters{};
		float m_indirectIntensity = 1.0f;
		uint32_t m_componentCount = 0u;
		std::string m_selectedName{};
	};

	void HashVec2(uint64_t& hash, const glm::vec2& value) noexcept
	{
		HashValue(hash, value.x);
		HashValue(hash, value.y);
	}

	void HashVec3(uint64_t& hash, const glm::vec3& value) noexcept
	{
		HashValue(hash, value.x);
		HashValue(hash, value.y);
		HashValue(hash, value.z);
	}

	void HashVec4(uint64_t& hash, const glm::vec4& value) noexcept
	{
		HashValue(hash, value.x);
		HashValue(hash, value.y);
		HashValue(hash, value.z);
		HashValue(hash, value.w);
	}

	void HashMatrix(uint64_t& hash, const glm::mat4& matrix) noexcept
	{
		for (glm::length_t column = 0; column < matrix.length(); ++column)
		{
			for (glm::length_t row = 0; row < matrix[column].length(); ++row)
			{
				HashValue(hash, matrix[column][row]);
			}
		}
	}

	void HashBounds(uint64_t& hash, const Math::AABB& bounds) noexcept
	{
		HashVec3(hash, bounds.m_min);
		HashVec3(hash, bounds.m_max);
	}

	void HashTriangles(
		uint64_t& hash,
		const TSharedPtr<TVector<Math::Triangle>>& triangles) noexcept
	{
		const size_t triangleCount = triangles ? triangles->Num() : 0u;
		HashValue(hash, triangleCount);
		if (!triangles)
		{
			return;
		}
		for (const Math::Triangle& triangle : *triangles)
		{
			for (size_t vertex = 0u; vertex < 3u; ++vertex)
			{
				HashVec3(hash, triangle.m_vertices[vertex]);
				HashVec3(hash, triangle.m_normals[vertex]);
				HashVec3(hash, triangle.m_tangent[vertex]);
				HashVec3(hash, triangle.m_bitangent[vertex]);
				HashVec2(hash, triangle.m_uvs[vertex]);
				HashVec2(hash, triangle.m_uvs2[vertex]);
				HashVec4(hash, triangle.m_colors[vertex]);
			}
			HashValue(hash, triangle.m_materialIndex);
		}
	}

	void HashMaterials(
		uint64_t& hash,
		const Raytracing::PathTracer::MaterialSnapshots& materials,
		bool bSurfaceOnly = false) noexcept
	{
		for (const auto& material : materials)
		{
			HashString(
				hash,
				material ? material->m_fileId.ToString() : std::string());
			const uint64_t revision = material ?
				(bSurfaceOnly ? material->m_surfaceRevision : material->m_contentRevision) : 0u;
			HashValue(hash, revision);
		}
	}

	void HashGeometrySettings(
		uint64_t& hash,
		const GIProbesBakeSettings& settings) noexcept
	{
		HashValue(hash, settings.m_minProbeSpacing);
		HashValue(hash, settings.m_normalBias);
		HashValue(hash, settings.m_viewBias);
		HashValue(hash, settings.m_maxRayDistance);
	}

	void HashLightingSettings(
		uint64_t& hash,
		const GIProbesBakeSettings& settings) noexcept
	{
		HashValue(hash, settings.m_bounceCount);
		HashValue(hash, settings.m_bIncludeSky);
		HashValue(hash, settings.m_bIncludeEmissive);
		HashValue(hash, settings.m_bIncludeDirectLighting);
	}

	void InitializeSceneHashes(
		const GIProbesSceneCaptureRequest& request,
		const std::string& worldName,
		uint64_t& outGeometryHash,
		uint64_t& outLightingHash) noexcept
	{
		outGeometryHash = Fnv1aOffsetBasis;
		outLightingHash = Fnv1aOffsetBasis;
		HashString(outGeometryHash, request.m_sourceIdentity);
		HashString(outGeometryHash, worldName);
		HashString(outLightingHash, request.m_sourceIdentity);
		HashString(outLightingHash, worldName);
		HashGeometrySettings(outGeometryHash, request.m_settings);
		HashLightingSettings(outLightingHash, request.m_settings);
	}

	void HashLightProxies(
		uint64_t& hash,
		const TVector<Raytracing::LightProxy>& lights) noexcept
	{
		for (const Raytracing::LightProxy& light : lights)
		{
			HashValue(hash, static_cast<uint32_t>(light.m_type));
			HashVec3(hash, light.m_worldPosition);
			HashVec3(hash, light.m_direction);
			HashVec3(hash, light.m_intensity);
			HashValue(hash, light.m_indirectLightingIntensity);
			HashVec3(hash, light.m_bounds);
			HashVec2(hash, light.m_cutOff);
			HashValue(hash, light.m_bCastShadows);
		}
	}

	ObservedSky ResolveObservedSky(World* world)
	{
		ObservedSky result;
		if (auto* lighting = world->GetECS<LightingECS>())
		{
			if (const auto sky = lighting->GetSky())
			{
				result.m_componentCount = static_cast<uint32_t>(lighting->GetNumSkies());
				result.m_parameters = sky->GetSkyParameters();
				result.m_indirectIntensity = sky->GetGiIndirectIntensity();
				result.m_selectedName = sky->GetOwner()->GetName();
			}
		}
		return result;
	}

	void ReportWarning(
		const GIProbesSceneWarningCallback& warning,
		std::string diagnostic)
	{
		if (warning && !diagnostic.empty())
		{
			warning(diagnostic);
		}
	}

	bool CaptureSceneEnvironment(World* world, const GIProbesSceneCaptureRequest& request,
		EnvironmentSource& outSource, std::string& outDiagnostic,
		const GIProbesSceneWarningCallback& warning = {})
	{
		std::string environmentMap;
		ObservedSky sky;
		if (request.m_settings.m_bIncludeSky)
		{
			if (auto* importer = App::GetSubmodule<FrameGraphImporter>())
				if (!importer->GetEnvironmentMap(environmentMap, outDiagnostic)) return false;
			if (environmentMap.empty()) sky = ResolveObservedSky(world);
		}
		if (!CaptureEnvironmentSource(environmentMap,
			sky.m_componentCount ? &sky.m_parameters : nullptr, outSource, outDiagnostic)) return false;
		outSource.m_constant = glm::max(request.m_fallbackEnvironment, glm::vec3(0.0f));
		outSource.m_skyIndirectIntensity = sky.m_indirectIntensity;
		if (sky.m_componentCount > 1u)
			ReportWarning(warning, "multiple SkyComponents are present; GI tracing uses '" + sky.m_selectedName + "'");
		return true;
	}

	bool ResolveFrozenModelGeometry(
		ModelPtr model,
		int32_t meshIndex,
		const std::string& sourceName,
		TMap<std::string, FrozenModelGeometry>& cache,
		FrozenModelGeometry& outGeometry,
		std::string& outDiagnostic)
	{
		if (!model || !model->IsStructurallyReady())
		{
			outDiagnostic = sourceName + " is not ready for GI tracing";
			return false;
		}

		std::string cacheKey = model->GetFileId().ToString();
		if (cacheKey.empty())
		{
			cacheKey = "runtime:" + std::to_string(
				reinterpret_cast<uintptr_t>(model.GetRawPtr()));
		}
		cacheKey += ":" + std::to_string(meshIndex);
		FrozenModelGeometry* cached = nullptr;
		if (cache.Find(cacheKey, cached) && cached)
		{
			outGeometry = *cached;
			return true;
		}

		if (!model->HasBLAS(meshIndex) &&
			(!model->HasCpuMeshes() || !model->BuildBLAS()))
		{
			outDiagnostic = sourceName +
				" has no CPU raytracing geometry; enable model BLAS generation";
			return false;
		}
		if (!model->HasBLAS(meshIndex))
		{
			outDiagnostic = sourceName +
				" has an empty raytracing acceleration structure";
			return false;
		}

		outGeometry.m_modelGeometry = model->GetBLASGeometry();
		outGeometry.m_localBounds = model->GetBoundsAABB(meshIndex);
		if (!outGeometry.m_localBounds.IsValid())
		{
			outDiagnostic = sourceName + " has invalid local bounds";
			return false;
		}
		outGeometry.m_contentHash = Fnv1aOffsetBasis;
		TMap<const Model::BLASData*, uint64_t> geometryHashes;
		for (const auto& instance : outGeometry.m_modelGeometry->GetInstances(meshIndex))
		{
			const auto* data = instance.m_geometry.GetRawPtr();
			uint64_t* hash = nullptr;
			if (!geometryHashes.Find(data, hash))
			{
				uint64_t value = Fnv1aOffsetBasis;
				HashTriangles(value, data->m_triangles);
				hash = &geometryHashes[data];
				*hash = value;
			}
			HashValue(outGeometry.m_contentHash, *hash);
			HashMatrix(outGeometry.m_contentHash, instance.m_modelMatrix);
		}
		cache[cacheKey] = outGeometry;
		return true;
	}

	bool AppendInstanceMaterials(
		const TVector<MaterialPtr>& sourceMaterials,
		TVector<MaterialPtr>& materials,
		Raytracing::PathTracer::TLASInstance& instance,
		std::string& outDiagnostic)
	{
		uint32_t requiredMaterialSlots = 1u;
		if (instance.m_modelGeometry)
		{
			for (const auto& mesh : instance.m_modelGeometry->GetInstances(instance.m_meshIndex))
				requiredMaterialSlots = (std::max)(requiredMaterialSlots, mesh.m_geometry->m_materialSlots);
		}
		else if (instance.m_triangles)
		{
			for (const Math::Triangle& triangle : *instance.m_triangles)
			{
				requiredMaterialSlots = (std::max)(
					requiredMaterialSlots,
					static_cast<uint32_t>(triangle.m_materialIndex) + 1u);
			}
		}
		const size_t materialCount = materials.Num();
		const size_t maximumMaterialIndex = static_cast<size_t>(
			(std::numeric_limits<int32_t>::max)());
		if (materialCount > maximumMaterialIndex ||
			requiredMaterialSlots > maximumMaterialIndex - materialCount)
		{
			outDiagnostic =
				"the GI scene exceeds the CPU path tracer material-index limit";
			return false;
		}
		instance.m_materialBaseOffset = static_cast<int32_t>(materialCount);
		TVector<MaterialPtr> resolvedMaterials;
		resolvedMaterials.Reserve(requiredMaterialSlots);
		for (uint32_t materialIndex = 0u;
			materialIndex < requiredMaterialSlots;
			++materialIndex)
		{
			MaterialPtr material = materialIndex < sourceMaterials.Num() ?
				sourceMaterials[materialIndex] :
				(sourceMaterials.IsEmpty() ?
					MaterialPtr{} : *sourceMaterials.Last());
			if (material && !material->IsReady())
			{
				const std::string fileId = material->GetFileId().ToString();
				outDiagnostic =
					"material slot " + std::to_string(materialIndex) +
					(fileId.empty() ? std::string() : " ('" + fileId + "')") +
					" is not ready for GI tracing";
				return false;
			}
			resolvedMaterials.Add(material);
		}
		for (const MaterialPtr& material : resolvedMaterials)
		{
			materials.Add(material);
		}
		return true;
	}

	GIProbesSceneRevision ObserveSceneRevision(
		World* world,
		const GIProbesSceneCaptureRequest& request,
		const EnvironmentSource& environment)
	{
		uint64_t geometry = 0u;
		uint64_t lighting = 0u;
		InitializeSceneHashes(
			request,
			world->GetName(),
			geometry,
			lighting);
		if (const auto* meshes = world->GetECS<StaticMeshRendererECS>())
		{
			HashValue(geometry, meshes->GetGlobalIlluminationGeometryRevision());
			HashValue(lighting, meshes->GetGlobalIlluminationContributorRevision());
		}
		if (const auto* landscape = world->GetECS<LandscapeECS>())
		{
			HashValue(geometry, landscape->GetGlobalIlluminationGeometryRevision());
			HashValue(lighting, landscape->GetGlobalIlluminationContributorRevision());
		}

		TVector<Raytracing::LightProxy> lights;
		glm::vec3 sunDirection{};
		if (const auto* lightingEcs = world->GetECS<LightingECS>())
		{
			const auto sky = lightingEcs->GetSky();
			const LightComponent* sun = sky ? sky->GetDirectionalLight().GetRawPtr() : nullptr;
			const bool bUsesSun = sun && sun->GetLightType() == ELightType::Directional &&
				IsGlobalIlluminationBakeContributor(sun->GetOwner()->GetMobilityType()) &&
				ContributesToBakedGlobalIllumination(sun->GetGlobalIlluminationMode());
			lightingEcs->GetGlobalIlluminationBakeLightProxies(lights, bUsesSun ? &sun->GetData() : nullptr);
			HashValue(lighting, bUsesSun);
			if (bUsesSun)
			{
				// SkyComponent derives this light's pose and ground-level lux from the sun angle.
				// Hash its authored inputs; compare that angle separately against the captured state.
				sunDirection = glm::vec3(sky->GetSkyParameters().m_lightDirection);
				HashVec3(lighting, sky->GetSunIlluminance());
				HashValue(lighting, sun->GetIndirectLightingIntensity());
				HashValue(lighting, sun->GetShadowType() != RHI::EShadowType::None);
			}
			if (environment.m_type == EEnvironmentSource::Sky)
			{
				sunDirection = glm::vec3(environment.m_sky.m_lightDirection);
				HashValue(lighting, environment.m_type);
				HashVec3(lighting, sky->GetSunIlluminance());
				HashVec3(lighting, sky->GetGroundAlbedo());
				HashValue(lighting, sky->GetGiIndirectIntensity());
			}
		}
		HashLightProxies(lighting, lights);
		if (environment.m_type != EEnvironmentSource::Sky)
		{
			HashValue(lighting, environment.GetRevision());
		}
		return { geometry, lighting, sunDirection };
	}
}

bool GIProbesSceneRevision::HasChanges(const GIProbesSceneRevision& previous,
	float sunAngleThresholdDegrees) const noexcept
{
	if (m_geometry != previous.m_geometry || m_lighting != previous.m_lighting)
	{
		return true;
	}
	if (m_sunDirection == previous.m_sunDirection)
	{
		return false;
	}
	const float angle = std::atan2(glm::length(glm::cross(m_sunDirection, previous.m_sunDirection)),
		glm::dot(m_sunDirection, previous.m_sunDirection));
	return angle >= glm::radians(sunAngleThresholdDegrees);
}

bool Sailor::ObserveGIProbesSceneRevision(World* world,
	const GIProbesSceneCaptureRequest& request, GIProbesSceneRevision& outRevision,
	std::string& outDiagnostic)
{
	SAILOR_PROFILE_FUNCTION();
	outRevision = {};
	if (!world)
	{
		outDiagnostic = "a loaded world is required to observe GI contributors";
		return false;
	}
	EnvironmentSource environment;
	if (!CaptureSceneEnvironment(world, request, environment, outDiagnostic)) return false;
	outRevision = ObserveSceneRevision(world, request, environment);
	outDiagnostic = "observed Static and Stationary GI contributor revisions";
	return true;
}

bool GIProbesSceneMaterialWatch::HasUnchangedMaterials() const noexcept
{
	for (const auto& watched : m_materials)
	{
		if (watched.m_first->GetContentRevision() != (*watched.m_second)->m_contentRevision)
		{
			return false;
		}
	}
	return true;
}

bool GIProbesSceneMaterialWatch::HasUnchangedSurfaces() const noexcept
{
	for (const auto& watched : m_materials)
	{
		if (watched.m_first->GetSurfaceRevision() != (*watched.m_second)->m_surfaceRevision)
		{
			return false;
		}
	}
	return true;
}

bool Sailor::CaptureGIProbesSceneLighting(
	World* world,
	const GIProbesSceneCaptureRequest& request,
	GIProbesSceneSnapshot& outScene,
	std::string& outDiagnostic,
	const GIProbesSceneWarningCallback& warning)
{
	if (!world || !Math::AllFinite(request.m_fallbackEnvironment))
	{
		outDiagnostic = "GI lighting capture requires a world and a finite fallback environment";
		return false;
	}
	if (!CaptureSceneEnvironment(world, request, outScene.m_environment, outDiagnostic, warning)) return false;
	outScene.m_environmentPixels = {};
	outScene.m_lights.Clear();

	if (outScene.m_environment.m_type == EEnvironmentSource::Texture)
	{
		auto* textures = App::GetSubmodule<TextureImporter>();
		const auto texture = textures->GetLoadedTexture(outScene.m_environment.m_texture.m_fileId);
		if (texture && texture->HasCpuData())
		{
			auto capture = textures->CaptureCpuTextures({ texture });
			capture->Wait();
			const auto& pixels = capture->GetResult()[0];
			if (pixels.m_source == outScene.m_environment.m_texture)
				outScene.m_environmentPixels = pixels;
		}
	}

	uint64_t lightingHash = Fnv1aOffsetBasis;
	HashString(lightingHash, request.m_sourceIdentity);
	HashString(lightingHash, world->GetName());
	HashLightingSettings(lightingHash, request.m_settings);

	if (auto* lighting = world->GetECS<LightingECS>())
	{
		lighting->GetGlobalIlluminationBakeLightProxies(outScene.m_lights);
	}
	HashLightProxies(lightingHash, outScene.m_lights);
	HashValue(lightingHash, outScene.m_environment.GetRevision());
	if (outScene.m_environment.m_type == EEnvironmentSource::Sky)
	{
		constexpr uint32_t SkyEnvironmentGeneratorVersion = 1u;
		HashValue(lightingHash, SkyEnvironmentGeneratorVersion);
		HashValue(lightingHash, Raytracing::ProbeBakeSkyEnvironmentWidth);
		HashValue(lightingHash, Raytracing::ProbeBakeSkyEnvironmentHeight);
	}

	HashMaterials(lightingHash, outScene.m_materials);
	outScene.m_lightingHash = lightingHash;
	outScene.m_sourceWorldHash = Fnv1aOffsetBasis;
	HashValue(outScene.m_sourceWorldHash, outScene.m_geometryHash);
	HashValue(outScene.m_sourceWorldHash, lightingHash);
	outScene.m_observedRevision = ObserveSceneRevision(world, request, outScene.m_environment);
	outDiagnostic = "captured GI lighting and its environment source revision";
	return true;
}

bool Sailor::CaptureGIProbesScene(
	World* world,
	const GIProbesSceneCaptureRequest& request,
	GIProbesSceneSnapshot& outScene,
	std::string& outDiagnostic,
	const GIProbesSceneWarningCallback& warning,
	GIProbesSceneMaterialWatch* materialWatch)
{
	SAILOR_PROFILE_FUNCTION();
	outScene = {};
	if (materialWatch)
	{
		*materialWatch = {};
	}
	outDiagnostic.clear();
	if (!world)
	{
		outDiagnostic = "a loaded world is required for GI scene capture";
		return false;
	}
	if (!Math::AllFinite(request.m_fallbackEnvironment))
	{
		outDiagnostic =
			"the GI fallback environment must contain finite values";
		return false;
	}

	TVector<MaterialPtr> runtimeMaterials;
	TVector<MeshCandidate> candidates;
	for (const GameObjectPtr& gameObject : world->GetGameObjects())
	{
		if (!gameObject ||
			!IsGlobalIlluminationBakeContributor(
				gameObject->GetMobilityType()))
		{
			continue;
		}
		MeshRendererComponentPtr renderer =
			gameObject->GetComponent<MeshRendererComponent>();
		if (!renderer)
		{
			continue;
		}
		candidates.Add({
			gameObject->GetInstanceId().ToString(),
			gameObject,
			renderer
		});
	}
	std::sort(
		candidates.begin(),
		candidates.end(),
		[](const MeshCandidate& lhs, const MeshCandidate& rhs)
		{
			return lhs.m_instanceId < rhs.m_instanceId;
		});

	uint64_t geometryHash = Fnv1aOffsetBasis;
	HashString(geometryHash, request.m_sourceIdentity);
	HashString(geometryHash, world->GetName());
	HashGeometrySettings(geometryHash, request.m_settings);
	TMap<std::string, FrozenModelGeometry> frozenModelGeometry;
	for (MeshCandidate& candidate : candidates)
	{
		ModelPtr model = candidate.m_renderer->GetModel();
		const int32_t meshIndex = candidate.m_renderer->GetMeshIndex();
		const std::string sourceName =
			"static mesh '" + candidate.m_gameObject->GetName() + "'";
		FrozenModelGeometry geometry;
		if (!ResolveFrozenModelGeometry(
				model,
				meshIndex,
				sourceName,
				frozenModelGeometry,
				geometry,
				outDiagnostic))
		{
			ReportWarning(warning, "skipped " + outDiagnostic);
			outDiagnostic.clear();
			continue;
		}

		const glm::mat4 worldMatrix = candidate.m_gameObject
			->GetTransformComponent().GetCachedWorldMatrix();
		const float determinant = glm::determinant(glm::mat3(worldMatrix));
		if (!Math::AllFinite(worldMatrix) ||
			!std::isfinite(determinant) ||
			std::abs(determinant) <= 1e-8f)
		{
			ReportWarning(
				warning,
				"skipped " + sourceName +
					": the world transform is non-invertible");
			continue;
		}

		Raytracing::PathTracer::TLASInstance instance;
		instance.m_blas.Clear();
		instance.m_modelGeometry = geometry.m_modelGeometry;
		instance.m_triangles = geometry.m_triangles;
		instance.m_meshIndex = meshIndex;
		instance.m_worldMatrix = worldMatrix;
		instance.m_inverseWorldMatrix = glm::inverse(worldMatrix);
		instance.m_worldBounds = geometry.m_localBounds;
		instance.m_worldBounds.Apply(worldMatrix);
		instance.m_debugName = sourceName;
		if (!instance.m_worldBounds.IsValid())
		{
			ReportWarning(
				warning,
				"skipped " + sourceName + ": the world bounds are invalid");
			continue;
		}
		TVector<MaterialPtr>& materials =
			candidate.m_renderer->GetMaterials();
		if (!AppendInstanceMaterials(
				materials,
				runtimeMaterials,
				instance,
				outDiagnostic))
		{
			ReportWarning(
				warning,
				"skipped " + sourceName + ": " + outDiagnostic);
			outDiagnostic.clear();
			continue;
		}

		outScene.m_instances.Add(std::move(instance));
		outScene.m_geometryBounds.Add(
			outScene.m_instances.Last()->m_worldBounds);
		outScene.m_worldBounds.Extend(
			outScene.m_instances.Last()->m_worldBounds);
		HashString(geometryHash, candidate.m_instanceId);
		HashString(geometryHash, model->GetFileId().ToString());
		HashValue(geometryHash, meshIndex);
		HashValue(geometryHash, geometry.m_contentHash);
		HashMatrix(geometryHash, worldMatrix);
		HashBounds(geometryHash, geometry.m_localBounds);
	}

	TVector<LandscapeBakeGeometrySnapshot> landscapeSnapshots;
	if (auto* landscape = world->GetECS<LandscapeECS>(); landscape &&
		!landscape->CollectBakeGeometrySnapshots(
			landscapeSnapshots,
			outDiagnostic))
	{
		ReportWarning(
			warning,
			"skipped landscape GI geometry: " + outDiagnostic);
		landscapeSnapshots.Clear();
		outDiagnostic.clear();
	}
	std::sort(
		landscapeSnapshots.begin(),
		landscapeSnapshots.end(),
		[](const LandscapeBakeGeometrySnapshot& lhs,
			const LandscapeBakeGeometrySnapshot& rhs)
		{
			return lhs.m_sourceId < rhs.m_sourceId;
		});
	for (const LandscapeBakeGeometrySnapshot& snapshot : landscapeSnapshots)
	{
		const std::string sourceName = snapshot.m_model ?
			"vegetation '" + snapshot.m_sourceId + "'" :
			"GI geometry '" + snapshot.m_sourceId + "'";
		const float determinant = glm::determinant(
			glm::mat3(snapshot.m_worldMatrix));
		if (!Math::AllFinite(snapshot.m_worldMatrix) ||
			!std::isfinite(determinant) ||
			std::abs(determinant) <= 1e-8f ||
			!snapshot.m_worldBounds.IsValid())
		{
			ReportWarning(
				warning,
				"skipped " + sourceName +
					": the transform or bounds are invalid");
			continue;
		}

		FrozenModelGeometry geometry;
		if (snapshot.m_model)
		{
			if (!ResolveFrozenModelGeometry(
					snapshot.m_model,
					snapshot.m_meshIndex,
					sourceName,
					frozenModelGeometry,
					geometry,
					outDiagnostic))
			{
				ReportWarning(warning, "skipped " + outDiagnostic);
				outDiagnostic.clear();
				continue;
			}
		}
		else if (snapshot.m_triangles &&
			!snapshot.m_triangles->IsEmpty())
		{
			geometry.m_triangles = snapshot.m_triangles;
		}
		else
		{
			ReportWarning(
				warning,
				"skipped " + sourceName +
					": immutable CPU triangles are unavailable");
			continue;
		}

		Raytracing::PathTracer::TLASInstance instance;
		instance.m_blas.Clear();
		instance.m_modelGeometry = geometry.m_modelGeometry;
		instance.m_triangles = geometry.m_triangles;
		instance.m_meshIndex = snapshot.m_meshIndex;
		instance.m_worldMatrix = snapshot.m_worldMatrix;
		instance.m_inverseWorldMatrix = glm::inverse(snapshot.m_worldMatrix);
		instance.m_worldBounds = snapshot.m_worldBounds;
		instance.m_debugName = sourceName;
		if (!AppendInstanceMaterials(
				snapshot.m_materials,
				runtimeMaterials,
				instance,
				outDiagnostic))
		{
			ReportWarning(
				warning,
				"skipped " + sourceName + ": " + outDiagnostic);
			outDiagnostic.clear();
			continue;
		}
		outScene.m_instances.Add(std::move(instance));
		outScene.m_geometryBounds.Add(snapshot.m_worldBounds);
		outScene.m_worldBounds.Extend(snapshot.m_worldBounds);

		HashString(geometryHash, snapshot.m_sourceId);
		HashValue(geometryHash, snapshot.m_sourceRevision);
		HashString(
			geometryHash,
			snapshot.m_model ?
				snapshot.m_model->GetFileId().ToString() : std::string());
		HashValue(geometryHash, snapshot.m_meshIndex);
		if (snapshot.m_model)
		{
			HashValue(geometryHash, geometry.m_contentHash);
		}
		HashMatrix(geometryHash, snapshot.m_worldMatrix);
		HashBounds(geometryHash, snapshot.m_worldBounds);
		if (!snapshot.m_model)
		{
			HashTriangles(geometryHash, snapshot.m_triangles);
		}
	}

	if (outScene.m_instances.IsEmpty() || !outScene.m_worldBounds.IsValid())
	{
		outDiagnostic =
			"the current world has no valid Static or Stationary GI geometry";
		return false;
	}

	outScene.m_materials = Raytracing::PathTracer::CaptureMaterials(runtimeMaterials,
		materialWatch ? &materialWatch->m_materials : nullptr);
	HashMaterials(geometryHash, outScene.m_materials, true);
	if (materialWatch)
	{
		materialWatch->m_slots = std::move(runtimeMaterials);
	}
	outScene.m_geometryHash = geometryHash;
	if (!CaptureGIProbesSceneLighting(world, request, outScene, outDiagnostic, warning))
	{
		return false;
	}
	outDiagnostic = "captured immutable Static and Stationary GI contributors";
	return true;
}

bool Sailor::PrepareGIProbesScene(
	const GIProbesSceneSnapshot& scene,
	const GIProbesBakeSettings& settings,
	const std::atomic<bool>* cancel,
	GIProbesPreparedScene& outPreparedScene,
	std::string& outDiagnostic,
	const Raytracing::PathTracer::ScenePreparationProgressCallback& progress,
	const GIProbesSceneWarningCallback& warning,
	const GIProbesPreparedScene* previous)
{
	SAILOR_PROFILE_FUNCTION();
	outPreparedScene = {};
	outDiagnostic.clear();
	const auto isCancelled = [cancel]() noexcept
	{
		return cancel && cancel->load(std::memory_order_acquire);
	};
	if (isCancelled())
	{
		outDiagnostic = "GI scene preparation was cancelled";
		return false;
	}

	GIProbesBakeSettings effectiveSettings = settings;
	effectiveSettings.m_skyIndirectIntensity =
		scene.m_environment.m_type == EEnvironmentSource::Sky ? scene.m_environment.m_skyIndirectIntensity : 1.0f;
	auto sampler = TSharedPtr<Raytracing::GIProbesPathTracer>::Make();
	bool bCancelled = false;
	const auto guardedProgress =
		[&progress, &isCancelled, &bCancelled](
			const Raytracing::PathTracer::ScenePreparationProgress& state)
		{
			bCancelled = bCancelled || isCancelled() || (progress && !progress(state)) || isCancelled();
			return !bCancelled;
		};
	const bool bReuseGeometry = previous && previous->m_sampler &&
		previous->m_geometryHash == scene.m_geometryHash;
	const bool bInitialized = bReuseGeometry ?
		sampler->InitializeLighting(*previous->m_sampler, scene.m_materials, scene.m_lights,
			effectiveSettings, scene.m_environment.m_constant, guardedProgress) :
		sampler->InitializeSnapshot(scene.m_instances, scene.m_materials,
			scene.m_lights, effectiveSettings, scene.m_environment.m_constant,
			guardedProgress, warning);
	if (!bInitialized)
	{
		outDiagnostic = bCancelled || isCancelled() ?
			"GI scene preparation was cancelled while building the CPU path tracer" :
			"the CPU path tracer could not prepare any valid GI geometry";
		return false;
	}

	const auto continueEnvironment = [&]()
	{
		if (guardedProgress({ Raytracing::PathTracer::EScenePreparationStage::Materials,
			scene.m_materials.Num(), scene.m_materials.Num() })) return true;
		outDiagnostic = "GI scene preparation was cancelled while preparing the environment";
		return false;
	};
	const auto setEnvironment = [&](const TVector<glm::vec4>& image, const glm::uvec2& extent)
	{
		if (sampler->SetEnvironmentLinear(image, extent, continueEnvironment)) return true;
		if (!bCancelled) outDiagnostic = "the CPU path tracer could not prepare the captured environment";
		return false;
	};

	if (effectiveSettings.m_bIncludeSky && scene.m_environment.m_type == EEnvironmentSource::Texture)
	{
		if (!continueEnvironment()) return false;
		const auto& source = scene.m_environment;
		const auto& captured = scene.m_environmentPixels;
		TextureImporter::ByteCode decoded;
		const auto* pixels = captured.m_pixels.GetRawPtr();
		int32_t width = captured.m_width, height = captured.m_height;
		uint32_t mipLevels = 1u;
		if (!pixels)
		{
			const bool bDecoded = TextureImporter::DecodeTextureCpu(source.m_texture, decoded, width, height, mipLevels);
			if (!continueEnvironment()) return false;
			if (!bDecoded)
			{
				outDiagnostic = "cannot decode the captured environment texture '" + source.m_texture.m_filepath + "'";
				return false;
			}
			pixels = &decoded;
		}
		TVector<glm::vec4> environment;
		environment.Resize(static_cast<size_t>(width) * height);
		for (size_t i = 0u; i < environment.Num(); ++i)
		{
			if (i % 1024u == 0u && !continueEnvironment()) return false;
			if (source.m_texture.m_bDecodeAsFloat)
				std::memcpy(&environment[i], pixels->GetData() + i * sizeof(glm::vec4), sizeof(glm::vec4));
			else
			{
				const uint8_t* pixel = pixels->GetData() + i * 4u;
				environment[i] = glm::vec4(pixel[0], pixel[1], pixel[2], pixel[3]) / 255.0f;
				if (RHI::IsSrgbFormat(source.m_format))
					environment[i] = Utils::SRGBToLinear(environment[i]);
			}
		}
		if (!setEnvironment(environment, glm::uvec2(width, height))) return false;
	}
	else if (effectiveSettings.m_bIncludeSky && scene.m_environment.m_type == EEnvironmentSource::Sky)
	{
		TVector<glm::vec4> transientSkyEnvironment;
		const glm::uvec2 environmentExtent(
			Raytracing::ProbeBakeSkyEnvironmentWidth,
			Raytracing::ProbeBakeSkyEnvironmentHeight);
		const bool bGenerated =
			Raytracing::GenerateSkyEnvironmentEquirectangular(
				scene.m_environment.m_sky,
				environmentExtent,
				transientSkyEnvironment,
				[&continueEnvironment](uint32_t, uint32_t)
				{
					return continueEnvironment();
				});
		if (!bGenerated)
		{
			if (!bCancelled) outDiagnostic = "the transient SkyComponent environment could not be generated";
			return false;
		}
		for (size_t index = 0u; index < transientSkyEnvironment.Num(); ++index)
		{
			if (index % 1024u == 0u && !continueEnvironment()) return false;
			auto& pixel = transientSkyEnvironment[index];
			pixel = glm::vec4(
				glm::max(glm::vec3(pixel), glm::vec3(0.0f)) *
					effectiveSettings.m_skyIndirectIntensity,
				pixel.a);
		}
		if (!setEnvironment(transientSkyEnvironment, environmentExtent)) return false;
	}
	if (isCancelled())
	{
		outDiagnostic = "GI scene preparation was cancelled";
		return false;
	}

	outPreparedScene.m_sampler = std::move(sampler);
	outPreparedScene.m_effectiveSettings = effectiveSettings;
	outPreparedScene.m_worldBounds = scene.m_worldBounds;
	outPreparedScene.m_geometryHash = scene.m_geometryHash;
	outPreparedScene.m_lightingHash = scene.m_lightingHash;
	outPreparedScene.m_observedRevision = scene.m_observedRevision;
	outDiagnostic = "prepared immutable CPU GI ray-tracing scene";
	return true;
}
