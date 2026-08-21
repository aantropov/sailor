#include "ECS/StaticMeshRendererECS.h"
#include "ECS/TransformECS.h"
#include "Engine/GameObject.h"
#include "AssetRegistry/Model/ModelImporter.h"
#include "AssetRegistry/Material/MaterialImporter.h"
#include "AssetRegistry/Texture/TextureImporter.h"
#include "RHI/Material.h"
#include "Components/AnimatorComponent.h"
#include "Components/MeshRendererComponent.h"

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
		size_t result = 1469598103934665603ull;
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
		TVector<glm::mat4>& outWorldMatrices,
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
				outWorldMatrices,
				modelBounds))
		{
			return false;
		}

		for (glm::mat4& modelMatrix : outWorldMatrices)
		{
			modelMatrix = ownerWorldMatrix * modelMatrix;
		}

		outWorldBounds = modelBounds;
		outWorldBounds.Apply(ownerWorldMatrix);
		return outWorldBounds.IsValid();
	}

	bool AreMatricesExactlyEqual(const glm::mat4& lhs, const glm::mat4& rhs)
	{
		for (glm::length_t column = 0; column < lhs.length(); ++column)
		{
			for (glm::length_t row = 0; row < lhs[column].length(); ++row)
			{
				if (lhs[column][row] != rhs[column][row])
				{
					return false;
				}
			}
		}

		return true;
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
		if (lhs.m_staticMeshEcs != rhs.m_staticMeshEcs ||
			lhs.m_worldAabb != rhs.m_worldAabb ||
			lhs.m_skeletonOffset != rhs.m_skeletonOffset ||
			lhs.m_meshes.Num() != rhs.m_meshes.Num())
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
				!AreMatricesExactlyEqual(lhsMesh.m_worldMatrix, rhsMesh.m_worldMatrix))
			{
				return false;
			}
		}

		return true;
	}

	RHI::RHIShadowCasterProxyPtr CreateShadowCasterProxy(
		StaticMeshRendererData& data,
		size_t componentIndex,
		const TVector<RHI::RHIMeshPtr>& meshes,
		const TVector<glm::mat4>& matrices,
		const Math::AABB& worldBounds,
		const glm::mat4& ownerWorldMatrix,
		uint32_t skeletonOffset,
		size_t currentFrame)
	{
		const size_t opaqueQueueTag = GetHash(std::string("Opaque"));
		const size_t maskedQueueTag = GetHash(std::string("Masked"));
		auto textureImporter = App::GetSubmodule<TextureImporter>();

		auto shadowCaster = RHI::RHIShadowCasterProxyPtr::Make();
		shadowCaster->m_staticMeshEcs = componentIndex;
		shadowCaster->m_worldAabb = worldBounds;
		shadowCaster->m_skeletonOffset = skeletonOffset;
		shadowCaster->m_frame = currentFrame;
		shadowCaster->m_meshes.Reserve(meshes.Num());

		for (size_t meshIndex = 0; meshIndex < meshes.Num(); ++meshIndex)
		{
			const size_t materialIndex = meshes[meshIndex]->ResolveMaterialIndex(
				meshIndex,
				data.GetMaterials().Num());
			auto material = data.GetMaterials()[materialIndex];
			const size_t renderQueueTag = material ? material->GetRenderState().GetTag() : 0u;
			if (renderQueueTag != opaqueQueueTag && renderQueueTag != maskedQueueTag)
			{
				continue;
			}

			RHI::RHIShadowMeshProxy shadowMesh;
			shadowMesh.m_mesh = meshes[meshIndex];
			shadowMesh.m_worldMatrix = matrices.Num() > meshIndex ? matrices[meshIndex] : ownerWorldMatrix;
			shadowMesh.m_renderQueueTag = renderQueueTag;
			if (material->GetRenderState().IsRequiredCustomDepthShader())
			{
				shadowMesh.m_customDepthMaterial = material->GetOrAddRHI(
					meshes[meshIndex]->m_vertexDescription);
				shadowMesh.m_customDepthShader = material->GetShader();
			}
			if (renderQueueTag == maskedQueueTag)
			{
				const glm::vec4* baseColorFactor = nullptr;
				if (!material->GetUniformsVec4().Find("material.baseColorFactor", baseColorFactor))
				{
					material->GetUniformsVec4().Find("material.albedo", baseColorFactor);
				}
				if (baseColorFactor)
				{
					shadowMesh.m_baseColorFactor = *baseColorFactor;
				}

				const float* alphaCutoff = nullptr;
				if (material->GetUniformsFloat().Find("material.alphaCutoff", alphaCutoff) && alphaCutoff)
				{
					shadowMesh.m_alphaCutoff = *alphaCutoff;
				}

				const TexturePtr* baseColorTexture = nullptr;
				if (!material->GetSamplers().Find("baseColorSampler", baseColorTexture))
				{
					material->GetSamplers().Find("albedoSampler", baseColorTexture);
				}
				if (textureImporter && baseColorTexture && *baseColorTexture)
				{
					shadowMesh.m_baseColorSampler = (uint32_t)textureImporter->GetTextureIndex((*baseColorTexture)->GetFileId());
				}
#if defined(__APPLE__)
				if (!shadowMesh.m_customDepthMaterial)
				{
					shadowMesh.m_materialTextureSamplers.Insert(0u);
					shadowMesh.m_materialTextureSamplers.Insert(shadowMesh.m_baseColorSampler);
				}
#endif
			}
#if defined(__APPLE__)
			if (shadowMesh.m_customDepthMaterial)
			{
				shadowMesh.m_materialTextureSamplers.Insert(0u);
				if (textureImporter)
				{
					for (const auto& sampler : material->GetSamplers())
					{
						const uint32_t textureIndex = sampler.m_second ?
							(uint32_t)textureImporter->GetTextureIndex(sampler.m_second->GetFileId()) : 0u;
						shadowMesh.m_materialTextureSamplers.Insert(textureIndex);
					}
				}
			}
#endif
			shadowCaster->m_meshes.Add(std::move(shadowMesh));
		}

		return shadowCaster->m_meshes.IsEmpty() ? RHI::RHIShadowCasterProxyPtr{} : shadowCaster;
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

	return (std::clamp)(selectedLod, minLod, maxLod);
}

void StaticMeshRendererData::SetLodSettings(
	uint32_t minLod,
	uint32_t maxLod,
	const TVector<float>& screenCoverageThresholds)
{
	m_minLod = minLod;
	m_maxLod = (std::max)(minLod, maxLod);
	m_screenCoverageThresholds = screenCoverageThresholds;
	for (float& threshold : m_screenCoverageThresholds)
	{
		threshold = std::isfinite(threshold) ?
			(std::clamp)(threshold, 0.0f, 1.0f) : 0.0f;
	}
	std::sort(
		m_screenCoverageThresholds.begin(),
		m_screenCoverageThresholds.end(),
		std::greater<float>());
}

void StaticMeshRendererECS::BeginPlay()
{
	m_rhiScene = RHI::RHIScenePtr::Make();
	m_lastMaterialContentRevision = Material::GetGlobalContentRevision();
	PublishSceneVersion();
}

void StaticMeshRendererECS::PublishSceneVersion(uint8_t spatialChangeMask)
{
	auto version = RHI::RHISpatialSceneVersionPtr::Make();
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
		if (bRebuildStatic)
		{
			version->m_staticOctree = TSharedPtr<TOctree<RHI::RenderInstanceHandle>>::Make(
				glm::ivec3(0, 0, 0), 16536 * 16, 4);
		}
		if (!bRebuildStatic)
		{
			version->m_staticOctree = m_publishedSceneVersion->m_staticOctree;
		}
		if (bRebuildStationary)
		{
			version->m_stationaryOctree = TSharedPtr<TOctree<RHI::RenderInstanceHandle>>::Make(
				glm::ivec3(0, 0, 0), 16536 * 16, 4);
		}
		if (!bRebuildStationary)
		{
			version->m_stationaryOctree = m_publishedSceneVersion->m_stationaryOctree;
		}
		if (bRebuildDynamic)
		{
			version->m_dynamicOctree = TSharedPtr<TOctree<RHI::RenderInstanceHandle>>::Make(
				glm::ivec3(0, 0, 0), 16536 * 16, 4);
		}
		if (!bRebuildDynamic)
		{
			version->m_dynamicOctree = m_publishedSceneVersion->m_dynamicOctree;
		}

		auto rebuildSpatialRoot = [&](const TSharedPtr<TVector<RHI::RenderInstanceHandle>>& handles,
			const TSharedPtr<TOctree<RHI::RenderInstanceHandle>>& octree)
			{
				if (!handles || !octree)
				{
					return;
				}
				for (const auto& handle : *handles)
				{
					const RHI::RHISceneInstanceRecord* record = nullptr;
					if (!version->m_sceneVersion->Resolve(handle, record) || !record)
					{
						continue;
					}
					glm::ivec3 octreeCenter{};
					glm::ivec3 octreeExtents{};
					GetConservativeOctreeBounds(
						record->m_worldBounds,
						octreeCenter,
						octreeExtents);
					octree->Update(octreeCenter, octreeExtents, handle);
				}
			};
		if (bRebuildStatic)
		{
			rebuildSpatialRoot(version->m_sceneVersion->m_staticHandles, version->m_staticOctree);
		}
		if (bRebuildStationary)
		{
			rebuildSpatialRoot(
				version->m_sceneVersion->m_stationaryHandles,
				version->m_stationaryOctree);
		}
		if (bRebuildDynamic)
		{
			rebuildSpatialRoot(version->m_sceneVersion->m_dynamicHandles, version->m_dynamicOctree);
		}
	}
	m_publishedSceneVersion = std::move(version);
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
	PublishSceneVersion(spatialChangeMask);
}

Tasks::ITaskPtr StaticMeshRendererECS::Tick(float deltaTime)
{
	constexpr uint32_t NumDirtyComponentsPerTask = 512;

	SAILOR_PROFILE_FUNCTION();

	const uint64_t materialContentRevision = Material::GetGlobalContentRevision();
	const bool bCheckMaterialRevisions = materialContentRevision != m_lastMaterialContentRevision;
	bool bHasCustomDepthShadowCasters = false;
	auto& dirtyComponents = m_componentScanScratch;
	dirtyComponents.Clear(false);
	dirtyComponents.Reserve(m_components.Num());
	for (size_t componentIndex = 0; componentIndex < m_components.Num(); ++componentIndex)
	{
		if (!IsComponentRegistered(componentIndex))
		{
			continue;
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
		bool bNeedsUpdate = data.m_bIsDirty ||
			(data.GetModel() && data.m_frameLastChange == 0);
		if (GameObjectPtr owner = data.m_owner.StaticCast<GameObject>())
		{
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

		if (bNeedsUpdate)
		{
			dirtyComponents.Add(componentIndex);
		}
	}
	if (dirtyComponents.IsEmpty())
	{
		m_lastMaterialContentRevision = materialContentRevision;
		return nullptr;
	}

	const size_t currentFrame = GetWorld()->GetCurrentFrame();
	auto prepareProxyUpdate = [this, currentFrame](size_t componentIndex)
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
				(data.GetModel() && data.m_frameLastChange == 0);
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

			// A transform-only update must not rebuild and then discard the full
			// mesh/material/shadow topology. Once the published resource stores
			// local mesh transforms, the scene record can carry the new mutable
			// state while every active version retains the immutable topology.
			if (!bTopologyDirty && !bMaterialsDirty && m_rhiScene)
			{
				RHI::RenderInstanceHandle* renderHandle = nullptr;
				RHI::RHISceneInstanceRecord previousRecord;
				if (m_renderInstanceHandles.Find(componentIndex, renderHandle) &&
					renderHandle &&
					m_rhiScene->ResolveCurrent(*renderHandle, previousRecord))
				{
					const auto previousResource =
						previousRecord.m_topology.DynamicCast<RHI::RHISceneProxyResource>();
					Math::AABB worldBounds = model->GetBoundsAABB(data.GetMeshIndex());
					worldBounds.Apply(ownerWorldMatrix);
					if (previousResource && previousResource->m_bMeshTransformsAreLocal &&
						worldBounds.IsValid())
					{
						result.m_worldBounds = worldBounds;
						result.m_shadowCaster = data.m_shadowCaster;
						result.m_state = owner->GetMobilityType() == EMobilityType::Static ?
							EPreparedProxyState::Static : EPreparedProxyState::Stationary;
						result.m_staticProxy.m_staticMeshEcs = componentIndex;
						result.m_staticProxy.m_mobility = owner->GetMobilityType();
						result.m_staticProxy.m_worldMatrix = ownerWorldMatrix;
						result.m_staticProxy.m_worldAabb = worldBounds;
						result.m_staticProxy.m_frame = currentFrame;
						result.m_staticProxy.m_skeletonOffset = result.m_skeletonOffset;
						result.m_staticProxy.m_bCastShadows = data.ShouldCastShadow();
						result.m_bStateOnly = true;
						return result;
					}
				}
			}

			TVector<RHI::RHIMeshPtr> selectedMeshes;
			TVector<glm::mat4> selectedMatrices;
			if (!CollectComponentRenderData(
					data,
					ownerWorldMatrix,
					selectedMeshes,
					selectedMatrices,
					result.m_worldBounds))
			{
				return result;
			}

			result.m_shadowCaster = CreateShadowCasterProxy(
				data,
				componentIndex,
				selectedMeshes,
				selectedMatrices,
				result.m_worldBounds,
				ownerWorldMatrix,
				result.m_skeletonOffset,
				currentFrame);

			result.m_state = owner->GetMobilityType() == EMobilityType::Static ?
				EPreparedProxyState::Static : EPreparedProxyState::Stationary;
			auto& proxy = result.m_staticProxy;
			proxy.m_staticMeshEcs = componentIndex;
			proxy.m_mobility = owner->GetMobilityType();
			proxy.m_worldMatrix = ownerWorldMatrix;
			proxy.m_frame = currentFrame;
			proxy.m_skeletonOffset = result.m_skeletonOffset;
			proxy.m_meshes = std::move(selectedMeshes);
			proxy.m_meshModelMatrices = std::move(selectedMatrices);
			proxy.m_worldAabb = result.m_worldBounds;
			proxy.m_bCastShadows = data.ShouldCastShadow();
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
			auto textureImporter = App::GetSubmodule<TextureImporter>();
#else
			auto textureImporter = App::GetSubmodule<TextureImporter>();
#endif
			for (size_t meshIndex = 0; meshIndex < proxy.m_meshes.Num(); ++meshIndex)
			{
				const size_t materialIndex = proxy.m_meshes[meshIndex]->ResolveMaterialIndex(
					meshIndex,
					data.GetMaterials().Num());
				auto material = data.GetMaterials()[materialIndex];
				proxy.m_renderQueueTags.Add(material->GetRenderState().GetTag());
				proxy.m_overrideMaterials.Add(material->GetOrAddRHI(proxy.m_meshes[meshIndex]->m_vertexDescription));

				glm::vec4 baseColorFactor{ 1.0f };
				const glm::vec4* materialBaseColorFactor = nullptr;
				if (!material->GetUniformsVec4().Find("material.baseColorFactor", materialBaseColorFactor))
				{
					material->GetUniformsVec4().Find("material.albedo", materialBaseColorFactor);
				}
				if (materialBaseColorFactor)
				{
					baseColorFactor = *materialBaseColorFactor;
				}
				proxy.m_baseColorFactors.Add(baseColorFactor);

				float alphaCutoff = 0.5f;
				const float* materialAlphaCutoff = nullptr;
				if (material->GetUniformsFloat().Find("material.alphaCutoff", materialAlphaCutoff) && materialAlphaCutoff)
				{
					alphaCutoff = *materialAlphaCutoff;
				}
				proxy.m_alphaCutoffs.Add(alphaCutoff);

				uint32_t baseColorSampler = 0u;
				const TexturePtr* baseColorTexture = nullptr;
				if (!material->GetSamplers().Find("baseColorSampler", baseColorTexture))
				{
					material->GetSamplers().Find("albedoSampler", baseColorTexture);
				}
				if (textureImporter && baseColorTexture && *baseColorTexture)
				{
					baseColorSampler = static_cast<uint32_t>(
						textureImporter->GetTextureIndex((*baseColorTexture)->GetFileId()));
				}
				proxy.m_baseColorSamplers.Add(baseColorSampler);
#if defined(__APPLE__)
				TSet<uint32_t> requestedTextures;
				requestedTextures.Insert(0u);
				if (textureImporter)
				{
					for (const auto& sampler : material->GetSamplers())
					{
						const uint32_t textureIndex = sampler.m_second ?
							(uint32_t)textureImporter->GetTextureIndex(sampler.m_second->GetFileId()) : 0u;
						requestedTextures.Insert(textureIndex);
					}
				}
				proxy.m_materialTextureSamplers.Add(std::move(requestedTextures));
#endif
			}

			return result;
		};

	auto& preparedUpdates = m_preparedUpdatesScratch;
	preparedUpdates.Clear(false);
	preparedUpdates.Resize(dirtyComponents.Num());
	if (dirtyComponents.Num() <= NumDirtyComponentsPerTask)
	{
		for (size_t index = 0; index < dirtyComponents.Num(); ++index)
		{
			preparedUpdates[index] = prepareProxyUpdate(dirtyComponents[index]);
		}
	}
	else
	{
		auto& tasks = m_prepareTasksScratch;
		tasks.Clear(false);
		const size_t numTasks = (dirtyComponents.Num() + NumDirtyComponentsPerTask - 1) / NumDirtyComponentsPerTask;
		tasks.Reserve(numTasks);
		for (size_t taskIndex = 0; taskIndex < numTasks; ++taskIndex)
		{
			const size_t beginIndex = taskIndex * NumDirtyComponentsPerTask;
			const size_t endIndex = (std::min)(beginIndex + NumDirtyComponentsPerTask, dirtyComponents.Num());
			auto task = Tasks::CreateTask(
				"StaticMeshRendererECS:Prepare Dirty Proxies",
				[beginIndex, endIndex, &dirtyComponents, &preparedUpdates, prepareProxyUpdate]()
				{
					for (size_t index = beginIndex; index < endIndex; ++index)
					{
						preparedUpdates[index] = prepareProxyUpdate(dirtyComponents[index]);
					}
				},
				EThreadType::Worker);
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
	const bool bPreviousHasCustomDepthShadowCasters =
		m_bHasCustomDepthShadowCasters;
	uint8_t spatialChangeMask = 0u;
	for (auto& update : preparedUpdates)
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
		if (update.m_bStateOnly &&
			(update.m_changeMask & RHI::ToMask(RHI::ESceneChangeBit::Transform)) != 0u &&
			update.m_staticProxy.m_bCastShadows)
		{
			bShadowCastersChanged = true;
		}

		update.m_staticProxy.m_shadowCaster = data.m_shadowCaster;
		if (update.m_staticProxy.m_shadowCaster)
		{
			update.m_staticProxy.m_shadowCaster->m_mobility = update.m_staticProxy.m_mobility;
		}
		if (m_rhiScene)
		{
			RHI::RHISceneInstanceRecord sceneRecord;
			sceneRecord.m_producerKey = update.m_componentIndex;
			sceneRecord.m_mobility = update.m_staticProxy.m_mobility;
			sceneRecord.m_worldMatrix = update.m_staticProxy.m_worldMatrix;
			sceneRecord.m_worldBounds = update.m_staticProxy.m_worldAabb;
			sceneRecord.m_topologyRevision = currentFrame;
			sceneRecord.m_materialRevision = Material::GetGlobalContentRevision();
			sceneRecord.m_skeletonOffset = update.m_staticProxy.m_skeletonOffset;
			sceneRecord.m_renderFlags = update.m_staticProxy.m_bCastShadows ? 1u : 0u;

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
					bool bCanReuseTopology = (update.m_changeMask & topologyChanges) == 0u;
					if (bCanReuseTopology &&
						(update.m_changeMask & RHI::ToMask(RHI::ESceneChangeBit::Transform)) != 0u)
					{
						const auto previousResource =
							previousRecord.m_topology.DynamicCast<RHI::RHISceneProxyResource>();
						bCanReuseTopology = previousResource &&
							previousResource->m_bMeshTransformsAreLocal;
					}
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
							RHI::RHISceneProxyResourcePtr::Make(std::move(update.m_staticProxy));
					}
					if (previousRecord.m_mobility != sceneRecord.m_mobility)
					{
						update.m_changeMask |= RHI::ToMask(RHI::ESceneChangeBit::Mobility);
					}
					if (const auto resource = sceneRecord.m_topology.DynamicCast<RHI::RHISceneProxyResource>())
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
					RHI::RHISceneProxyResourcePtr::Make(std::move(update.m_staticProxy));
				if (const auto resource = sceneRecord.m_topology.DynamicCast<RHI::RHISceneProxyResource>())
				{
					sceneRecord.m_shadowRevision = resource->m_shadowRevision;
				}
				m_renderInstanceHandles[update.m_componentIndex] = m_rhiScene->AddInstance(sceneRecord);
				bSceneRecordsChanged = true;
				spatialChangeMask |= GetSpatialChangeMask(sceneRecord.m_mobility);
			}
		}

		data.m_skeletonOffset = update.m_skeletonOffset;
		cacheMaterialRevisions();

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
	if (!bMaterialVersionsPending)
	{
		m_lastMaterialContentRevision = materialContentRevision;
	}

	if (bShadowCastersChanged)
	{
		++m_shadowCastersRevision;
	}
	m_bHasCustomDepthShadowCasters = bHasCustomDepthShadowCasters;
	if (bSceneRecordsChanged || bShadowCastersChanged ||
		bPreviousHasCustomDepthShadowCasters != m_bHasCustomDepthShadowCasters)
	{
		PublishSceneVersion(spatialChangeMask);
	}

	return nullptr;
}

void StaticMeshRendererECS::CopySceneView(RHI::RHISceneViewPtr& outProxies)
{
	SAILOR_PROFILE_FUNCTION();

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
	m_componentScanScratch.Clear();
	m_preparedUpdatesScratch.Clear();
	m_prepareTasksScratch.Clear();
	m_bHasCustomDepthShadowCasters = false;
}
