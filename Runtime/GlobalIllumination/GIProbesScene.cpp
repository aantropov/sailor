#include "GlobalIllumination/GIProbesScene.h"

#include "AssetRegistry/Material/MaterialImporter.h"
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
		const TVector<MaterialPtr>& materials) noexcept
	{
		for (const MaterialPtr& material : materials)
		{
			HashString(
				hash,
				material ? material->GetFileId().ToString() : std::string());
			const uint64_t revision = material ?
				material->GetContentRevision() : 0u;
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

	void HashSkyParameters(
		uint64_t& hash,
		const SkyParameters& sky) noexcept
	{
		HashVec4(hash, sky.m_lightDirection);
		HashVec4(hash, sky.m_sunIlluminance);
		HashVec4(hash, sky.m_groundRadiance);
		HashValue(hash, sky.m_cloudsAttenuation1);
		HashValue(hash, sky.m_cloudsAttenuation2);
		HashValue(hash, sky.m_cloudsDensity);
		HashValue(hash, sky.m_cloudsCoverage);
		HashValue(hash, sky.m_phaseInfluence1);
		HashValue(hash, sky.m_phaseInfluence2);
		HashValue(hash, sky.m_eccentrisy1);
		HashValue(hash, sky.m_eccentrisy2);
		HashValue(hash, sky.m_fog);
		HashValue(hash, sky.m_cloudScatteringScale);
		HashValue(hash, sky.m_ambient);
		HashValue(hash, sky.m_scatteringSteps);
		HashValue(hash, sky.m_scatteringDensity);
		HashValue(hash, sky.m_scatteringIntensity);
		HashValue(hash, sky.m_scatteringPhase);
		HashValue(hash, sky.m_sunShaftsIntensity);
		HashValue(hash, sky.m_sunShaftsDistance);
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
		std::string selectedInstanceId;
		for (const GameObjectPtr& gameObject : world->GetGameObjects())
		{
			if (!gameObject)
			{
				continue;
			}
			const auto sky = gameObject->GetComponent<SkyComponent>();
			if (!sky)
			{
				continue;
			}
			++result.m_componentCount;
			const std::string instanceId =
				gameObject->GetInstanceId().ToString();
			if (result.m_componentCount == 1u ||
				instanceId < selectedInstanceId)
			{
				result.m_parameters = sky->GetSkyParameters();
				result.m_indirectIntensity = sky->GetGiIndirectIntensity();
				result.m_selectedName = gameObject->GetName();
				selectedInstanceId = instanceId;
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
		const auto& sourceTriangles = model->GetBLASTriangles(meshIndex);
		if (!model->HasBLAS(meshIndex) || sourceTriangles.IsEmpty())
		{
			outDiagnostic = sourceName +
				" has an empty raytracing acceleration structure";
			return false;
		}

		outGeometry.m_triangles =
			TSharedPtr<TVector<Math::Triangle>>::Make(sourceTriangles);
		outGeometry.m_localBounds = model->GetBoundsAABB(meshIndex);
		if (!outGeometry.m_localBounds.IsValid())
		{
			outDiagnostic = sourceName + " has invalid local bounds";
			return false;
		}
		outGeometry.m_contentHash = Fnv1aOffsetBasis;
		HashTriangles(outGeometry.m_contentHash, outGeometry.m_triangles);
		cache[cacheKey] = outGeometry;
		return true;
	}

	bool AppendInstanceMaterials(
		const TSharedPtr<TVector<Math::Triangle>>& triangles,
		const TVector<MaterialPtr>& sourceMaterials,
		GIProbesSceneSnapshot& scene,
		Raytracing::PathTracer::TLASInstance& instance,
		std::string& outDiagnostic)
	{
		uint32_t requiredMaterialSlots = 1u;
		if (triangles)
		{
			for (const Math::Triangle& triangle : *triangles)
			{
				requiredMaterialSlots = (std::max)(
					requiredMaterialSlots,
					static_cast<uint32_t>(triangle.m_materialIndex) + 1u);
			}
		}
		const size_t materialCount = scene.m_materials.Num();
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
			scene.m_materials.Add(material);
		}
		return true;
	}
}

bool Sailor::ObserveGIProbesSceneRevision(
	World* world,
	const GIProbesSceneCaptureRequest& request,
	GIProbesSceneRevision& outRevision,
	std::string& outDiagnostic)
{
	outRevision = {};
	outDiagnostic.clear();
	if (!world)
	{
		outDiagnostic = "a loaded world is required to observe GI contributors";
		return false;
	}

	uint64_t geometry = 0u;
	uint64_t lighting = 0u;
	InitializeSceneHashes(
		request,
		world->GetName(),
		geometry,
		lighting);
	if (const auto* meshes = world->GetECS<StaticMeshRendererECS>())
	{
		HashValue(
			geometry,
			meshes->GetGlobalIlluminationContributorRevision());
	}
	if (const auto* landscape = world->GetECS<LandscapeECS>())
	{
		HashValue(
			geometry,
			landscape->GetGlobalIlluminationContributorRevision());
	}

	TVector<Raytracing::LightProxy> lights;
	if (const auto* lightingEcs = world->GetECS<LightingECS>())
	{
		lightingEcs->GetGlobalIlluminationBakeLightProxies(lights);
	}
	HashLightProxies(lighting, lights);
	HashVec3(lighting, glm::max(request.m_fallbackEnvironment, glm::vec3(0.0f)));
	if (request.m_settings.m_bIncludeSky)
	{
		const ObservedSky sky = ResolveObservedSky(world);
		const bool bHasSky = sky.m_componentCount > 0u;
		HashValue(lighting, bHasSky);
		if (bHasSky)
		{
			HashSkyParameters(lighting, sky.m_parameters);
			HashValue(lighting, sky.m_indirectIntensity);
		}
	}
	outRevision.m_geometry = geometry;
	outRevision.m_lighting = lighting;
	outDiagnostic = "observed Static and Stationary GI contributor revisions";
	return true;
}

bool GIProbesSceneSnapshot::HasUnchangedMaterials() const noexcept
{
	if (m_materials.Num() != m_materialRevisions.Num())
	{
		return false;
	}
	for (size_t index = 0u; index < m_materials.Num(); ++index)
	{
		const MaterialPtr& material = m_materials[index];
		const uint64_t revision = material ?
			material->GetContentRevision() : 0u;
		if (revision != m_materialRevisions[index])
		{
			return false;
		}
	}
	return true;
}

bool Sailor::CaptureGIProbesScene(
	World* world,
	const GIProbesSceneCaptureRequest& request,
	GIProbesSceneSnapshot& outScene,
	std::string& outDiagnostic,
	const GIProbesSceneWarningCallback& warning)
{
	outScene = {};
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
	outScene.m_fallbackEnvironment = glm::max(
		request.m_fallbackEnvironment,
		glm::vec3(0.0f));

	if (request.m_settings.m_bIncludeSky)
	{
		const ObservedSky sky = ResolveObservedSky(world);
		if (sky.m_componentCount > 0u)
		{
			outScene.m_skyParameters = sky.m_parameters;
			outScene.m_skyIndirectIntensity = sky.m_indirectIntensity;
			outScene.m_bHasSkyEnvironment = true;
		}
		if (sky.m_componentCount > 1u)
		{
			ReportWarning(
				warning,
				"multiple SkyComponents are present; GI tracing uses '" +
					sky.m_selectedName + "'");
		}
	}

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

	uint64_t geometryHash = 0u;
	uint64_t lightingHash = 0u;
	InitializeSceneHashes(
		request,
		world->GetName(),
		geometryHash,
		lightingHash);
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
				instance.m_triangles,
				materials,
				outScene,
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
		HashMaterials(geometryHash, materials);
		HashMaterials(lightingHash, materials);
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
		instance.m_triangles = geometry.m_triangles;
		instance.m_meshIndex = snapshot.m_meshIndex;
		instance.m_worldMatrix = snapshot.m_worldMatrix;
		instance.m_inverseWorldMatrix = glm::inverse(snapshot.m_worldMatrix);
		instance.m_worldBounds = snapshot.m_worldBounds;
		instance.m_debugName = sourceName;
		if (!AppendInstanceMaterials(
				instance.m_triangles,
				snapshot.m_materials,
				outScene,
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
		HashMaterials(geometryHash, snapshot.m_materials);
		HashMaterials(lightingHash, snapshot.m_materials);
	}

	if (outScene.m_instances.IsEmpty() || !outScene.m_worldBounds.IsValid())
	{
		outDiagnostic =
			"the current world has no valid Static or Stationary GI geometry";
		return false;
	}

	if (auto* lighting = world->GetECS<LightingECS>())
	{
		lighting->GetGlobalIlluminationBakeLightProxies(outScene.m_lights);
	}
	HashLightProxies(lightingHash, outScene.m_lights);
	HashVec3(lightingHash, outScene.m_fallbackEnvironment);
	HashValue(lightingHash, outScene.m_bHasSkyEnvironment);
	if (outScene.m_bHasSkyEnvironment)
	{
		constexpr uint32_t SkyEnvironmentGeneratorVersion = 1u;
		HashValue(lightingHash, SkyEnvironmentGeneratorVersion);
		HashValue(lightingHash, Raytracing::ProbeBakeSkyEnvironmentWidth);
		HashValue(lightingHash, Raytracing::ProbeBakeSkyEnvironmentHeight);
		HashSkyParameters(lightingHash, outScene.m_skyParameters);
		HashValue(lightingHash, outScene.m_skyIndirectIntensity);
	}

	outScene.m_materialRevisions.Reserve(outScene.m_materials.Num());
	for (const MaterialPtr& material : outScene.m_materials)
	{
		outScene.m_materialRevisions.Add(
			material ? material->GetContentRevision() : 0u);
	}
	outScene.m_geometryHash = geometryHash;
	outScene.m_lightingHash = lightingHash;
	outScene.m_sourceWorldHash = Fnv1aOffsetBasis;
	HashValue(outScene.m_sourceWorldHash, geometryHash);
	HashValue(outScene.m_sourceWorldHash, lightingHash);
	if (!ObserveGIProbesSceneRevision(
			world,
			request,
			outScene.m_observedRevision,
			outDiagnostic))
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
	const GIProbesSceneWarningCallback& warning)
{
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
	if (!scene.HasUnchangedMaterials())
	{
		outDiagnostic =
			"a GI material changed after the immutable scene snapshot was captured";
		return false;
	}

	GIProbesBakeSettings effectiveSettings = settings;
	effectiveSettings.m_skyIndirectIntensity =
		scene.m_bHasSkyEnvironment ? scene.m_skyIndirectIntensity : 1.0f;
	auto sampler = TSharedPtr<Raytracing::GIProbesPathTracer>::Make();
	const auto guardedProgress =
		[&progress, &isCancelled](
			const Raytracing::PathTracer::ScenePreparationProgress& state)
		{
			return !isCancelled() && (!progress || progress(state));
		};
	if (!sampler->Initialize(
			scene.m_instances,
			scene.m_materials,
			scene.m_lights,
			effectiveSettings,
			scene.m_fallbackEnvironment,
			guardedProgress,
			warning))
	{
		outDiagnostic = isCancelled() ?
			"GI scene preparation was cancelled while building the CPU path tracer" :
			"the CPU path tracer could not prepare any valid GI geometry";
		return false;
	}

	if (effectiveSettings.m_bIncludeSky &&
		scene.m_bHasSkyEnvironment)
	{
		TVector<glm::vec4> transientSkyEnvironment;
		const glm::uvec2 environmentExtent(
			Raytracing::ProbeBakeSkyEnvironmentWidth,
			Raytracing::ProbeBakeSkyEnvironmentHeight);
		const bool bGenerated =
			Raytracing::GenerateSkyEnvironmentEquirectangular(
				scene.m_skyParameters,
				environmentExtent,
				transientSkyEnvironment,
				[&isCancelled](uint32_t, uint32_t)
				{
					return !isCancelled();
				});
		if (!bGenerated)
		{
			outDiagnostic = isCancelled() ?
				"GI scene preparation was cancelled while generating the sky environment" :
				"the transient SkyComponent environment could not be generated";
			return false;
		}
		for (glm::vec4& pixel : transientSkyEnvironment)
		{
			pixel = glm::vec4(
				glm::max(glm::vec3(pixel), glm::vec3(0.0f)) *
					effectiveSettings.m_skyIndirectIntensity,
				pixel.a);
		}
		sampler->SetEnvironmentLinear(
			transientSkyEnvironment,
			environmentExtent);
	}
	if (isCancelled())
	{
		outDiagnostic = "GI scene preparation was cancelled";
		return false;
	}
	if (!scene.HasUnchangedMaterials())
	{
		outDiagnostic =
			"a GI material changed while the CPU sampling scene was prepared";
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
