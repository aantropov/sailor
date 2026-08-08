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

		TVector<glm::mat4> modelMatrices;
		Math::AABB modelBounds;
		if (!data.GetModel()->CollectRenderData(
				data.GetMeshIndex(),
				outMeshes,
				modelMatrices,
				modelBounds))
		{
			return false;
		}

		outWorldMatrices.Clear();
		outWorldMatrices.Reserve(modelMatrices.Num());
		for (const glm::mat4& modelMatrix : modelMatrices)
		{
			outWorldMatrices.Add(ownerWorldMatrix * modelMatrix);
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
				lhsMesh.m_customDepthMaterial != rhsMesh.m_customDepthMaterial ||
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
			const auto& material = data.GetMaterials()[materialIndex];
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
				shadowMesh.m_customDepthMaterial = material;
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

	enum class EPreparedProxyState : uint8_t
	{
		Remove,
		Retry,
		Stationary,
		Static
	};

	struct PreparedProxyUpdate
	{
		size_t m_componentIndex = ECS::InvalidIndex;
		EPreparedProxyState m_state = EPreparedProxyState::Remove;
		uint32_t m_skeletonOffset = StaticMeshRendererData::InvalidSkeletonOffset;
		RHI::RHIMeshProxy m_stationaryProxy{};
		RHI::RHISceneViewProxy m_staticProxy{};
		RHI::RHIShadowCasterProxyPtr m_shadowCaster{};
		Math::AABB m_worldBounds{};
	};
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
	m_sceneViewProxiesCache = RHI::RHISceneViewPtr::Make();
	m_lastMaterialContentRevision = Material::GetGlobalContentRevision();
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
	if (!m_sceneViewProxiesCache)
	{
		return;
	}

	RHI::RHIMeshProxy stationaryProxy{};
	stationaryProxy.m_staticMeshEcs = index;
	m_sceneViewProxiesCache->m_stationaryOctree.Remove(stationaryProxy);

	RHI::RHISceneViewProxy staticProxy{};
	staticProxy.m_staticMeshEcs = index;
	m_sceneViewProxiesCache->m_staticOctree.Remove(staticProxy);

	if (component.m_shadowCaster)
	{
		component.m_shadowCaster.Clear();
		++m_sceneViewProxiesCache->m_shadowCastersRevision;
	}
}

Tasks::ITaskPtr StaticMeshRendererECS::Tick(float deltaTime)
{
	constexpr uint32_t NumDirtyComponentsPerTask = 512;

	SAILOR_PROFILE_FUNCTION();

	if (!m_sceneViewProxiesCache)
	{
		return nullptr;
	}

	const uint64_t materialContentRevision = Material::GetGlobalContentRevision();
	const bool bCheckMaterialRevisions = materialContentRevision != m_lastMaterialContentRevision;
	bool bHasCustomDepthShadowCasters = false;
	TVector<size_t> dirtyComponents;
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
		bool bNeedsUpdate = data.m_bIsDirty || (data.GetModel() && data.m_frameLastChange == 0);
		if (GameObjectPtr owner = data.m_owner.StaticCast<GameObject>())
		{
			bNeedsUpdate |= owner->GetTransformComponent().GetFrameLastChange() > data.m_frameLastChange;
		}

		if (!bNeedsUpdate && bCheckMaterialRevisions)
		{
			bool bMaterialsChanged = data.m_materialContentRevisions.Num() != data.GetMaterials().Num();
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
	m_lastMaterialContentRevision = materialContentRevision;

	if (dirtyComponents.IsEmpty())
	{
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

			const ModelPtr& model = data.GetModel();
			if (!model->IsReady())
			{
				result.m_state = EPreparedProxyState::Retry;
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
				result.m_state = EPreparedProxyState::Retry;
				return result;
			}
			for (const auto& material : data.GetMaterials())
			{
				if (!material)
				{
					result.m_state = EPreparedProxyState::Retry;
					return result;
				}
				if (!material->IsReady())
				{
					result.m_state = EPreparedProxyState::Retry;
					return result;
				}
			}

			const auto& ownerTransform = owner->GetTransformComponent();
			const glm::mat4& ownerWorldMatrix = ownerTransform.GetCachedWorldMatrix();
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

			if (auto animator = owner->GetComponent<AnimatorComponent>())
			{
				result.m_skeletonOffset = animator->GetSkeletonOffset();
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

			if (owner->GetMobilityType() == EMobilityType::Stationary)
			{
				result.m_state = EPreparedProxyState::Stationary;
				result.m_stationaryProxy.m_staticMeshEcs = componentIndex;
				result.m_stationaryProxy.m_worldMatrix = ownerWorldMatrix;
				return result;
			}

			if (owner->GetMobilityType() != EMobilityType::Static)
			{
				return result;
			}

			result.m_state = EPreparedProxyState::Static;
			auto& proxy = result.m_staticProxy;
			proxy.m_staticMeshEcs = componentIndex;
			proxy.m_worldMatrix = ownerWorldMatrix;
			proxy.m_frame = currentFrame;
			proxy.m_skeletonOffset = result.m_skeletonOffset;
			proxy.m_meshes = std::move(selectedMeshes);
			proxy.m_meshModelMatrices = std::move(selectedMatrices);
			proxy.m_worldAabb = result.m_worldBounds;
			proxy.m_bCastShadows = data.ShouldCastShadow();
			proxy.m_overrideMaterials.Reserve(proxy.m_meshes.Num());
			proxy.m_renderQueueTags.Reserve(proxy.m_meshes.Num());
#if defined(__APPLE__)
			proxy.m_materialTextureSamplers.Reserve(proxy.m_meshes.Num());
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

	TVector<PreparedProxyUpdate> preparedUpdates;
	preparedUpdates.Reserve(dirtyComponents.Num());
	if (dirtyComponents.Num() <= NumDirtyComponentsPerTask)
	{
		for (size_t componentIndex : dirtyComponents)
		{
			preparedUpdates.Add(prepareProxyUpdate(componentIndex));
		}
	}
	else
	{
		TVector<Tasks::TaskPtr<TVector<PreparedProxyUpdate>>> tasks;
		const size_t numTasks = (dirtyComponents.Num() + NumDirtyComponentsPerTask - 1) / NumDirtyComponentsPerTask;
		tasks.Reserve(numTasks);
		for (size_t taskIndex = 0; taskIndex < numTasks; ++taskIndex)
		{
			const size_t beginIndex = taskIndex * NumDirtyComponentsPerTask;
			const size_t endIndex = (std::min)(beginIndex + NumDirtyComponentsPerTask, dirtyComponents.Num());
			auto task = Tasks::CreateTask<TVector<PreparedProxyUpdate>>(
				"StaticMeshRendererECS:Prepare Dirty Proxies",
				[beginIndex, endIndex, &dirtyComponents, prepareProxyUpdate]()
				{
					TVector<PreparedProxyUpdate> result;
					result.Reserve(endIndex - beginIndex);
					for (size_t index = beginIndex; index < endIndex; ++index)
					{
						result.Add(prepareProxyUpdate(dirtyComponents[index]));
					}
					return result;
				},
				EThreadType::Worker);
			task->Run();
			tasks.Add(task);
		}

		for (auto& task : tasks)
		{
			task->Wait();
			preparedUpdates.AddRange(std::move(task->m_result));
		}
	}

	bool bShadowCastersChanged = false;
	for (auto& update : preparedUpdates)
	{
		if (!IsComponentRegistered(update.m_componentIndex))
		{
			continue;
		}

		auto& data = m_components[update.m_componentIndex];
		RHI::RHIMeshProxy stationaryKey{};
		stationaryKey.m_staticMeshEcs = update.m_componentIndex;
		RHI::RHISceneViewProxy staticKey{};
		staticKey.m_staticMeshEcs = update.m_componentIndex;

		if (update.m_state == EPreparedProxyState::Retry)
		{
			m_sceneViewProxiesCache->m_stationaryOctree.Remove(stationaryKey);
			m_sceneViewProxiesCache->m_staticOctree.Remove(staticKey);
			if (data.m_shadowCaster)
			{
				data.m_shadowCaster.Clear();
				bShadowCastersChanged = true;
			}
			data.m_bIsDirty = true;
			continue;
		}

		if (update.m_state == EPreparedProxyState::Remove)
		{
			m_sceneViewProxiesCache->m_stationaryOctree.Remove(stationaryKey);
			m_sceneViewProxiesCache->m_staticOctree.Remove(staticKey);
			if (data.m_shadowCaster)
			{
				data.m_shadowCaster.Clear();
				bShadowCastersChanged = true;
			}
			data.m_materialContentRevisions.Clear();
			data.m_bIsDirty = false;
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

		if (update.m_state == EPreparedProxyState::Stationary)
		{
			m_sceneViewProxiesCache->m_staticOctree.Remove(staticKey);
			update.m_stationaryProxy.m_shadowCaster = data.m_shadowCaster;
			m_sceneViewProxiesCache->m_stationaryOctree.Update(
				glm::ivec3(update.m_worldBounds.GetCenter()),
				glm::ivec3(update.m_worldBounds.GetExtents()),
				update.m_stationaryProxy);
		}
		else
		{
			m_sceneViewProxiesCache->m_stationaryOctree.Remove(stationaryKey);
			update.m_staticProxy.m_shadowCaster = data.m_shadowCaster;
			m_sceneViewProxiesCache->m_staticOctree.Update(
				glm::ivec3(update.m_worldBounds.GetCenter()),
				glm::ivec3(update.m_worldBounds.GetExtents()),
				update.m_staticProxy);
		}

		data.m_skeletonOffset = update.m_skeletonOffset;
		data.m_materialContentRevisions.Resize(data.GetMaterials().Num());
		for (size_t materialIndex = 0; materialIndex < data.GetMaterials().Num(); ++materialIndex)
		{
			const auto& material = data.GetMaterials()[materialIndex];
			data.m_materialContentRevisions[materialIndex] = material ? material->GetContentRevision() : 0ull;
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

	if (bShadowCastersChanged)
	{
		++m_sceneViewProxiesCache->m_shadowCastersRevision;
	}
	m_sceneViewProxiesCache->m_bHasCustomDepthShadowCasters = bHasCustomDepthShadowCasters;

	return nullptr;
}

void StaticMeshRendererECS::CopySceneView(RHI::RHISceneViewPtr& outProxies)
{
	SAILOR_PROFILE_FUNCTION();

	outProxies->m_stationaryOctree = m_sceneViewProxiesCache->m_stationaryOctree;
	outProxies->m_staticOctree = m_sceneViewProxiesCache->m_staticOctree;
	outProxies->m_shadowCastersRevision = m_sceneViewProxiesCache->m_shadowCastersRevision;
	outProxies->m_bHasCustomDepthShadowCasters = m_sceneViewProxiesCache->m_bHasCustomDepthShadowCasters;
}

void StaticMeshRendererECS::EndPlay()
{
	ECS::TSystem<StaticMeshRendererECS, StaticMeshRendererData>::EndPlay();
	m_sceneViewProxiesCache.Clear();
}
