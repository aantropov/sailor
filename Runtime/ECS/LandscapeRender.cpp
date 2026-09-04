#include "ECS/LandscapeECSInternal.h"

#include "AssetRegistry/Material/MaterialImporter.h"
#include "AssetRegistry/Model/ModelImporter.h"
#include "AssetRegistry/Texture/TextureImporter.h"
#include "Containers/Hash.h"
#include "Core/StringHash.h"

#include <cmath>
#include <limits>
#include <utility>

namespace Sailor::LandscapeECSInternal
{
	float GetProfileValue(const TVector<float>& values, size_t index, float fallback)
	{
		return index < values.Num() && std::isfinite(values[index]) ? values[index] : fallback;
	}

	void AppendShadowMesh(RHI::RHIShadowCasterProxy& shadowCaster,
		const RHI::RHIMeshPtr& mesh,
		const glm::mat4& worldMatrix,
		const MaterialPtr& material,
		float maxCameraDistance)
	{
		if (!mesh || !material)
		{
			return;
		}

		const size_t opaqueQueueTag = "Opaque"_h.GetHash();
		const size_t maskedQueueTag = "Masked"_h.GetHash();
		const size_t renderQueueTag = material->GetRenderState().GetTag();
		if (renderQueueTag != opaqueQueueTag && renderQueueTag != maskedQueueTag)
		{
			return;
		}

		RHI::RHIShadowMeshProxy shadowMesh;
		shadowMesh.m_mesh = mesh;
		shadowMesh.m_worldMatrix = worldMatrix;
		shadowMesh.m_renderQueueTag = renderQueueTag;
		shadowMesh.m_maxCameraDistance = maxCameraDistance;
		if (material->GetRenderState().IsRequiredCustomDepthShader())
		{
			auto rhiMaterialSource = material;
			shadowMesh.m_customDepthMaterial = rhiMaterialSource->GetOrAddRHI(mesh->m_vertexDescription);
			shadowMesh.m_customDepthShader = material->GetShader();
		}

		auto* textureImporter = App::GetSubmodule<TextureImporter>();
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
				shadowMesh.m_baseColorSampler =
					static_cast<uint32_t>(textureImporter->GetTextureIndex((*baseColorTexture)->GetFileId()));
			}
		}
#if defined(__APPLE__)
		shadowMesh.m_materialTextureSamplers.Insert(0u);
		if (textureImporter)
		{
			for (const auto& sampler : material->GetSamplers())
			{
				shadowMesh.m_materialTextureSamplers.Insert(
					sampler.m_second
						? static_cast<uint32_t>(textureImporter->GetTextureIndex(sampler.m_second->GetFileId()))
						: 0u);
			}
		}
#endif
		shadowCaster.m_meshes.Add(std::move(shadowMesh));
	}

	void AppendDepthMaterialMetadata(RHI::RHISceneViewProxy& proxy, const MaterialPtr& material)
	{
		glm::vec4 baseColorFactor{1.0f};
		float alphaCutoff = 0.5f;
		uint32_t baseColorSampler = 0u;
		if (material->GetRenderState().GetTag() != "Masked"_h.GetHash())
		{
			proxy.m_baseColorFactors.Add(baseColorFactor);
			proxy.m_alphaCutoffs.Add(alphaCutoff);
			proxy.m_baseColorSamplers.Add(baseColorSampler);
			return;
		}

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

		const float* materialAlphaCutoff = nullptr;
		if (material->GetUniformsFloat().Find("material.alphaCutoff", materialAlphaCutoff) && materialAlphaCutoff)
		{
			alphaCutoff = *materialAlphaCutoff;
		}
		proxy.m_alphaCutoffs.Add(alphaCutoff);

		const TexturePtr* baseColorTexture = nullptr;
		if (!material->GetSamplers().Find("baseColorSampler", baseColorTexture))
		{
			material->GetSamplers().Find("albedoSampler", baseColorTexture);
		}
		auto* textureImporter = App::GetSubmodule<TextureImporter>();
		if (textureImporter && baseColorTexture && *baseColorTexture)
		{
			baseColorSampler =
				static_cast<uint32_t>(textureImporter->GetTextureIndex((*baseColorTexture)->GetFileId()));
		}
		proxy.m_baseColorSamplers.Add(baseColorSampler);
	}

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

	static size_t LandscapeVegetationProxyId(size_t componentIndex, size_t chunkIndex, size_t instanceIndex)
	{
		return (size_t(3) << (sizeof(size_t) * 8u - 2u)) | ((componentIndex & 0xfffffu) << 36u) |
			   ((chunkIndex & 0xfffffu) << 16u) | (instanceIndex & 0xffffu);
	}

	EVegetationProxyBuildResult BuildLandscapeVegetationProxy(size_t componentIndex,
		size_t chunkIndex,
		size_t profileIndex,
		const LandscapeVegetationProfile& profile,
		const glm::mat4& ownerMatrix,
		uint64_t frame,
		LandscapeVegetationRenderInstances instances,
		EMobilityType mobility,
		uint64_t revision,
		LandscapeVegetationRenderProxy& result)
	{
		const bool bUseMaterialOverride = static_cast<bool>(profile.m_materialFileId);
		if (!profile.m_model || !profile.m_model->IsReady() ||
			(bUseMaterialOverride && (!profile.m_material || !profile.m_material->IsReady())))
		{
			return EVegetationProxyBuildResult::Pending;
		}

		TVector<RHI::RHIMeshPtr> vegetationMeshes;
		TVector<glm::mat4> vegetationModelMatrices;
		Math::AABB vegetationBounds;
		if (!profile.m_model->CollectRenderData(
				profile.m_meshIndex, vegetationMeshes, vegetationModelMatrices, vegetationBounds))
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
		vegetationProxy.m_staticMeshEcs = LandscapeVegetationProxyId(componentIndex, chunkIndex, profileIndex);
		vegetationProxy.m_mobility = mobility;
		vegetationProxy.m_worldMatrix = ownerMatrix;
		vegetationProxy.m_frame = frame;
		vegetationProxy.m_bCastShadows = profile.m_shadowMode != ELandscapeVegetationShadowMode::None;
		vegetationProxy.m_lodPolicy.m_bEnabled = true;
		vegetationProxy.m_lodPolicy.m_minLod = profile.m_minLod;
		vegetationProxy.m_lodPolicy.m_maxLod = profile.m_maxLod;
		vegetationProxy.m_lodPolicy.m_screenCoverageThresholds = profile.m_screenCoverageThresholds;
		vegetationProxy.m_lodPolicy.m_maxCameraDistance = profile.m_cullDistance;

		RHI::RHIInstancedMeshGroup instanceGroup;
		instanceGroup.m_bCastShadows = vegetationProxy.m_bCastShadows;
		instanceGroup.m_maxShadowDistance = (std::min)(profile.m_cullDistance,
			profile.m_shadowMode == ELandscapeVegetationShadowMode::NearOnly ? profile.m_shadowDistance
																			 : (std::numeric_limits<float>::max)());
		instanceGroup.m_materials.Reserve(vegetationMeshes.Num());
		instanceGroup.m_sourceMaterialShaders.Reserve(vegetationMeshes.Num());
		instanceGroup.m_renderQueueTags.Reserve(vegetationMeshes.Num());
		instanceGroup.m_baseColorFactors.Reserve(vegetationMeshes.Num());
		instanceGroup.m_baseColorSamplers.Reserve(vegetationMeshes.Num());
		instanceGroup.m_alphaCutoffs.Reserve(vegetationMeshes.Num());
#if defined(__APPLE__)
		instanceGroup.m_materialTextureSamplers.Resize(vegetationMeshes.Num());
#endif
		auto* textureImporter = App::GetSubmodule<TextureImporter>();
		for (size_t meshIndex = 0u; meshIndex < vegetationMeshes.Num(); ++meshIndex)
		{
			MaterialPtr& material = vegetationMaterials[meshIndex];
			instanceGroup.m_sourceMaterialShaders.Add(material->GetShader());
			instanceGroup.m_materials.Add(material->GetOrAddRHI(vegetationMeshes[meshIndex]->m_vertexDescription));
			instanceGroup.m_renderQueueTags.Add(material->GetRenderState().GetTag());

			glm::vec4 baseColorFactor{1.0f};
			const glm::vec4* materialBaseColorFactor = nullptr;
			if (!material->GetUniformsVec4().Find("material.baseColorFactor", materialBaseColorFactor))
			{
				material->GetUniformsVec4().Find("material.albedo", materialBaseColorFactor);
			}
			if (materialBaseColorFactor)
			{
				baseColorFactor = *materialBaseColorFactor;
			}
			instanceGroup.m_baseColorFactors.Add(baseColorFactor);

			float alphaCutoff = 0.5f;
			const float* materialAlphaCutoff = nullptr;
			if (material->GetUniformsFloat().Find("material.alphaCutoff", materialAlphaCutoff) && materialAlphaCutoff)
			{
				alphaCutoff = *materialAlphaCutoff;
			}
			instanceGroup.m_alphaCutoffs.Add(alphaCutoff);

			uint32_t baseColorSampler = 0u;
			const TexturePtr* baseColorTexture = nullptr;
			if (!material->GetSamplers().Find("baseColorSampler", baseColorTexture))
			{
				material->GetSamplers().Find("albedoSampler", baseColorTexture);
			}
			if (textureImporter && baseColorTexture && *baseColorTexture)
			{
				baseColorSampler =
					static_cast<uint32_t>(textureImporter->GetTextureIndex((*baseColorTexture)->GetFileId()));
			}
			instanceGroup.m_baseColorSamplers.Add(baseColorSampler);
#if defined(__APPLE__)
			auto& requested = instanceGroup.m_materialTextureSamplers[meshIndex];
			requested.Insert(0u);
			if (textureImporter)
			{
				for (const auto& sampler : material->GetSamplers())
				{
					requested.Insert(sampler.m_second ? static_cast<uint32_t>(textureImporter->GetTextureIndex(
															sampler.m_second->GetFileId()))
													  : 0u);
				}
			}
#endif
		}

		const uint32_t instanceCount = static_cast<uint32_t>(instances.m_transforms.Num());
		Math::AABB batchedVegetationBounds;
		for (const auto& localInstanceMatrix : instances.m_transforms)
		{
			const glm::mat4 instanceMatrix = ownerMatrix * localInstanceMatrix;
			Math::AABB instanceBounds = vegetationBounds;
			instanceBounds.Apply(instanceMatrix);
			batchedVegetationBounds.Extend(instanceBounds);
		}
		if (instances.m_transforms.IsEmpty() || vegetationMeshes.IsEmpty() || !batchedVegetationBounds.IsValid())
		{
			return EVegetationProxyBuildResult::NoRenderData;
		}
		instanceGroup.m_meshes = std::move(vegetationMeshes);
		instanceGroup.m_meshTransforms = std::move(vegetationModelMatrices);
		instanceGroup.m_instanceTransforms = std::move(instances.m_transforms);
		instanceGroup.m_instanceLodBiases = std::move(instances.m_lodBiases);
		instanceGroup.m_instanceCullDistanceScales = std::move(instances.m_cullDistanceScales);
		instanceGroup.m_instanceShadowDistanceScales = std::move(instances.m_shadowDistanceScales);

		vegetationProxy.m_worldAabb = batchedVegetationBounds;
		auto vegetationShadowCaster = RHI::RHIShadowCasterProxyPtr::Make();
		vegetationShadowCaster->m_staticMeshEcs = vegetationProxy.m_staticMeshEcs;
		vegetationShadowCaster->m_mobility = mobility;
		vegetationShadowCaster->m_skeletonOffset = (std::numeric_limits<uint32_t>::max)();
		vegetationShadowCaster->m_frame = vegetationProxy.m_frame;
		vegetationShadowCaster->m_lodPolicy = vegetationProxy.m_lodPolicy;
		vegetationShadowCaster->m_worldAabb = vegetationProxy.m_worldAabb;
		vegetationProxy.m_shadowCaster =
			vegetationProxy.m_bCastShadows ? vegetationShadowCaster : RHI::RHIShadowCasterProxyPtr{};
		vegetationProxy.m_instancedGroups.Add(std::move(instanceGroup));

		GetOctreeBounds(vegetationProxy.m_worldAabb, result.m_octreeCenter, result.m_octreeExtents);
		result.m_resource = RHI::RHISceneProxyResourcePtr::Make(std::move(vegetationProxy));
		result.m_profileIndex = profileIndex;
		result.m_instanceCount = instanceCount;
		result.m_revision = revision;
		result.m_residency = profile.m_residency;
		result.m_mobility = mobility;
		return EVegetationProxyBuildResult::Success;
	}

	bool AreVegetationProfileSettingsEqual(const LandscapeVegetationProfile& lhs, const LandscapeVegetationProfile& rhs)
	{
		return lhs.m_modelFileId == rhs.m_modelFileId && lhs.m_materialFileId == rhs.m_materialFileId &&
			   lhs.m_meshIndex == rhs.m_meshIndex && lhs.m_instancesPerChunk == rhs.m_instancesPerChunk &&
			   lhs.m_residency == rhs.m_residency && lhs.m_priority == rhs.m_priority &&
			   lhs.m_minScale == rhs.m_minScale && lhs.m_maxScale == rhs.m_maxScale &&
			   lhs.m_groundOffset == rhs.m_groundOffset && lhs.m_shadowMode == rhs.m_shadowMode &&
			   lhs.m_shadowDistance == rhs.m_shadowDistance && lhs.m_minLod == rhs.m_minLod &&
			   lhs.m_maxLod == rhs.m_maxLod && lhs.m_screenCoverageThresholds == rhs.m_screenCoverageThresholds &&
			   lhs.m_cullDistance == rhs.m_cullDistance && lhs.m_colliderRadius == rhs.m_colliderRadius &&
			   lhs.m_colliderHeight == rhs.m_colliderHeight && lhs.m_colliderOffsetY == rhs.m_colliderOffsetY;
	}

	uint64_t CalculateVegetationMaterialRenderMetadataRevision(const LandscapeVegetationProfile& profile)
	{
		size_t result = Fnv1aOffsetBasis;
		if (profile.m_materialFileId)
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
