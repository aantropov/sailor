#include "ECS/LandscapeECS.h"
#include "ECS/LandscapeECSInternal.h"
#include "ECS/LandscapeStreaming.h"

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
using namespace Sailor::LandscapeECSInternal;
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
	component.m_runtimeMaterial.Clear();
	++m_shadowCastersRevision;
	PublishSceneVersion();
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
		chunk.m_physicsBodies.Clear();
	}
}

void LandscapeECS::DestroyChunkPhysicsBodies(LandscapeData& component, LandscapeChunk& chunk)
{
	auto* physics = GetWorld() ? GetWorld()->GetECS<PhysicsECS>() : nullptr;
	for (uint32_t bodyId : chunk.m_physicsBodies)
	{
		if (physics)
		{
			physics->DestroyExternalBody(bodyId);
		}
		component.m_physicsBodies.Remove(bodyId);
	}
	chunk.m_physicsBodies.Clear();
}

Tasks::ITaskPtr LandscapeECS::Tick(float)
{
	SAILOR_PROFILE_FUNCTION();
	bool bSceneChanged = false;
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
		const bool transformChanged =
			owner && owner->GetTransformComponent().GetFrameLastChange() > data.GetFrameLastChange();
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
			data.m_runtimeMaterial && data.m_material &&
			data.m_cachedSourceMaterialRenderMetadataRevision != data.m_material->GetRenderMetadataRevision();
		bool bVegetationMaterialRenderMetadataRevisionChanged = false;
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
			bVegetationMaterialRenderMetadataRevisionChanged |=
				profile.m_cachedMaterialRenderMetadataRevision !=
				CalculateVegetationMaterialRenderMetadataRevision(profile);
		}
		if (bTerrainMaterialRenderMetadataRevisionChanged)
		{
			data.m_runtimeMaterial.Clear();
		}
		if (bTerrainMaterialRenderMetadataRevisionChanged || bVegetationMaterialRenderMetadataRevisionChanged)
		{
			data.RequestFullRebuild();
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
		if (!data.IsDirty() && !transformChanged)
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
			data.m_runtimeMaterial = Material::CreateInstance(GetWorld(), data.m_material);
			static const char* LayerNames[] = {"layer0Sampler", "layer1Sampler", "layer2Sampler", "layer3Sampler"};
			auto* textureImporter = App::GetSubmodule<TextureImporter>();
			for (size_t layer = 0; textureImporter && layer < data.m_layerTextures.Num() && layer < 4u; ++layer)
			{
				TexturePtr texture;
				if (data.m_layerTextures[layer] &&
					textureImporter->LoadTexture_Immediate(data.m_layerTextures[layer], texture) && texture)
				{
					data.m_runtimeMaterial->SetSampler(LayerNames[layer], texture);
				}
			}
			data.m_runtimeMaterial->UpdateRHIResourceAndUniforms();
			data.m_cachedSourceMaterialContentRevision = data.m_material->GetContentRevision();
			data.m_cachedSourceMaterialRenderMetadataRevision = data.m_material->GetRenderMetadataRevision();
		}

		auto* modelImporter = App::GetSubmodule<ModelImporter>();
		auto* materialImporter = App::GetSubmodule<MaterialImporter>();
		for (auto& profile : data.m_vegetationProfiles)
		{
			if (!profile.m_model && modelImporter && profile.m_modelFileId)
			{
				modelImporter->LoadModel_Immediate(profile.m_modelFileId, profile.m_model);
			}
			if (profile.m_materialFileId && !profile.m_material && materialImporter)
			{
				materialImporter->LoadMaterial_Immediate(profile.m_materialFileId, profile.m_material);
			}
			else if (!profile.m_materialFileId && !profile.m_bModelMaterialsRequested && modelImporter)
			{
				modelImporter->LoadDefaultMaterials(profile.m_modelFileId, profile.m_modelMaterials);
				profile.m_bModelMaterialsRequested = true;
			}
		}
		TryLoadVegetationAsset(data);

		const size_t numLandscapeChunks = static_cast<size_t>(data.m_chunksX) * data.m_chunksZ;
		const bool bRebuildAllChunks =
			data.m_bRebuildAllChunks || transformChanged || data.m_chunks.Num() != numLandscapeChunks;
		TVector<uint32_t> chunksToBuild;
		if (bRebuildAllChunks)
		{
			chunksToBuild.Resize(numLandscapeChunks);
			for (uint32_t chunkIndex = 0u; chunkIndex < chunksToBuild.Num(); ++chunkIndex)
			{
				chunksToBuild[chunkIndex] = chunkIndex;
			}
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
			TrySaveVegetationAsset(data);
			data.m_bIsDirty = false;
			data.SetLastChange(owner->GetTransformComponent().GetFrameLastChange());
			continue;
		}

		LandscapeCpuTexture heightmap;
		if (data.m_heightmapTexture && !DecodeCpuTexture(data.m_heightmapTexture, heightmap))
		{
			SAILOR_LOG_ERROR("LandscapeECS: failed to decode heightmap %s; procedural height will be used.",
				data.m_heightmapTexture.ToString().c_str());
		}
		TVector<LandscapeCpuTexture> materialMasks;
		materialMasks.Reserve(data.m_materialMasks.Num());
		for (const FileId& maskFileId : data.m_materialMasks)
		{
			LandscapeCpuTexture mask;
			if (!DecodeCpuTexture(maskFileId, mask) && maskFileId)
			{
				SAILOR_LOG_ERROR("LandscapeECS: failed to decode material mask %s.", maskFileId.ToString().c_str());
			}
			materialMasks.Add(std::move(mask));
		}

		TVector<Tasks::TaskPtr<LandscapeChunkCpuData>> tasks;
		tasks.Reserve(chunksToBuild.Num());
		for (uint32_t chunkIndex : chunksToBuild)
		{
			const uint32_t x = chunkIndex % data.m_chunksX;
			const uint32_t z = chunkIndex / data.m_chunksX;
			auto task = Tasks::CreateTask<LandscapeChunkCpuData>(
				"LandscapeECS:Build Chunk",
				[&data, &heightmap, &materialMasks, x, z]()
				{ return BuildChunk(data, heightmap, materialMasks, x, z); },
				EThreadType::Worker);
			task->Run();
			tasks.Add(task);
		}

		if (bRebuildAllChunks)
		{
			DestroyPhysicsBodies(data);
			data.m_chunks.Clear();
			data.m_chunks.Resize(numLandscapeChunks);
		}
		size_t vegetationRenderProxies = 0u;
		size_t vegetationProfilesNotReady = 0u;
		size_t vegetationProfilesWithoutRenderData = 0u;
		bool bVegetationResourcesPending = false;
		const glm::mat4 ownerMatrix = owner->GetTransformComponent().GetCachedWorldMatrix();
		const EMobilityType ownerMobility = owner->GetMobilityType();
		const Math::Transform ownerTransform = Math::Transform::FromMatrix(ownerMatrix);
		auto* physics = GetWorld()->GetECS<PhysicsECS>();
		for (size_t taskIndex = 0u; taskIndex < tasks.Num(); ++taskIndex)
		{
			const uint32_t chunkIndex = chunksToBuild[taskIndex];
			tasks[taskIndex]->Wait();
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
			chunk.m_bakeVegetation.Reserve(cpu.m_vegetation.Num());
			for (const auto& placement : cpu.m_vegetation)
			{
				LandscapeBakeVegetationInstance bakeInstance;
				bakeInstance.m_profileIndex = placement.m_profileIndex;
				bakeInstance.m_localMatrix = placement.m_transform;
				chunk.m_bakeVegetation.Add(std::move(bakeInstance));
			}
			TVector<glm::vec3> collisionVertices;
			collisionVertices.Resize(terrainVertexCount);
			for (size_t vertexIndex = 0u; vertexIndex < terrainVertexCount; ++vertexIndex)
			{
				collisionVertices[vertexIndex] = cpu.m_vertices[vertexIndex].m_position;
				chunk.m_heightSamples[vertexIndex] = cpu.m_vertices[vertexIndex].m_position.y;
			}
			uint32_t physicsBodyId = RigidBodyData::InvalidBodyId;
			if (physics && physics->CreateStaticTriangleMesh(owner->GetInstanceId(),
							   collisionVertices,
							   cpu.m_collisionIndices,
							   glm::vec3(ownerTransform.m_position),
							   ownerTransform.m_rotation,
							   glm::vec3(ownerTransform.m_scale),
							   physicsBodyId))
			{
				data.m_physicsBodies.Add(physicsBodyId);
				chunk.m_physicsBodies.Add(physicsBodyId);
			}
			TVector<Physics::CollisionShapeDesc> vegetationCollisionShapes;
			for (const auto& placement : cpu.m_vegetation)
			{
				if (placement.m_profileIndex >= data.m_vegetationProfiles.Num())
				{
					continue;
				}
				const auto& profile = data.m_vegetationProfiles[placement.m_profileIndex];
				if (profile.m_colliderRadius <= 0.0f)
				{
					continue;
				}
				const Math::Transform instanceTransform = Math::Transform::FromMatrix(placement.m_transform);
				const float instanceScale = (std::max)({std::abs(instanceTransform.m_scale.x),
					std::abs(instanceTransform.m_scale.y),
					std::abs(instanceTransform.m_scale.z)});
				Physics::CollisionShapeDesc shape;
				shape.m_type = Physics::ECollisionShapeType::Capsule;
				shape.m_center = glm::vec3(
					instanceTransform.TransformPosition(glm::vec4(0.0f, profile.m_colliderOffsetY, 0.0f, 1.0f)));
				shape.m_rotation = instanceTransform.m_rotation;
				shape.m_radius = profile.m_colliderRadius * instanceScale;
				shape.m_height = profile.m_colliderHeight * instanceScale;
				vegetationCollisionShapes.Add(std::move(shape));
			}
			if (physics && !vegetationCollisionShapes.IsEmpty() &&
				physics->CreateStaticCompound(owner->GetInstanceId(),
					vegetationCollisionShapes,
					glm::vec3(ownerTransform.m_position),
					ownerTransform.m_rotation,
					glm::vec3(ownerTransform.m_scale),
					physicsBodyId))
			{
				data.m_physicsBodies.Add(physicsBodyId);
				chunk.m_physicsBodies.Add(physicsBodyId);
			}
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

			RHI::RHISceneViewProxy proxy;
			proxy.m_staticMeshEcs = LandscapeProxyId(componentIndex, chunkIndex);
			proxy.m_mobility = ResolveLandscapeProxyMobility(ownerMobility, ELandscapeVegetationResidency::Persistent);
			proxy.m_worldMatrix = ownerMatrix;
			proxy.m_worldAabb = cpu.m_localBounds;
			proxy.m_worldAabb.Apply(ownerMatrix);
			proxy.m_frame = GetWorld()->GetCurrentFrame();
			proxy.m_bCastShadows = true;
			proxy.m_lodPolicy.m_bEnabled = !mesh->m_lods.IsEmpty();
			proxy.m_lodPolicy.m_minLod = 0u;
			proxy.m_lodPolicy.m_maxLod = mesh->GetNumLods() - 1u;
			proxy.m_lodPolicy.m_cameraDistanceThresholds = data.m_lodDistances;
			if (proxy.m_lodPolicy.m_cameraDistanceThresholds.Num() >= mesh->GetNumLods())
			{
				proxy.m_lodPolicy.m_cameraDistanceThresholds.Resize(mesh->GetNumLods() - 1u);
			}
			proxy.m_meshes.Add(mesh);
			proxy.m_meshModelMatrices.Add(ownerMatrix);
			proxy.m_overrideMaterials.Add(data.m_runtimeMaterial->GetOrAddRHI(mesh->m_vertexDescription));
			proxy.m_renderQueueTags.Add(data.m_runtimeMaterial->GetRenderState().GetTag());
			AppendDepthMaterialMetadata(proxy, data.m_runtimeMaterial);
			auto shadowCaster = RHI::RHIShadowCasterProxyPtr::Make();
			shadowCaster->m_staticMeshEcs = proxy.m_staticMeshEcs;
			shadowCaster->m_mobility = proxy.m_mobility;
			shadowCaster->m_lodPolicy = proxy.m_lodPolicy;
			shadowCaster->m_skeletonOffset = (std::numeric_limits<uint32_t>::max)();
			shadowCaster->m_frame = proxy.m_frame;
			AppendShadowMesh(
				*shadowCaster, mesh, ownerMatrix, data.m_runtimeMaterial, (std::numeric_limits<float>::max)());

			shadowCaster->m_worldAabb = proxy.m_worldAabb;
			proxy.m_shadowCaster = shadowCaster->m_meshes.IsEmpty() ? RHI::RHIShadowCasterProxyPtr{} : shadowCaster;
#if defined(__APPLE__)
			auto* textureImporter = App::GetSubmodule<TextureImporter>();
			proxy.m_materialTextureSamplers.Resize(1u);
			proxy.m_materialTextureSamplers[0].Insert(0u);
			if (textureImporter)
			{
				for (const auto& sampler : data.m_runtimeMaterial->GetSamplers())
				{
					proxy.m_materialTextureSamplers[0].Insert(
						sampler.m_second
							? static_cast<uint32_t>(textureImporter->GetTextureIndex(sampler.m_second->GetFileId()))
							: 0u);
				}
			}
#endif
			GetOctreeBounds(proxy.m_worldAabb, chunk.m_octreeCenter, chunk.m_octreeExtents);

			for (size_t profileIndex = 0u; profileIndex < data.m_vegetationProfiles.Num(); ++profileIndex)
			{
				auto& profile = data.m_vegetationProfiles[profileIndex];
				if (profile.m_residency == ELandscapeVegetationResidency::Grass)
				{
					continue;
				}

				LandscapeVegetationRenderInstances instances;
				instances.m_transforms.Reserve(profile.m_instancesPerChunk);
				instances.m_lodBiases.Reserve(profile.m_instancesPerChunk);
				instances.m_cullDistanceScales.Reserve(profile.m_instancesPerChunk);
				instances.m_shadowDistanceScales.Reserve(profile.m_instancesPerChunk);
				for (const auto& placement : cpu.m_vegetation)
				{
					if (placement.m_profileIndex != profileIndex)
					{
						continue;
					}
					AppendRenderInstance(placement, instances);
				}

				LandscapeVegetationRenderProxy vegetationRenderProxy;
				const auto buildResult = BuildLandscapeVegetationProxy(componentIndex,
					chunkIndex,
					profileIndex,
					profile,
					ownerMatrix,
					proxy.m_frame,
					std::move(instances),
					ResolveLandscapeProxyMobility(ownerMobility, profile.m_residency),
					data.m_buildRevision + 1u,
					vegetationRenderProxy);
				if (buildResult == EVegetationProxyBuildResult::Pending)
				{
					++vegetationProfilesNotReady;
					bVegetationResourcesPending = true;
					continue;
				}
				if (buildResult == EVegetationProxyBuildResult::NoRenderData)
				{
					++vegetationProfilesWithoutRenderData;
					continue;
				}
				chunk.m_vegetationProxies.Add(std::move(vegetationRenderProxy));
				++vegetationRenderProxies;
			}
			chunk.m_resource = RHI::RHISceneProxyResourcePtr::Make(std::move(proxy));
			chunk.m_buildRevision = ++data.m_buildRevision;
			data.m_chunks[chunkIndex] = std::move(chunk);
		}
		// Model and material import tasks complete before their GPU upload fences do.
		// Keep the component dirty while valid vegetation resources are pending so
		// the next frame can populate the vegetation proxies after those fences signal.
		data.m_dirtyChunks.Clear();
		data.m_bRebuildAllChunks = bVegetationResourcesPending;
		data.m_bIsDirty = bVegetationResourcesPending;
		for (auto& profile : data.m_vegetationProfiles)
		{
			profile.m_cachedMaterialRenderMetadataRevision = CalculateVegetationMaterialRenderMetadataRevision(profile);
		}
		data.SetLastChange(owner->GetTransformComponent().GetFrameLastChange());
		++m_shadowCastersRevision;
		bSceneChanged = true;
		size_t vegetationPerChunk = 0u;
		for (const auto& profile : data.m_vegetationProfiles)
		{
			vegetationPerChunk += profile.m_instancesPerChunk;
		}
		SAILOR_LOG("LandscapeECS: rebuilt %zu of %zu chunks with %zu collision bodies (%ux%u, %.1fm, resolution %u), "
				   "%zu vegetation profiles, %zu instances per chunk and %zu render proxies (%zu profile loads not "
				   "ready, %zu without render data), %zu sculpt and %zu paint stamps, revision %llu.",
			chunksToBuild.Num(),
			data.m_chunks.Num(),
			data.m_physicsBodies.Num(),
			data.m_chunksX,
			data.m_chunksZ,
			data.m_chunkSize,
			data.m_chunkResolution,
			data.m_vegetationProfiles.Num(),
			vegetationPerChunk,
			vegetationRenderProxies,
			vegetationProfilesNotReady,
			vegetationProfilesWithoutRenderData,
			data.m_sculptStamps.Num() / 5u,
			data.m_paintStamps.Num() / 5u,
			static_cast<unsigned long long>(data.m_buildRevision));
		TrySaveVegetationAsset(data);
	}
	bSceneChanged |= UpdateGrassResidency(cameraTransforms, cameras);
	if (bSceneChanged)
	{
		PublishSceneVersion();
	}
	return nullptr;
}
