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
#include "RHI/CommandList.h"
#include "Settings/GraphicsSettings.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>

using namespace Sailor;
using namespace Sailor::RHI;

namespace
{
	bool TryNormalizeProxyTransforms(RHISceneProxyResource& resource)
	{
		const glm::mat4& referenceWorld = resource.m_proxy.m_worldMatrix;
		const float determinant = glm::determinant(referenceWorld);
		if (!std::isfinite(determinant) || std::abs(determinant) <= 1e-8f)
		{
			return false;
		}

		const glm::mat4 inverseWorld = glm::inverse(referenceWorld);
		if (!Math::AllFinite(inverseWorld))
		{
			return false;
		}

		for (auto& meshMatrix : resource.m_proxy.m_meshModelMatrices)
		{
			meshMatrix = inverseWorld * meshMatrix;
		}
		if (resource.m_proxy.m_shadowCaster)
		{
			resource.m_proxy.m_shadowCaster = RHIShadowCasterProxyPtr::Make(
				*resource.m_proxy.m_shadowCaster);
			for (auto& shadowMesh : resource.m_proxy.m_shadowCaster->m_meshes)
			{
				shadowMesh.m_worldMatrix = inverseWorld * shadowMesh.m_worldMatrix;
			}
		}
		resource.m_bMeshTransformsAreLocal = true;
		return true;
	}

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
	void HashTextureSet(size_t& result, const TSet<uint32_t>& textures)
	{
		auto sortedTextures = textures.ToVector();
		sortedTextures.Sort();
		HashCombine(result, sortedTextures.Num());
		for (uint32_t texture : sortedTextures)
		{
			HashCombine(result, texture);
		}
	}
#endif

	void CalculateProxyResourceRevisions(RHISceneProxyResource& resource)
	{
		const auto& proxy = resource.m_proxy;
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
			HashTextureSet(mainRevision, textures);
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
				HashTextureSet(mainRevision, textures);
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
				HashTextureSet(shadowRevision, shadowMesh.m_materialTextureSamplers);
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
}

RHISceneProxyResource::RHISceneProxyResource(const RHISceneViewProxy& proxy) :
	m_proxy(proxy)
{
	TryNormalizeProxyTransforms(*this);
	CalculateProxyResourceRevisions(*this);
}

RHISceneProxyResource::RHISceneProxyResource(RHISceneViewProxy&& proxy) :
	m_proxy(std::move(proxy))
{
	TryNormalizeProxyTransforms(*this);
	CalculateProxyResourceRevisions(*this);
}

const RHISceneViewProxy* RHIVisibleSceneProxy::GetSource() const
{
	return m_resource ? &m_resource->m_proxy : nullptr;
}

const glm::mat4& RHIVisibleSceneProxy::GetWorldMatrix() const
{
	if (m_record)
	{
		return m_record->m_worldMatrix;
	}
	if (const auto* source = GetSource())
	{
		return source->m_worldMatrix;
	}

	static const glm::mat4 identity{ 1.0f };
	return identity;
}

const Math::AABB& RHIVisibleSceneProxy::GetWorldBounds() const
{
	if (m_record)
	{
		return m_record->m_worldBounds;
	}
	if (const auto* source = GetSource())
	{
		return source->m_worldAabb;
	}

	static const Math::AABB empty{};
	return empty;
}

EMobilityType RHIVisibleSceneProxy::GetMobility() const
{
	if (m_record)
	{
		return m_record->m_mobility;
	}
	if (const auto* source = GetSource())
	{
		return source->m_mobility;
	}
	return EMobilityType::Static;
}

uint32_t RHIVisibleSceneProxy::GetSkeletonOffset() const
{
	if (m_record)
	{
		return m_record->m_skeletonOffset;
	}
	if (const auto* source = GetSource())
	{
		return source->m_skeletonOffset;
	}
	return (std::numeric_limits<uint32_t>::max)();
}

uint32_t RHIVisibleSceneProxy::GetRenderFlags() const
{
	if (m_record)
	{
		return m_record->m_renderFlags;
	}
	if (const auto* source = GetSource())
	{
		return source->m_bCastShadows ? 1u : 0u;
	}
	return 0u;
}

uint64_t RHIVisibleSceneProxy::GetContentRevision() const
{
	return m_record ? m_record->m_topologyRevision : 0ull;
}

glm::mat4 RHIVisibleSceneProxy::ResolveMeshWorldMatrix(size_t meshIndex) const
{
	const auto* source = GetSource();
	if (!source || meshIndex >= source->m_meshModelMatrices.Num())
	{
		return GetWorldMatrix();
	}
	return m_resource->m_bMeshTransformsAreLocal ?
		GetWorldMatrix() * source->m_meshModelMatrices[meshIndex] :
		source->m_meshModelMatrices[meshIndex];
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
	const auto* proxy = m_resource ? &m_resource->m_proxy : nullptr;
	return proxy && proxy->m_shadowCaster ? proxy->m_shadowCaster.GetRawPtr() : nullptr;
}

const glm::mat4& RHIVisibleShadowCaster::GetWorldMatrix() const
{
	if (m_record)
	{
		return m_record->m_worldMatrix;
	}
	if (m_resource)
	{
		return m_resource->m_proxy.m_worldMatrix;
	}

	static const glm::mat4 identity{ 1.0f };
	return identity;
}

const Math::AABB& RHIVisibleShadowCaster::GetWorldBounds() const
{
	if (m_record)
	{
		return m_record->m_worldBounds;
	}
	if (const auto* source = GetSource())
	{
		return source->m_worldAabb;
	}

	static const Math::AABB empty{};
	return empty;
}

EMobilityType RHIVisibleShadowCaster::GetMobility() const
{
	if (m_record)
	{
		return m_record->m_mobility;
	}
	if (m_resource)
	{
		return m_resource->m_proxy.m_mobility;
	}
	return EMobilityType::Static;
}

uint32_t RHIVisibleShadowCaster::GetSkeletonOffset() const
{
	if (m_record)
	{
		return m_record->m_skeletonOffset;
	}
	if (const auto* source = GetSource())
	{
		return source->m_skeletonOffset;
	}
	return (std::numeric_limits<uint32_t>::max)();
}

uint64_t RHIVisibleShadowCaster::GetProducerKey() const
{
	if (m_record)
	{
		return m_record->m_producerKey;
	}
	return m_resource ? m_resource->m_proxy.m_staticMeshEcs : 0ull;
}

uint64_t RHIVisibleShadowCaster::GetContentRevision() const
{
	return m_resource ? m_resource->m_shadowRevision : 0ull;
}

glm::mat4 RHIVisibleShadowCaster::ResolveMeshWorldMatrix(
	const RHIShadowMeshProxy& shadowMesh) const
{
	return m_resource && m_resource->m_bMeshTransformsAreLocal ?
		GetWorldMatrix() * shadowMesh.m_worldMatrix :
		shadowMesh.m_worldMatrix;
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

	// TODO: Check the sync between CPUFrame and Recording
	for (const auto& camera : m_cameras)
	{
		auto task = Tasks::CreateTaskWithResult<RHI::RHICommandListPtr>("Record DebugContext Draw Command List",
			[=]()
			{
				const auto& matrix = camera.GetProjectionMatrix() * camera.GetViewMatrix();
				RHI::RHICommandListPtr secondaryCmdList = RHI::Renderer::GetDriver()->CreateCommandList(true, RHI::ECommandListQueue::Graphics);
				Sailor::RHI::Renderer::GetDriver()->SetDebugName(secondaryCmdList, "Draw Debug Mesh");
				auto commands = App::GetSubmodule<Renderer>()->GetDriverCommands();
				commands->BeginSecondaryCommandList(secondaryCmdList, false, true);
				world->GetDebugContext()->DrawDebugMesh(
					secondaryCmdList,
					matrix,
					debugDrawSnapshot,
					renderExtent);
				commands->EndCommandList(secondaryCmdList);

				return secondaryCmdList;
			}, EThreadType::RHI);

		task->Run();

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
	for (auto& snapshot : m_snapshots)
	{
		snapshot.ResetForReuse();
	}
	m_submissionContext.Clear();
	m_submissionCompletionToken.Clear();
	m_sceneVersions.Clear(false);
	m_virtualSceneVersions.Clear(false);
	m_retainedSceneVersions.Clear();
	m_sceneRevision = 0ull;
	m_renderMode = ESceneViewRenderMode::Lit;
	m_shadowCastersRevision = 0ull;
	m_bHasCustomDepthShadowCasters = false;
	m_pathTracerProxies.Clear(false);
	m_pathTracerTLASInstances.Clear(false);
	m_pathTracerMaterials.Clear(false);
	m_pathTracerLights.Clear(false);
}

void RHISceneViewSnapshot::ResetForReuse()
{
	m_submissionContext.Clear();
	m_previousMotionFrame.Clear();
	m_sceneVersions.Clear();
	m_sceneRevision = 0ull;
	m_renderMode = ESceneViewRenderMode::Lit;
	m_deltaTime = 0.0f;
	m_frame = 0ull;
	m_cameraIndex = 0u;
	m_cameraTransform = {};
	m_proxies.Clear(false);
	m_lodMeshes.Clear(false);
	m_instancedLodOffsets.Clear(false);
	m_pathTracerProxies.Clear(false);
	m_pathTracerTLASInstances.Clear(false);
	m_pathTracerMaterials.Clear(false);
	m_pathTracerLights.Clear(false);
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
	if (!source || meshIndex >= source->m_meshes.Num())
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
	TMap<const RHISceneProxyResource*, bool> topologyHasLods;
	const glm::vec3 cameraPosition(m_cameraTransform.m_position);
	auto prepare = [&](const RHIVisibleSceneProxy& proxy)
		{
			LodOffsets offsets;
			const auto* source = proxy.GetSource();
			if (!source || !source->m_lodPolicy.m_bEnabled)
			{
				return offsets;
			}
			bool* hasLods = nullptr;
			if (!topologyHasLods.Find(proxy.m_resource, hasLods))
			{
				bool bHasLods = false;
				for (const auto& mesh : source->m_meshes)
				{
					bHasLods |= mesh && mesh->GetNumLods() > 1u;
				}
				if (source->m_shadowCaster)
				{
					for (const auto& mesh : source->m_shadowCaster->m_meshes)
					{
						bHasLods |= mesh.m_mesh && mesh.m_mesh->GetNumLods() > 1u;
					}
				}
				for (const auto& group : source->m_instancedGroups)
				{
					for (const auto& mesh : group.m_meshes)
					{
						bHasLods |= mesh && mesh->GetNumLods() > 1u;
					}
				}
				topologyHasLods.Insert(proxy.m_resource, bHasLods);
				if (!bHasLods)
				{
					return offsets;
				}
			}
			else if (!*hasLods)
			{
				return offsets;
			}
			// Record identity also distinguishes equal handles in different scene roots.
			const void* key = proxy.m_record ? static_cast<const void*>(proxy.m_record) : proxy.m_resource;
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
			RHIVisibleSceneProxy proxy;
			proxy.m_handle = caster.m_handle;
			proxy.m_record = caster.m_record;
			proxy.m_resource = caster.m_resource;
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

	HashCombine(m_sceneRevision, sceneVersion->m_revision);
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

TSharedPtr<TVector<RHISceneVersionPtr>> RHISceneView::GetRetainedSceneVersions()
{
	if (!m_retainedSceneVersions)
	{
		m_retainedSceneVersions = TSharedPtr<TVector<RHISceneVersionPtr>>::Make();
		*m_retainedSceneVersions = m_virtualSceneVersions;
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

TVector<RHIVisibleSceneProxy> RHISceneView::TraceScene(const Math::Frustum& frustum, bool bSkipMaterials) const
{
	TVector<RHIVisibleSceneProxy> result;
	TraceScene(frustum, result, bSkipMaterials);
	return result;
}

void RHISceneView::TraceScene(
	const Math::Frustum& frustum,
	TVector<RHIVisibleSceneProxy>& result,
	bool bSkipMaterials) const
{
	SAILOR_PROFILE_FUNCTION();
	(void)bSkipMaterials;

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

				RHIVisibleSceneProxy visible;
				visible.m_handle = handle;
				visible.m_record = record;
				visible.m_resource = resource;
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

				RHIVisibleShadowCaster visible;
				visible.m_handle = handle;
				visible.m_record = record;
				visible.m_resource = resource;
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
		res.m_sceneVersions = GetRetainedSceneVersions();
		res.m_sceneRevision = m_sceneRevision;
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
		res.m_pathTracerProxies = m_pathTracerProxies;
		res.m_pathTracerTLASInstances = m_pathTracerTLASInstances;
		res.m_pathTracerMaterials = m_pathTracerMaterials;
		res.m_pathTracerLights = m_pathTracerLights;

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
		TraceScene(frustum, res.m_proxies, false);
		const glm::vec3 cameraPosition = glm::vec3(m_cameraTransforms[i].m_position);
		size_t visibleProxyWriteIndex = 0u;
		for (size_t visibleProxyReadIndex = 0u;
			visibleProxyReadIndex < res.m_proxies.Num();
			++visibleProxyReadIndex)
		{
			auto& proxy = res.m_proxies[visibleProxyReadIndex];
			const auto* source = proxy.GetSource();
			bool bKeepProxy = source != nullptr;
			float cameraDistance = 0.0f;
			if (source)
			{
				const glm::vec3 closest = glm::clamp(
					cameraPosition,
					proxy.GetWorldBounds().m_min,
					proxy.GetWorldBounds().m_max);
				cameraDistance = glm::distance(cameraPosition, closest);
			}
			if (source && std::isfinite(source->m_lodPolicy.m_maxCameraDistance))
			{
				bKeepProxy = cameraDistance <=
					source->m_lodPolicy.m_maxCameraDistance;
			}
			if (!bKeepProxy)
			{
				continue;
			}
			if (visibleProxyWriteIndex != visibleProxyReadIndex)
			{
				res.m_proxies[visibleProxyWriteIndex] = std::move(proxy);
			}
			++visibleProxyWriteIndex;
		}
		res.m_proxies.Resize(visibleProxyWriteIndex);
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
				const auto* lhsSource = lhs.GetSource();
				const auto* rhsSource = rhs.GetSource();
				const size_t lhsProducer = lhsSource ? lhsSource->m_staticMeshEcs : 0u;
				const size_t rhsProducer = rhsSource ? rhsSource->m_staticMeshEcs : 0u;
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
