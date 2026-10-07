#include "ECS/LandscapeECS.h"
#include "Landscape/LandscapeInternal.h"

#include "AssetRegistry/Material/MaterialImporter.h"
#include "AssetRegistry/Model/ModelImporter.h"
#include "ECS/TransformECS.h"
#include "Engine/GameObject.h"
#include "RHI/SceneView.h"

#include <string>
#include <utility>

using namespace Sailor;
using namespace Sailor::LandscapeInternal;
using namespace Sailor::Tasks;

void LandscapeECS::PublishSceneVersion()
{
	auto version = TSharedPtr<RHI::RHISpatialSceneVersion>::Make();
	version->m_revision = ++m_sceneVersionRevision;
	version->m_shadowCastersRevision = m_shadowCastersRevision;
	version->m_scene = m_rhiScene;
	TSet<size_t> activeProducerKeys;
	size_t staticSpatialHash = Fnv1aOffsetBasis;
	size_t stationarySpatialHash = Fnv1aOffsetBasis;
	size_t dynamicSpatialHash = Fnv1aOffsetBasis;
	bool bHasStaticSpatialEntries = false;
	bool bHasStationarySpatialEntries = false;
	bool bHasDynamicSpatialEntries = false;
	auto hashSpatialEntry = [&staticSpatialHash,
								&stationarySpatialHash,
								&dynamicSpatialHash,
								&bHasStaticSpatialEntries,
								&bHasStationarySpatialEntries,
								&bHasDynamicSpatialEntries](const RHI::RenderInstanceHandle& handle,
								const glm::ivec3& center,
								const glm::ivec3& extents,
								EMobilityType mobility)
	{
		size_t* spatialHash = nullptr;
		switch (mobility)
		{
		case EMobilityType::Static:
			bHasStaticSpatialEntries = true;
			spatialHash = &staticSpatialHash;
			break;
		case EMobilityType::Stationary:
			bHasStationarySpatialEntries = true;
			spatialHash = &stationarySpatialHash;
			break;
		case EMobilityType::Dynamic:
			bHasDynamicSpatialEntries = true;
			spatialHash = &dynamicSpatialHash;
			break;
		}
		if (!spatialHash)
		{
			return;
		}
		HashCombine(*spatialHash,
			handle.m_slot,
			handle.m_generation,
			center.x,
			center.y,
			center.z,
			extents.x,
			extents.y,
			extents.z);
	};

	auto publishProxy = [this, &activeProducerKeys](size_t producerKey,
							const RHI::RHISceneProxyResourcePtr& resource,
							uint64_t buildRevision,
							const glm::mat4& worldMatrix,
							const Math::AABB& worldBounds,
							EMobilityType mobility) -> RHI::RenderInstanceHandle
	{
		if (!m_rhiScene || !resource)
		{
			return {};
		}

		activeProducerKeys.Insert(producerKey);
		RHI::RenderInstanceHandle* handle = nullptr;
		RHI::RHISceneInstanceRecord currentRecord;
		const bool bHasCurrentRecord = m_renderInstanceHandles.Find(producerKey, handle) && handle &&
			m_rhiScene->ResolveCurrent(*handle, currentRecord);
		RHI::SceneChangeMask changes = 0u;
		if (bHasCurrentRecord)
		{
			if (currentRecord.m_topology != resource)
			{
				changes |= RHI::ToMask(RHI::ESceneChangeBit::ReplaceChunkRange) |
					RHI::ToMask(RHI::ESceneChangeBit::MeshOrLodTopology) | RHI::ToMask(RHI::ESceneChangeBit::Material) |
					RHI::ToMask(RHI::ESceneChangeBit::ShadowState) | RHI::ToMask(RHI::ESceneChangeBit::Bounds);
			}
			if (currentRecord.m_worldMatrix != worldMatrix)
			{
				changes |= RHI::ToMask(RHI::ESceneChangeBit::Transform) | RHI::ToMask(RHI::ESceneChangeBit::Bounds);
			}
			if (currentRecord.m_mobility != mobility)
			{
				changes |= RHI::ToMask(RHI::ESceneChangeBit::Mobility);
			}
			if (changes == 0u)
			{
				return *handle;
			}
		}

		RHI::RHISceneInstanceRecord record;
		record.m_producerKey = producerKey;
		record.m_mobility = mobility;
		record.m_worldMatrix = worldMatrix;
		record.m_worldBounds = worldBounds;
		record.m_topology = resource;
		record.m_topologyRevision = buildRevision;
		record.m_materialRevision = currentRecord.m_topology == resource ?
			currentRecord.m_materialRevision : Material::GetGlobalContentRevision();
		record.m_shadowRevision = resource->m_shadowRevision;
		record.m_renderFlags = resource->m_proxy.m_shadowCaster ? 1u : 0u;

		if (bHasCurrentRecord)
		{
			m_rhiScene->UpdateInstance(*handle, record, changes);
		}
		else
		{
			m_renderInstanceHandles[producerKey] = m_rhiScene->AddInstance(record);
			handle = &m_renderInstanceHandles[producerKey];
		}
		return handle ? *handle : RHI::RenderInstanceHandle{};
	};

	for (size_t componentIndex = 0; componentIndex < m_components.Num(); ++componentIndex)
	{
		if (!IsComponentRegistered(componentIndex))
		{
			continue;
		}

		auto& data = m_components[componentIndex];
		auto owner = const_cast<ObjectPtr&>(data.GetOwner()).StaticCast<GameObject>();
		const auto& worldMatrix = owner->GetTransformComponent().GetCachedWorldMatrix();
		const auto ownerMobility = owner->GetMobilityType();
		for (size_t chunkIndex = 0; chunkIndex < data.m_chunks.Num(); ++chunkIndex)
		{
			auto& chunk = data.m_chunks[chunkIndex];
			Math::AABB worldBounds = chunk.m_localBounds;
			worldBounds.Apply(worldMatrix);
			GetOctreeBounds(worldBounds, chunk.m_octreeCenter, chunk.m_octreeExtents);
			const auto chunkHandle = publishProxy(LandscapeProxyId(componentIndex, chunkIndex),
				chunk.m_resource, chunk.m_buildRevision, worldMatrix, worldBounds, ownerMobility);
			if (chunkHandle.IsValid())
			{
				hashSpatialEntry(chunkHandle, chunk.m_octreeCenter, chunk.m_octreeExtents, ownerMobility);
			}
			for (auto& vegetation : chunk.m_vegetationProxies)
			{
				worldBounds = vegetation.m_localBounds;
				worldBounds.Apply(worldMatrix);
				GetOctreeBounds(worldBounds, vegetation.m_octreeCenter, vegetation.m_octreeExtents);
				vegetation.m_mobility = ResolveLandscapeProxyMobility(ownerMobility, vegetation.m_residency);
				const auto vegetationHandle =
					publishProxy(LandscapeVegetationProxyId(componentIndex, chunkIndex, vegetation.m_profileIndex),
						vegetation.m_resource, vegetation.m_revision, worldMatrix, worldBounds, vegetation.m_mobility);
				if (vegetationHandle.IsValid())
				{
					hashSpatialEntry(
						vegetationHandle, vegetation.m_octreeCenter, vegetation.m_octreeExtents, vegetation.m_mobility);
				}
			}
		}

		for (const auto& profile : data.m_vegetationProfiles)
		{
			version->m_bHasCustomDepthShadowCasters |=
				profile.m_material && profile.m_settings.m_shadowMode != ELandscapeVegetationShadowMode::None &&
				profile.m_material->GetRenderState().IsRequiredCustomDepthShader();
		}
	}
	if (m_rhiScene)
	{
		for (size_t producerKey : m_renderInstanceHandles.GetKeys())
		{
			if (activeProducerKeys.Contains(producerKey))
			{
				continue;
			}
			RHI::RenderInstanceHandle* handle = nullptr;
			if (m_renderInstanceHandles.Find(producerKey, handle) && handle)
			{
				m_rhiScene->RemoveInstance(*handle);
			}
			m_renderInstanceHandles.Remove(producerKey);
		}

		const bool bStaticSpatialChanged = !m_publishedSceneVersion || m_staticSpatialHash != staticSpatialHash;
		const bool bStationarySpatialChanged =
			!m_publishedSceneVersion || m_stationarySpatialHash != stationarySpatialHash;
		const bool bDynamicSpatialChanged = !m_publishedSceneVersion || m_dynamicSpatialHash != dynamicSpatialHash;
		if (bStaticSpatialChanged || bStationarySpatialChanged || bDynamicSpatialChanged)
		{
			++m_spatialRevision;
		}

		auto rebuildSpatialTree =
			[this](EMobilityType mobility, bool bHasEntries, TSharedPtr<const RHI::RHISceneSpatialIndex>& tree)
		{
			if (!bHasEntries)
			{
				tree.Clear();
				return;
			}
			auto index = TSharedPtr<RHI::RHISceneSpatialIndex>::Make(glm::ivec3(0, 0, 0), 16536 * 16, 4);
			auto appendSpatialEntry = [this, &index](size_t producerKey,
										  const glm::ivec3& center,
										  const glm::ivec3& extents)
			{
				RHI::RenderInstanceHandle* handle = nullptr;
				if (m_renderInstanceHandles.Find(producerKey, handle) && handle)
				{
					index->Update(center, extents, *handle);
				}
			};
			for (size_t componentIndex = 0u; componentIndex < m_components.Num(); ++componentIndex)
			{
				if (!IsComponentRegistered(componentIndex))
				{
					continue;
				}
				const auto& data = m_components[componentIndex];
				auto owner = const_cast<ObjectPtr&>(data.GetOwner()).StaticCast<GameObject>();
				for (size_t chunkIndex = 0; chunkIndex < data.m_chunks.Num(); ++chunkIndex)
				{
					const auto& chunk = data.m_chunks[chunkIndex];
					if (chunk.m_resource && owner->GetMobilityType() == mobility)
					{
						appendSpatialEntry(LandscapeProxyId(componentIndex, chunkIndex), chunk.m_octreeCenter, chunk.m_octreeExtents);
					}
					for (const auto& vegetation : chunk.m_vegetationProxies)
					{
						if (vegetation.m_mobility == mobility)
						{
							appendSpatialEntry(LandscapeVegetationProxyId(componentIndex, chunkIndex, vegetation.m_profileIndex),
								vegetation.m_octreeCenter, vegetation.m_octreeExtents);
						}
					}
				}
			}
			tree = std::move(index);
		};

		if (bStaticSpatialChanged)
		{
			rebuildSpatialTree(EMobilityType::Static, bHasStaticSpatialEntries, version->m_staticOctree);
		}
		else
		{
			version->m_staticOctree = m_publishedSceneVersion->m_staticOctree;
		}
		if (bStationarySpatialChanged)
		{
			rebuildSpatialTree(EMobilityType::Stationary, bHasStationarySpatialEntries, version->m_stationaryOctree);
		}
		else
		{
			version->m_stationaryOctree = m_publishedSceneVersion->m_stationaryOctree;
		}
		if (bDynamicSpatialChanged)
		{
			rebuildSpatialTree(EMobilityType::Dynamic, bHasDynamicSpatialEntries, version->m_dynamicOctree);
		}
		else
		{
			version->m_dynamicOctree = m_publishedSceneVersion->m_dynamicOctree;
		}
		m_staticSpatialHash = staticSpatialHash;
		m_stationarySpatialHash = stationarySpatialHash;
		m_dynamicSpatialHash = dynamicSpatialHash;
		version->m_sceneVersion = m_rhiScene->PublishVersion(
			Material::GetGlobalContentRevision(), m_shadowCastersRevision, m_spatialRevision);
	}

	m_publishedSceneVersion = std::move(version);
	m_bHasPendingSceneChanges = false;
}

void LandscapeECS::AppendSceneView(RHI::RHISceneViewPtr& sceneView)
{
	if (!sceneView)
	{
		return;
	}
	// Capture occurs on the world owner, including removals after ECS Tick.
	if (m_bHasPendingSceneChanges && (!GetWorld() || !GetWorld()->IsClearing()))
	{
		PublishSceneVersion();
	}
	sceneView->AddSceneVersion(m_publishedSceneVersion);
}

bool LandscapeECS::CollectBakeGeometrySnapshots(TVector<LandscapeBakeGeometrySnapshot>& outSnapshots,
	std::string& outDiagnostic) const
{
	outSnapshots.Clear();
	outDiagnostic.clear();

	for (size_t componentIndex = 0u; componentIndex < m_components.Num(); ++componentIndex)
	{
		if (!IsComponentRegistered(componentIndex))
		{
			continue;
		}

		const auto& data = m_components[componentIndex];
		GameObjectPtr owner = const_cast<ObjectPtr&>(data.GetOwner()).StaticCast<GameObject>();
		if (!owner || owner->GetMobilityType() == EMobilityType::Dynamic)
		{
			continue;
		}

		const std::string landscapeId = owner->GetInstanceId().ToString();
		const size_t expectedChunks = static_cast<size_t>(data.m_chunksX) * data.m_chunksZ;
		if (data.IsDirty() || data.m_chunks.Num() != expectedChunks)
		{
			outDiagnostic = "landscape '" + owner->GetName() +
							"' is still rebuilding; wait for its chunks and resources before baking";
			return false;
		}
		if (!data.m_runtimeMaterial || !data.m_runtimeMaterial->IsReady())
		{
			outDiagnostic = "landscape '" + owner->GetName() + "' material is not ready for baking";
			return false;
		}

		const glm::mat4 ownerMatrix = owner->GetTransformComponent().GetCachedWorldMatrix();
		for (size_t chunkIndex = 0u; chunkIndex < data.m_chunks.Num(); ++chunkIndex)
		{
			const auto& chunk = data.m_chunks[chunkIndex];
			if (!chunk.m_bakeTriangles || chunk.m_bakeTriangles->IsEmpty() || !chunk.m_localBounds.IsValid() ||
				chunk.m_buildRevision == 0u)
			{
				outDiagnostic = "landscape '" + owner->GetName() +
								"' has an incomplete CPU bake snapshot; rebuild the landscape before baking";
				return false;
			}

			LandscapeBakeGeometrySnapshot terrain;
			terrain.m_sourceId = "landscape:" + landscapeId + ":chunk:" + std::to_string(chunkIndex);
			terrain.m_triangles = chunk.m_bakeTriangles;
			terrain.m_worldMatrix = ownerMatrix;
			terrain.m_worldBounds = chunk.m_localBounds;
			terrain.m_worldBounds.Apply(ownerMatrix);
			terrain.m_materials.Add(data.m_runtimeMaterial);
			terrain.m_sourceRevision = chunk.m_buildRevision;
			outSnapshots.Add(std::move(terrain));

			for (size_t instanceIndex = 0u; instanceIndex < chunk.m_bakeVegetation.Num(); ++instanceIndex)
			{
				const auto& placement = chunk.m_bakeVegetation[instanceIndex];
				if (placement.m_profileIndex >= data.m_vegetationProfiles.Num())
				{
					outDiagnostic = "landscape '" + owner->GetName() + "' contains an invalid vegetation bake profile";
					return false;
				}

				const auto& profile = data.m_vegetationProfiles[placement.m_profileIndex];
				if (!profile.m_model || !profile.m_model->IsStructurallyReady())
				{
					outDiagnostic = "vegetation profile " + std::to_string(placement.m_profileIndex) +
									" on landscape '" + owner->GetName() + "' is not ready for baking";
					return false;
				}
				if (profile.m_settings.m_materialFileId && (!profile.m_material || !profile.m_material->IsReady()))
				{
					outDiagnostic = "vegetation material for profile " + std::to_string(placement.m_profileIndex) +
									" on landscape '" + owner->GetName() + "' is not ready for baking";
					return false;
				}
				if (!profile.m_settings.m_materialFileId && !profile.m_bAreModelMaterialsPublished)
				{
					outDiagnostic = "vegetation model materials for profile " +
									std::to_string(placement.m_profileIndex) + " on landscape '" + owner->GetName() +
									"' are not ready for baking";
					return false;
				}
				for (const MaterialPtr& material : profile.m_modelMaterials)
				{
					if (material && !material->IsReady())
					{
						outDiagnostic = "vegetation model material for profile " +
										std::to_string(placement.m_profileIndex) + " on landscape '" +
										owner->GetName() + "' is not ready for baking";
						return false;
					}
				}

				LandscapeBakeGeometrySnapshot vegetation;
				vegetation.m_sourceId = "landscape:" + landscapeId + ":chunk:" + std::to_string(chunkIndex) +
										":profile:" + std::to_string(placement.m_profileIndex) +
										":instance:" + std::to_string(instanceIndex);
				vegetation.m_model = profile.m_model;
				vegetation.m_meshIndex = profile.m_settings.m_meshIndex;
				vegetation.m_worldMatrix = ownerMatrix * placement.m_transform;
				vegetation.m_worldBounds = profile.m_model->GetBoundsAABB(profile.m_settings.m_meshIndex);
				vegetation.m_worldBounds.Apply(vegetation.m_worldMatrix);
				if (profile.m_settings.m_materialFileId)
				{
					vegetation.m_materials.Add(profile.m_material);
				}
				else
				{
					vegetation.m_materials = profile.m_modelMaterials;
				}
				vegetation.m_sourceRevision = chunk.m_vegetationRevision;
				outSnapshots.Add(std::move(vegetation));
			}
		}
	}

	return true;
}

uint64_t LandscapeECS::GetGlobalIlluminationGeometryRevision() const noexcept
{
	if (!m_publishedSceneVersion || !m_publishedSceneVersion->m_sceneVersion)
	{
		return 0u;
	}
	const RHI::RHISceneVersion& version = *m_publishedSceneVersion->m_sceneVersion;
	uint64_t revision = version.m_staticRevision;
	HashCombine(revision, version.m_stationaryRevision);
	return revision;
}

uint64_t LandscapeECS::GetGlobalIlluminationContributorRevision() const noexcept
{
	uint64_t revision = GetGlobalIlluminationGeometryRevision();
	if (m_publishedSceneVersion && m_publishedSceneVersion->m_sceneVersion)
	{
		HashCombine(revision, m_publishedSceneVersion->m_sceneVersion->m_materialRevision);
	}
	return revision;
}

void LandscapeECS::EndPlay()
{
	for (auto& component : m_components)
	{
		DestroyPhysicsBodies(component);
	}
	ECS::TSystem<LandscapeECS, LandscapeData>::EndPlay();
	m_shadowCastersRevision = 0u;
	m_sceneVersionRevision = 0u;
	m_spatialRevision = 0u;
	m_staticSpatialHash = 0u;
	m_stationarySpatialHash = 0u;
	m_dynamicSpatialHash = 0u;
	m_publishedSceneVersion.Clear();
	m_rhiScene.Clear();
	m_renderInstanceHandles.Clear();
	m_bHasPendingSceneChanges = false;
}
