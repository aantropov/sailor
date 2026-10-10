#include "ECS/LandscapeECS.h"
#include "Landscape/LandscapeInternal.h"
#include "Landscape/LandscapeStreaming.h"

#include "AssetRegistry/Material/MaterialImporter.h"
#include "AssetRegistry/Model/ModelImporter.h"
#include "AssetRegistry/Texture/TextureImporter.h"
#include "AssetRegistry/AssetRegistry.h"
#include "Components/LandscapeComponent.h"
#include "Core/StringHash.h"
#include "ECS/CameraECS.h"
#include "ECS/TransformECS.h"
#include "ECS/PhysicsECS.h"
#include "Engine/GameObject.h"
#include "Math/Transform.h"
#include "RHI/GraphicsDriver.h"
#include "RHI/Renderer.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>

using namespace Sailor;
using namespace Sailor::LandscapeInternal;
using namespace Sailor::Tasks;

void LandscapeECS::BeginPlay()
{
	m_rhiScene = RHI::RHIScenePtr::Make();
	PublishSceneVersion();
}

void LandscapeECS::MarkDirty(GameObjectPtr owner)
{
	if (!owner)
	{
		return;
	}

	for (const auto& component : owner->GetComponents())
	{
		if (auto landscape = component.DynamicCast<LandscapeComponent>())
		{
			const size_t componentIndex = landscape->GetComponentIndex();
			if (IsComponentRegistered(componentIndex))
			{
				m_components[componentIndex].RequestFullRebuild();
			}
		}
	}
}

void LandscapeECS::OnComponentUnregistered(size_t, LandscapeData& component)
{
	DestroyPhysicsBodies(component);
	component.m_chunks.Clear();
	component.m_pendingVegetation.Clear();
	component.m_importMapLoads.Clear();
	component.m_layerTextureLoads.Clear();
	component.m_vegetationProfiles.Clear();
	component.m_runtimeMaterial.Clear();
	++m_shadowCastersRevision;
	if (!GetWorld() || !GetWorld()->IsClearing())
	{
		m_bHasPendingSceneChanges = true;
	}
}

void LandscapeECS::DestroyPhysicsBodies(LandscapeData& component)
{
	auto* physics = GetWorld() ? GetWorld()->GetECS<PhysicsECS>() : nullptr;
	if (physics)
	{
		for (uint32_t bodyId : component.m_physicsBodies)
		{
			physics->DestroyExternalBody(bodyId);
		}
	}
	component.m_physicsBodies.Clear();
	for (auto& chunk : component.m_chunks)
	{
		chunk.m_terrainBodyId = RigidBodyData::InvalidBodyId;
		chunk.m_vegetationBodyId = RigidBodyData::InvalidBodyId;
	}
}

void LandscapeECS::DestroyPhysicsBody(LandscapeData& component, uint32_t& bodyId)
{
	if (bodyId != RigidBodyData::InvalidBodyId)
	{
		if (auto* physics = GetWorld() ? GetWorld()->GetECS<PhysicsECS>() : nullptr)
		{
			physics->DestroyExternalBody(bodyId);
		}
		component.m_physicsBodies.Remove(bodyId);
		bodyId = RigidBodyData::InvalidBodyId;
	}
}

void LandscapeECS::DestroyChunkPhysicsBodies(LandscapeData& component, LandscapeChunk& chunk)
{
	DestroyPhysicsBody(component, chunk.m_terrainBodyId);
	DestroyPhysicsBody(component, chunk.m_vegetationBodyId);
}

void LandscapeECS::CreateChunkPhysicsBodies(LandscapeData& data, LandscapeChunk& chunk,
	const TVector<glm::vec3>& vertices, const TVector<uint32_t>& indices, const Math::Transform& transform)
{
	auto* physics = GetWorld()->GetECS<PhysicsECS>();
	if (!physics) return;
	auto owner = const_cast<ObjectPtr&>(data.GetOwner()).StaticCast<GameObject>();
	if (physics->CreateStaticTriangleMesh(owner->GetInstanceId(), vertices, indices,
		glm::vec3(transform.m_position), transform.GetRotation(), glm::vec3(transform.m_scale), chunk.m_terrainBodyId))
	{
		data.m_physicsBodies.Add(chunk.m_terrainBodyId);
	}
	RebuildVegetationPhysics(data, chunk, transform);
}

void LandscapeECS::RebuildVegetationPhysics(LandscapeData& data, LandscapeChunk& chunk, const Math::Transform& transform)
{
	DestroyPhysicsBody(data, chunk.m_vegetationBodyId);
	auto* physics = GetWorld()->GetECS<PhysicsECS>();
	if (!physics) return;
	auto owner = const_cast<ObjectPtr&>(data.GetOwner()).StaticCast<GameObject>();
	TVector<Physics::CollisionShapeDesc> shapes;
	for (const auto& placement : chunk.m_bakeVegetation)
	{
		if (placement.m_profileIndex >= data.m_vegetationProfiles.Num()) continue;
		const auto& profile = data.m_vegetationProfiles[placement.m_profileIndex];
		if (profile.m_settings.m_colliderRadius <= 0.0f) continue;
		const Math::Transform instanceTransform = Math::Transform::FromMatrix(placement.m_transform);
		const float instanceScale = (std::max)({ std::abs(instanceTransform.m_scale.x),
			std::abs(instanceTransform.m_scale.y), std::abs(instanceTransform.m_scale.z) });
		Physics::CollisionShapeDesc shape;
		shape.m_type = Physics::ECollisionShapeType::Capsule;
		shape.m_center = glm::vec3(instanceTransform.TransformPosition(glm::vec4(0, profile.m_settings.m_colliderOffsetY, 0, 1)));
		shape.m_rotation = instanceTransform.GetRotation();
		shape.m_radius = profile.m_settings.m_colliderRadius * instanceScale;
		shape.m_height = profile.m_settings.m_colliderHeight * instanceScale;
		shapes.Add(std::move(shape));
	}
	if (!shapes.IsEmpty() && physics->CreateStaticCompound(owner->GetInstanceId(), shapes,
		glm::vec3(transform.m_position), transform.GetRotation(), glm::vec3(transform.m_scale), chunk.m_vegetationBodyId))
	{
		data.m_physicsBodies.Add(chunk.m_vegetationBodyId);
	}
}

void LandscapeECS::UpdatePhysicsTransform(LandscapeData& data, const Math::Transform& transform)
{
	const glm::vec3 scale(transform.m_scale);
	if (glm::any(glm::greaterThan(glm::abs(scale - data.m_physicsScale), glm::vec3(0.000001f))))
	{
		for (auto& chunk : data.m_chunks)
		{
			// Reconstruct indexed collision geometry from the retained local bake triangles.
			TVector<uint32_t> indices;
			AppendLandscapeLodIndices(chunk.m_heightResolution,
				BuildLandscapeLodCoordinates(chunk.m_heightResolution, 1u), false, indices);
			TVector<glm::vec3> vertices;
			vertices.Resize(chunk.m_heightSamples.Num());
			for (size_t index = 0; index < indices.Num(); ++index)
			{
				vertices[indices[index]] = (*chunk.m_bakeTriangles)[index / 3].m_vertices[index % 3];
			}
			DestroyChunkPhysicsBodies(data, chunk);
			CreateChunkPhysicsBodies(data, chunk, vertices, indices, transform);
		}
	}
	else if (auto* physics = GetWorld()->GetECS<PhysicsECS>())
	{
		for (uint32_t bodyId : data.m_physicsBodies)
		{
			physics->SetExternalBodyTransform(bodyId, glm::vec3(transform.m_position), transform.GetRotation());
		}
	}
	data.m_physicsScale = scale;
}

void LandscapeECS::UpdateChunkVegetation(size_t componentIndex, size_t chunkIndex,
	const TVector<LandscapeVegetationInstance>& placements, const TVector<uint32_t>& profiles)
{
	auto& data = m_components[componentIndex];
	auto& chunk = data.m_chunks[chunkIndex];
	const auto owner = const_cast<ObjectPtr&>(data.GetOwner()).StaticCast<GameObject>();
	data.m_pendingVegetation.RemoveAll([&](const auto& pending)
		{ return pending.m_chunkIndex == chunkIndex && profiles.Contains(static_cast<uint32_t>(pending.m_profileIndex)); });
	chunk.m_bakeVegetation.RemoveAll([&](const auto& instance)
		{ return profiles.Contains(static_cast<uint32_t>(instance.m_profileIndex)); });
	for (const auto& placement : placements)
		chunk.m_bakeVegetation.Add(placement);
	std::stable_sort(chunk.m_bakeVegetation.begin(), chunk.m_bakeVegetation.end(),
		[](const auto& lhs, const auto& rhs) { return lhs.m_profileIndex < rhs.m_profileIndex; });
	chunk.m_vegetationRevision = ++data.m_vegetationRevision;
	for (uint32_t profileIndex : profiles)
	{
		const auto matchesProfile = [=](const auto& proxy) { return proxy.m_profileIndex == profileIndex; };
		if (profileIndex >= data.m_vegetationProfiles.Num() ||
			data.m_vegetationProfiles[profileIndex].m_settings.m_residency == ELandscapeVegetationResidency::Grass)
		{
			// The residency scheduler selects grass again, even if its capacity has not changed.
			chunk.m_vegetationProxies.RemoveAll(matchesProfile);
			continue;
		}
		const auto& profile = data.m_vegetationProfiles[profileIndex];
		LandscapeVegetationRenderInstances instances;
		for (const auto& placement : placements)
			if (placement.m_profileIndex == profileIndex) AppendRenderInstance(placement, instances);
		chunk.m_vegetationProxies.RemoveAll([&](const auto& proxy)
			{ return matchesProfile(proxy) && proxy.m_residency != profile.m_settings.m_residency; });
		LandscapeVegetationRenderProxy proxy;
		const auto result = BuildLandscapeVegetationProxy(profileIndex, profile, std::move(instances),
			ResolveLandscapeProxyMobility(owner->GetMobilityType(), profile.m_settings.m_residency), chunk.m_vegetationRevision, proxy);
		if (result == EVegetationProxyBuildResult::Pending)
		{
			data.m_pendingVegetation.Add({ chunkIndex, profileIndex, std::move(instances) });
		}
		else if (result == EVegetationProxyBuildResult::Success)
		{
			const size_t previous = chunk.m_vegetationProxies.FindIf(matchesProfile);
			if (previous != size_t(-1)) chunk.m_vegetationProxies[previous] = std::move(proxy);
			else chunk.m_vegetationProxies.Add(std::move(proxy));
		}
		else
		{
			chunk.m_vegetationProxies.RemoveAll(matchesProfile);
		}
	}
}

bool LandscapeECS::UpdateVegetationSources(size_t componentIndex, uint64_t previousTerrainRevision)
{
	auto& data = m_components[componentIndex];
	if (data.m_dirtyVegetationProfiles.IsEmpty() && !data.m_bIsVegetationCollisionDirty) return false;
	auto profiles = data.m_dirtyVegetationProfiles.ToVector();
	profiles.Sort();
	auto owner = const_cast<ObjectPtr&>(data.GetOwner()).StaticCast<GameObject>();
	const auto transform = Math::Transform::FromMatrix(owner->GetTransformComponent().GetCachedWorldMatrix());
	const LandscapeCpuTexture heightmap = !data.m_importMapLoads.IsEmpty() && data.m_importMapLoads[0] ?
		LandscapeCpuTexture(data.m_importMapLoads[0]->GetResult()) : LandscapeCpuTexture{};
	TVector<Tasks::TaskPtr<TVector<LandscapeVegetationInstance>>> tasks;
	tasks.Resize(data.m_chunks.Num());
	for (size_t index = 0; index < data.m_chunks.Num(); ++index)
	{
		const auto& chunk = data.m_chunks[index];
		if (profiles.IsEmpty() || chunk.m_buildRevision > previousTerrainRevision) continue;
		tasks[index] = Tasks::CreateTask<TVector<LandscapeVegetationInstance>>("LandscapeECS:Build Vegetation"_h,
			[&data, &heightmap, &profiles, x = chunk.m_chunkX, z = chunk.m_chunkZ]()
			{
				TVector<LandscapeVegetationInstance> instances;
				for (uint32_t profile : profiles)
					if (profile < data.m_vegetationProfiles.Num())
						AppendVegetationInstances(data, heightmap, x, z, profile, instances);
				return instances;
			}, EThreadType::Worker);
		tasks[index]->Run();
	}
	bool bChanged = false;
	for (size_t index = 0; index < data.m_chunks.Num(); ++index)
	{
		auto& chunk = data.m_chunks[index];
		if (chunk.m_buildRevision > previousTerrainRevision) continue;
		if (tasks[index])
		{
			tasks[index]->Wait();
			UpdateChunkVegetation(componentIndex, index, tasks[index]->GetResult(), profiles);
			bChanged = true;
		}
		if (data.m_bIsVegetationCollisionDirty) RebuildVegetationPhysics(data, chunk, transform);
	}
	data.m_dirtyVegetationProfiles.Clear();
	data.m_bIsVegetationCollisionDirty = false;
	if (bChanged) ++m_shadowCastersRevision;
	return bChanged;
}

bool LandscapeECS::UpdatePendingVegetation(size_t componentIndex)
{
	auto& data = m_components[componentIndex];
	if (data.m_pendingVegetation.IsEmpty())
	{
		return false;
	}
	auto owner = const_cast<ObjectPtr&>(data.GetOwner()).StaticCast<GameObject>();
	bool bChanged = false;
	for (size_t index = 0u; index < data.m_pendingVegetation.Num();)
	{
		auto& pending = data.m_pendingVegetation[index];
		auto& chunk = data.m_chunks[pending.m_chunkIndex];
		const auto& profile = data.m_vegetationProfiles[pending.m_profileIndex];
		LandscapeVegetationRenderProxy proxy;
		const auto result = BuildLandscapeVegetationProxy(pending.m_profileIndex,
			profile,
			std::move(pending.m_instances),
			ResolveLandscapeProxyMobility(owner->GetMobilityType(), profile.m_settings.m_residency),
			chunk.m_vegetationRevision,
			proxy);
		if (result == EVegetationProxyBuildResult::Pending)
		{
			++index;
			continue;
		}
		const size_t previous = chunk.m_vegetationProxies.FindIf([&pending](const auto& value)
			{ return value.m_profileIndex == pending.m_profileIndex; });
		if (result == EVegetationProxyBuildResult::Success)
		{
			if (previous != size_t(-1))
			{
				proxy.m_viewRevision = chunk.m_vegetationProxies[previous].m_viewRevision;
				chunk.m_vegetationProxies[previous] = std::move(proxy);
			}
			else
			{
				chunk.m_vegetationProxies.Add(std::move(proxy));
			}
			bChanged = true;
		}
		else if (previous != size_t(-1))
		{
			chunk.m_vegetationProxies.RemoveAtSwap(previous);
			bChanged = true;
		}
		data.m_pendingVegetation.RemoveAtSwap(index);
	}
	if (bChanged)
	{
		++m_shadowCastersRevision;
	}
	return bChanged;
}

void LandscapeECS::UpdateVegetationResources(LandscapeData& data)
{
	auto* modelImporter = App::GetSubmodule<ModelImporter>();
	auto* materialImporter = App::GetSubmodule<MaterialImporter>();
	for (auto& profile : data.m_vegetationProfiles)
	{
		if (!profile.m_model && modelImporter && profile.m_settings.m_modelFileId)
		{
			if (!profile.m_modelLoad)
			{
				ModelPtr loading;
				profile.m_modelLoad = modelImporter->LoadModel(profile.m_settings.m_modelFileId, loading);
				if (!profile.m_modelLoad) profile.m_modelLoad = Tasks::TaskPtr<ModelPtr>::Make(ModelPtr{});
			}
			if (profile.m_modelLoad->IsFinished()) profile.m_model = profile.m_modelLoad->GetResult();
		}
		if (profile.m_settings.m_materialFileId && !profile.m_material && materialImporter)
		{
			if (!profile.m_materialLoad)
			{
				MaterialPtr loading;
				profile.m_materialLoad = materialImporter->LoadMaterial(profile.m_settings.m_materialFileId, loading);
				if (!profile.m_materialLoad) profile.m_materialLoad = Tasks::TaskPtr<MaterialPtr>::Make(MaterialPtr{});
			}
			if (profile.m_materialLoad->IsFinished()) profile.m_material = profile.m_materialLoad->GetResult();
		}
		else if (!profile.m_settings.m_materialFileId && modelImporter)
		{
			if (!profile.m_modelMaterialsLoad)
			{
				profile.m_modelMaterialsLoad = modelImporter->LoadDefaultMaterials(profile.m_settings.m_modelFileId, profile.m_loadingModelMaterials);
			}
			if (profile.m_modelMaterialsLoad->IsFinished() && !profile.m_bAreModelMaterialsPublished)
			{
				profile.m_modelMaterials = std::move(profile.m_loadingModelMaterials);
				profile.m_bAreModelMaterialsPublished = true;
			}
		}
	}
}

void LandscapeECS::Tick(float)
{
	SAILOR_PROFILE_FUNCTION();
	bool bSceneChanged = m_bHasPendingSceneChanges;
	const auto* cameraEcs = GetWorld() ? GetWorld()->GetECS<CameraECS>() : nullptr;
	const TVector<Math::Transform> noCameraTransforms;
	const TVector<CameraData> noCameras;
	const auto& cameraTransforms = cameraEcs ? cameraEcs->GetActiveCameraTransforms() : noCameraTransforms;
	const auto& cameras = cameraEcs ? cameraEcs->GetActiveCameras() : noCameras;
	for (size_t componentIndex = 0; componentIndex < m_components.Num(); ++componentIndex)
	{
		if (!IsComponentRegistered(componentIndex))
		{
			continue;
		}

		auto& data = m_components[componentIndex];
		GameObjectPtr owner = const_cast<ObjectPtr&>(data.GetOwner()).StaticCast<GameObject>();
		const bool bTransformChanged =
			owner && owner->GetTransformComponent().GetFrameLastChange() > data.GetFrameLastChange();
		if (bTransformChanged && !data.m_chunks.IsEmpty())
		{
			UpdatePhysicsTransform(data, Math::Transform::FromMatrix(owner->GetTransformComponent().GetCachedWorldMatrix()));
			data.SetLastChange(owner->GetTransformComponent().GetFrameLastChange());
			++m_shadowCastersRevision;
			bSceneChanged = true;
		}
		UpdateVegetationResources(data);
		const bool bTerrainMaterialReady = data.m_material && data.m_material->IsReady();
		if (data.m_runtimeMaterial)
		{
			// Publishing is fence-gated. Calling IsReady every tick advances the
			// runtime material only after its cloned bindings are upload-complete.
			data.m_runtimeMaterial->IsReady();
		}
		const bool bTerrainMaterialContentRevisionChanged =
			data.m_runtimeMaterial && data.m_material &&
			data.m_cachedSourceMaterialContentRevision != data.m_material->GetContentRevision();
		const bool bTerrainMaterialRenderMetadataRevisionChanged =
			!data.m_runtimeMaterial || (data.m_material &&
			data.m_cachedSourceMaterialRenderMetadataRevision != data.m_material->GetRenderMetadataRevision());
		bool bVegetationRenderRevisionChanged = false;
		for (const auto& profile : data.m_vegetationProfiles)
		{
			if (profile.m_material)
			{
				profile.m_material->IsReady();
			}
			for (const auto& material : profile.m_modelMaterials)
			{
				if (material)
				{
					material->IsReady();
				}
			}
			bVegetationRenderRevisionChanged |=
				profile.m_cachedRenderRevision != CalculateVegetationRenderRevision(profile);
		}
		if (bTerrainMaterialRenderMetadataRevisionChanged)
		{
			data.m_runtimeMaterial.Clear();
		}
		else if (bTerrainMaterialContentRevisionChanged)
		{
			if (bTerrainMaterialReady)
			{
				// Landscape keeps its layer samplers on a private material instance.
				// Copy only source uniform values and version its bindings in place;
				// chunk meshes, physics, vegetation and scene records remain shared.
				data.m_runtimeMaterial->SynchronizeUniformValues(*data.m_material);
				data.m_cachedSourceMaterialContentRevision = data.m_material->GetContentRevision();
			}
			else
			{
				data.MarkDirty();
			}
		}
		if (!data.IsDirty() && data.m_pendingVegetation.IsEmpty() &&
			!bTerrainMaterialRenderMetadataRevisionChanged && !bVegetationRenderRevisionChanged)
		{
			continue;
		}
		if (!owner || !bTerrainMaterialReady)
		{
			data.MarkDirty();
			continue;
		}

		if (!data.m_runtimeMaterial)
		{
			const StringHash LayerNames[] = { "layer0Sampler"_h, "layer1Sampler"_h, "layer2Sampler"_h, "layer3Sampler"_h };
			auto* textureImporter = App::GetSubmodule<TextureImporter>();
			data.m_layerTextureLoads.Resize(data.m_layerTextures.Num());
			for (size_t layer = 0; textureImporter && layer < data.m_layerTextures.Num() && layer < 4u; ++layer)
			{
				auto& load = data.m_layerTextureLoads[layer];
				if (data.m_layerTextures[layer] && !load)
				{
					TexturePtr loading;
					load = textureImporter->LoadTexture(data.m_layerTextures[layer], loading);
					if (!load) load = Tasks::TaskPtr<TexturePtr>::Make(TexturePtr{});
				}
			}
			if (data.m_layerTextureLoads.ContainsIf([](const auto& load)
				{
					return load && (!load->IsFinished() || (load->GetResult() && !load->GetResult()->IsReady()));
				})) continue;

			data.m_runtimeMaterial = Material::CreateInstance(GetWorld(), data.m_material);
			for (size_t layer = 0; layer < data.m_layerTextureLoads.Num(); ++layer)
			{
				const auto& load = data.m_layerTextureLoads[layer];
				if (load && load->GetResult()) data.m_runtimeMaterial->SetSampler(LayerNames[layer], load->GetResult());
			}
			data.m_runtimeMaterial->UpdateRHIResourceAndUniforms();
			data.m_cachedSourceMaterialContentRevision = data.m_material->GetContentRevision();
			data.m_cachedSourceMaterialRenderMetadataRevision = data.m_material->GetRenderMetadataRevision();
		}

		bool bMaterialProxiesChanged = false;
		if (bTerrainMaterialRenderMetadataRevisionChanged)
		{
			for (size_t chunkIndex = 0u; chunkIndex < data.m_chunks.Num(); ++chunkIndex)
			{
				UpdateTerrainRenderProxy(componentIndex, chunkIndex, data.m_chunks[chunkIndex].m_resource->m_proxy.m_meshes[0]);
				bMaterialProxiesChanged = true;
			}
		}
		bMaterialProxiesChanged |= UpdateVegetationRenderProxies(componentIndex);
		if (bMaterialProxiesChanged)
		{
			++m_shadowCastersRevision;
			bSceneChanged = true;
		}
		if (!data.IsDirty())
		{
			bSceneChanged |= UpdatePendingVegetation(componentIndex);
			continue;
		}
		TryLoadVegetationAsset(data);

		const size_t numLandscapeChunks = static_cast<size_t>(data.m_chunksX) * data.m_chunksZ;
		bool bRebuildAllChunks =
			data.m_bRebuildAllChunks || data.m_chunks.Num() != numLandscapeChunks;
		TVector<uint32_t> chunksToBuild;
		auto selectAllChunks = [&]()
		{
			chunksToBuild.Resize(numLandscapeChunks);
			for (uint32_t chunkIndex = 0u; chunkIndex < chunksToBuild.Num(); ++chunkIndex)
			{
				chunksToBuild[chunkIndex] = chunkIndex;
			}
		};
		if (bRebuildAllChunks)
		{
			selectAllChunks();
		}
		else
		{
			chunksToBuild = data.m_dirtyChunks.ToVector();
			size_t validChunkWriteIndex = 0u;
			for (const uint32_t chunkIndex : chunksToBuild)
			{
				if (chunkIndex < numLandscapeChunks)
				{
					chunksToBuild[validChunkWriteIndex++] = chunkIndex;
				}
			}
			chunksToBuild.Resize(validChunkWriteIndex);
			chunksToBuild.Sort();
		}
		if (chunksToBuild.IsEmpty())
		{
			bSceneChanged |= UpdateVegetationSources(componentIndex, data.m_buildRevision);
			bSceneChanged |= UpdatePendingVegetation(componentIndex);
			data.m_bIsDirty = false;
			data.SetLastChange(owner->GetTransformComponent().GetFrameLastChange());
			TrySaveVegetationAsset(data);
			continue;
		}

		auto* textureImporter = App::GetSubmodule<TextureImporter>();
		auto requestMap = [&](size_t index)
		{
			const FileId id = index == 0 ? data.m_heightmapTexture : data.m_materialMasks[index - 1];
			return id ? textureImporter->LoadCpuTexture(id) : Tasks::TaskPtr<TextureImporter::CpuTextureSnapshot>{};
		};
		if (data.m_importMapLoads.IsEmpty())
		{
			for (size_t index = 0; index <= data.m_materialMasks.Num(); ++index)
				data.m_importMapLoads.Add(requestMap(index));
		}
		if (data.m_importMapLoads.ContainsIf([](const auto& task) { return task && !task->IsFinished(); })) continue;
		// A different source may have arrived while another map was still decoding.
		for (size_t index = 0; index < data.m_importMapLoads.Num(); ++index)
		{
			auto request = requestMap(index);
			if (request != data.m_importMapLoads[index])
			{
				data.m_importMapLoads[index] = std::move(request);
				data.RequestFullRebuild();
			}
		}
		if (data.m_importMapLoads.ContainsIf([](const auto& task) { return task && !task->IsFinished(); })) continue;
		if (data.m_bRebuildAllChunks && !bRebuildAllChunks)
		{
			bRebuildAllChunks = true;
			selectAllChunks();
		}
		auto readMap = [&](size_t index)
		{
			const auto& task = data.m_importMapLoads[index];
			return task ? LandscapeCpuTexture(task->GetResult()) : LandscapeCpuTexture{};
		};
		LandscapeCpuTexture heightmap = readMap(0);
		if (data.m_heightmapTexture && !heightmap.IsValid())
		{
			SAILOR_LOG_ERROR("LandscapeECS: failed to decode heightmap %s; procedural height will be used.",
				data.m_heightmapTexture.ToString().c_str());
		}
		TVector<LandscapeCpuTexture> materialMasks;
		materialMasks.Reserve(data.m_materialMasks.Num());
		for (size_t index = 0; index < data.m_materialMasks.Num(); ++index)
		{
			LandscapeCpuTexture mask = readMap(index + 1);
			if (!mask.IsValid() && data.m_materialMasks[index])
			{
				SAILOR_LOG_ERROR("LandscapeECS: failed to decode material mask %s.", data.m_materialMasks[index].ToString().c_str());
			}
			materialMasks.Add(std::move(mask));
		}
		// Retain completed requests to detect source revisions on the next geometry edit.

		TVector<Tasks::TaskPtr<LandscapeChunkCpuData>> tasks;
		tasks.Reserve(chunksToBuild.Num());
		for (uint32_t chunkIndex : chunksToBuild)
		{
			const uint32_t x = chunkIndex % data.m_chunksX;
			const uint32_t z = chunkIndex / data.m_chunksX;
			auto task = Tasks::CreateTask<LandscapeChunkCpuData>(
				"LandscapeECS:Build Chunk"_h,
				[&data, &heightmap, &materialMasks, x, z]()
				{ return BuildChunk(data, heightmap, materialMasks, x, z); },
				EThreadType::Worker);
			task->Run();
			tasks.Add(task);
		}

		// Keep the published terrain and collision until every replacement upload is accepted.
		TVector<RHI::RHIMeshPtr> meshes;
		meshes.Reserve(tasks.Num());
		for (auto& task : tasks)
		{
			task->Wait();
			const auto& cpu = task->m_result;
			auto mesh = RHI::Renderer::GetDriver()->CreateMesh();
			mesh->m_vertexDescription =
				RHI::Renderer::GetDriver()->GetOrAddVertexDescription<RHI::VertexP3N3T3B3UV2C4>();
			mesh->m_bounds = cpu.m_localBounds;
			mesh->m_materialIndex = 0u;
			mesh->m_indexCount =
				cpu.m_lodIndexCounts.IsEmpty() ? static_cast<uint32_t>(cpu.m_indices.Num()) : cpu.m_lodIndexCounts[0];
			mesh->m_firstIndex = 0u;
			mesh->m_vertexOffset = 0u;
			RHI::Renderer::GetDriver()->UpdateMesh(mesh,
				cpu.m_vertices.GetData(),
				cpu.m_vertices.Num() * sizeof(RHI::VertexP3N3T3B3UV2C4),
				cpu.m_indices.GetData(),
				cpu.m_indices.Num() * sizeof(uint32_t));
			if (mesh->HasInitializationFailed())
			{
				break;
			}
			meshes.Add(std::move(mesh));
		}
		if (meshes.Num() != tasks.Num())
		{
			// Unvisited CPU tasks still borrow this iteration's heightmap and masks.
			for (auto& task : tasks) task->Wait();
			data.MarkDirty();
			continue;
		}

		if (bRebuildAllChunks)
		{
			DestroyPhysicsBodies(data);
			data.m_chunks.Clear();
			data.m_chunks.Resize(numLandscapeChunks);
			data.m_pendingVegetation.Clear();
		}
		else
		{
			for (size_t index = 0u; index < data.m_pendingVegetation.Num();)
			{
				if (data.m_dirtyChunks.Contains(static_cast<uint32_t>(data.m_pendingVegetation[index].m_chunkIndex)))
				{
					data.m_pendingVegetation.RemoveAtSwap(index);
				}
				else
				{
					++index;
				}
			}
		}
		const uint64_t previousTerrainRevision = data.m_buildRevision;
		TVector<uint32_t> allProfiles;
		for (uint32_t profile = 0; profile < data.m_vegetationProfiles.Num(); ++profile) allProfiles.Add(profile);
		const glm::mat4 ownerMatrix = owner->GetTransformComponent().GetCachedWorldMatrix();
		const Math::Transform ownerTransform = Math::Transform::FromMatrix(ownerMatrix);
		for (size_t taskIndex = 0u; taskIndex < tasks.Num(); ++taskIndex)
		{
			const uint32_t chunkIndex = chunksToBuild[taskIndex];
			auto& cpu = tasks[taskIndex]->m_result;
			if (!bRebuildAllChunks)
			{
				DestroyChunkPhysicsBodies(data, data.m_chunks[chunkIndex]);
			}
			LandscapeChunk chunk;
			chunk.m_chunkX = cpu.m_chunkX;
			chunk.m_chunkZ = cpu.m_chunkZ;
			chunk.m_heightResolution = data.m_chunkResolution;
			const size_t terrainVertexCount =
				static_cast<size_t>(data.m_chunkResolution + 1u) * (data.m_chunkResolution + 1u);
			chunk.m_heightSamples.Resize(terrainVertexCount);
			chunk.m_bakeTriangles = TSharedPtr<TVector<Math::Triangle>>::Make(std::move(cpu.m_bakeTriangles));
			chunk.m_localBounds = cpu.m_localBounds;
			TVector<glm::vec3> collisionVertices;
			collisionVertices.Resize(terrainVertexCount);
			for (size_t vertexIndex = 0u; vertexIndex < terrainVertexCount; ++vertexIndex)
			{
				collisionVertices[vertexIndex] = cpu.m_vertices[vertexIndex].m_position;
				chunk.m_heightSamples[vertexIndex] = cpu.m_vertices[vertexIndex].m_position.y;
			}
			auto mesh = std::move(meshes[taskIndex]);
			for (size_t lodIndex = 1u; lodIndex < cpu.m_lodIndexCounts.Num(); ++lodIndex)
			{
				auto lodMesh = RHI::Renderer::GetDriver()->CreateMesh();
				lodMesh->m_vertexDescription = mesh->m_vertexDescription;
				lodMesh->m_vertexBuffer = mesh->m_vertexBuffer;
				lodMesh->m_indexBuffer = mesh->m_indexBuffer;
				lodMesh->m_bounds = mesh->m_bounds;
				lodMesh->m_materialIndex = mesh->m_materialIndex;
				lodMesh->m_bakedVolumeScale = mesh->m_bakedVolumeScale;
				lodMesh->m_indexCount = cpu.m_lodIndexCounts[lodIndex];
				lodMesh->m_firstIndex = cpu.m_lodFirstIndices[lodIndex];
				lodMesh->m_vertexOffset = 0u;
				mesh->m_lods.Add(std::move(lodMesh));
			}

			chunk.m_buildRevision = ++data.m_buildRevision;
			data.m_chunks[chunkIndex] = std::move(chunk);
			UpdateChunkVegetation(componentIndex, chunkIndex, cpu.m_vegetation, allProfiles);
			CreateChunkPhysicsBodies(data, data.m_chunks[chunkIndex], collisionVertices, cpu.m_collisionIndices, ownerTransform);
			UpdateTerrainRenderProxy(componentIndex, chunkIndex, std::move(mesh));
		}
		UpdateVegetationSources(componentIndex, previousTerrainRevision);
		// Pending vegetation retains its placements; upload completion does not dirty the terrain.
		data.m_dirtyChunks.Clear();
		data.m_bRebuildAllChunks = false;
		data.m_bIsDirty = false;
		data.m_physicsScale = glm::vec3(ownerTransform.m_scale);
		data.SetLastChange(owner->GetTransformComponent().GetFrameLastChange());
		++m_shadowCastersRevision;
		bSceneChanged = true;
		size_t vegetationPerChunk = 0u;
		for (const auto& profile : data.m_vegetationProfiles)
		{
			vegetationPerChunk += profile.m_settings.m_instancesPerChunk;
		}
		SAILOR_LOG("LandscapeECS: rebuilt %zu of %zu chunks with %zu collision bodies (%ux%u, %.1fm, resolution %u), "
				   "%zu vegetation profiles, %zu instances per chunk, %zu sculpt and %zu paint stamps, revision %llu.",
			chunksToBuild.Num(),
			data.m_chunks.Num(),
			data.m_physicsBodies.Num(),
			data.m_chunksX,
			data.m_chunksZ,
			data.m_chunkSize,
			data.m_chunkResolution,
			data.m_vegetationProfiles.Num(),
			vegetationPerChunk,
			data.m_sculptStamps.Num(),
			data.m_paintStamps.Num(),
			static_cast<unsigned long long>(data.m_buildRevision));
		TrySaveVegetationAsset(data);
	}
	bSceneChanged |= UpdateGrassResidency(cameraTransforms, cameras);
	if (bSceneChanged)
	{
		PublishSceneVersion();
	}
}
