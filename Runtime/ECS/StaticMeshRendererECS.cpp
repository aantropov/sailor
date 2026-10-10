#include "ECS/StaticMeshRendererECS.h"
#include "ECS/TransformECS.h"
#include "Engine/GameObject.h"
#include "AssetRegistry/Model/ModelImporter.h"
#include "AssetRegistry/Material/MaterialImporter.h"
#include "RHI/Material.h"
#include "Components/AnimatorComponent.h"
#include "Components/MeshRendererComponent.h"
#include "Core/StringHash.h"
#include "GlobalIllumination/GISettings.h"
#include "Settings/GraphicsSettings.h"

#include <algorithm>
#include <cmath>
#include <functional>

using namespace Sailor;
using namespace Sailor::Tasks;

namespace
{
	constexpr uint8_t StaticSpatialChange = 1u << 0u;
	constexpr uint8_t StationarySpatialChange = 1u << 1u;
	constexpr uint8_t DynamicSpatialChange = 1u << 2u;

	uint8_t GetSpatialChangeMask(EMobilityType mobility)
	{
		switch (mobility)
		{
		case EMobilityType::Static:
			return StaticSpatialChange;
		case EMobilityType::Stationary:
			return StationarySpatialChange;
		case EMobilityType::Dynamic:
			return DynamicSpatialChange;
		}
		return DynamicSpatialChange;
	}

	uint64_t CalculateMaterialRenderMetadataSignature(
		const TVector<MaterialPtr>& materials)
	{
		size_t result = Fnv1aOffsetBasis;
		HashCombine(result, materials.Num());
		for (const auto& material : materials)
		{
			HashCombine(result,
				material,
				material ? material->GetRenderMetadataRevision() : 0ull);
		}
		return static_cast<uint64_t>(result);
	}

	bool HasRenderableSelection(const StaticMeshRendererData& data)
	{
		const ModelPtr& model = data.GetModel();
		return model && model->IsReady() &&
			model->GetBoundsAABB(data.GetMeshIndex()).IsValid();
	}

	bool CollectComponentRenderData(
		const StaticMeshRendererData& data,
		const glm::mat4& ownerWorldMatrix,
		TVector<RHI::RHIMeshPtr>& outMeshes,
		TVector<glm::mat4>& outLocalMatrices,
		Math::AABB& outWorldBounds)
	{
		if (!HasRenderableSelection(data))
		{
			return false;
		}

		Math::AABB modelBounds;
		if (!data.GetModel()->CollectRenderData(
				data.GetMeshIndex(),
				outMeshes,
				outLocalMatrices,
				modelBounds))
		{
			return false;
		}

		outWorldBounds = modelBounds;
		outWorldBounds.Apply(ownerWorldMatrix);
		return outWorldBounds.IsValid();
	}

	void GetConservativeOctreeBounds(
		const Math::AABB& bounds,
		glm::ivec3& outCenter,
		glm::ivec3& outExtents)
	{
		const glm::ivec3 minimum = glm::ivec3(glm::floor(bounds.m_min));
		const glm::ivec3 maximum = glm::ivec3(glm::ceil(bounds.m_max));
		outCenter = minimum + (maximum - minimum) / 2;
		outExtents = glm::max(
			maximum - outCenter,
			outCenter - minimum);
		outExtents = glm::max(outExtents, glm::ivec3(1));
	}

	bool AreShadowCastersEqual(
		const RHI::RHIShadowCasterProxy& lhs,
		const RHI::RHIShadowCasterProxy& rhs)
	{
		if (&lhs == &rhs)
		{
			return true;
		}
		if (lhs.m_meshes.Num() != rhs.m_meshes.Num())
		{
			return false;
		}

		for (size_t index = 0; index < lhs.m_meshes.Num(); ++index)
		{
			const auto& lhsMesh = lhs.m_meshes[index];
			const auto& rhsMesh = rhs.m_meshes[index];
			if (lhsMesh.m_mesh != rhsMesh.m_mesh ||
				lhsMesh.m_renderQueueTag != rhsMesh.m_renderQueueTag ||
				lhsMesh.m_baseColorFactor != rhsMesh.m_baseColorFactor ||
				lhsMesh.m_alphaCutoff != rhsMesh.m_alphaCutoff ||
				lhsMesh.m_baseColorSampler != rhsMesh.m_baseColorSampler ||
				lhsMesh.m_maxCameraDistance != rhsMesh.m_maxCameraDistance ||
				lhsMesh.m_customDepthMaterial != rhsMesh.m_customDepthMaterial ||
				lhsMesh.m_customDepthShader != rhsMesh.m_customDepthShader ||
#if defined(__APPLE__)
				lhsMesh.m_materialTextureSamplers != rhsMesh.m_materialTextureSamplers ||
#endif
				!Math::AreExactlyEqual(lhsMesh.m_localMatrix, rhsMesh.m_localMatrix))
			{
				return false;
			}
		}

		return true;
	}

}

uint32_t StaticMeshRendererData::ResolveLod(
	float screenCoverage,
	uint32_t numAvailableLods) const
{
	if (numAvailableLods == 0u)
	{
		return 0u;
	}

	const uint32_t highestAvailableLod = numAvailableLods - 1u;
	const uint32_t minLod = (std::min)(m_minLod, highestAvailableLod);
	const uint32_t maxLod = (std::max)(
		minLod,
		(std::min)(m_maxLod, highestAvailableLod));
	uint32_t selectedLod = 0u;
	const float coverage = (std::clamp)(
		screenCoverage,
		0.0f,
		1.0f);
	for (size_t thresholdIndex = 0;
		thresholdIndex < m_screenCoverageThresholds.Num();
		++thresholdIndex)
	{
		const float threshold = m_screenCoverageThresholds[thresholdIndex];
		if (!std::isfinite(threshold) || coverage >= threshold)
		{
			break;
		}

		selectedLod = static_cast<uint32_t>(thresholdIndex + 1u);
	}

	const int32_t lodBias = App::GetInstance() ?
		App::GetActiveGraphicsSettings().m_lodBias : 0;
	return Settings::ApplyLodBias(
		selectedLod,
		numAvailableLods,
		minLod,
		maxLod,
		lodBias);
}

void StaticMeshRendererData::SetLodSettings(
	uint32_t minLod,
	uint32_t maxLod,
	const TVector<float>& screenCoverageThresholds)
{
	TVector<float> thresholds = screenCoverageThresholds;
	NormalizeLodSettings(minLod, maxLod, thresholds);
	if (m_minLod == minLod && m_maxLod == maxLod &&
		m_screenCoverageThresholds == thresholds)
	{
		return;
	}

	m_minLod = minLod;
	m_maxLod = maxLod;
	m_screenCoverageThresholds = std::move(thresholds);
	MarkDirty();
}

void StaticMeshRendererData::NormalizeLodSettings(
	uint32_t minLod,
	uint32_t& maxLod,
	TVector<float>& screenCoverageThresholds)
{
	maxLod = (std::max)(minLod, maxLod);
	for (float& threshold : screenCoverageThresholds)
	{
		threshold = std::isfinite(threshold) ?
			(std::clamp)(threshold, 0.0f, 1.0f) : 0.0f;
	}
	std::sort(
		screenCoverageThresholds.begin(),
		screenCoverageThresholds.end(),
		std::greater<float>());
}

void StaticMeshRendererECS::BeginPlay()
{
	m_rhiScene = RHI::RHIScenePtr::Make();
	m_lastMaterialContentRevision = Material::GetGlobalContentRevision();
	m_giMaterialRevision = 0;
	PublishSceneVersion();
}

void StaticMeshRendererECS::PublishSceneVersion(uint8_t spatialChangeMask)
{
	SAILOR_PROFILE_FUNCTION();
	spatialChangeMask |= m_pendingSpatialChangeMask;
	auto version = TSharedPtr<RHI::RHISpatialSceneVersion>::Make();
	version->m_revision = ++m_sceneVersionRevision;
	if (spatialChangeMask != 0u || !m_publishedSceneVersion)
	{
		++m_spatialRevision;
	}
	version->m_shadowCastersRevision = m_shadowCastersRevision;
	version->m_bHasCustomDepthShadowCasters = m_bHasCustomDepthShadowCasters;
	version->m_scene = m_rhiScene;
	if (m_rhiScene)
	{
		version->m_sceneVersion = m_rhiScene->PublishVersion(
			m_lastMaterialContentRevision,
			m_shadowCastersRevision,
			m_spatialRevision);

		const bool bRebuildStatic = !m_publishedSceneVersion ||
			(spatialChangeMask & StaticSpatialChange) != 0u;
		const bool bRebuildStationary = !m_publishedSceneVersion ||
			(spatialChangeMask & StationarySpatialChange) != 0u;
		const bool bRebuildDynamic = !m_publishedSceneVersion ||
			(spatialChangeMask & DynamicSpatialChange) != 0u;
		auto rebuildSpatialRoot = [&](const TSharedPtr<const TVector<RHI::RenderInstanceHandle>>& handles)
			{
				SAILOR_PROFILE_SCOPE("Rebuild scene spatial root");
				const size_t numHandles = handles ? handles->Num() : 0u;
				const auto* scheduler = App::GetSubmodule<Tasks::Scheduler>();
				const size_t numWorkers = scheduler ? (std::max)(size_t(1u), size_t(scheduler->GetNumWorkerThreads())) : 1u;
				const size_t numPartitions = (std::max)(size_t(1u),
					(std::min)(numWorkers, (numHandles + 511u) / 512u));
				const size_t partitionSize = (numHandles + numPartitions - 1u) / numPartitions;
				auto index = TSharedPtr<RHI::RHISceneSpatialIndex>::Make(
					glm::ivec3(0), 16536 * 16, 4, numPartitions, (std::max)(size_t(64u), partitionSize));
				auto buildPartition = [&](size_t partition)
					{
						SAILOR_PROFILE_SCOPE("Build scene spatial partition");
						const size_t end = (std::min)(numHandles, (partition + 1u) * partitionSize);
						for (size_t i = partition * partitionSize; i < end; ++i)
						{
							const auto handle = (*handles)[i];
							const RHI::RHISceneInstanceRecord* record = nullptr;
							if (!version->m_sceneVersion->Resolve(handle, record) || !record)
							{
								continue;
							}
							glm::ivec3 center{}, extents{};
							GetConservativeOctreeBounds(record->m_worldBounds, center, extents);
							index->Update(center, extents, handle, partition);
						}
					};
				if (numPartitions == 1u)
				{
					buildPartition(0u);
				}
				else
				{
					TVector<Tasks::ITaskPtr> tasks;
					for (size_t partition = 0u; partition < numPartitions; ++partition)
					{
						auto task = Tasks::CreateTask("Build scene spatial partition"_h,
							[&, partition]() { buildPartition(partition); }, EThreadType::Worker);
						task->Run();
						tasks.Add(task);
					}
					for (auto& task : tasks)
					{
						task->Wait();
					}
				}
				return index;
			};
		version->m_staticOctree = bRebuildStatic ?
			rebuildSpatialRoot(version->m_sceneVersion->m_staticHandles) : m_publishedSceneVersion->m_staticOctree;
		version->m_stationaryOctree = bRebuildStationary ?
			rebuildSpatialRoot(version->m_sceneVersion->m_stationaryHandles) : m_publishedSceneVersion->m_stationaryOctree;
		version->m_dynamicOctree = bRebuildDynamic ?
			rebuildSpatialRoot(version->m_sceneVersion->m_dynamicHandles) : m_publishedSceneVersion->m_dynamicOctree;
	}
	m_publishedSceneVersion = std::move(version);
	m_pendingSpatialChangeMask = 0u;
	m_bHasPendingSceneChanges = false;
}

void StaticMeshRendererECS::MarkDirty(GameObjectPtr owner)
{
	if (!owner)
	{
		return;
	}

	for (const auto& component : owner->GetComponents())
	{
		if (auto meshRenderer = component.DynamicCast<MeshRendererComponent>())
		{
			const size_t componentIndex = meshRenderer->GetComponentIndex();
			if (IsComponentRegistered(componentIndex))
			{
				m_components[componentIndex].MarkDirty();
			}
		}
	}
}

uint64_t StaticMeshRendererECS::GetGlobalIlluminationGeometryRevision() const noexcept
{
	if (!m_publishedSceneVersion ||
		!m_publishedSceneVersion->m_sceneVersion)
	{
		return 0u;
	}
	const RHI::RHISceneVersion& version =
		*m_publishedSceneVersion->m_sceneVersion;
	uint64_t revision = version.m_staticRevision;
	HashCombine(revision, version.m_stationaryRevision);
	return revision;
}

uint64_t StaticMeshRendererECS::GetGlobalIlluminationContributorRevision() const noexcept
{
	uint64_t revision = GetGlobalIlluminationGeometryRevision();
	if (m_publishedSceneVersion && m_publishedSceneVersion->m_sceneVersion)
	{
		HashCombine(revision, m_publishedSceneVersion->m_sceneVersion->m_materialRevision,
			m_giMaterialRevision);
	}
	return revision;
}

void StaticMeshRendererECS::OnComponentUnregistered(size_t index, StaticMeshRendererData& component)
{
	uint8_t spatialChangeMask = 0u;
	RHI::RenderInstanceHandle* renderHandle = nullptr;
	if (m_rhiScene && m_renderInstanceHandles.Find(index, renderHandle) && renderHandle)
	{
		RHI::RHISceneInstanceRecord previousRecord;
		if (m_rhiScene->ResolveCurrent(*renderHandle, previousRecord))
		{
			spatialChangeMask |= GetSpatialChangeMask(previousRecord.m_mobility);
		}
		m_rhiScene->RemoveInstance(*renderHandle);
		m_renderInstanceHandles.Remove(index);
	}

	if (component.m_shadowCaster)
	{
		component.m_shadowCaster.Clear();
		++m_shadowCastersRevision;
	}
	// Tick or the next scene capture publishes the batch, not each removal.
	if (!GetWorld() || !GetWorld()->IsClearing())
	{
		m_pendingSpatialChangeMask |= spatialChangeMask;
		m_bHasPendingSceneChanges = true;
	}
}

void StaticMeshRendererECS::Tick(float deltaTime)
{
	constexpr uint32_t NumDirtyComponentsPerTask = 512;

	SAILOR_PROFILE_FUNCTION();

	const uint64_t materialContentRevision = Material::GetGlobalContentRevision();
	const bool bCheckMaterialRevisions = materialContentRevision != m_lastMaterialContentRevision;
	bool bHasCustomDepthShadowCasters = false;
	auto needsUpdate = [this, bCheckMaterialRevisions](size_t componentIndex, bool& bHasCustomDepthShadowCasters)
	{
		if (!IsComponentRegistered(componentIndex))
		{
			return false;
		}

		auto& registeredData = m_components[componentIndex];
		if (registeredData.ShouldCastShadow())
		{
			for (const auto& material : registeredData.GetMaterials())
			{
				if (material && material->GetRenderState().IsRequiredCustomDepthShader())
				{
					bHasCustomDepthShadowCasters = true;
					break;
				}
			}
		}

		auto& data = m_components[componentIndex];
		// A transform evaluated at frame zero is valid. Publication is tracked by
		// the scene handle, not by the last observed transform timestamp.
		bool bNeedsUpdate = data.m_bIsDirty ||
			(data.GetModel() && !m_renderInstanceHandles.ContainsKey(componentIndex));
		if (data.m_owner)
		{
			// The ECS owner handle stays valid until these workers join. Borrow it
			// to avoid per-mesh contention on the world's shared allocator.
			auto* owner = static_cast<GameObject*>(data.m_owner.GetRawPtr());
			bNeedsUpdate |= owner->GetTransformComponent().GetFrameLastChange() > data.m_frameLastChange;
		}

		if (!bNeedsUpdate && bCheckMaterialRevisions)
		{
			bool bMaterialsChanged =
				data.m_materialContentRevisions.Num() != data.GetMaterials().Num() + 1u;
			for (size_t materialIndex = 0; !bMaterialsChanged && materialIndex < data.GetMaterials().Num(); ++materialIndex)
			{
				const auto& material = data.GetMaterials()[materialIndex];
				const uint64_t currentRevision = material ? material->GetContentRevision() : 0ull;
				bMaterialsChanged = data.m_materialContentRevisions[materialIndex] != currentRevision;
			}

			bNeedsUpdate = bMaterialsChanged;
		}

		return bNeedsUpdate;
	};

	const size_t currentFrame = GetWorld()->GetCurrentFrame();
	// Dirty-proxy workers only read the last published scene. Keep that immutable
	// version alive through the join instead of serializing every record lookup
	// on the mutable scene's lock.
	const auto previousSceneVersion = m_rhiScene ?
		m_rhiScene->GetCurrentVersion() : RHI::RHISceneVersionPtr{};
	auto prepareProxyUpdate = [this, &previousSceneVersion](size_t componentIndex)
		{
			PreparedProxyUpdate result;
			result.m_componentIndex = componentIndex;
			if (!IsComponentRegistered(componentIndex))
			{
				return result;
			}

			auto& data = m_components[componentIndex];
			ObjectPtr ownerObject = data.GetOwner();
			GameObjectPtr owner = ownerObject.StaticCast<GameObject>();
			if (!owner || !data.GetModel())
			{
				return result;
			}

			const bool bTopologyDirty = data.m_bIsDirty ||
				!m_renderInstanceHandles.ContainsKey(componentIndex);
			const bool bTransformDirty = owner->GetTransformComponent().GetFrameLastChange() > data.m_frameLastChange;
			const bool bMaterialsDirty =
				data.m_materialContentRevisions.Num() != data.GetMaterials().Num() + 1u ||
				data.m_materialContentRevisions[data.GetMaterials().Num()] !=
					CalculateMaterialRenderMetadataSignature(data.GetMaterials());
			if (bTransformDirty)
			{
				result.m_changeMask |= RHI::ToMask(RHI::ESceneChangeBit::Transform) |
					RHI::ToMask(RHI::ESceneChangeBit::Bounds);
			}
			if (bTopologyDirty)
			{
				result.m_changeMask |= RHI::ToMask(RHI::ESceneChangeBit::MeshOrLodTopology) |
					RHI::ToMask(RHI::ESceneChangeBit::Material) |
					RHI::ToMask(RHI::ESceneChangeBit::RenderState) |
					RHI::ToMask(RHI::ESceneChangeBit::ShadowState) |
					RHI::ToMask(RHI::ESceneChangeBit::Bounds);
			}
			else if (bMaterialsDirty)
			{
				result.m_changeMask |= RHI::ToMask(RHI::ESceneChangeBit::Material) |
					RHI::ToMask(RHI::ESceneChangeBit::RenderState) |
					RHI::ToMask(RHI::ESceneChangeBit::ShadowState);
			}
			const ModelPtr& model = data.GetModel();
			if (!model->IsReady())
			{
				result.m_state = EPreparedProxyState::Pending;
				return result;
			}

			const bool bInvalidMeshIndex = data.GetMeshIndex() != Model::AllMeshes &&
				!model->IsSourceMeshIndexValid(data.GetMeshIndex());
			if (bInvalidMeshIndex)
			{
				if (!data.m_bInvalidMeshIndexReported)
				{
					SAILOR_LOG(
						"MeshRenderer ignored invalid glTF mesh index %d for model %s.",
						data.GetMeshIndex(),
						model->GetFileId().ToString().c_str());
					data.m_bInvalidMeshIndexReported = true;
				}
				return result;
			}
			data.m_bInvalidMeshIndexReported = false;

			if (data.GetMaterials().IsEmpty())
			{
				return result;
			}
			for (const auto& material : data.GetMaterials())
			{
				if (!material)
				{
					result.m_state = EPreparedProxyState::Pending;
					return result;
				}
				if (!material->IsReady())
				{
					result.m_state = !bTopologyDirty && !bTransformDirty && !bMaterialsDirty ?
						EPreparedProxyState::PendingMaterialVersion : EPreparedProxyState::Pending;
					return result;
				}
			}
			if (!bTopologyDirty && !bTransformDirty && !bMaterialsDirty)
			{
				// Uniform-only changes publish a new immutable RHI material version.
				// Scene topology and instance records remain valid and are selected by
				// the submission's material cutoff during packet construction.
				result.m_state = EPreparedProxyState::MaterialVersionOnly;
				return result;
			}

			const auto& ownerTransform = owner->GetTransformComponent();
			const glm::mat4& ownerWorldMatrix = ownerTransform.GetCachedWorldMatrix();
			if (auto animator = owner->GetComponent<AnimatorComponent>())
			{
				result.m_skeletonOffset = animator->GetSkeletonOffset();
			}

			// Transform-only updates retain the published local mesh/material topology.
			if (!bTopologyDirty && !bMaterialsDirty && m_rhiScene)
			{
				RHI::RenderInstanceHandle* renderHandle = nullptr;
				const RHI::RHISceneInstanceRecord* previousRecord = nullptr;
				if (m_renderInstanceHandles.Find(componentIndex, renderHandle) &&
					renderHandle &&
					previousSceneVersion &&
					previousSceneVersion->Resolve(*renderHandle, previousRecord) && previousRecord)
				{
					const auto* previousResource = dynamic_cast<const RHI::RHISceneProxyResource*>(
						previousRecord->m_topology.GetRawPtr());
					Math::AABB worldBounds = model->GetBoundsAABB(data.GetMeshIndex());
					worldBounds.Apply(ownerWorldMatrix);
					if (previousResource && worldBounds.IsValid())
					{
						result.m_shadowCaster = data.m_shadowCaster;
						result.m_state = owner->GetMobilityType() == EMobilityType::Static ?
							EPreparedProxyState::Static : EPreparedProxyState::Stationary;
						result.m_sceneUpdate.m_handle = *renderHandle;
						auto& record = result.m_sceneUpdate.m_record;
						// Retain immutable mesh/material metadata; never expose ECS
						// storage to the next render submission. Binding generations
						// are retained separately by its material publication cutoff.
						record = *previousRecord;
						record.m_mobility = owner->GetMobilityType();
						record.m_worldMatrix = ownerWorldMatrix;
						record.m_worldBounds = worldBounds;
						record.m_skeletonOffset = result.m_skeletonOffset;
						if (record.m_mobility != previousRecord->m_mobility)
						{
							result.m_changeMask |= RHI::ToMask(RHI::ESceneChangeBit::Mobility);
						}
						result.m_spatialChangeMask = GetSpatialChangeMask(previousRecord->m_mobility) |
							GetSpatialChangeMask(record.m_mobility);
						result.m_sceneUpdate.m_changeMask = result.m_changeMask;
						result.m_bStateOnly = true;
						return result;
					}
				}
			}

			TVector<RHI::RHIMeshPtr> selectedMeshes;
			TVector<glm::mat4> selectedMatrices;
			auto& record = result.m_sceneUpdate.m_record;
			record.m_producerKey = componentIndex;
			record.m_mobility = owner->GetMobilityType();
			record.m_worldMatrix = ownerWorldMatrix;
			record.m_skeletonOffset = result.m_skeletonOffset;
			record.m_renderFlags = data.ShouldCastShadow() ? 1u : 0u;
			if (!CollectComponentRenderData(
					data,
					ownerWorldMatrix,
					selectedMeshes,
					selectedMatrices,
					record.m_worldBounds))
			{
				return result;
			}

			auto shadowCaster = TSharedPtr<RHI::RHIShadowCasterProxy>::Make();
			shadowCaster->m_meshes.Reserve(selectedMeshes.Num());

			result.m_state = owner->GetMobilityType() == EMobilityType::Static ?
				EPreparedProxyState::Static : EPreparedProxyState::Stationary;
			auto& proxy = result.m_staticProxy.emplace();
			proxy.m_meshes = std::move(selectedMeshes);
			proxy.m_meshModelMatrices = std::move(selectedMatrices);
			proxy.m_lodPolicy.m_bEnabled = true;
			proxy.m_lodPolicy.m_minLod = data.m_minLod;
			proxy.m_lodPolicy.m_maxLod = data.m_maxLod;
			proxy.m_lodPolicy.m_screenCoverageThresholds = data.m_screenCoverageThresholds;
			proxy.m_overrideMaterials.Reserve(proxy.m_meshes.Num());
			proxy.m_renderQueueTags.Reserve(proxy.m_meshes.Num());
			proxy.m_baseColorFactors.Reserve(proxy.m_meshes.Num());
			proxy.m_baseColorSamplers.Reserve(proxy.m_meshes.Num());
			proxy.m_alphaCutoffs.Reserve(proxy.m_meshes.Num());
#if defined(__APPLE__)
			proxy.m_materialTextureSamplers.Reserve(proxy.m_meshes.Num());
#endif
			for (size_t meshIndex = 0; meshIndex < proxy.m_meshes.Num(); ++meshIndex)
			{
				const auto& mesh = proxy.m_meshes[meshIndex];
				const size_t materialIndex = mesh->ResolveMaterialIndex(meshIndex, data.GetMaterials().Num());
				auto material = data.GetMaterials()[materialIndex];
				auto rhiMaterial = material->GetOrAddRHI(mesh->m_vertexDescription);
				const auto& metadata = material->GetRenderMetadata();
				metadata.AppendTo(proxy, rhiMaterial);
				metadata.AppendShadowMesh(*shadowCaster, mesh, proxy.m_meshModelMatrices[meshIndex], rhiMaterial);
			}
			if (!shadowCaster->m_meshes.IsEmpty()) result.m_shadowCaster = std::move(shadowCaster);

			return result;
		};

	auto& batches = m_preparedBatchesScratch;
	const size_t numBatches = (m_components.Num() + NumDirtyComponentsPerTask - 1u) / NumDirtyComponentsPerTask;
	batches.Resize(numBatches);
	auto prepareBatch = [&](size_t batchIndex)
		{
			SAILOR_PROFILE_SCOPE("Prepare dirty mesh proxies");
			auto& batch = batches[batchIndex];
			batch.m_updates.Clear(false);
			batch.m_sceneUpdates.Clear(false);
			batch.m_bHasCustomDepthShadowCasters = false;
			const size_t end = (std::min)(m_components.Num(), (batchIndex + 1u) * NumDirtyComponentsPerTask);
			for (size_t index = batchIndex * NumDirtyComponentsPerTask; index < end; ++index)
			{
				if (needsUpdate(index, batch.m_bHasCustomDepthShadowCasters))
				{
					auto update = prepareProxyUpdate(index);
					if (update.m_bStateOnly)
					{
						batch.m_sceneUpdates.Add(std::move(update.m_sceneUpdate));
					}
					batch.m_updates.Add(std::move(update));
				}
			}
		};
	if (numBatches <= 1u || !App::GetSubmodule<Tasks::Scheduler>())
	{
		for (size_t index = 0u; index < numBatches; ++index)
		{
			prepareBatch(index);
		}
	}
	else
	{
		auto& tasks = m_prepareTasksScratch;
		tasks.Clear(false);
		tasks.Reserve(numBatches);
		for (size_t index = 0u; index < numBatches; ++index)
		{
			auto task = Tasks::CreateTask("StaticMeshRendererECS:Prepare Dirty Proxies"_h,
				[&, index]() { prepareBatch(index); }, EThreadType::Worker);
			task->Run();
			tasks.Add(task);
		}
		for (auto& task : tasks)
		{
			task->Wait();
		}
	}

	bool bShadowCastersChanged = false;
	bool bSceneRecordsChanged = false;
	bool bMaterialVersionsPending = false;
	bool bGIMaterialsChanged = false;
	const bool bPreviousHasCustomDepthShadowCasters =
		m_bHasCustomDepthShadowCasters;
	uint8_t spatialChangeMask = 0u;
	{
		SAILOR_PROFILE_SCOPE("Apply prepared mesh batches");
		for (auto& batch : batches)
		{
			bHasCustomDepthShadowCasters |= batch.m_bHasCustomDepthShadowCasters;
			for (auto& update : batch.m_updates)
			{
				if (!IsComponentRegistered(update.m_componentIndex))
				{
					continue;
				}

				auto& data = m_components[update.m_componentIndex];
				auto cacheMaterialRevisions = [&data]()
					{
						data.m_materialContentRevisions.Resize(data.GetMaterials().Num() + 1u);
						for (size_t materialIndex = 0u;
							materialIndex < data.GetMaterials().Num(); ++materialIndex)
						{
							const auto& material = data.GetMaterials()[materialIndex];
							data.m_materialContentRevisions[materialIndex] = material ?
								material->GetContentRevision() : 0ull;
						}
						data.m_materialContentRevisions[data.GetMaterials().Num()] =
							CalculateMaterialRenderMetadataSignature(data.GetMaterials());
					};
				if (update.m_state == EPreparedProxyState::Pending)
				{
					data.m_bIsDirty = true;
					continue;
				}
				if (update.m_state == EPreparedProxyState::PendingMaterialVersion)
				{
					bMaterialVersionsPending = true;
					continue;
				}
				if (update.m_state == EPreparedProxyState::MaterialVersionOnly)
				{
					const auto owner = data.m_owner.StaticCast<GameObject>();
					bGIMaterialsChanged |= IsGlobalIlluminationBakeContributor(owner->GetMobilityType());
					cacheMaterialRevisions();
					continue;
				}

				if (update.m_state == EPreparedProxyState::Remove)
				{
					if (data.m_shadowCaster)
					{
						data.m_shadowCaster.Clear();
						bShadowCastersChanged = true;
					}
					data.m_materialContentRevisions.Clear();
					data.m_bIsDirty = false;
					RHI::RenderInstanceHandle* renderHandle = nullptr;
					if (m_rhiScene && m_renderInstanceHandles.Find(update.m_componentIndex, renderHandle) && renderHandle)
					{
						RHI::RHISceneInstanceRecord previousRecord;
						if (m_rhiScene->ResolveCurrent(*renderHandle, previousRecord))
						{
							spatialChangeMask |= GetSpatialChangeMask(previousRecord.m_mobility);
						}
						bSceneRecordsChanged |= m_rhiScene->RemoveInstance(*renderHandle);
						m_renderInstanceHandles.Remove(update.m_componentIndex);
					}
					continue;
				}

				if (update.m_bStateOnly)
				{
					bShadowCastersChanged |= data.ShouldCastShadow();
					spatialChangeMask |= update.m_spatialChangeMask;
				}
				else
				{
					const auto shadowInstanceChanges = RHI::ToMask(RHI::ESceneChangeBit::Transform) |
						RHI::ToMask(RHI::ESceneChangeBit::Bounds) | RHI::ToMask(RHI::ESceneChangeBit::Mobility) |
						RHI::ToMask(RHI::ESceneChangeBit::SkeletonOffset) | RHI::ToMask(RHI::ESceneChangeBit::ShadowState);
					bShadowCastersChanged |= data.ShouldCastShadow() && (update.m_changeMask & shadowInstanceChanges) != 0u;
					if (data.m_shadowCaster && update.m_shadowCaster &&
						AreShadowCastersEqual(*data.m_shadowCaster, *update.m_shadowCaster))
					{
						update.m_shadowCaster = data.m_shadowCaster;
					}
					else if (data.m_shadowCaster != update.m_shadowCaster)
					{
						data.m_shadowCaster = update.m_shadowCaster;
						bShadowCastersChanged = true;
					}

					update.m_staticProxy->m_shadowCaster = data.m_shadowCaster;
					if (m_rhiScene)
					{
						auto& sceneRecord = update.m_sceneUpdate.m_record;
						sceneRecord.m_topologyRevision = currentFrame;
						sceneRecord.m_materialRevision = Material::GetGlobalContentRevision();

						RHI::RenderInstanceHandle* renderHandle = nullptr;
						if (m_renderInstanceHandles.Find(update.m_componentIndex, renderHandle) && renderHandle)
						{
							RHI::RHISceneInstanceRecord previousRecord;
							const bool bResolvedPrevious =
								m_rhiScene->ResolveCurrent(*renderHandle, previousRecord);
							if (bResolvedPrevious)
							{
								const RHI::SceneChangeMask topologyChanges =
									RHI::ToMask(RHI::ESceneChangeBit::MeshOrLodTopology) |
									RHI::ToMask(RHI::ESceneChangeBit::Material) |
									RHI::ToMask(RHI::ESceneChangeBit::RenderState) |
									RHI::ToMask(RHI::ESceneChangeBit::ShadowState);
								const bool bCanReuseTopology = (update.m_changeMask & topologyChanges) == 0u;
								if (bCanReuseTopology)
								{
									sceneRecord.m_topology = previousRecord.m_topology;
									sceneRecord.m_topologyRevision = previousRecord.m_topologyRevision;
									if ((update.m_changeMask &
										RHI::ToMask(RHI::ESceneChangeBit::Material)) == 0u)
									{
										sceneRecord.m_materialRevision = previousRecord.m_materialRevision;
									}
								}
								else
								{
									update.m_changeMask |=
										RHI::ToMask(RHI::ESceneChangeBit::MeshOrLodTopology);
									sceneRecord.m_topology =
										RHI::RHISceneProxyResourcePtr::Make(std::move(*update.m_staticProxy));
								}
								if (previousRecord.m_mobility != sceneRecord.m_mobility)
								{
									update.m_changeMask |= RHI::ToMask(RHI::ESceneChangeBit::Mobility);
								}
								if (const auto resource = sceneRecord.m_topology.DynamicCast<const RHI::RHISceneProxyResource>())
								{
									sceneRecord.m_shadowRevision = resource->m_shadowRevision;
								}
							}
							const RHI::SceneChangeMask spatialChanges =
								RHI::ToMask(RHI::ESceneChangeBit::Transform) |
								RHI::ToMask(RHI::ESceneChangeBit::Bounds) |
								RHI::ToMask(RHI::ESceneChangeBit::Mobility);
							const bool bUpdated = m_rhiScene->UpdateInstance(
								*renderHandle,
								sceneRecord,
								update.m_changeMask);
							bSceneRecordsChanged |= bUpdated;
							if (bUpdated && (update.m_changeMask & spatialChanges) != 0u)
							{
								if (bResolvedPrevious)
								{
									spatialChangeMask |= GetSpatialChangeMask(previousRecord.m_mobility);
								}
								spatialChangeMask |= GetSpatialChangeMask(sceneRecord.m_mobility);
							}
						}
						else
						{
							sceneRecord.m_topology =
								RHI::RHISceneProxyResourcePtr::Make(std::move(*update.m_staticProxy));
							if (const auto resource = sceneRecord.m_topology.DynamicCast<const RHI::RHISceneProxyResource>())
							{
								sceneRecord.m_shadowRevision = resource->m_shadowRevision;
							}
							m_renderInstanceHandles[update.m_componentIndex] = m_rhiScene->AddInstance(sceneRecord);
							bSceneRecordsChanged = true;
							spatialChangeMask |= GetSpatialChangeMask(sceneRecord.m_mobility);
						}
					}

				}

				data.m_skeletonOffset = update.m_skeletonOffset;
				if (!update.m_bStateOnly || bCheckMaterialRevisions)
				{
					cacheMaterialRevisions();
				}

				ObjectPtr ownerObject = data.GetOwner();
				GameObjectPtr owner = ownerObject.StaticCast<GameObject>();
				if (owner)
				{
					data.m_frameLastChange = owner->GetTransformComponent().GetFrameLastChange();
					if ((update.m_state == EPreparedProxyState::Static && data.m_frameLastChange == 0) ||
						data.m_frameLastChange != owner->GetFrameLastChange())
					{
						UpdateGameObject(owner, currentFrame);
					}
				}
				data.m_bIsDirty = false;
			}
			if (m_rhiScene && !batch.m_sceneUpdates.IsEmpty())
			{
				bSceneRecordsChanged |= m_rhiScene->UpdateInstances(batch.m_sceneUpdates) != 0u;
			}
		}
	}

	if (!bMaterialVersionsPending)
	{
		m_lastMaterialContentRevision = materialContentRevision;
	}
	if (bGIMaterialsChanged)
	{
		// Uniform-only edits leave the RHI scene intact but still invalidate GI.
		++m_giMaterialRevision;
	}

	if (bShadowCastersChanged)
	{
		++m_shadowCastersRevision;
	}
	m_bHasCustomDepthShadowCasters = bHasCustomDepthShadowCasters;
	if (m_bHasPendingSceneChanges || bSceneRecordsChanged || bShadowCastersChanged ||
		bPreviousHasCustomDepthShadowCasters != m_bHasCustomDepthShadowCasters)
	{
		PublishSceneVersion(spatialChangeMask);
	}
}

void StaticMeshRendererECS::CopySceneView(RHI::RHISceneViewPtr& outProxies)
{
	SAILOR_PROFILE_FUNCTION();

	// Pending destruction runs after ECS Tick; capture still runs on the world owner.
	if (m_bHasPendingSceneChanges && (!GetWorld() || !GetWorld()->IsClearing()))
	{
		PublishSceneVersion(0u);
	}
	outProxies->AddSceneVersion(m_publishedSceneVersion);
}

void StaticMeshRendererECS::EndPlay()
{
	ECS::TSystem<StaticMeshRendererECS, StaticMeshRendererData>::EndPlay();
	m_publishedSceneVersion.Clear();
	m_rhiScene.Clear();
	m_renderInstanceHandles.Clear();
	m_sceneVersionRevision = 0ull;
	m_spatialRevision = 0ull;
	m_shadowCastersRevision = 0ull;
	m_giMaterialRevision = 0;
	m_preparedBatchesScratch.Clear();
	m_prepareTasksScratch.Clear();
	m_bHasCustomDepthShadowCasters = false;
	m_pendingSpatialChangeMask = 0u;
	m_bHasPendingSceneChanges = false;
}
