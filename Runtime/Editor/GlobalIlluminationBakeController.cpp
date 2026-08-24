#include "Editor/GlobalIlluminationBakeController.h"

#include "AssetRegistry/AssetRegistry.h"
#include "AssetRegistry/GlobalIllumination/ProbeVolumeBinary.h"
#include "AssetRegistry/GlobalIllumination/ProbeVolumeBaker.h"
#include "AssetRegistry/GlobalIllumination/ProbeVolumeImporter.h"
#include "AssetRegistry/World/WorldPrefabImporter.h"
#include "AssetRegistry/World/WorldPrefabAssetInfo.h"
#include "AssetRegistry/Material/MaterialImporter.h"
#include "Components/MeshRendererComponent.h"
#include "ECS/LandscapeECS.h"
#include "ECS/LightingECS.h"
#include "ECS/TransformECS.h"
#include "Engine/GameObject.h"
#include "Engine/World.h"
#include "Math/Math.h"
#include "Raytracing/ProbeVolumePathTracer.h"
#include "Tasks/Scheduler.h"
#include "YamlExceptionBoundary.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <functional>
#include <limits>

using namespace Sailor;

namespace
{
	struct ProbeBakeScene final
	{
		TVector<Raytracing::PathTracer::TLASInstance> m_instances{};
		TVector<MaterialPtr> m_materials{};
		TVector<uint64_t> m_materialRevisions{};
		TVector<Raytracing::LightProxy> m_lights{};
		TVector<Math::AABB> m_geometryBounds{};
		ProbeVolumeDataPtr m_layoutSource{};
		std::filesystem::path m_outputPath{};
		glm::vec3 m_volumeMin{};
		glm::vec3 m_volumeMax{};
		uint64_t m_sourceWorldHash = 0u;
	};

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

	void UpdateStatus(
		const TSharedPtr<GlobalIlluminationBakeController::SharedState>& state,
		const std::function<void(EditorProbeVolumeBakeStatus&)>& update)
	{
		state->m_lock.Lock();
		update(state->m_status);
		state->m_lock.Unlock();
	}

	void Fail(
		const TSharedPtr<GlobalIlluminationBakeController::SharedState>& state,
		std::string diagnostic)
	{
		UpdateStatus(
			state,
			[&diagnostic](EditorProbeVolumeBakeStatus& status)
			{
				status.m_state = EEditorProbeVolumeBakeState::Failed;
				status.m_stage = "Failed";
				status.m_diagnostic = std::move(diagnostic);
			});
	}

	void Cancelled(
		const TSharedPtr<GlobalIlluminationBakeController::SharedState>& state,
		std::string diagnostic)
	{
		UpdateStatus(
			state,
			[&diagnostic](EditorProbeVolumeBakeStatus& status)
			{
				status.m_state = EEditorProbeVolumeBakeState::Cancelled;
				status.m_stage = "Cancelled";
				status.m_diagnostic = std::move(diagnostic);
			});
	}

	template<typename T>
	void HashValue(uint64_t& hash, const T& value)
	{
		const uint8_t* bytes = reinterpret_cast<const uint8_t*>(&value);
		for (size_t index = 0u; index < sizeof(T); ++index)
		{
			hash ^= bytes[index];
			hash *= 1099511628211ull;
		}
	}

	void HashString(uint64_t& hash, const std::string& value)
	{
		for (char character : value)
		{
			hash ^= static_cast<uint8_t>(character);
			hash *= 1099511628211ull;
		}
	}

	void HashVec2(uint64_t& hash, const glm::vec2& value)
	{
		HashValue(hash, value.x);
		HashValue(hash, value.y);
	}

	void HashVec3(uint64_t& hash, const glm::vec3& value)
	{
		HashValue(hash, value.x);
		HashValue(hash, value.y);
		HashValue(hash, value.z);
	}

	void HashVec4(uint64_t& hash, const glm::vec4& value)
	{
		HashValue(hash, value.x);
		HashValue(hash, value.y);
		HashValue(hash, value.z);
		HashValue(hash, value.w);
	}

	void HashMatrix(uint64_t& hash, const glm::mat4& matrix)
	{
		for (glm::length_t column = 0; column < matrix.length(); ++column)
		{
			for (glm::length_t row = 0; row < matrix[column].length(); ++row)
			{
				HashValue(hash, matrix[column][row]);
			}
		}
	}

	void HashBounds(uint64_t& hash, const Math::AABB& bounds)
	{
		HashVec3(hash, bounds.m_min);
		HashVec3(hash, bounds.m_max);
	}

	void HashTriangles(
		uint64_t& hash,
		const TSharedPtr<TVector<Math::Triangle>>& triangles)
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

	bool IsFiniteMatrix(const glm::mat4& matrix)
	{
		for (glm::length_t column = 0; column < matrix.length(); ++column)
		{
			for (glm::length_t row = 0; row < matrix[column].length(); ++row)
			{
				if (!std::isfinite(matrix[column][row]))
				{
					return false;
				}
			}
		}
		return true;
	}

	bool IsFinite(const glm::vec3& value)
	{
		return std::isfinite(value.x) &&
			std::isfinite(value.y) &&
			std::isfinite(value.z);
	}

	bool IsProbesPath(const std::string& value)
	{
		std::string extension =
			std::filesystem::path(value).extension().string();
		std::transform(
			extension.begin(),
			extension.end(),
			extension.begin(),
			[](unsigned char character)
			{
				return static_cast<char>(std::tolower(character));
			});
		return extension == ".probes";
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
			outDiagnostic = sourceName + " is not ready for baking";
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
		outGeometry.m_contentHash = 1469598103934665603ull;
		HashTriangles(outGeometry.m_contentHash, outGeometry.m_triangles);
		cache[cacheKey] = outGeometry;
		return true;
	}

	bool AppendInstanceMaterials(
		const TSharedPtr<TVector<Math::Triangle>>& triangles,
		const TVector<MaterialPtr>& sourceMaterials,
		ProbeBakeScene& scene,
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
				"the bake scene exceeds the CPU path tracer material-index limit";
			return false;
		}
		instance.m_materialBaseOffset = static_cast<int32_t>(materialCount);
		for (uint32_t materialIndex = 0u;
			materialIndex < requiredMaterialSlots;
			++materialIndex)
		{
			MaterialPtr material = materialIndex < sourceMaterials.Num() ?
				sourceMaterials[materialIndex] :
				(sourceMaterials.IsEmpty() ? MaterialPtr{} : *sourceMaterials.Last());
			if (material)
			{
				material->IsReady();
			}
			scene.m_materials.Add(material);
		}
		return true;
	}

	void HashMaterials(
		uint64_t& hash,
		const TVector<MaterialPtr>& materials)
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

	bool MaterialsMatchSnapshot(const ProbeBakeScene& scene) noexcept
	{
		if (scene.m_materials.Num() != scene.m_materialRevisions.Num())
		{
			return false;
		}
		for (size_t index = 0u; index < scene.m_materials.Num(); ++index)
		{
			const MaterialPtr& material = scene.m_materials[index];
			const uint64_t revision = material ?
				material->GetContentRevision() : 0u;
			if (revision != scene.m_materialRevisions[index])
			{
				return false;
			}
		}
		return true;
	}

	bool IsEditorOnlyPrefab(const YAML::Node& prefab)
	{
		const YAML::Node components = prefab["components"];
		if (!components || !components.IsSequence())
		{
			return false;
		}
		for (const YAML::Node& component : components)
		{
			const YAML::Node typeName = component["typename"];
			if (typeName &&
				typeName.IsScalar() &&
				typeName.Scalar() == "Sailor::EditorComponent")
			{
				return true;
			}
		}
		return false;
	}

	YAML::Node MakeProbeBakeComparableWorldDocument(
		const YAML::Node& source)
	{
		YAML::Node result = YAML::Clone(source);
		const YAML::Node prefabs = result["prefabs"];
		if (!prefabs || !prefabs.IsSequence())
		{
			return result;
		}

		YAML::Node filteredPrefabs(YAML::NodeType::Sequence);
		for (const YAML::Node& prefab : prefabs)
		{
			if (!IsEditorOnlyPrefab(prefab))
			{
				filteredPrefabs.push_back(YAML::Clone(prefab));
			}
		}
		result["prefabs"] = std::move(filteredPrefabs);
		return result;
	}

	bool IsSavedWorldSnapshot(
		World* world,
		WorldPrefabAssetInfoPtr worldAssetInfo,
		std::string& outDiagnostic)
	{
		auto* worldImporter = App::GetSubmodule<WorldPrefabImporter>();
		if (!world || !worldAssetInfo || !worldImporter)
		{
			outDiagnostic =
				"the current world cannot be matched to a saved .world asset";
			return false;
		}

		std::string savedText;
		if (!AssetRegistry::ReadTextFile(
				worldAssetInfo->GetAssetFilepath(),
				savedText))
		{
			outDiagnostic = "the selected .world asset cannot be read";
			return false;
		}

		YAML::Node savedDocument;
		std::string yamlDiagnostic;
		if (!External::TryLoadYaml(
				savedText,
				savedDocument,
				yamlDiagnostic))
		{
			outDiagnostic = "the selected .world asset is invalid: " +
				yamlDiagnostic;
			return false;
		}

		WorldPrefabPtr savedWorld = worldImporter->Create();
		if (!savedWorld ||
			!External::GuardYamlExceptions(
				[&savedWorld, &savedDocument]()
				{
					savedWorld->Deserialize(savedDocument);
				},
				yamlDiagnostic) ||
			!savedWorld->IsReady())
		{
			if (yamlDiagnostic.empty() && savedWorld)
			{
				yamlDiagnostic = savedWorld->GetLoadDiagnostic();
			}
			outDiagnostic =
				"the selected .world asset cannot be normalized: " +
				(yamlDiagnostic.empty()
					? std::string("unknown world format error")
					: yamlDiagnostic);
			return false;
		}

		WorldPrefabPtr currentWorld = WorldPrefab::FromWorld(world);
		if (!currentWorld || !currentWorld->IsReady())
		{
			outDiagnostic =
				"the current world cannot be serialized for probe-volume baking";
			if (currentWorld && !currentWorld->GetLoadDiagnostic().empty())
			{
				outDiagnostic += ": " + currentWorld->GetLoadDiagnostic();
			}
			return false;
		}

		if (!AreWorldDocumentsEquivalentForProbeBake(
				savedDocument,
				currentWorld->Serialize(),
				yamlDiagnostic))
		{
			outDiagnostic =
				yamlDiagnostic.empty()
					? "the current level has unsaved changes or does not match the selected .world asset; save it before baking"
					: yamlDiagnostic;
			return false;
		}
		return true;
	}

	bool GatherScene(
		World* world,
		const EditorProbeVolumeBakeRequest& request,
		ProbeBakeScene& scene,
		std::string& outDiagnostic)
	{
		auto* assetRegistry = App::GetSubmodule<AssetRegistry>();
		if (!world || !assetRegistry)
		{
			outDiagnostic = "a loaded Editor world and AssetRegistry are required";
			return false;
		}
		WorldPrefabAssetInfoPtr worldAssetInfo = request.m_worldAsset
			? assetRegistry->GetAssetInfoPtr<WorldPrefabAssetInfoPtr>(
				request.m_worldAsset)
			: nullptr;
		if (!worldAssetInfo)
		{
			outDiagnostic =
				"probe-volume baking requires the current world to be saved as a registered .world asset";
			return false;
		}
		if (!IsSavedWorldSnapshot(world, worldAssetInfo, outDiagnostic))
		{
			return false;
		}
		if (request.m_stateName.empty())
		{
			outDiagnostic = "probe-volume state name cannot be empty";
			return false;
		}
		if (!IsFinite(request.m_fallbackEnvironment))
		{
			outDiagnostic =
				"the probe-volume fallback environment must contain finite values";
			return false;
		}
		if (!IsProbesPath(request.m_outputVirtualPath) ||
			!assetRegistry->ResolveWorkspaceContentPathForWrite(
				request.m_outputVirtualPath,
				scene.m_outputPath))
		{
			outDiagnostic =
				"the bake output must be a safe workspace-relative .probes path";
			return false;
		}
		std::error_code fileError;
		if (!request.m_bOverwrite &&
			std::filesystem::exists(scene.m_outputPath, fileError) &&
			!fileError)
		{
			outDiagnostic = "the target .probes asset already exists";
			return false;
		}

		if (request.m_layoutSource)
		{
			auto* importer = App::GetSubmodule<ProbeVolumeImporter>();
			ProbeVolumeAssetPtr layoutAsset;
			if (!importer ||
				!importer->LoadProbeVolume_Immediate(
					request.m_layoutSource,
					layoutAsset) ||
				!layoutAsset ||
				!layoutAsset->IsReady())
			{
				outDiagnostic =
					"the selected layout source is not a readable .probes asset";
				return false;
			}
			scene.m_layoutSource = layoutAsset->GetSnapshot().m_data;
			if (!scene.m_layoutSource)
			{
				outDiagnostic =
					"the selected layout source has no valid probe data";
				return false;
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

		Math::AABB worldBounds;
		uint64_t sourceHash = 1469598103934665603ull;
		TMap<std::string, FrozenModelGeometry> frozenModelGeometry;
		HashString(sourceHash, request.m_worldAsset.ToString());
		HashString(sourceHash, world->GetName());
		for (MeshCandidate& candidate : candidates)
		{
			ModelPtr model = candidate.m_renderer->GetModel();
			const int32_t meshIndex = candidate.m_renderer->GetMeshIndex();
			FrozenModelGeometry geometry;
			if (!ResolveFrozenModelGeometry(
					model,
					meshIndex,
					"static mesh '" + candidate.m_gameObject->GetName() + "'",
					frozenModelGeometry,
					geometry,
					outDiagnostic))
			{
				return false;
			}

			const glm::mat4 worldMatrix = candidate.m_gameObject
				->GetTransformComponent().GetCachedWorldMatrix();
			const float determinant = glm::determinant(glm::mat3(worldMatrix));
			if (!IsFiniteMatrix(worldMatrix) ||
				!std::isfinite(determinant) ||
				std::abs(determinant) <= 1e-8f)
			{
				outDiagnostic = "static mesh '" + candidate.m_gameObject->GetName() +
					"' has a non-invertible world transform";
				return false;
			}

			Raytracing::PathTracer::TLASInstance instance;
			// Never retain model-owned geometry in a background bake. The copied
			// triangles are the immutable snapshot; their private BLAS is built by
			// the bake task below.
			instance.m_blas.Clear();
			instance.m_triangles = geometry.m_triangles;
			instance.m_meshIndex = meshIndex;
			instance.m_worldMatrix = worldMatrix;
			instance.m_inverseWorldMatrix = glm::inverse(worldMatrix);
			instance.m_worldBounds = geometry.m_localBounds;
			instance.m_worldBounds.Apply(worldMatrix);
			if (!instance.m_worldBounds.IsValid())
			{
				outDiagnostic = "static mesh '" + candidate.m_gameObject->GetName() +
					"' has invalid world bounds";
				return false;
			}
			TVector<MaterialPtr>& materials =
				candidate.m_renderer->GetMaterials();
			if (!AppendInstanceMaterials(
				instance.m_triangles,
				materials,
				scene,
				instance,
				outDiagnostic))
			{
				return false;
			}

			scene.m_instances.Add(std::move(instance));
			scene.m_geometryBounds.Add(
				scene.m_instances.Last()->m_worldBounds);
			worldBounds.Extend(scene.m_instances.Last()->m_worldBounds);
			HashString(sourceHash, candidate.m_instanceId);
			HashString(sourceHash, model->GetFileId().ToString());
			HashValue(sourceHash, meshIndex);
			HashValue(sourceHash, geometry.m_contentHash);
			HashMatrix(sourceHash, worldMatrix);
			HashBounds(sourceHash, geometry.m_localBounds);
			HashMaterials(sourceHash, materials);
		}

		TVector<LandscapeBakeGeometrySnapshot> landscapeSnapshots;
		if (auto* landscape = world->GetECS<LandscapeECS>();
			landscape && !landscape->CollectBakeGeometrySnapshots(
				landscapeSnapshots,
				outDiagnostic))
		{
			return false;
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
			const float determinant = glm::determinant(
				glm::mat3(snapshot.m_worldMatrix));
			if (!IsFiniteMatrix(snapshot.m_worldMatrix) ||
				!std::isfinite(determinant) ||
				std::abs(determinant) <= 1e-8f ||
				!snapshot.m_worldBounds.IsValid())
			{
				outDiagnostic = "bake geometry '" + snapshot.m_sourceId +
					"' has an invalid transform or bounds";
				return false;
			}

			FrozenModelGeometry geometry;
			if (snapshot.m_model)
			{
				if (!ResolveFrozenModelGeometry(
						snapshot.m_model,
						snapshot.m_meshIndex,
						"vegetation '" + snapshot.m_sourceId + "'",
						frozenModelGeometry,
						geometry,
						outDiagnostic))
				{
					return false;
				}
			}
			else if (snapshot.m_triangles &&
				!snapshot.m_triangles->IsEmpty())
			{
				geometry.m_triangles = snapshot.m_triangles;
			}
			else
			{
				outDiagnostic = "bake geometry '" + snapshot.m_sourceId +
					"' has no immutable CPU triangles";
				return false;
			}

			Raytracing::PathTracer::TLASInstance instance;
			// The immutable triangle snapshot is self-contained; retaining the
			// source model here would expose the background bake to hot reloads.
			instance.m_blas.Clear();
			instance.m_triangles = geometry.m_triangles;
			instance.m_meshIndex = snapshot.m_meshIndex;
			instance.m_worldMatrix = snapshot.m_worldMatrix;
			instance.m_inverseWorldMatrix = glm::inverse(snapshot.m_worldMatrix);
			instance.m_worldBounds = snapshot.m_worldBounds;
			if (!AppendInstanceMaterials(
				instance.m_triangles,
				snapshot.m_materials,
				scene,
				instance,
				outDiagnostic))
			{
				return false;
			}
			scene.m_instances.Add(std::move(instance));
			scene.m_geometryBounds.Add(snapshot.m_worldBounds);
			worldBounds.Extend(snapshot.m_worldBounds);

			HashString(sourceHash, snapshot.m_sourceId);
			HashValue(sourceHash, snapshot.m_sourceRevision);
			HashString(
				sourceHash,
				snapshot.m_model ?
					snapshot.m_model->GetFileId().ToString() : std::string());
			HashValue(sourceHash, snapshot.m_meshIndex);
			if (snapshot.m_model)
			{
				HashValue(sourceHash, geometry.m_contentHash);
			}
			HashMatrix(sourceHash, snapshot.m_worldMatrix);
			HashBounds(sourceHash, snapshot.m_worldBounds);
			if (!snapshot.m_model)
			{
				HashTriangles(sourceHash, snapshot.m_triangles);
			}
			HashMaterials(sourceHash, snapshot.m_materials);
		}

		if (scene.m_instances.IsEmpty() || !worldBounds.IsValid())
		{
			outDiagnostic =
				"the current world has no non-dynamic bakeable geometry";
			return false;
		}

		if (auto* lighting = world->GetECS<LightingECS>())
		{
			lighting->GetLightProxies(scene.m_lights);
		}
		for (const Raytracing::LightProxy& light : scene.m_lights)
		{
			HashValue(
				sourceHash,
				static_cast<uint32_t>(light.m_type));
			HashVec3(sourceHash, light.m_worldPosition);
			HashVec3(sourceHash, light.m_direction);
			HashVec3(sourceHash, light.m_intensity);
			HashValue(sourceHash, light.m_indirectLightingIntensity);
			HashVec3(sourceHash, light.m_attenuation);
			HashVec3(sourceHash, light.m_bounds);
			HashVec2(sourceHash, light.m_cutOff);
		}
		HashVec3(sourceHash, request.m_fallbackEnvironment);
		scene.m_materialRevisions.Reserve(scene.m_materials.Num());
		for (const MaterialPtr& material : scene.m_materials)
		{
			scene.m_materialRevisions.Add(
				material ? material->GetContentRevision() : 0u);
		}

		if (scene.m_layoutSource)
		{
			scene.m_volumeMin = scene.m_layoutSource->m_volumeMin;
			scene.m_volumeMax = scene.m_layoutSource->m_volumeMax;
		}
		else if (request.m_bAutoBounds)
		{
			const float padding = (std::max)(
				request.m_settings.m_minProbeSpacing,
				0.5f);
			scene.m_volumeMin = worldBounds.m_min - glm::vec3(padding);
			scene.m_volumeMax = worldBounds.m_max + glm::vec3(padding);
		}
		else
		{
			scene.m_volumeMin = request.m_volumeMin;
			scene.m_volumeMax = request.m_volumeMax;
		}
		scene.m_sourceWorldHash = sourceHash;
		return true;
	}
}

bool Sailor::IsGlobalIlluminationBakeContributor(
	EMobilityType mobility) noexcept
{
	return mobility == EMobilityType::Static ||
		mobility == EMobilityType::Stationary;
}

bool Sailor::AreWorldDocumentsEquivalentForProbeBake(
	const YAML::Node& savedDocument,
	const YAML::Node& currentDocument,
	std::string& outDiagnostic)
{
	outDiagnostic.clear();
	std::string normalizedSavedWorld;
	std::string normalizedCurrentWorld;
	std::string yamlDiagnostic;
	if (!External::TryDumpYaml(
			MakeProbeBakeComparableWorldDocument(savedDocument),
			normalizedSavedWorld,
			yamlDiagnostic) ||
		!External::TryDumpYaml(
			MakeProbeBakeComparableWorldDocument(currentDocument),
			normalizedCurrentWorld,
			yamlDiagnostic))
	{
		outDiagnostic = "the current and saved worlds cannot be compared: " +
			yamlDiagnostic;
		return false;
	}
	if (normalizedSavedWorld != normalizedCurrentWorld)
	{
		outDiagnostic =
			"the current level has unsaved changes or does not match the selected .world asset; save it before baking";
		return false;
	}
	return true;
}

GlobalIlluminationBakeController::GlobalIlluminationBakeController() :
	m_state(TSharedPtr<SharedState>::Make())
{}

GlobalIlluminationBakeController::~GlobalIlluminationBakeController()
{
	std::string diagnostic;
	Cancel(diagnostic);
	Wait();
}

bool GlobalIlluminationBakeController::Start(
	World* world,
	const EditorProbeVolumeBakeRequest& request,
	std::string& outDiagnostic)
{
	if (GetStatus().IsRunning())
	{
		outDiagnostic = "a probe-volume bake is already running";
		return false;
	}

	auto state = TSharedPtr<SharedState>::Make();
	state->m_status.m_state = EEditorProbeVolumeBakeState::Preparing;
	state->m_status.m_stage = "Preparing immutable scene snapshot";
	state->m_status.m_outputVirtualPath = request.m_outputVirtualPath;
	m_state = state;
	m_task.Clear();

	auto scene = TSharedPtr<ProbeBakeScene>::Make();
	if (!GatherScene(world, request, *scene, outDiagnostic))
	{
		Fail(state, outDiagnostic);
		return false;
	}

	auto* scheduler = App::GetSubmodule<Tasks::Scheduler>();
	if (!scheduler)
	{
		outDiagnostic = "the task scheduler is unavailable";
		Fail(state, outDiagnostic);
		return false;
	}

	m_task = Tasks::CreateTask(
		"Bake adaptive irradiance probe volume",
		[state, scene, request]()
		{
			const auto started = std::chrono::steady_clock::now();
			try
			{
				if (state->m_cancel.load(std::memory_order_acquire))
				{
					Cancelled(state, "probe-volume bake was cancelled before sampling");
					return;
				}
				UpdateStatus(
					state,
					[](EditorProbeVolumeBakeStatus& status)
					{
						status.m_state = EEditorProbeVolumeBakeState::Baking;
						status.m_stage = "Preparing CPU path tracer";
					});

				ProbeVolumeBakeSettings effectiveSettings = request.m_settings;
				if (scene->m_layoutSource)
				{
					effectiveSettings.m_maxSubdivisionLevel =
						scene->m_layoutSource->m_bakeSettings.m_maxSubdivisionLevel;
					effectiveSettings.m_minProbeSpacing =
						scene->m_layoutSource->m_bakeSettings.m_minProbeSpacing;
					effectiveSettings.m_normalBias =
						scene->m_layoutSource->m_bakeSettings.m_normalBias;
					effectiveSettings.m_viewBias =
						scene->m_layoutSource->m_bakeSettings.m_viewBias;
					effectiveSettings.m_maxRayDistance =
						scene->m_layoutSource->m_bakeSettings.m_maxRayDistance;
				}

				Raytracing::ProbeVolumePathTracer sampler;
				if (!MaterialsMatchSnapshot(*scene))
				{
					Fail(
						state,
						"a bake material changed after the immutable scene snapshot was captured; restart the bake");
					return;
				}
				auto reportPreparation =
					[state, started](
						const Raytracing::PathTracer::ScenePreparationProgress&
							progress) -> bool
					{
						if (state->m_cancel.load(std::memory_order_acquire))
						{
							return false;
						}
						const bool bPreparingGeometry = progress.m_stage ==
							Raytracing::PathTracer::EScenePreparationStage::Geometry;
						const float stageFraction = progress.m_total > 0u ?
							static_cast<float>(progress.m_completed) /
								static_cast<float>(progress.m_total) : 1.0f;
						const float progressBase = bPreparingGeometry ? 0.0f : 0.06f;
						const float progressRange = bPreparingGeometry ? 0.06f : 0.04f;
						const std::string stage =
							(bPreparingGeometry ?
								"Preparing bake geometry (" :
								"Preparing bake materials (") +
							std::to_string(progress.m_completed) + "/" +
							std::to_string(progress.m_total) + ")";
						UpdateStatus(
							state,
							[&stage, started, progressBase, progressRange,
								stageFraction](EditorProbeVolumeBakeStatus& status)
							{
								status.m_state =
									EEditorProbeVolumeBakeState::Baking;
								status.m_progress = progressBase +
									progressRange * stageFraction;
								status.m_stage = stage;
								status.m_elapsedSeconds =
									std::chrono::duration<float>(
										std::chrono::steady_clock::now() - started)
										.count();
							});
						return !state->m_cancel.load(std::memory_order_acquire);
					};
				if (!sampler.Initialize(
						scene->m_instances,
						scene->m_materials,
						scene->m_lights,
						effectiveSettings,
						request.m_fallbackEnvironment,
						reportPreparation))
				{
					if (state->m_cancel.load(std::memory_order_acquire))
					{
						Cancelled(
							state,
							"probe-volume bake was cancelled while preparing the CPU path tracer");
					}
					else
					{
						Fail(
							state,
							"the CPU path tracer could not prepare the bake scene or decode all material textures");
					}
					return;
				}
				if (!MaterialsMatchSnapshot(*scene))
				{
					Fail(
						state,
						"a bake material changed while its CPU sampling snapshot was prepared; restart the bake");
					return;
				}

				ProbeVolumeBakeRequest bakeRequest;
				bakeRequest.m_stateName = request.m_stateName;
				bakeRequest.m_volumeMin = scene->m_volumeMin;
				bakeRequest.m_volumeMax = scene->m_volumeMax;
				bakeRequest.m_settings = effectiveSettings;
				bakeRequest.m_sceneGeometryBounds = scene->m_geometryBounds;
				bakeRequest.m_sourceWorldHash = scene->m_sourceWorldHash;
				bakeRequest.m_layoutSource = scene->m_layoutSource.GetRawPtr();
				bakeRequest.m_cancel = &state->m_cancel;
				bakeRequest.m_progress =
					[state, started](const ProbeVolumeBakeProgress& progress)
					{
						UpdateStatus(
							state,
							[&progress, started](EditorProbeVolumeBakeStatus& status)
							{
								status.m_state = EEditorProbeVolumeBakeState::Baking;
								status.m_progress = 0.1f +
									0.9f * progress.m_fraction;
								status.m_completedProbes = progress.m_completedProbes;
								status.m_totalProbes = progress.m_totalProbes;
								status.m_stage = progress.m_stage;
								status.m_elapsedSeconds =
									std::chrono::duration<float>(
										std::chrono::steady_clock::now() - started).count();
							});
					};

				ProbeVolumeBakeResult result =
					ProbeVolumeBaker::Bake(bakeRequest, sampler);
				if (!result.IsSuccess())
				{
					if (result.m_status == EProbeVolumeBakeStatus::Cancelled ||
						state->m_cancel.load(std::memory_order_acquire))
					{
						Cancelled(state, std::move(result.m_diagnostic));
					}
					else
					{
						Fail(state, std::move(result.m_diagnostic));
					}
					return;
				}

				state->m_lock.Lock();
				if (state->m_cancel.load(std::memory_order_acquire))
				{
					state->m_status.m_state =
						EEditorProbeVolumeBakeState::Cancelled;
					state->m_status.m_stage = "Cancelled";
					state->m_status.m_diagnostic =
						"probe-volume bake was cancelled before its atomic commit";
					state->m_lock.Unlock();
					return;
				}
				state->m_status.m_state = EEditorProbeVolumeBakeState::Saving;
				state->m_status.m_stage = "Saving one baked state atomically";
				state->m_lock.Unlock();
				std::string saveDiagnostic;
				if (!ProbeVolumeBinary::SaveAtomic(
						scene->m_outputPath,
						*result.m_data,
						saveDiagnostic,
						request.m_bOverwrite))
				{
					Fail(state, std::move(saveDiagnostic));
					return;
				}

				UpdateStatus(
					state,
					[&result, &saveDiagnostic, started](
						EditorProbeVolumeBakeStatus& status)
					{
						status.m_state = EEditorProbeVolumeBakeState::Succeeded;
						status.m_progress = 1.0f;
						status.m_stage = "Completed";
						status.m_brickCount = static_cast<uint32_t>(
							result.m_data->m_bricks.Num());
						status.m_probeCount = static_cast<uint32_t>(
							result.m_data->m_probes.Num());
						status.m_layoutHash = result.m_data->m_layoutHash;
						status.m_transportHash = result.m_data->m_transportHash;
						status.m_lightingHash = result.m_data->m_lightingHash;
						status.m_elapsedSeconds =
							std::chrono::duration<float>(
								std::chrono::steady_clock::now() - started).count();
						status.m_diagnostic = std::move(saveDiagnostic);
					});
			}
			catch (const std::exception& exception)
			{
				Fail(
					state,
					std::string("probe-volume bake failed: ") +
						exception.what());
			}
			catch (...)
			{
				Fail(state, "probe-volume bake failed: unknown error");
			}
		},
		EThreadType::Background);
	scheduler->Run(m_task);
	outDiagnostic = "started background probe-volume bake";
	return true;
}

bool GlobalIlluminationBakeController::Cancel(std::string& outDiagnostic)
{
	m_state->m_lock.Lock();
	const EEditorProbeVolumeBakeState state = m_state->m_status.m_state;
	if (state == EEditorProbeVolumeBakeState::Saving)
	{
		m_state->m_lock.Unlock();
		outDiagnostic =
			"the probe-volume bake is already committing its output atomically";
		return false;
	}
	if (state != EEditorProbeVolumeBakeState::Preparing &&
		state != EEditorProbeVolumeBakeState::Baking)
	{
		m_state->m_lock.Unlock();
		outDiagnostic = "no probe-volume bake is running";
		return false;
	}
	m_state->m_cancel.store(true, std::memory_order_release);
	m_state->m_lock.Unlock();
	outDiagnostic = "requested probe-volume bake cancellation";
	return true;
}

EditorProbeVolumeBakeStatus
GlobalIlluminationBakeController::GetStatus() const
{
	if (!m_state)
	{
		return {};
	}
	m_state->m_lock.Lock();
	EditorProbeVolumeBakeStatus status = m_state->m_status;
	m_state->m_lock.Unlock();
	return status;
}

void GlobalIlluminationBakeController::Wait()
{
	if (m_task && !m_task->IsFinished())
	{
		m_task->Wait();
	}
}
