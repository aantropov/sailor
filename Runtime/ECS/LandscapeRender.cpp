#include "ECS/LandscapeECSInternal.h"

#include "AssetRegistry/Material/MaterialImporter.h"
#include "AssetRegistry/Model/ModelImporter.h"
#include "Containers/Hash.h"
#include "Core/StringHash.h"
#include "Engine/GameObject.h"
#include "Engine/World.h"

#include <cmath>
#include <limits>
#include <utility>

using namespace Sailor;
using namespace Sailor::LandscapeECSInternal;

void LandscapeECS::UpdateTerrainRenderProxy(size_t componentIndex, size_t chunkIndex, RHI::RHIMeshPtr mesh)
{
	auto& data = m_components[componentIndex];
	auto& chunk = data.m_chunks[chunkIndex];
	// The scene record carries the owner transform; retained topology stays local.
	const glm::mat4 localMatrix(1.0f);
	RHI::RHISceneViewProxy proxy;
	proxy.m_lodPolicy.m_bEnabled = !mesh->m_lods.IsEmpty();
	proxy.m_lodPolicy.m_minLod = 0u;
	proxy.m_lodPolicy.m_maxLod = mesh->GetNumLods() - 1u;
	proxy.m_lodPolicy.m_cameraDistanceThresholds = data.m_lodDistances;
	if (proxy.m_lodPolicy.m_cameraDistanceThresholds.Num() >= mesh->GetNumLods())
	{
		proxy.m_lodPolicy.m_cameraDistanceThresholds.Resize(mesh->GetNumLods() - 1u);
	}
	proxy.m_meshes.Add(mesh);
	proxy.m_meshModelMatrices.Add(localMatrix);
	auto rhiMaterial = data.m_runtimeMaterial->GetOrAddRHI(mesh->m_vertexDescription);
	const auto& metadata = data.m_runtimeMaterial->GetRenderMetadata();
	metadata.AppendTo(proxy, rhiMaterial);
	auto shadowCaster = TSharedPtr<RHI::RHIShadowCasterProxy>::Make();
	metadata.AppendShadowMesh(*shadowCaster, mesh, localMatrix, rhiMaterial);
	proxy.m_shadowCaster = shadowCaster->m_meshes.IsEmpty() ? RHI::RHIShadowCasterProxyPtr{} : shadowCaster;
	chunk.m_resource = RHI::RHISceneProxyResourcePtr::Make(std::move(proxy));
}

bool LandscapeECS::UpdateVegetationRenderProxies(size_t componentIndex)
{
	auto& data = m_components[componentIndex];
	bool bChanged = false;
	for (size_t profileIndex = 0u; profileIndex < data.m_vegetationProfiles.Num(); ++profileIndex)
	{
		auto& profile = data.m_vegetationProfiles[profileIndex];
		const auto revision = CalculateVegetationRenderRevision(profile);
		if (profile.m_cachedRenderRevision == revision || data.m_dirtyVegetationProfiles.Contains(static_cast<uint32_t>(profileIndex)))
		{
			continue;
		}
		bool bProfileReady = true;
		for (size_t chunkIndex = 0u; chunkIndex < data.m_chunks.Num(); ++chunkIndex)
		{
			if (data.m_pendingVegetation.FindIf([=](const auto& pending)
				{ return pending.m_chunkIndex == chunkIndex && pending.m_profileIndex == profileIndex; }) != size_t(-1))
			{
				continue;
			}
			auto& proxies = data.m_chunks[chunkIndex].m_vegetationProxies;
			const size_t index = proxies.FindIf([=](const auto& proxy) { return proxy.m_profileIndex == profileIndex; });
			const auto* previous = index != size_t(-1) ? &proxies[index] : nullptr;
			if ((!previous && profile.m_settings.m_residency == ELandscapeVegetationResidency::Grass) ||
				(previous && previous->m_renderRevision == revision))
			{
				continue;
			}
			LandscapeVegetationRenderInstances instances;
			if (previous)
			{
				const auto& group = previous->m_resource->m_proxy.m_instancedGroups[0];
				instances = { group.m_instanceTransforms, group.m_instanceLodBiases,
					group.m_instanceCullDistanceScales, group.m_instanceShadowDistanceScales };
			}
			else
			{
				for (const auto& placement : data.m_chunks[chunkIndex].m_bakeVegetation)
					if (placement.m_profileIndex == profileIndex) AppendRenderInstance(placement, instances);
			}
			const auto owner = const_cast<ObjectPtr&>(data.GetOwner()).StaticCast<GameObject>();
			LandscapeVegetationRenderProxy proxy;
			const auto result = BuildLandscapeVegetationProxy(profileIndex, profile, std::move(instances),
				ResolveLandscapeProxyMobility(owner->GetMobilityType(), profile.m_settings.m_residency),
				previous ? previous->m_revision : data.m_chunks[chunkIndex].m_vegetationRevision, proxy);
			if (result == EVegetationProxyBuildResult::Pending)
			{
				if (profile.m_settings.m_residency == ELandscapeVegetationResidency::Persistent)
				{
					data.m_pendingVegetation.Add({ chunkIndex, profileIndex, std::move(instances) });
				}
				else
				{
					// Retry metadata on the current residents; the grass scheduler may replace or evict them.
					bProfileReady = false;
				}
				continue;
			}
			if (result == EVegetationProxyBuildResult::Success)
			{
				if (previous)
				{
					proxy.m_viewRevision = previous->m_viewRevision;
					proxies[index] = std::move(proxy);
				}
				else proxies.Add(std::move(proxy));
				bChanged = true;
			}
			else if (previous)
			{
				proxies.RemoveAtSwap(index);
				bChanged = true;
			}
		}
		if (bProfileReady)
		{
			profile.m_cachedRenderRevision = revision;
		}
	}
	return bChanged;
}

namespace Sailor::LandscapeECSInternal
{
	void GetOctreeBounds(const Math::AABB& bounds, glm::ivec3& center, glm::ivec3& extents)
	{
		const glm::ivec3 minimum = glm::ivec3(glm::floor(bounds.m_min));
		const glm::ivec3 maximum = glm::ivec3(glm::ceil(bounds.m_max));
		center = minimum + (maximum - minimum) / 2;
		extents = glm::max(glm::max(maximum - center, center - minimum), glm::ivec3(1));
	}

	size_t LandscapeProxyId(size_t componentIndex, size_t chunkIndex)
	{
		return (size_t(1) << (sizeof(size_t) * 8u - 1u)) | ((componentIndex & 0x7fffffffu) << 24u) |
			   (chunkIndex & 0xffffffu);
	}

	size_t LandscapeVegetationProxyId(size_t componentIndex, size_t chunkIndex, size_t profileIndex)
	{
		return (size_t(3) << (sizeof(size_t) * 8u - 2u)) | ((componentIndex & 0xfffffu) << 36u) |
			   ((chunkIndex & 0xfffffu) << 16u) | (profileIndex & 0xffffu);
	}

	EVegetationProxyBuildResult BuildLandscapeVegetationProxy(size_t profileIndex,
		const LandscapeVegetationProfile& profile,
		LandscapeVegetationRenderInstances&& instances,
		EMobilityType mobility,
		uint64_t revision,
		LandscapeVegetationRenderProxy& result)
	{
		if (instances.m_transforms.IsEmpty())
		{
			return EVegetationProxyBuildResult::NoRenderData;
		}
		const bool bUseMaterialOverride = static_cast<bool>(profile.m_settings.m_materialFileId);
		if (!profile.m_model || !profile.m_model->IsReady() ||
			(bUseMaterialOverride && (!profile.m_material || !profile.m_material->IsReady())))
		{
			return EVegetationProxyBuildResult::Pending;
		}

		TVector<RHI::RHIMeshPtr> vegetationMeshes;
		TVector<glm::mat4> vegetationModelMatrices;
		Math::AABB vegetationBounds;
		if (!profile.m_model->CollectRenderData(
				profile.m_settings.m_meshIndex, vegetationMeshes, vegetationModelMatrices, vegetationBounds))
		{
			return EVegetationProxyBuildResult::NoRenderData;
		}

		TVector<MaterialPtr> vegetationMaterials;
		vegetationMaterials.Reserve(vegetationMeshes.Num());
		for (size_t meshIndex = 0u; meshIndex < vegetationMeshes.Num(); ++meshIndex)
		{
			MaterialPtr material = profile.m_material;
			if (!bUseMaterialOverride)
			{
				const size_t materialIndex =
					vegetationMeshes[meshIndex]->ResolveMaterialIndex(meshIndex, profile.m_modelMaterials.Num());
				material = materialIndex < profile.m_modelMaterials.Num() ? profile.m_modelMaterials[materialIndex]
																		  : MaterialPtr{};
			}
			if (!material || !material->IsReady())
			{
				return EVegetationProxyBuildResult::Pending;
			}
			vegetationMaterials.Add(std::move(material));
		}

		RHI::RHISceneViewProxy vegetationProxy;
		vegetationProxy.m_lodPolicy.m_bEnabled = true;
		vegetationProxy.m_lodPolicy.m_minLod = profile.m_settings.m_minLod;
		vegetationProxy.m_lodPolicy.m_maxLod = profile.m_settings.m_maxLod;
		vegetationProxy.m_lodPolicy.m_screenCoverageThresholds = profile.m_settings.m_screenCoverageThresholds;
		vegetationProxy.m_lodPolicy.m_maxCameraDistance = profile.m_settings.m_cullDistance;

		RHI::RHIInstancedMeshGroup instanceGroup;
		instanceGroup.m_bCastShadows = profile.m_settings.m_shadowMode != ELandscapeVegetationShadowMode::None;
		instanceGroup.m_maxShadowDistance = (std::min)(profile.m_settings.m_cullDistance,
			profile.m_settings.m_shadowMode == ELandscapeVegetationShadowMode::NearOnly ? profile.m_settings.m_shadowDistance
																			 : (std::numeric_limits<float>::max)());
		instanceGroup.m_materials.Reserve(vegetationMeshes.Num());
		instanceGroup.m_sourceMaterialShaders.Reserve(vegetationMeshes.Num());
		instanceGroup.m_renderQueueTags.Reserve(vegetationMeshes.Num());
		instanceGroup.m_baseColorFactors.Reserve(vegetationMeshes.Num());
		instanceGroup.m_baseColorSamplers.Reserve(vegetationMeshes.Num());
		instanceGroup.m_alphaCutoffs.Reserve(vegetationMeshes.Num());
#if defined(__APPLE__)
		instanceGroup.m_materialTextureSamplers.Reserve(vegetationMeshes.Num());
#endif
		for (size_t meshIndex = 0u; meshIndex < vegetationMeshes.Num(); ++meshIndex)
		{
			MaterialPtr& material = vegetationMaterials[meshIndex];
			material->GetRenderMetadata().AppendTo(instanceGroup,
				material->GetOrAddRHI(vegetationMeshes[meshIndex]->m_vertexDescription));
		}

		const uint32_t instanceCount = static_cast<uint32_t>(instances.m_transforms.Num());
		Math::AABB batchedVegetationBounds;
		for (const auto& localInstanceMatrix : instances.m_transforms)
		{
			Math::AABB instanceBounds = vegetationBounds;
			instanceBounds.Apply(localInstanceMatrix);
			batchedVegetationBounds.Extend(instanceBounds);
		}
		if (vegetationMeshes.IsEmpty() || !batchedVegetationBounds.IsValid())
		{
			return EVegetationProxyBuildResult::NoRenderData;
		}
		instanceGroup.m_meshes = std::move(vegetationMeshes);
		instanceGroup.m_meshTransforms = std::move(vegetationModelMatrices);
		instanceGroup.m_instanceTransforms = std::move(instances.m_transforms);
		instanceGroup.m_instanceLodBiases = std::move(instances.m_lodBiases);
		instanceGroup.m_instanceCullDistanceScales = std::move(instances.m_cullDistanceScales);
		instanceGroup.m_instanceShadowDistanceScales = std::move(instances.m_shadowDistanceScales);

		vegetationProxy.m_shadowCaster =
			instanceGroup.m_bCastShadows ? RHI::RHIShadowCasterProxyPtr::Make() : RHI::RHIShadowCasterProxyPtr{};
		vegetationProxy.m_instancedGroups.Add(std::move(instanceGroup));

		result.m_resource = RHI::RHISceneProxyResourcePtr::Make(std::move(vegetationProxy));
		result.m_localBounds = batchedVegetationBounds;
		result.m_profileIndex = profileIndex;
		result.m_instanceCount = instanceCount;
		result.m_revision = revision;
		result.m_renderRevision = CalculateVegetationRenderRevision(profile);
		result.m_residency = profile.m_settings.m_residency;
		result.m_mobility = mobility;
		return EVegetationProxyBuildResult::Success;
	}

	uint64_t CalculateVegetationRenderRevision(const LandscapeVegetationProfile& profile)
	{
		size_t result = Fnv1aOffsetBasis;
		HashCombine(result, profile.m_settings.m_modelFileId, profile.m_model, profile.m_settings.m_meshIndex, profile.m_settings.m_residency,
			profile.m_settings.m_shadowMode, profile.m_settings.m_shadowDistance, profile.m_settings.m_minLod, profile.m_settings.m_maxLod, profile.m_settings.m_cullDistance);
		for (float threshold : profile.m_settings.m_screenCoverageThresholds) HashCombine(result, threshold);
		if (profile.m_settings.m_materialFileId)
		{
			HashCombine(result,
				profile.m_material,
				profile.m_material ? profile.m_material->GetRenderMetadataRevision() : 0ull);
		}
		else
		{
			HashCombine(result, profile.m_modelMaterials.Num());
			for (const auto& material : profile.m_modelMaterials)
			{
				HashCombine(result, material, material ? material->GetRenderMetadataRevision() : 0ull);
			}
		}
		return static_cast<uint64_t>(result);
	}

}
