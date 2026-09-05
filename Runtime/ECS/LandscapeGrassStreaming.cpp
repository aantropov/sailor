#include "ECS/LandscapeECS.h"
#include "ECS/LandscapeECSInternal.h"
#include "ECS/TransformECS.h"

#include "Engine/GameObject.h"
#include "Settings/GraphicsSettings.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <utility>

using namespace Sailor;
using namespace Sailor::LandscapeECSInternal;
using namespace Sailor::Tasks;

bool LandscapeECS::UpdateGrassResidency(const TVector<Math::Transform>& cameraTransforms,
	const TVector<CameraData>& cameras)
{
	auto findResident = [](const LandscapeChunk& chunk, size_t profileIndex) -> const LandscapeVegetationRenderProxy*
	{
		for (const auto& proxy : chunk.m_vegetationProxies)
		{
			if (IsLandscapeGrassProxy(proxy) && proxy.m_profileIndex == profileIndex)
			{
				return &proxy;
			}
		}
		return nullptr;
	};
	auto toChunkCoordinate = [](double localPosition, double extent, double chunkSize)
	{
		const double coordinate = std::floor((localPosition + extent * 0.5) / chunkSize);
		return static_cast<int32_t>((std::clamp)(coordinate,
			static_cast<double>((std::numeric_limits<int32_t>::lowest)()),
			static_cast<double>((std::numeric_limits<int32_t>::max)())));
	};

	const size_t numCameras = (std::min)(cameraTransforms.Num(), cameras.Num());
	m_cameraPositionsScratch.Resize(numCameras);
	m_cameraFrustumsScratch.Resize(numCameras);
	for (size_t cameraIndex = 0u; cameraIndex < numCameras; ++cameraIndex)
	{
		const auto& camera = cameras[cameraIndex];
		const float aspect = std::isfinite(camera.GetAspect()) && camera.GetAspect() > 0.0f ? camera.GetAspect() : 1.0f;
		const float fov = std::isfinite(camera.GetFov()) ? (std::clamp)(camera.GetFov(), 1.0f, 179.0f) : 90.0f;
		const float zNear = std::isfinite(camera.GetZNear()) ? (std::max)(camera.GetZNear(), 0.001f) : 0.1f;
		const float zFar = std::isfinite(camera.GetZFar()) ? (std::max)(camera.GetZFar(), zNear + 0.001f) : 1000.0f;
		m_cameraPositionsScratch[cameraIndex] = glm::vec3(cameraTransforms[cameraIndex].m_position);
		m_cameraFrustumsScratch[cameraIndex].ExtractFrustumPlanes(
			cameraTransforms[cameraIndex].Matrix(), aspect, fov, zNear, zFar);
	}

	m_grassCandidatesScratch.Clear(false);
	for (size_t componentIndex = 0u; componentIndex < m_components.Num(); ++componentIndex)
	{
		if (!IsComponentRegistered(componentIndex))
		{
			continue;
		}
		auto& component = m_components[componentIndex];
		component.m_activeGrassInstances = 0u;
		GameObjectPtr owner = const_cast<ObjectPtr&>(component.GetOwner()).StaticCast<GameObject>();
		if (!owner || component.m_chunks.IsEmpty() || numCameras == 0u)
		{
			continue;
		}

		const glm::mat4 ownerMatrix = owner->GetTransformComponent().GetCachedWorldMatrix();
		const glm::mat4 inverseOwnerMatrix = glm::inverse(ownerMatrix);
		const double landscapeWidth = static_cast<double>(component.m_chunksX) * component.m_chunkSize;
		const double landscapeDepth = static_cast<double>(component.m_chunksZ) * component.m_chunkSize;
		m_cameraChunkCoordinatesScratch.Clear(false);
		for (size_t cameraIndex = 0u; cameraIndex < numCameras; ++cameraIndex)
		{
			const auto& cameraTransform = cameraTransforms[cameraIndex];
			const glm::vec4 localPosition = inverseOwnerMatrix * glm::vec4(glm::vec3(cameraTransform.m_position), 1.0f);
			if (!std::isfinite(localPosition.x) || !std::isfinite(localPosition.z))
			{
				continue;
			}
			m_cameraChunkCoordinatesScratch.Add(
				glm::ivec2(toChunkCoordinate(localPosition.x, landscapeWidth, component.m_chunkSize),
					toChunkCoordinate(localPosition.z, landscapeDepth, component.m_chunkSize)));
		}
		if (m_cameraChunkCoordinatesScratch.IsEmpty())
		{
			continue;
		}

		for (size_t chunkIndex = 0u; chunkIndex < component.m_chunks.Num(); ++chunkIndex)
		{
			const auto& chunk = component.m_chunks[chunkIndex];
			if (!chunk.m_resource)
			{
				continue;
			}
			bool bChunkResident = false;
			for (const auto& proxy : chunk.m_vegetationProxies)
			{
				bChunkResident |= IsLandscapeGrassProxy(proxy);
			}
			const Math::AABB& worldBounds = chunk.m_resource->m_proxy.m_worldAabb;
			bool bOverlapsCameraFrustum = false;
			for (const auto& frustum : m_cameraFrustumsScratch)
			{
				if (DoesLandscapeGrassChunkOverlapFrustum(
						worldBounds, frustum, bChunkResident ? component.m_grassResidencyHysteresis : 0.0f))
				{
					bOverlapsCameraFrustum = true;
					break;
				}
			}
			if (!bOverlapsCameraFrustum)
			{
				continue;
			}

			uint32_t chunkRing = (std::numeric_limits<uint32_t>::max)();
			uint32_t chunkManhattanDistance = (std::numeric_limits<uint32_t>::max)();
			for (const glm::ivec2& cameraChunk : m_cameraChunkCoordinatesScratch)
			{
				const uint64_t distanceX =
					static_cast<uint64_t>(std::abs(static_cast<int64_t>(cameraChunk.x) - chunk.m_chunkX));
				const uint64_t distanceZ =
					static_cast<uint64_t>(std::abs(static_cast<int64_t>(cameraChunk.y) - chunk.m_chunkZ));
				const uint32_t ring = static_cast<uint32_t>((std::min)((std::max)(distanceX, distanceZ),
					static_cast<uint64_t>((std::numeric_limits<uint32_t>::max)())));
				const uint32_t manhattanDistance = static_cast<uint32_t>(
					(std::min)(distanceX + distanceZ, static_cast<uint64_t>((std::numeric_limits<uint32_t>::max)())));
				if (ring < chunkRing || (ring == chunkRing && manhattanDistance < chunkManhattanDistance))
				{
					chunkRing = ring;
					chunkManhattanDistance = manhattanDistance;
				}
			}

			float minCameraDistance = (std::numeric_limits<float>::infinity)();
			for (size_t cameraIndex = 0u; cameraIndex < numCameras; ++cameraIndex)
			{
				const glm::vec3& cameraPosition = m_cameraPositionsScratch[cameraIndex];
				const glm::vec3 closest = glm::clamp(cameraPosition, worldBounds.m_min, worldBounds.m_max);
				minCameraDistance = (std::min)(minCameraDistance, glm::distance(cameraPosition, closest));
			}

			for (size_t profileIndex = 0u; profileIndex < component.m_vegetationProfiles.Num(); ++profileIndex)
			{
				const auto& profile = component.m_vegetationProfiles[profileIndex];
				if (profile.m_residency != ELandscapeVegetationResidency::Grass || !profile.m_modelFileId)
				{
					continue;
				}
				const uint32_t instanceCapacity = GetVegetationInstanceCapacity(component, chunk, profileIndex);
				if (instanceCapacity == 0u)
				{
					continue;
				}
				const bool bResident = findResident(chunk, profileIndex) != nullptr;
				const float residencyDistance =
					profile.m_cullDistance + (bResident ? component.m_grassResidencyHysteresis : 0.0f);
				if (minCameraDistance > residencyDistance)
				{
					continue;
				}

				LandscapeGrassCandidate candidate;
				candidate.m_componentIndex = componentIndex;
				candidate.m_chunkIndex = chunkIndex;
				candidate.m_profileIndex = profileIndex;
				candidate.m_capacity = instanceCapacity;
				candidate.m_chunkRing = chunkRing;
				candidate.m_chunkManhattanDistance = chunkManhattanDistance;
				candidate.m_priority = profile.m_priority;
				candidate.m_bChunkResident = bChunkResident;
				m_grassCandidatesScratch.Add(std::move(candidate));
			}
		}
	}

	const uint32_t instanceBudget = (std::min)(App::GetActiveGraphicsSettings().m_vegetationInstanceBudget, 1048576u);
	const size_t numCandidates = m_grassCandidatesScratch.Num();
	SelectLandscapeGrassResidency(m_grassCandidatesScratch, instanceBudget, m_grassSelectionsScratch);
	for (auto& selection : m_grassSelectionsScratch)
	{
		if (!IsComponentRegistered(selection.m_componentIndex))
		{
			continue;
		}
		auto& component = m_components[selection.m_componentIndex];
		if (selection.m_profileIndex >= component.m_vegetationProfiles.Num())
		{
			continue;
		}
		if (selection.m_chunkIndex >= component.m_chunks.Num())
		{
			continue;
		}
		const uint32_t instanceCapacity = GetVegetationInstanceCapacity(
			component, component.m_chunks[selection.m_chunkIndex], selection.m_profileIndex);
		if (selection.m_instanceCount >= instanceCapacity)
		{
			continue;
		}
		GameObjectPtr owner = const_cast<ObjectPtr&>(component.GetOwner()).StaticCast<GameObject>();
		if (!owner)
		{
			continue;
		}
		selection.m_viewRevision = CalculateGrassViewRevision(component,
			instanceCapacity,
			glm::inverse(owner->GetTransformComponent().GetCachedWorldMatrix()),
			m_cameraPositionsScratch);
	}
	m_grassSelectionsScratch.Sort(
		[](const LandscapeGrassSelection& lhs, const LandscapeGrassSelection& rhs)
		{
			if (lhs.m_componentIndex != rhs.m_componentIndex)
			{
				return lhs.m_componentIndex < rhs.m_componentIndex;
			}
			if (lhs.m_chunkIndex != rhs.m_chunkIndex)
			{
				return lhs.m_chunkIndex < rhs.m_chunkIndex;
			}
			return lhs.m_profileIndex < rhs.m_profileIndex;
		});
	auto findSelection =
		[this](size_t componentIndex, size_t chunkIndex, size_t profileIndex) -> const LandscapeGrassSelection*
	{
		size_t first = 0u;
		size_t last = m_grassSelectionsScratch.Num();
		while (first < last)
		{
			const size_t middle = first + (last - first) / 2u;
			const auto& selection = m_grassSelectionsScratch[middle];
			const bool bBefore =
				selection.m_componentIndex < componentIndex ||
				(selection.m_componentIndex == componentIndex &&
					(selection.m_chunkIndex < chunkIndex ||
						(selection.m_chunkIndex == chunkIndex && selection.m_profileIndex < profileIndex)));
			if (bBefore)
			{
				first = middle + 1u;
			}
			else
			{
				last = middle;
			}
		}
		if (first >= m_grassSelectionsScratch.Num())
		{
			return nullptr;
		}
		const auto& selection = m_grassSelectionsScratch[first];
		return selection.m_componentIndex == componentIndex && selection.m_chunkIndex == chunkIndex &&
					   selection.m_profileIndex == profileIndex
				   ? &selection
				   : nullptr;
	};

	m_grassBuildRequestsScratch.Clear(false);
	for (size_t componentIndex = 0u; componentIndex < m_components.Num(); ++componentIndex)
	{
		if (!IsComponentRegistered(componentIndex))
		{
			continue;
		}
		auto& component = m_components[componentIndex];
		GameObjectPtr owner = const_cast<ObjectPtr&>(component.GetOwner()).StaticCast<GameObject>();
		if (!owner)
		{
			continue;
		}
		const glm::mat4 ownerMatrix = owner->GetTransformComponent().GetCachedWorldMatrix();
		for (size_t chunkIndex = 0u; chunkIndex < component.m_chunks.Num(); ++chunkIndex)
		{
			auto& chunk = component.m_chunks[chunkIndex];
			for (size_t profileIndex = 0u; profileIndex < component.m_vegetationProfiles.Num(); ++profileIndex)
			{
				const auto& profile = component.m_vegetationProfiles[profileIndex];
				if (profile.m_residency != ELandscapeVegetationResidency::Grass)
				{
					continue;
				}
				const auto* resident = findResident(chunk, profileIndex);
				const uint32_t residentCount = resident ? resident->m_instanceCount : 0u;
				const auto* selection = findSelection(componentIndex, chunkIndex, profileIndex);
				const uint32_t selectedCount = selection ? selection->m_instanceCount : 0u;
				const uint32_t instanceCapacity = GetVegetationInstanceCapacity(component, chunk, profileIndex);
				const bool bPartialSelection = selection && selectedCount < instanceCapacity;
				if (selectedCount == 0u ||
					(residentCount == selectedCount &&
						(!bPartialSelection || resident->m_viewRevision == selection->m_viewRevision)))
				{
					continue;
				}

				const LandscapeData* componentData = &component;
				const LandscapeChunk* chunkData = &chunk;
				auto task = Tasks::CreateTask<LandscapeVegetationRenderInstances>(
					"LandscapeECS:Build Grass Transforms",
					[componentData, chunkData, profileIndex, selectedCount, ownerMatrix, this]()
					{
						return BuildGrassInstanceTransforms(*componentData,
							*chunkData,
							profileIndex,
							selectedCount,
							ownerMatrix,
							m_cameraPositionsScratch);
					},
					EThreadType::Worker);
				task->Run();
				GrassTransformBuildRequest request;
				request.m_componentIndex = componentIndex;
				request.m_chunkIndex = chunkIndex;
				request.m_profileIndex = profileIndex;
				request.m_instanceCount = selectedCount;
				request.m_viewRevision = selection->m_viewRevision;
				request.m_task = std::move(task);
				m_grassBuildRequestsScratch.Add(std::move(request));
			}
		}
	}

	bool bChanged = false;
	uint32_t activeInstances = 0u;
	const uint64_t frame = GetWorld()->GetCurrentFrame();
	auto findBuildRequest =
		[this](size_t componentIndex, size_t chunkIndex, size_t profileIndex) -> GrassTransformBuildRequest*
	{
		size_t first = 0u;
		size_t last = m_grassBuildRequestsScratch.Num();
		while (first < last)
		{
			const size_t middle = first + (last - first) / 2u;
			const auto& request = m_grassBuildRequestsScratch[middle];
			const bool bBefore = request.m_componentIndex < componentIndex ||
								 (request.m_componentIndex == componentIndex &&
									 (request.m_chunkIndex < chunkIndex || (request.m_chunkIndex == chunkIndex &&
																			   request.m_profileIndex < profileIndex)));
			if (bBefore)
			{
				first = middle + 1u;
			}
			else
			{
				last = middle;
			}
		}
		if (first >= m_grassBuildRequestsScratch.Num())
		{
			return nullptr;
		}
		auto& request = m_grassBuildRequestsScratch[first];
		return request.m_componentIndex == componentIndex && request.m_chunkIndex == chunkIndex &&
					   request.m_profileIndex == profileIndex
				   ? &request
				   : nullptr;
	};
	for (size_t componentIndex = 0u; componentIndex < m_components.Num(); ++componentIndex)
	{
		if (!IsComponentRegistered(componentIndex))
		{
			continue;
		}
		auto& component = m_components[componentIndex];
		uint32_t componentActiveInstances = 0u;
		GameObjectPtr owner = const_cast<ObjectPtr&>(component.GetOwner()).StaticCast<GameObject>();
		const glm::mat4 ownerMatrix = owner ? owner->GetTransformComponent().GetCachedWorldMatrix() : glm::mat4(1.0f);
		for (size_t chunkIndex = 0u; chunkIndex < component.m_chunks.Num(); ++chunkIndex)
		{
			auto& chunk = component.m_chunks[chunkIndex];
			bool bNeedsUpdate = false;
			for (size_t profileIndex = 0u; profileIndex < component.m_vegetationProfiles.Num(); ++profileIndex)
			{
				const auto& profile = component.m_vegetationProfiles[profileIndex];
				if (profile.m_residency != ELandscapeVegetationResidency::Grass)
				{
					continue;
				}
				const auto* resident = findResident(chunk, profileIndex);
				const uint32_t residentCount = resident ? resident->m_instanceCount : 0u;
				const auto* selection = findSelection(componentIndex, chunkIndex, profileIndex);
				const uint32_t selectedCount = selection ? selection->m_instanceCount : 0u;
				const uint32_t instanceCapacity = GetVegetationInstanceCapacity(component, chunk, profileIndex);
				const bool bPartialSelection = selection && selectedCount < instanceCapacity;
				bNeedsUpdate |=
					residentCount != selectedCount ||
					(resident && bPartialSelection && resident->m_viewRevision != selection->m_viewRevision);
				componentActiveInstances += residentCount;
			}
			if (!bNeedsUpdate)
			{
				continue;
			}

			TVector<LandscapeVegetationRenderProxy> nextProxies;
			nextProxies.Reserve(chunk.m_vegetationProxies.Num());
			for (const auto& proxy : chunk.m_vegetationProxies)
			{
				if (!IsLandscapeGrassProxy(proxy))
				{
					nextProxies.Add(proxy);
				}
			}
			bool bChunkChanged = false;
			for (size_t profileIndex = 0u; profileIndex < component.m_vegetationProfiles.Num(); ++profileIndex)
			{
				const auto& profile = component.m_vegetationProfiles[profileIndex];
				if (profile.m_residency != ELandscapeVegetationResidency::Grass)
				{
					continue;
				}

				const auto* resident = findResident(chunk, profileIndex);
				const auto* selection = findSelection(componentIndex, chunkIndex, profileIndex);
				const uint32_t selectedCount = selection ? selection->m_instanceCount : 0u;
				if (selectedCount == 0u)
				{
					bChunkChanged |= resident != nullptr;
					if (resident)
					{
						componentActiveInstances -= resident->m_instanceCount;
					}
					continue;
				}
				const bool bPartialSelection =
					selectedCount < GetVegetationInstanceCapacity(component, chunk, profileIndex);
				if (resident && resident->m_instanceCount == selectedCount &&
					(!bPartialSelection || resident->m_viewRevision == selection->m_viewRevision))
				{
					nextProxies.Add(*resident);
					continue;
				}
				if (!owner)
				{
					continue;
				}

				auto* buildRequest = findBuildRequest(componentIndex, chunkIndex, profileIndex);
				if (!buildRequest || buildRequest->m_instanceCount != selectedCount ||
					buildRequest->m_viewRevision != selection->m_viewRevision)
				{
					if (resident)
					{
						nextProxies.Add(*resident);
					}
					continue;
				}
				buildRequest->m_task->Wait();
				auto instances = std::move(buildRequest->m_task->m_result);
				LandscapeVegetationRenderProxy streamedProxy;
				const uint64_t revision = (uint64_t(1u) << 63u) | ++component.m_streamingRevision;
				const auto buildResult = BuildLandscapeVegetationProxy(componentIndex,
					chunkIndex,
					profileIndex,
					profile,
					ownerMatrix,
					frame,
					std::move(instances),
					ResolveLandscapeProxyMobility(owner->GetMobilityType(), profile.m_residency),
					revision,
					streamedProxy);
				if (buildResult != EVegetationProxyBuildResult::Success)
				{
					if (resident && resident->m_instanceCount <= selectedCount)
					{
						nextProxies.Add(*resident);
					}
					else if (resident)
					{
						componentActiveInstances -= resident->m_instanceCount;
						bChunkChanged = true;
					}
					continue;
				}
				streamedProxy.m_viewRevision = selection->m_viewRevision;
				if (resident)
				{
					componentActiveInstances -= resident->m_instanceCount;
				}
				nextProxies.Add(std::move(streamedProxy));
				componentActiveInstances += selectedCount;
				bChunkChanged = true;
			}

			if (bChunkChanged)
			{
				chunk.m_vegetationProxies = std::move(nextProxies);
				bChanged = true;
			}
		}
		component.m_activeGrassInstances = componentActiveInstances;
		activeInstances += componentActiveInstances;
	}
	for (auto& buildRequest : m_grassBuildRequestsScratch)
	{
		buildRequest.m_task->Wait();
	}
	if (bChanged)
	{
		++m_shadowCastersRevision;
		SAILOR_LOG("LandscapeECS: grass residency changed to %u of %u graphics-quality instances across %zu visible "
				   "candidates.",
			activeInstances,
			instanceBudget,
			numCandidates);
	}
	return bChanged;
}
