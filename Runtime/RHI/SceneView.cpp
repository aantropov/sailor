#include "SceneView.h"
#include "Core/StringHash.h"
#include "ECS/CameraECS.h"
#include "ECS/TransformECS.h"
#include "ECS/StaticMeshRendererECS.h"
#include "Engine/GameObject.h"
#include "Math/Transform.h"
#include "Engine/World.h"
#include "AssetRegistry/Model/ModelImporter.h"
#include "AssetRegistry/Material/MaterialImporter.h"
#include "AssetRegistry/Texture/TextureImporter.h"
#include "RHI/DebugContext.h"
#include "RHI/MaterialMetadata.h"
#include "RHI/CommandList.h"
#include "Settings/GraphicsSettings.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>

using namespace Sailor;
using namespace Sailor::RHI;

#if defined(__APPLE__)
void RHI::NormalizeTextureSamplers(TVector<uint32_t>& textures)
{
	textures.Add(0u);
	std::sort(textures.begin(), textures.end());
	size_t count = 0;
	for (uint32_t texture : textures)
	{
		if (texture >= TextureImporter::MaxTexturesInScene)
		{
			break;
		}
		if (count == 0u || textures[count - 1u] != texture)
		{
			textures[count++] = texture;
		}
	}
	textures.Resize(count);
}
#endif

void RHIMaterialMetadata::AppendTo(RHISceneViewProxy& proxy, RHIMaterialPtr material) const
{
	proxy.m_overrideMaterials.Add(std::move(material));
	proxy.m_renderQueueTags.Add(m_renderQueueTag);
	proxy.m_baseColorFactors.Add(m_baseColorFactor);
	proxy.m_alphaCutoffs.Add(m_alphaCutoff);
	proxy.m_baseColorSamplers.Add(m_baseColorSampler);
#if defined(__APPLE__)
	proxy.m_materialTextureSamplers.Add(m_textureSamplers);
#endif
}

void RHIMaterialMetadata::AppendTo(RHIInstancedMeshGroup& group, RHIMaterialPtr material) const
{
	group.m_materials.Add(std::move(material));
	group.m_sourceMaterialShaders.Add(m_shader);
	group.m_renderQueueTags.Add(m_renderQueueTag);
	group.m_baseColorFactors.Add(m_baseColorFactor);
	group.m_alphaCutoffs.Add(m_alphaCutoff);
	group.m_baseColorSamplers.Add(m_baseColorSampler);
#if defined(__APPLE__)
	group.m_materialTextureSamplers.Add(m_textureSamplers);
#endif
}

void RHIMaterialMetadata::AppendShadowMesh(RHIShadowCasterProxy& caster, const RHIMeshPtr& mesh,
	const glm::mat4& matrix, const RHIMaterialPtr& material, float maxCameraDistance) const
{
	const bool bMasked = m_renderQueueTag == "Masked"_h.GetHash();
	if (m_renderQueueTag != "Opaque"_h.GetHash() && !bMasked) return;

	RHIShadowMeshProxy shadowMesh;
	shadowMesh.m_mesh = mesh;
	shadowMesh.m_localMatrix = matrix;
	shadowMesh.m_renderQueueTag = m_renderQueueTag;
	shadowMesh.m_maxCameraDistance = maxCameraDistance;
	if (m_bRequiresCustomDepthShader)
	{
		shadowMesh.m_customDepthMaterial = material;
		shadowMesh.m_customDepthShader = m_shader;
#if defined(__APPLE__)
		shadowMesh.m_materialTextureSamplers = m_textureSamplers;
#endif
	}
	if (bMasked)
	{
		shadowMesh.m_baseColorFactor = m_baseColorFactor;
		shadowMesh.m_alphaCutoff = m_alphaCutoff;
		shadowMesh.m_baseColorSampler = m_baseColorSampler;
#if defined(__APPLE__)
		if (!m_bRequiresCustomDepthShader) shadowMesh.m_materialTextureSamplers.Add(m_baseColorSampler);
#endif
	}
#if defined(__APPLE__)
	NormalizeTextureSamplers(shadowMesh.m_materialTextureSamplers);
#endif
	caster.m_meshes.Add(std::move(shadowMesh));
}

namespace
{
	void HashMatrix(size_t& result, const glm::mat4& matrix)
	{
		HashCombine(result, std::hash<glm::mat4>{}(matrix));
	}

	int32_t ResolveInstanceLodBias(
		const RHIInstancedMeshGroup& group,
		size_t instanceIndex)
	{
		return instanceIndex < group.m_instanceLodBiases.Num() ?
			group.m_instanceLodBiases[instanceIndex] : 0;
	}

	float ResolveInstanceDistanceScale(
		const TVector<float>& scales,
		size_t instanceIndex)
	{
		if (instanceIndex >= scales.Num() ||
			!std::isfinite(scales[instanceIndex]) ||
			scales[instanceIndex] <= 0.0f)
		{
			return 1.0f;
		}
		return scales[instanceIndex];
	}

	void HashMaterialVersion(size_t& result, const RHIMaterialPtr& material)
	{
		if (!material)
		{
			HashCombine(result, 0u);
			return;
		}

		const auto version = material->GetVersion();
		HashCombine(
			result,
			material,
			version ? version->GetVersionId() : 0ull,
			Sailor::GetHash(material->GetRenderState()));
	}

#if defined(__APPLE__)
	void HashTextureSamplers(size_t& result, const TVector<uint32_t>& textures)
	{
		HashCombine(result, textures.Num());
		for (uint32_t texture : textures)
		{
			HashCombine(result, texture);
		}
	}
#endif

	void CalculateProxyResourceRevisions(RHISceneProxyResource& resource)
	{
		const auto& proxy = resource.m_proxy;
		// LOD chains are complete before this immutable topology is published.
		resource.m_bHasLods = false;
		size_t geometryRevision = Fnv1aOffsetBasis;
		HashCombine(
			geometryRevision,
			proxy.m_meshes.Num(),
			proxy.m_meshModelMatrices.Num(),
			proxy.m_instancedGroups.Num(),
			proxy.m_lodPolicy.m_bEnabled,
			proxy.m_lodPolicy.m_minLod,
			proxy.m_lodPolicy.m_maxLod,
			std::hash<float>{}(proxy.m_lodPolicy.m_maxCameraDistance));
		for (float threshold : proxy.m_lodPolicy.m_screenCoverageThresholds)
		{
			HashCombine(geometryRevision, std::hash<float>{}(threshold));
		}
		for (float threshold : proxy.m_lodPolicy.m_cameraDistanceThresholds)
		{
			HashCombine(geometryRevision, std::hash<float>{}(threshold));
		}
		for (const auto& mesh : proxy.m_meshes)
		{
			resource.m_bHasLods |= mesh && mesh->GetNumLods() > 1u;
			HashCombine(geometryRevision, mesh);
		}
		for (const auto& matrix : proxy.m_meshModelMatrices)
		{
			HashMatrix(geometryRevision, matrix);
		}
		for (const auto& group : proxy.m_instancedGroups)
		{
			HashCombine(
				geometryRevision,
				group.m_instanceTransforms.Num(),
				group.m_instanceLodBiases.Num(),
				group.m_instanceCullDistanceScales.Num(),
				group.m_instanceShadowDistanceScales.Num(),
				group.m_meshes.Num(),
				group.m_bCastShadows,
				std::hash<float>{}(group.m_maxShadowDistance));
			for (const auto& mesh : group.m_meshes)
			{
				resource.m_bHasLods |= mesh && mesh->GetNumLods() > 1u;
				HashCombine(geometryRevision, mesh);
			}
			for (const auto& matrix : group.m_meshTransforms)
			{
				HashMatrix(geometryRevision, matrix);
			}
			for (const auto& matrix : group.m_instanceTransforms)
			{
				HashMatrix(geometryRevision, matrix);
			}
			for (int32_t lodBias : group.m_instanceLodBiases)
			{
				HashCombine(geometryRevision, lodBias);
			}
			for (float scale : group.m_instanceCullDistanceScales)
			{
				HashCombine(geometryRevision, std::hash<float>{}(scale));
			}
			for (float scale : group.m_instanceShadowDistanceScales)
			{
				HashCombine(geometryRevision, std::hash<float>{}(scale));
			}
		}
		resource.m_geometryRevision = geometryRevision;

		size_t mainRevision = geometryRevision;
		for (const auto& material : proxy.m_overrideMaterials)
		{
			HashMaterialVersion(mainRevision, material);
		}
#if defined(__APPLE__)
		for (const auto& textures : proxy.m_materialTextureSamplers)
		{
			HashTextureSamplers(mainRevision, textures);
		}
#endif
		for (const auto& group : proxy.m_instancedGroups)
		{
			for (const auto& material : group.m_materials)
			{
				HashMaterialVersion(mainRevision, material);
			}
#if defined(__APPLE__)
			for (const auto& textures : group.m_materialTextureSamplers)
			{
				HashTextureSamplers(mainRevision, textures);
			}
#endif
		}
		resource.m_mainRevision = mainRevision;

		const size_t maskedQueue = "Masked"_h.GetHash();
		size_t depthRevision = geometryRevision;
		auto hashDepthMaterial = [&](const RHIMaterialPtr& material,
			size_t renderQueue,
			const glm::vec4& baseColor,
			uint32_t baseColorSampler,
			float alphaCutoff)
			{
				if (!material)
				{
					HashCombine(depthRevision, 0u);
					return;
				}

				const auto& state = material->GetRenderState();
				HashCombine(
					depthRevision,
					renderQueue,
					static_cast<uint32_t>(state.GetCullMode()),
					state.IsRequiredCustomDepthShader());
				if (state.IsRequiredCustomDepthShader())
				{
					HashMaterialVersion(depthRevision, material);
				}
				if (renderQueue == maskedQueue)
				{
					HashCombine(
						depthRevision,
						std::hash<glm::vec4>{}(baseColor),
						baseColorSampler,
						std::hash<float>{}(alphaCutoff));
				}
			};
		for (size_t index = 0u; index < proxy.m_overrideMaterials.Num(); ++index)
		{
			hashDepthMaterial(
				proxy.m_overrideMaterials[index],
				index < proxy.m_renderQueueTags.Num() ? proxy.m_renderQueueTags[index] : 0u,
				index < proxy.m_baseColorFactors.Num() ? proxy.m_baseColorFactors[index] : glm::vec4(1.0f),
				index < proxy.m_baseColorSamplers.Num() ? proxy.m_baseColorSamplers[index] : 0u,
				index < proxy.m_alphaCutoffs.Num() ? proxy.m_alphaCutoffs[index] : 0.5f);
		}
		for (const auto& group : proxy.m_instancedGroups)
		{
			for (size_t index = 0u; index < group.m_materials.Num(); ++index)
			{
				hashDepthMaterial(
					group.m_materials[index],
					index < group.m_renderQueueTags.Num() ? group.m_renderQueueTags[index] : 0u,
					index < group.m_baseColorFactors.Num() ? group.m_baseColorFactors[index] : glm::vec4(1.0f),
					index < group.m_baseColorSamplers.Num() ? group.m_baseColorSamplers[index] : 0u,
					index < group.m_alphaCutoffs.Num() ? group.m_alphaCutoffs[index] : 0.5f);
			}
		}
		resource.m_depthRevision = depthRevision;

		size_t shadowRevision = geometryRevision;
		if (proxy.m_shadowCaster)
		{
			for (const auto& shadowMesh : proxy.m_shadowCaster->m_meshes)
			{
				resource.m_bHasLods |= shadowMesh.m_mesh && shadowMesh.m_mesh->GetNumLods() > 1u;
				HashMatrix(shadowRevision, shadowMesh.m_localMatrix);
				HashCombine(
					shadowRevision,
					shadowMesh.m_mesh,
					shadowMesh.m_renderQueueTag,
					std::hash<glm::vec4>{}(shadowMesh.m_baseColorFactor),
					shadowMesh.m_baseColorSampler,
					std::hash<float>{}(shadowMesh.m_alphaCutoff),
					std::hash<float>{}(shadowMesh.m_maxCameraDistance));
				if (shadowMesh.m_customDepthMaterial)
				{
					HashMaterialVersion(
						shadowRevision,
						shadowMesh.m_customDepthMaterial);
					HashCombine(shadowRevision, shadowMesh.m_customDepthShader);
				}
#if defined(__APPLE__)
				HashTextureSamplers(shadowRevision, shadowMesh.m_materialTextureSamplers);
#endif
			}
		}
		for (const auto& group : proxy.m_instancedGroups)
		{
			HashCombine(
				shadowRevision,
				group.m_bCastShadows,
				std::hash<float>{}(group.m_maxShadowDistance));
			for (size_t index = 0u; index < group.m_materials.Num(); ++index)
			{
				HashCombine(
					shadowRevision,
					index < group.m_renderQueueTags.Num() ? group.m_renderQueueTags[index] : 0u,
					index < group.m_baseColorFactors.Num() ?
						std::hash<glm::vec4>{}(group.m_baseColorFactors[index]) : 0u,
					index < group.m_baseColorSamplers.Num() ? group.m_baseColorSamplers[index] : 0u,
					index < group.m_alphaCutoffs.Num() ?
						std::hash<float>{}(group.m_alphaCutoffs[index]) : 0u);
				if (index < group.m_sourceMaterialShaders.Num() &&
					group.m_sourceMaterialShaders[index] &&
					index < group.m_materials.Num() &&
					group.m_materials[index] &&
					group.m_materials[index]->GetRenderState().IsRequiredCustomDepthShader())
				{
					const auto materialVersion = group.m_materials[index]->GetVersion();
					HashCombine(
						shadowRevision,
						group.m_sourceMaterialShaders[index],
						group.m_materials[index],
						materialVersion ? materialVersion->GetVersionId() : 0ull);
				}
			}
		}
		resource.m_shadowRevision = shadowRevision;
	}

	void PrepareProxyResource(RHISceneProxyResource& resource)
	{
		auto& proxy = resource.m_proxy;
		auto shadowCaster = proxy.m_shadowCaster ?
			TSharedPtr<RHIShadowCasterProxy>::Make(*proxy.m_shadowCaster) : TSharedPtr<RHIShadowCasterProxy>{};
#if defined(__APPLE__)
		for (auto& textures : proxy.m_materialTextureSamplers)
		{
			NormalizeTextureSamplers(textures);
		}
		for (auto& group : proxy.m_instancedGroups)
		{
			for (auto& textures : group.m_materialTextureSamplers)
			{
				NormalizeTextureSamplers(textures);
			}
		}
		if (shadowCaster)
		{
			for (auto& mesh : shadowCaster->m_meshes)
			{
				NormalizeTextureSamplers(mesh.m_materialTextureSamplers);
			}
		}
#endif
		proxy.m_shadowCaster = std::move(shadowCaster);
		CalculateProxyResourceRevisions(resource);
	}
}

RHISceneProxyResource::RHISceneProxyResource(const RHISceneViewProxy& proxy) :
	m_proxy(proxy)
{
	PrepareProxyResource(*this);
}

RHISceneProxyResource::RHISceneProxyResource(RHISceneViewProxy&& proxy) :
	m_proxy(std::move(proxy))
{
	PrepareProxyResource(*this);
}

const RHISceneViewProxy* RHIVisibleSceneProxy::GetSource() const
{
	return &m_resource->m_proxy;
}

const glm::mat4& RHIVisibleSceneProxy::GetWorldMatrix() const
{
	return m_record->m_worldMatrix;
}

const Math::AABB& RHIVisibleSceneProxy::GetWorldBounds() const
{
	return m_record->m_worldBounds;
}

EMobilityType RHIVisibleSceneProxy::GetMobility() const
{
	return m_record->m_mobility;
}

uint32_t RHIVisibleSceneProxy::GetSkeletonOffset() const
{
	return m_record->m_skeletonOffset;
}

uint32_t RHIVisibleSceneProxy::GetRenderFlags() const
{
	return m_record->m_renderFlags;
}

uint64_t RHIVisibleSceneProxy::GetContentRevision() const
{
	return m_record->m_topologyRevision;
}

glm::mat4 RHIVisibleSceneProxy::ResolveMeshWorldMatrix(size_t meshIndex) const
{
	const auto* source = GetSource();
	if (meshIndex >= source->m_meshModelMatrices.Num())
	{
		return GetWorldMatrix();
	}
	return GetWorldMatrix() * source->m_meshModelMatrices[meshIndex];
}

glm::mat4 RHIVisibleSceneProxy::ResolveInstancedMeshWorldMatrix(
	const RHIInstancedMeshGroup& group,
	size_t instanceIndex,
	size_t meshIndex) const
{
	if (instanceIndex >= group.m_instanceTransforms.Num())
	{
		return GetWorldMatrix();
	}
	const glm::mat4 meshTransform = meshIndex < group.m_meshTransforms.Num() ?
		group.m_meshTransforms[meshIndex] : glm::mat4(1.0f);
	return GetWorldMatrix() * group.m_instanceTransforms[instanceIndex] * meshTransform;
}

bool RHIVisibleSceneProxy::IsInstancedMeshWithinDistance(
	const RHIInstancedMeshGroup& group,
	size_t instanceIndex,
	size_t meshIndex,
	const glm::vec3& cameraPosition,
	float maxDistance) const
{
	if (!std::isfinite(maxDistance) || meshIndex >= group.m_meshes.Num() ||
		!group.m_meshes[meshIndex])
	{
		return true;
	}
	Math::AABB worldBounds = group.m_meshes[meshIndex]->m_bounds;
	worldBounds.Apply(ResolveInstancedMeshWorldMatrix(
		group,
		instanceIndex,
		meshIndex));
	if (!worldBounds.IsValid())
	{
		return true;
	}
	const glm::vec3 closestPoint = glm::clamp(
		cameraPosition,
		worldBounds.m_min,
		worldBounds.m_max);
	const float instanceMaxDistance = maxDistance * ResolveInstanceDistanceScale(
		group.m_instanceCullDistanceScales,
		instanceIndex);
	return glm::distance(cameraPosition, closestPoint) <= instanceMaxDistance;
}

const RHIShadowCasterProxy* RHIVisibleShadowCaster::GetSource() const
{
	return m_resource->m_proxy.m_shadowCaster.GetRawPtr();
}

const glm::mat4& RHIVisibleShadowCaster::GetWorldMatrix() const
{
	return m_record->m_worldMatrix;
}

const Math::AABB& RHIVisibleShadowCaster::GetWorldBounds() const
{
	return m_record->m_worldBounds;
}

EMobilityType RHIVisibleShadowCaster::GetMobility() const
{
	return m_record->m_mobility;
}

uint32_t RHIVisibleShadowCaster::GetSkeletonOffset() const
{
	return m_record->m_skeletonOffset;
}

uint64_t RHIVisibleShadowCaster::GetProducerKey() const
{
	return m_record->m_producerKey;
}

uint64_t RHIVisibleShadowCaster::GetContentRevision() const
{
	return m_resource->m_shadowRevision;
}

glm::mat4 RHIVisibleShadowCaster::ResolveMeshWorldMatrix(
	const RHIShadowMeshProxy& shadowMesh) const
{
	return GetWorldMatrix() * shadowMesh.m_localMatrix;
}

glm::mat4 RHIVisibleShadowCaster::ResolveInstancedMeshWorldMatrix(
	const RHIInstancedMeshGroup& group,
	size_t instanceIndex,
	size_t meshIndex) const
{
	if (instanceIndex >= group.m_instanceTransforms.Num())
	{
		return GetWorldMatrix();
	}
	const glm::mat4 meshTransform = meshIndex < group.m_meshTransforms.Num() ?
		group.m_meshTransforms[meshIndex] : glm::mat4(1.0f);
	return GetWorldMatrix() * group.m_instanceTransforms[instanceIndex] * meshTransform;
}

bool RHIVisibleShadowCaster::IsInstancedMeshWithinDistance(
	const RHIInstancedMeshGroup& group,
	size_t instanceIndex,
	size_t meshIndex,
	const glm::vec3& cameraPosition,
	float maxDistance) const
{
	if (!std::isfinite(maxDistance) || meshIndex >= group.m_meshes.Num() ||
		!group.m_meshes[meshIndex])
	{
		return true;
	}
	Math::AABB worldBounds = group.m_meshes[meshIndex]->m_bounds;
	worldBounds.Apply(ResolveInstancedMeshWorldMatrix(
		group,
		instanceIndex,
		meshIndex));
	if (!worldBounds.IsValid())
	{
		return true;
	}
	const glm::vec3 closestPoint = glm::clamp(
		cameraPosition,
		worldBounds.m_min,
		worldBounds.m_max);
	const float instanceMaxDistance = maxDistance * ResolveInstanceDistanceScale(
		group.m_instanceShadowDistanceScales,
		instanceIndex);
	return glm::distance(cameraPosition, closestPoint) <= instanceMaxDistance;
}

float Sailor::RHI::CalculateScreenCoverage(
	const Math::AABB& worldBounds,
	const glm::mat4& viewMatrix,
	const glm::mat4& projectionMatrix)
{
	if (!worldBounds.IsValid() ||
		!Math::AllFinite(viewMatrix) ||
		!Math::AllFinite(projectionMatrix))
	{
		return 0.0f;
	}

	const glm::vec3 min = worldBounds.m_min;
	const glm::vec3 max = worldBounds.m_max;
	const std::array<glm::vec3, 8u> corners = {
		glm::vec3(min.x, min.y, min.z),
		glm::vec3(max.x, min.y, min.z),
		glm::vec3(min.x, max.y, min.z),
		glm::vec3(max.x, max.y, min.z),
		glm::vec3(min.x, min.y, max.z),
		glm::vec3(max.x, min.y, max.z),
		glm::vec3(min.x, max.y, max.z),
		glm::vec3(max.x, max.y, max.z)
	};

	const glm::mat4 viewProjection = projectionMatrix * viewMatrix;
	std::array<glm::vec4, 8u> clipCorners{};
	uint32_t numCornersInFront = 0u;
	for (size_t cornerIndex = 0u;
		cornerIndex < corners.size();
		++cornerIndex)
	{
		glm::vec4& clip = clipCorners[cornerIndex];
		clip = viewProjection * glm::vec4(corners[cornerIndex], 1.0f);
		if (!Math::AllFinite(clip))
		{
			return 0.0f;
		}
		numCornersInFront += clip.w > 1e-5f ? 1u : 0u;
	}
	if (numCornersInFront == 0u)
	{
		return 0.0f;
	}
	if (numCornersInFront < corners.size())
	{
		return 1.0f;
	}

	glm::vec2 minNdc((std::numeric_limits<float>::max)());
	glm::vec2 maxNdc((std::numeric_limits<float>::lowest)());
	for (const glm::vec4& clip : clipCorners)
	{
		const glm::vec2 ndc = glm::vec2(clip) / clip.w;
		minNdc = glm::min(minNdc, ndc);
		maxNdc = glm::max(maxNdc, ndc);
	}

	minNdc = glm::max(minNdc, glm::vec2(-1.0f));
	maxNdc = glm::min(maxNdc, glm::vec2(1.0f));
	const glm::vec2 coveredNdc = glm::max(
		maxNdc - minNdc,
		glm::vec2(0.0f));
	return (std::clamp)(
		coveredNdc.x * coveredNdc.y * 0.25f,
		0.0f,
		1.0f);
}

uint32_t RHI::RHILodPolicy::Resolve(float screenCoverage, uint32_t numAvailableLods) const
{
	return Resolve(screenCoverage, 0.0f, numAvailableLods);
}

uint32_t RHI::RHILodPolicy::Resolve(
	float screenCoverage,
	float cameraDistance,
	uint32_t numAvailableLods) const
{
	if (numAvailableLods == 0u)
	{
		return 0u;
	}

	const uint32_t highestAvailableLod = numAvailableLods - 1u;
	const uint32_t minLod = (std::min)(m_minLod, highestAvailableLod);
	const uint32_t maxLod = (std::max)(minLod, (std::min)(m_maxLod, highestAvailableLod));
	uint32_t selectedLod = 0u;
	if (!m_cameraDistanceThresholds.IsEmpty())
	{
		const float distance = std::isfinite(cameraDistance) ?
			(std::max)(cameraDistance, 0.0f) :
			(std::numeric_limits<float>::infinity)();
		for (float threshold : m_cameraDistanceThresholds)
		{
			if (!std::isfinite(threshold) || distance < threshold)
			{
				break;
			}
			++selectedLod;
		}
	}
	else
	{
		const float coverage = (std::clamp)(screenCoverage, 0.0f, 1.0f);
		for (size_t index = 0u; index < m_screenCoverageThresholds.Num(); ++index)
		{
			if (!std::isfinite(m_screenCoverageThresholds[index]) ||
				coverage >= m_screenCoverageThresholds[index])
			{
				break;
			}
			selectedLod = static_cast<uint32_t>(index + 1u);
		}
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

void RHISceneView::PrepareDebugDrawCommandLists(
	WorldPtr world,
	const glm::ivec2& renderExtent)
{
	m_debugDraw.Reserve(m_cameras.Num());
	const DebugContext::DrawSnapshot debugDrawSnapshot = world->GetDebugContext()->GetDrawSnapshot();

	for (const auto& camera : m_cameras)
	{
		const glm::mat4 viewProjection = camera.GetProjectionMatrix() * camera.GetViewMatrix();
		auto task = Tasks::CreateTaskWithResult<RHI::RHICommandListPtr>("Record DebugContext Draw Command List"_h,
			[debugDrawSnapshot, viewProjection, renderExtent]()
			{
				RHI::RHICommandListPtr secondaryCmdList = RHI::Renderer::GetDriver()->CreateCommandList(true, RHI::ECommandListQueue::Graphics);
				Sailor::RHI::Renderer::GetDriver()->SetDebugName(secondaryCmdList, "Draw Debug Mesh"_h);
				auto commands = App::GetSubmodule<Renderer>()->GetDriverCommands();
				commands->BeginSecondaryCommandList(secondaryCmdList, false, true);
				DebugContext::DrawDebugMesh(
					secondaryCmdList,
					viewProjection,
					debugDrawSnapshot,
					renderExtent);
				commands->EndCommandList(secondaryCmdList);

				return secondaryCmdList;
			}, EThreadType::RHI);

		m_debugDraw.Emplace(std::move(task));
	}
}

void RHISceneView::Clear()
{
	m_rhiLightsData.Clear();
	m_rhiLightsDataPerCamera.Clear(false);
	m_boneMatrices.Clear();

	m_cameras.Clear(false);
	m_cameraTransforms.Clear(false);
	m_shadowMapsToUpdate.Clear(false);
	m_shadowMapsToBlit.Clear(false);
	m_shadowIndices.Clear(false);
	m_shadowAtlasTiles.Clear(false);
	m_shadowMatrices.Clear(false);
	m_cpuLightsData.Clear();
	m_lightingRevision = 0ull;
	m_cpuBoneMatrices.Clear();
	m_animationRevision = 0ull;
	m_globalIlluminationMode =
		EGlobalIlluminationMode::Baked;
	m_bGlobalIlluminationEnabled = true;
	m_globalIllumination.Clear();

	m_drawImGui.Clear();
	m_debugDraw.Clear(false);
	{
		SAILOR_PROFILE_SCOPE("Reset submitted view snapshots");
		for (auto& snapshot : m_snapshots)
		{
			snapshot.ResetForReuse();
		}
	}
	m_submissionContext.Clear();
	m_submissionCompletionToken.Clear();
	{
		SAILOR_PROFILE_SCOPE("Release submitted scene versions");
		m_sceneVersions.Clear(false);
		m_virtualSceneVersions.Clear(false);
		m_retainedSceneVersions.Clear();
	}
	m_renderMode = ESceneViewRenderMode::Lit;
	m_shadowCastersRevision = 0ull;
	m_bHasCustomDepthShadowCasters = false;
	m_pathTracerScene.Clear();
}

UboFrameData RHISceneViewSnapshot::GetFrameData(const glm::ivec2& extent) const
{
	UboFrameData frame;
	frame.m_cameraPosition = m_cameraTransform.m_position;
	frame.m_projection = m_camera->GetProjectionMatrix();
	frame.m_invProjection = m_camera->GetInvProjection();
	frame.m_cameraZNearZFar = glm::vec2(m_camera->GetZNear(), m_camera->GetZFar());
	frame.m_currentTime = m_currentTime;
	frame.m_deltaTime = m_deltaTime;
	frame.m_view = m_camera->GetViewMatrix();
	frame.m_viewportSize = extent;
	return frame;
}

void RHISceneViewSnapshot::ResetForReuse()
{
	SAILOR_PROFILE_FUNCTION();
	m_submissionContext.Clear();
	m_submissionCompletionToken.Clear();
	m_world = nullptr;
	m_currentTime = 0.0f;
	m_previousMotionFrame.Clear();
	m_sceneVersions.Clear();
	m_renderMode = ESceneViewRenderMode::Lit;
	m_deltaTime = 0.0f;
	m_frame = 0ull;
	m_cameraIndex = 0u;
	m_cameraTransform = {};
	{
		SAILOR_PROFILE_SCOPE("Release visible proxies and LOD meshes");
		m_proxies.Clear(false);
		m_lodMeshes.Clear(false);
		m_instancedLodOffsets.Clear(false);
	}
	m_pathTracerScene.Clear();
	m_totalNumLights = 0u;
	m_shadowMapsToUpdate.Clear(false);
	m_shadowMapsToBlit.Clear(false);
	m_shadowIndices.Clear(false);
	m_shadowAtlasTiles.Clear(false);
	m_frameBindings.Clear();
	m_rhiLightsData.Clear();
	m_rhiLightCullingData.Clear();
	m_cpuLightsData.Clear();
	m_shadowMatrices.Clear(false);
	m_lightingRevision = 0ull;
	m_boneMatrices.Clear();
	m_cpuBoneMatrices.Clear();
	m_animationRevision = 0ull;
	m_globalIlluminationMode =
		EGlobalIlluminationMode::Baked;
	m_bGlobalIlluminationEnabled = true;
	m_globalIllumination.Clear();
	m_debugDrawSecondaryCmdList.Clear();
	m_drawImGui.Clear();
}

const RHIMeshPtr& RHISceneViewSnapshot::ResolveMesh(const RHIVisibleSceneProxy& proxy, size_t meshIndex) const
{
	const auto* source = proxy.GetSource();
	if (meshIndex >= source->m_meshes.Num())
	{
		static const RHIMeshPtr empty;
		return empty;
	}
	return proxy.m_meshLodOffset == RHIVisibleSceneProxy::InvalidIndex ?
		source->m_meshes[meshIndex] : m_lodMeshes[proxy.m_meshLodOffset + meshIndex];
}

const RHIMeshPtr& RHISceneViewSnapshot::ResolveMesh(const RHIVisibleShadowCaster& proxy, size_t meshIndex) const
{
	const auto* source = proxy.GetSource();
	if (!source || meshIndex >= source->m_meshes.Num())
	{
		static const RHIMeshPtr empty;
		return empty;
	}
	return proxy.m_meshLodOffset == RHIVisibleSceneProxy::InvalidIndex ?
		source->m_meshes[meshIndex].m_mesh : m_lodMeshes[proxy.m_meshLodOffset + meshIndex];
}

void RHISceneViewSnapshot::PrepareLods(const glm::mat4& viewMatrix, const glm::mat4& projectionMatrix)
{
	SAILOR_PROFILE_FUNCTION();
	m_lodMeshes.Clear(false);
	m_instancedLodOffsets.Clear(false);
	struct LodOffsets
	{
		uint32_t m_main = RHIVisibleSceneProxy::InvalidIndex;
		uint32_t m_shadow = RHIVisibleSceneProxy::InvalidIndex;
		uint32_t m_instanced = RHIVisibleSceneProxy::InvalidIndex;
	};
	TMap<const void*, LodOffsets> preparedInstances;
	const glm::vec3 cameraPosition(m_cameraTransform.m_position);
	auto prepare = [&](const RHIVisibleSceneProxy& proxy)
		{
			LodOffsets offsets;
			const auto* source = proxy.GetSource();
			if (!source->m_lodPolicy.m_bEnabled || !proxy.m_resource->m_bHasLods)
			{
				return offsets;
			}
			// Record identity also distinguishes equal handles in different scene roots.
			const void* key = proxy.m_record;
			LodOffsets* existing = nullptr;
			if (preparedInstances.Find(key, existing))
			{
				return *existing;
			}
			const auto& policy = source->m_lodPolicy;
			const float coverage = policy.m_cameraDistanceThresholds.IsEmpty() ?
				CalculateScreenCoverage(proxy.GetWorldBounds(), viewMatrix, projectionMatrix) : 1.0f;
			const float cameraDistance = glm::distance(cameraPosition,
				glm::clamp(cameraPosition, proxy.GetWorldBounds().m_min, proxy.GetWorldBounds().m_max));
			auto selectMesh = [&](const RHIMeshPtr& mesh, float meshCoverage, float distance, int32_t instanceBias)
				{
					if (!mesh || mesh->GetNumLods() <= 1u)
					{
						return mesh;
					}
					uint32_t lod = policy.Resolve(meshCoverage, distance, mesh->GetNumLods());
					if (instanceBias != 0)
					{
						lod = Settings::ApplyLodBias(lod, mesh->GetNumLods(),
							policy.m_minLod, policy.m_maxLod, instanceBias);
					}
					if (lod > 0u)
					{
						if (auto selected = mesh->GetLod(lod))
						{
							return selected;
						}
					}
					return mesh;
				};
			offsets.m_main = static_cast<uint32_t>(m_lodMeshes.Num());
			for (const auto& mesh : source->m_meshes)
			{
				m_lodMeshes.Add(selectMesh(mesh, coverage, cameraDistance, 0));
			}
			offsets.m_shadow = static_cast<uint32_t>(m_lodMeshes.Num());
			if (source->m_shadowCaster)
			{
				for (const auto& shadowMesh : source->m_shadowCaster->m_meshes)
				{
					RHIMeshPtr selected;
					bool bSelected = false;
					for (size_t meshIndex = 0u; meshIndex < source->m_meshes.Num(); ++meshIndex)
					{
						if (source->m_meshes[meshIndex] == shadowMesh.m_mesh)
						{
							selected = m_lodMeshes[offsets.m_main + meshIndex];
							bSelected = true;
							break;
						}
					}
					if (!bSelected)
					{
						selected = selectMesh(shadowMesh.m_mesh, coverage, cameraDistance, 0);
					}
					m_lodMeshes.Add(std::move(selected));
				}
			}
			offsets.m_instanced = static_cast<uint32_t>(m_instancedLodOffsets.Num());
			for (const auto& group : source->m_instancedGroups)
			{
				m_instancedLodOffsets.Add(static_cast<uint32_t>(m_lodMeshes.Num()));
				for (size_t meshIndex = 0u; meshIndex < group.m_meshes.Num(); ++meshIndex)
				{
					const auto& mesh = group.m_meshes[meshIndex];
					for (size_t instanceIndex = 0u; instanceIndex < group.m_instanceTransforms.Num(); ++instanceIndex)
					{
						if (!mesh || mesh->GetNumLods() <= 1u)
						{
							m_lodMeshes.Add(mesh);
							continue;
						}
						Math::AABB bounds = mesh->m_bounds;
						bounds.Apply(proxy.ResolveInstancedMeshWorldMatrix(group, instanceIndex, meshIndex));
						const float instanceCoverage = policy.m_cameraDistanceThresholds.IsEmpty() ?
							CalculateScreenCoverage(bounds, viewMatrix, projectionMatrix) : 1.0f;
						const float distance = glm::distance(cameraPosition,
							glm::clamp(cameraPosition, bounds.m_min, bounds.m_max));
						m_lodMeshes.Add(selectMesh(mesh, instanceCoverage, distance,
							ResolveInstanceLodBias(group, instanceIndex)));
					}
				}
			}
			preparedInstances.Insert(key, offsets);
			return offsets;
		};

	for (auto& proxy : m_proxies)
	{
		const auto offsets = prepare(proxy);
		proxy.m_meshLodOffset = offsets.m_main;
		proxy.m_instancedLodOffset = offsets.m_instanced;
	}
	for (auto& pass : m_shadowMapsToUpdate)
	{
		for (auto& caster : pass.m_meshList)
		{
			RHIVisibleSceneProxy proxy(caster.m_handle, *caster.m_record, *caster.m_resource);
			const auto offsets = prepare(proxy);
			caster.m_meshLodOffset = offsets.m_shadow;
			caster.m_instancedLodOffset = offsets.m_instanced;
		}
	}
}

uint64_t RHISceneViewSnapshot::GetMobilityRevision(EMobilityType mobility) const
{
	size_t result = Fnv1aOffsetBasis;
	HashCombine(result, static_cast<uint32_t>(mobility));
	if (!m_sceneVersions)
	{
		return static_cast<uint64_t>(result);
	}

	HashCombine(result, m_sceneVersions->Num());
	for (const auto& sceneVersion : *m_sceneVersions)
	{
		if (!sceneVersion)
		{
			HashCombine(result, 0ull);
			continue;
		}

		uint64_t revision = sceneVersion->m_dynamicRevision;
		size_t numHandles = sceneVersion->m_dynamicHandles ?
			sceneVersion->m_dynamicHandles->Num() : 0u;
		switch (mobility)
		{
		case EMobilityType::Static:
			revision = sceneVersion->m_staticRevision;
			numHandles = sceneVersion->m_staticHandles ?
				sceneVersion->m_staticHandles->Num() : 0u;
			break;
		case EMobilityType::Stationary:
			revision = sceneVersion->m_stationaryRevision;
			numHandles = sceneVersion->m_stationaryHandles ?
				sceneVersion->m_stationaryHandles->Num() : 0u;
			break;
		case EMobilityType::Dynamic:
			break;
		}
		HashCombine(
			result,
			sceneVersion->m_sceneIdentity,
			revision,
			numHandles);
	}
	return static_cast<uint64_t>(result);
}

void RHISceneView::AddSceneVersion(RHISpatialSceneVersionPtr sceneVersion)
{
	if (!sceneVersion)
	{
		return;
	}

	HashCombine(
		m_shadowCastersRevision,
		sceneVersion->m_shadowCastersRevision);
	m_bHasCustomDepthShadowCasters |= sceneVersion->m_bHasCustomDepthShadowCasters;
	if (sceneVersion->m_sceneVersion)
	{
		m_virtualSceneVersions.Add(sceneVersion->m_sceneVersion);
		m_retainedSceneVersions.Clear();
	}
	m_sceneVersions.Emplace(std::move(sceneVersion));
}

TSharedPtr<const TVector<RHISceneVersionPtr>> RHISceneView::GetRetainedSceneVersions()
{
	if (!m_retainedSceneVersions)
	{
		m_retainedSceneVersions = TSharedPtr<const TVector<RHISceneVersionPtr>>::Make(m_virtualSceneVersions);
	}
	return m_retainedSceneVersions;
}

void RHISceneView::SetSubmissionContext(RHIRenderSubmissionContextPtr submissionContext)
{
	m_submissionContext = std::move(submissionContext);
	for (auto& snapshot : m_snapshots)
	{
		snapshot.m_submissionContext = m_submissionContext;
	}
}

RHISubmissionCompletionTokenPtr RHISceneView::GetOrCreateSubmissionCompletionToken()
{
	if (!m_submissionCompletionToken)
	{
		m_submissionCompletionToken = RHISubmissionCompletionTokenPtr::Make();
	}
	return m_submissionCompletionToken;
}

bool RHISceneView::IsCurrentSubmissionCompletionToken(
	const RHISubmissionCompletionTokenPtr& token) const
{
	return token && token == m_submissionCompletionToken;
}

void RHISceneView::CompleteSubmissionResources(bool bSucceeded)
{
	if (m_submissionCompletionToken)
	{
		m_submissionCompletionToken->Complete(bSucceeded);
	}
}

TVector<RHIVisibleSceneProxy> RHISceneView::TraceScene(const Math::Frustum& frustum) const
{
	TVector<RHIVisibleSceneProxy> result;
	TraceScene(frustum, result);
	return result;
}

void RHISceneView::TraceScene(
	const Math::Frustum& frustum,
	TVector<RHIVisibleSceneProxy>& result) const
{
	SAILOR_PROFILE_FUNCTION();

	result.Clear(false);
	size_t numCandidates = 0u;
	for (const auto& spatialVersion : m_sceneVersions)
	{
		if (spatialVersion)
		{
			numCandidates += spatialVersion->m_dynamicOctree ?
				spatialVersion->m_dynamicOctree->Num() : 0u;
			numCandidates += spatialVersion->m_stationaryOctree ?
				spatialVersion->m_stationaryOctree->Num() : 0u;
			numCandidates += spatialVersion->m_staticOctree ?
				spatialVersion->m_staticOctree->Num() : 0u;
		}
	}
	result.Reserve(numCandidates);

	for (const auto& spatialVersion : m_sceneVersions)
	{
		if (!spatialVersion || !spatialVersion->m_sceneVersion)
		{
			continue;
		}

		auto appendHandle = [&result, &sceneVersion = spatialVersion->m_sceneVersion](
			const RenderInstanceHandle& handle)
			{
				const RHISceneInstanceRecord* record = nullptr;
				if (!sceneVersion->Resolve(handle, record) || !record)
				{
					return;
				}
				const auto* resource = dynamic_cast<const RHISceneProxyResource*>(
					record->m_topology.GetRawPtr());
				if (!resource)
				{
					return;
				}

				RHIVisibleSceneProxy visible(handle, *record, *resource);
				result.Add(std::move(visible));
		};
		if (spatialVersion->m_dynamicOctree)
		{
			spatialVersion->m_dynamicOctree->Trace(frustum, appendHandle);
		}
		if (spatialVersion->m_stationaryOctree)
		{
			spatialVersion->m_stationaryOctree->Trace(frustum, appendHandle);
		}
		if (spatialVersion->m_staticOctree)
		{
			spatialVersion->m_staticOctree->Trace(frustum, appendHandle);
		}
	}

}

TVector<RHIVisibleShadowCaster> RHISceneView::TraceShadowCasters(
	const Math::Frustum& frustum) const
{
	TVector<RHIVisibleShadowCaster> result;
	TraceShadowCasters(frustum, result);
	return result;
}

void RHISceneView::TraceShadowCasters(
	const Math::Frustum& frustum,
	TVector<RHIVisibleShadowCaster>& result) const
{
	SAILOR_PROFILE_FUNCTION();

	result.Clear(false);
	size_t numShadowCandidates = 0u;
	for (const auto& sceneVersion : m_sceneVersions)
	{
		if (sceneVersion)
		{
			numShadowCandidates += sceneVersion->m_dynamicOctree ?
				sceneVersion->m_dynamicOctree->Num() : 0u;
			numShadowCandidates += sceneVersion->m_stationaryOctree ?
				sceneVersion->m_stationaryOctree->Num() : 0u;
			numShadowCandidates += sceneVersion->m_staticOctree ?
				sceneVersion->m_staticOctree->Num() : 0u;
		}
	}
	result.Reserve(numShadowCandidates);
	for (const auto& spatialVersion : m_sceneVersions)
	{
		if (!spatialVersion || !spatialVersion->m_sceneVersion)
		{
			continue;
		}

		auto appendHandle = [&result,
			&sceneVersion = spatialVersion->m_sceneVersion](
			const RenderInstanceHandle& handle)
			{
				const RHISceneInstanceRecord* record = nullptr;
				if (!sceneVersion->Resolve(handle, record) || !record ||
					(record->m_renderFlags & 1u) == 0u)
				{
					return;
				}
				const auto* resource = dynamic_cast<const RHISceneProxyResource*>(
					record->m_topology.GetRawPtr());
				if (!resource || !resource->m_proxy.m_shadowCaster)
				{
					return;
				}

				RHIVisibleShadowCaster visible(handle, *record, *resource);
				result.Add(std::move(visible));
		};
		if (spatialVersion->m_dynamicOctree)
		{
			spatialVersion->m_dynamicOctree->Trace(frustum, appendHandle);
		}
		if (spatialVersion->m_stationaryOctree)
		{
			spatialVersion->m_stationaryOctree->Trace(frustum, appendHandle);
		}
		if (spatialVersion->m_staticOctree)
		{
			spatialVersion->m_staticOctree->Trace(frustum, appendHandle);
		}
	}

}

void RHISceneView::PrepareSnapshots()
{
	SAILOR_PROFILE_FUNCTION();
	m_snapshots.Resize(m_cameras.Num());

	for (uint32_t i = 0; i < m_cameras.Num(); i++)
	{
		auto& camera = m_cameras[i];
		auto& res = m_snapshots[i];
		res.ResetForReuse();
		res.m_submissionContext = m_submissionContext;
		res.m_submissionCompletionToken = GetOrCreateSubmissionCompletionToken();
		res.m_world = m_world;
		res.m_currentTime = m_currentTime;
		res.m_sceneVersions = GetRetainedSceneVersions();
		res.m_renderMode = m_renderMode;

		Math::Frustum frustum;

		frustum.ExtractFrustumPlanes(m_cameraTransforms[i].Matrix(), camera.GetAspect(), camera.GetFov(), camera.GetZNear(), camera.GetZFar());

		res.m_deltaTime = m_deltaTime;
		res.m_frame = m_world->GetCurrentFrame();
		res.m_cameraIndex = i;
		res.m_cameraTransform = m_cameraTransforms[i];
		if (!res.m_camera)
		{
			res.m_camera = TUniquePtr<CameraData>::Make();
		}
		*res.m_camera = camera;
		res.m_pathTracerScene = m_pathTracerScene;

		res.m_totalNumLights = m_totalNumLights;
		res.m_rhiLightsData = i < m_rhiLightsDataPerCamera.Num() ?
			m_rhiLightsDataPerCamera[i] : m_rhiLightsData;
		res.m_boneMatrices = m_boneMatrices;
		res.m_drawImGui = m_drawImGui;
		res.m_shadowMapsToUpdate = std::move(m_shadowMapsToUpdate[i]);
		res.m_shadowMapsToBlit = std::move(m_shadowMapsToBlit[i]);
		res.m_shadowIndices = std::move(m_shadowIndices[i]);
		res.m_shadowAtlasTiles = std::move(m_shadowAtlasTiles[i]);
		res.m_shadowMatrices = std::move(m_shadowMatrices[i]);
		res.m_cpuLightsData = m_cpuLightsData;
		res.m_lightingRevision = m_lightingRevision;
		res.m_cpuBoneMatrices = m_cpuBoneMatrices;
		res.m_animationRevision = m_animationRevision;
		res.m_globalIlluminationMode = m_globalIlluminationMode;
		res.m_bGlobalIlluminationEnabled =
			m_bGlobalIlluminationEnabled;
		res.m_globalIllumination = m_globalIllumination;
		TraceScene(frustum, res.m_proxies);
		const glm::vec3 cameraPosition = glm::vec3(m_cameraTransforms[i].m_position);
		size_t visibleProxyWriteIndex = 0u;
		for (size_t visibleProxyReadIndex = 0u;
			visibleProxyReadIndex < res.m_proxies.Num();
			++visibleProxyReadIndex)
		{
			auto& proxy = res.m_proxies[visibleProxyReadIndex];
			const auto* source = proxy.GetSource();
			const glm::vec3 closest = glm::clamp(cameraPosition, proxy.GetWorldBounds().m_min, proxy.GetWorldBounds().m_max);
			if (std::isfinite(source->m_lodPolicy.m_maxCameraDistance) &&
				glm::distance(cameraPosition, closest) > source->m_lodPolicy.m_maxCameraDistance)
			{
				continue;
			}
			if (visibleProxyWriteIndex != visibleProxyReadIndex)
			{
				res.m_proxies[visibleProxyWriteIndex] = std::move(proxy);
			}
			++visibleProxyWriteIndex;
		}
		res.m_proxies.RemoveAt(visibleProxyWriteIndex, res.m_proxies.Num() - visibleProxyWriteIndex);
		res.m_proxies.Sort([](const RHIVisibleSceneProxy& lhs, const RHIVisibleSceneProxy& rhs)
			{
				if (lhs.m_handle.IsValid() != rhs.m_handle.IsValid())
				{
					return lhs.m_handle.IsValid();
				}
				if (lhs.m_handle.IsValid())
				{
					if (lhs.m_handle.m_slot != rhs.m_handle.m_slot)
					{
						return lhs.m_handle.m_slot < rhs.m_handle.m_slot;
					}
					if (lhs.m_handle.m_generation != rhs.m_handle.m_generation)
					{
						return lhs.m_handle.m_generation < rhs.m_handle.m_generation;
					}
				}
				const uint64_t lhsProducer = lhs.m_record->m_producerKey;
				const uint64_t rhsProducer = rhs.m_record->m_producerKey;
				if (lhsProducer != rhsProducer)
				{
					return lhsProducer < rhsProducer;
				}
				return reinterpret_cast<uintptr_t>(lhs.m_resource) <
					reinterpret_cast<uintptr_t>(rhs.m_resource);
			});
		if (i < m_debugDraw.Num())
		{
			res.m_debugDrawSecondaryCmdList = m_debugDraw[i];
		}
	}
}

const TVector<RHIMaterialPtr>& RHISceneViewProxy::GetMaterials() const
{
	// TODO: Create default materials inside model
	return m_overrideMaterials;
}
