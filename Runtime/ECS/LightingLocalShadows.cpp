#include "ECS/LightingECS.h"
#include "ECS/LightingECSInternal.h"
#include "ECS/TransformECS.h"
#include "Engine/GameObject.h"
#include "RHI/RenderTarget.h"
#include "RHI/SceneView.h"
#include "RHI/Texture.h"
#include "Settings/GraphicsSettings.h"

#include <algorithm>
#include <array>
#include <utility>

using namespace Sailor;
using namespace Sailor::LightingECSInternal;
using namespace Sailor::Tasks;

void LightingECS::ReleaseLocalShadowAllocation(uint32_t componentIndex)
{
	if (componentIndex >= m_localShadowAllocations.Num())
	{
		return;
	}

	auto& allocation = m_localShadowAllocations[componentIndex];
	if (allocation.m_componentIndex != componentIndex)
	{
		return;
	}

	ReleaseLocalShadowTiles(allocation.m_atlasIndex, allocation.m_tiles);
	for (uint32_t slot : allocation.m_slots)
	{
		if (slot >= m_shadowMapOwners.Num())
		{
			continue;
		}

		m_shadowMapOwners[slot] = InvalidShadowMapIndex;
	}
	for (auto& flightResources : m_shadowFlightResources)
	{
		if (flightResources.m_localShadowSnapshots.Num() > componentIndex)
		{
			flightResources.m_localShadowSnapshots[componentIndex].Clear();
		}
	}

	allocation = {};
}

void LightingECS::ReleaseLocalShadowTiles(uint32_t atlasIndex, const TVector<glm::ivec4>& tiles)
{
	if (atlasIndex >= m_localShadowAtlases.Num() || !m_localShadowAtlases[atlasIndex].m_texture)
	{
		return;
	}

	auto& occupancy = m_localShadowAtlases[atlasIndex].m_occupancy;
	for (const auto& tile : tiles)
	{
		const uint32_t firstX = static_cast<uint32_t>(tile.x) / LocalShadowMinResolution;
		const uint32_t firstY = static_cast<uint32_t>(tile.y) / LocalShadowMinResolution;
		const uint32_t cellCount = static_cast<uint32_t>(tile.z) / LocalShadowMinResolution;
		for (uint32_t y = firstY; y < firstY + cellCount; ++y)
		{
			for (uint32_t x = firstX; x < firstX + cellCount; ++x)
			{
				occupancy[y * LocalShadowAtlasCellsPerAxis + x] = 0;
			}
		}
	}
}

bool LightingECS::TryCreateLocalShadowAtlas(uint32_t& outAtlasIndex)
{
	if (m_shadowMapsMb + LocalShadowAtlasMemoryMb > m_shadowsMemoryBudgetMb + 0.001f)
	{
		return false;
	}

	uint32_t atlasIndex = static_cast<uint32_t>(m_localShadowAtlases.Num());
	for (uint32_t i = 0; i < m_localShadowAtlases.Num(); ++i)
	{
		if (!m_localShadowAtlases[i].m_texture)
		{
			atlasIndex = i;
			break;
		}
	}
	if (NumCascades + atlasIndex >= MaxShadowMapSamplers)
	{
		return false;
	}

	const auto usage = RHI::ETextureUsageBit::ColorAttachment_Bit | RHI::ETextureUsageBit::TextureTransferSrc_Bit |
					   RHI::ETextureUsageBit::TextureTransferDst_Bit | RHI::ETextureUsageBit::Sampled_Bit;
	auto texture = RHI::Renderer::GetDriver()->CreateRenderTarget(glm::ivec2(LocalShadowAtlasResolution),
		1,
		ShadowMapFormat,
		RHI::ETextureFiltration::Nearest,
		RHI::ETextureClamping::Clamp,
		usage);
	if (!texture)
	{
		return false;
	}

	char debugName[64];
	sprintf_s(debugName, sizeof(debugName), "Shadow Map, Local Light Atlas %u", atlasIndex);
	RHI::Renderer::GetDriver()->SetDebugName(texture, debugName);

	LocalShadowAtlas atlas{};
	atlas.m_texture = texture;
	atlas.m_occupancy.Resize(static_cast<size_t>(LocalShadowAtlasCellsPerAxis) * LocalShadowAtlasCellsPerAxis);
	for (auto& occupied : atlas.m_occupancy)
	{
		occupied = 0;
	}
	if (atlasIndex == m_localShadowAtlases.Num())
	{
		m_localShadowAtlases.Add(std::move(atlas));
	}
	else
	{
		m_localShadowAtlases[atlasIndex] = std::move(atlas);
	}

	m_shadowMapTextures[NumCascades + atlasIndex] = texture;
	m_writableLocalShadowAtlases.set(atlasIndex);
	m_bShadowMapBindingsDirty = true;
	m_shadowMapsMb += LocalShadowAtlasMemoryMb;
	outAtlasIndex = atlasIndex;
	return true;
}

bool LightingECS::EnsureWritableLocalShadowAtlas(uint32_t atlasIndex,
	uint32_t flightSlot,
	LightingShadowFlightResources& flightResources)
{
	if (atlasIndex >= m_localShadowAtlases.Num() || atlasIndex >= m_writableLocalShadowAtlases.size() ||
		!m_localShadowAtlases[atlasIndex].m_texture)
	{
		return false;
	}
	if (flightResources.m_localShadowAtlasTextures.Num() <= atlasIndex)
	{
		flightResources.m_localShadowAtlasTextures.Resize(static_cast<size_t>(atlasIndex) + 1u);
	}
	auto& writableTexture = flightResources.m_localShadowAtlasTextures[atlasIndex];
	if (m_writableLocalShadowAtlases.test(atlasIndex))
	{
		if (!writableTexture)
		{
			writableTexture = m_localShadowAtlases[atlasIndex].m_texture;
		}
		return writableTexture.IsValid();
	}

	if (!writableTexture)
	{
		if (m_shadowMapsMb + LocalShadowAtlasMemoryMb > m_shadowsMemoryBudgetMb + 0.001f)
		{
			return false;
		}
		const auto usage = RHI::ETextureUsageBit::ColorAttachment_Bit | RHI::ETextureUsageBit::TextureTransferSrc_Bit |
						   RHI::ETextureUsageBit::TextureTransferDst_Bit | RHI::ETextureUsageBit::Sampled_Bit;
		writableTexture = RHI::Renderer::GetDriver()->CreateRenderTarget(glm::ivec2(LocalShadowAtlasResolution),
			1,
			ShadowMapFormat,
			RHI::ETextureFiltration::Nearest,
			RHI::ETextureClamping::Clamp,
			usage);
		if (writableTexture)
		{
			m_shadowMapsMb += LocalShadowAtlasMemoryMb;
		}
	}
	if (!writableTexture)
	{
		return false;
	}

	char debugName[64];
	sprintf_s(debugName, sizeof(debugName), "Shadow Map, Local Atlas %u, Flight %u", atlasIndex, flightSlot);
	RHI::Renderer::GetDriver()->SetDebugName(writableTexture, debugName);
	m_localShadowAtlases[atlasIndex].m_texture = writableTexture;
	m_shadowMapTextures[NumCascades + atlasIndex] = writableTexture;
	m_writableLocalShadowAtlases.set(atlasIndex);
	m_bShadowMapBindingsDirty = true;
	return true;
}

bool LightingECS::TryAllocateLocalShadowTilesInAtlas(uint32_t atlasIndex,
	uint32_t count,
	uint32_t resolution,
	TVector<glm::ivec4>& outTiles)
{
	outTiles.Clear(false);
	if (atlasIndex >= m_localShadowAtlases.Num() || !m_localShadowAtlases[atlasIndex].m_texture)
	{
		return false;
	}

	auto& occupancy = m_localShadowAtlases[atlasIndex].m_occupancy;
	const uint32_t cellsPerTile = resolution / LocalShadowMinResolution;
	for (uint32_t tileIndex = 0; tileIndex < count; ++tileIndex)
	{
		bool bAllocated = false;
		for (uint32_t y = 0; y + cellsPerTile <= LocalShadowAtlasCellsPerAxis && !bAllocated; y += cellsPerTile)
		{
			for (uint32_t x = 0; x + cellsPerTile <= LocalShadowAtlasCellsPerAxis; x += cellsPerTile)
			{
				bool bFree = true;
				for (uint32_t cellY = y; cellY < y + cellsPerTile && bFree; ++cellY)
				{
					for (uint32_t cellX = x; cellX < x + cellsPerTile; ++cellX)
					{
						if (occupancy[cellY * LocalShadowAtlasCellsPerAxis + cellX] != 0)
						{
							bFree = false;
							break;
						}
					}
				}

				if (!bFree)
				{
					continue;
				}

				for (uint32_t cellY = y; cellY < y + cellsPerTile; ++cellY)
				{
					for (uint32_t cellX = x; cellX < x + cellsPerTile; ++cellX)
					{
						occupancy[cellY * LocalShadowAtlasCellsPerAxis + cellX] = 1;
					}
				}
				outTiles.Add(glm::ivec4(static_cast<int32_t>(x * LocalShadowMinResolution),
					static_cast<int32_t>(y * LocalShadowMinResolution),
					static_cast<int32_t>(resolution),
					static_cast<int32_t>(resolution)));
				bAllocated = true;
				break;
			}
		}

		if (!bAllocated)
		{
			ReleaseLocalShadowTiles(atlasIndex, outTiles);
			outTiles.Clear(false);
			return false;
		}
	}
	return true;
}

bool LightingECS::TryAllocateLocalShadowTiles(uint32_t count,
	uint32_t desiredResolution,
	uint32_t& outAtlasIndex,
	TVector<glm::ivec4>& outTiles)
{
	uint32_t resolution = glm::clamp(desiredResolution, LocalShadowMinResolution, LocalShadowAtlasResolution);
	while (resolution >= LocalShadowMinResolution)
	{
		for (uint32_t atlasIndex = 0; atlasIndex < m_localShadowAtlases.Num(); ++atlasIndex)
		{
			if (TryAllocateLocalShadowTilesInAtlas(atlasIndex, count, resolution, outTiles))
			{
				outAtlasIndex = atlasIndex;
				return true;
			}
		}

		uint32_t atlasIndex = InvalidShadowMapIndex;
		if (TryCreateLocalShadowAtlas(atlasIndex) &&
			TryAllocateLocalShadowTilesInAtlas(atlasIndex, count, resolution, outTiles))
		{
			outAtlasIndex = atlasIndex;
			return true;
		}
		resolution /= 2;
	}

	return false;
}

uint32_t LightingECS::CalculateLocalShadowResolution(const LightData& light,
	float distanceToCamera,
	const CameraData& cameraData,
	uint32_t viewportHeight) const
{
	const ELightShadowQuality effectiveQuality =
		Settings::ApplyShadowQualityCap(light.m_shadowQuality, App::GetActiveGraphicsSettings().m_shadowQuality);
	const uint32_t qualityLimit = GetLocalShadowResolution(effectiveQuality);
	const float safeDistance = (std::max)(distanceToCamera, 0.01f);
	const float focalLengthPixels = static_cast<float>((std::max)(viewportHeight, 1u)) /
									(2.0f * glm::tan(glm::radians(cameraData.GetFov()) * 0.5f));
	const float projectedDiameter = 2.0f * light.m_radius * focalLengthPixels / safeDistance;
	const uint32_t requestedPixels = static_cast<uint32_t>((std::max)(projectedDiameter, 1.0f));

	uint32_t resolution = LocalShadowMinResolution;
	while (resolution < requestedPixels && resolution < qualityLimit)
	{
		resolution *= 2;
	}
	return (std::min)(resolution, qualityLimit);
}

bool LightingECS::EnsureLocalShadowAllocation(uint32_t componentIndex,
	ELightType lightType,
	uint32_t desiredResolution,
	uint64_t frame)
{
	if (m_localShadowAllocations.Num() <= componentIndex)
	{
		m_localShadowAllocations.Resize(static_cast<size_t>(componentIndex) + 1);
	}

	auto& current = m_localShadowAllocations[componentIndex];
	if (current.m_componentIndex == componentIndex && current.m_lightType == lightType && !current.m_slots.IsEmpty())
	{
		if (desiredResolution < current.m_resolution)
		{
			uint32_t destinationAtlasIndex = InvalidShadowMapIndex;
			TVector<glm::ivec4> destinationTiles;
			if (TryAllocateLocalShadowTiles(static_cast<uint32_t>(current.m_tiles.Num()),
					desiredResolution,
					destinationAtlasIndex,
					destinationTiles))
			{
				ReleaseLocalShadowTiles(current.m_atlasIndex, current.m_tiles);
				current.m_atlasIndex = destinationAtlasIndex;
				current.m_tiles = std::move(destinationTiles);
				current.m_resolution = static_cast<uint32_t>(current.m_tiles[0].z);
				current.m_revision = ++m_localShadowAllocationRevision;
				if (current.m_revision == 0ull)
				{
					current.m_revision = ++m_localShadowAllocationRevision;
				}
			}
		}

		if (desiredResolution <= current.m_resolution)
		{
			current.m_requestedResolution = desiredResolution;
			current.m_lastUsedFrame = frame;
			return true;
		}

		// Increasing resolution requires a new shadow render. Keep the old
		// allocation alive until a replacement has been found below.
	}

	const uint32_t mapCount = GetLocalShadowMapCount(lightType);
	if (mapCount == 0)
	{
		return false;
	}

	uint32_t firstSlot = InvalidShadowMapIndex;
	if (current.m_componentIndex == componentIndex && current.m_lightType == lightType &&
		current.m_slots.Num() == mapCount)
	{
		firstSlot = current.m_slots[0];
	}
	else
	{
		auto findFreeSlots = [&]()
		{
			for (uint32_t candidate = NumCascades; candidate + mapCount <= MaxShadowsInView; ++candidate)
			{
				bool bRangeAvailable = true;
				for (uint32_t offset = 0; offset < mapCount; ++offset)
				{
					if (m_shadowMapOwners[candidate + offset] != InvalidShadowMapIndex)
					{
						bRangeAvailable = false;
						break;
					}
				}

				if (bRangeAvailable)
				{
					return candidate;
				}
			}
			return InvalidShadowMapIndex;
		};

		firstSlot = findFreeSlots();
		while (firstSlot == InvalidShadowMapIndex && EvictLeastRecentlyUsedLocalShadowAllocation(componentIndex, frame))
		{
			firstSlot = findFreeSlots();
		}
	}

	if (firstSlot == InvalidShadowMapIndex)
	{
		if (current.m_componentIndex == componentIndex)
		{
			current.m_lastUsedFrame = frame;
			return true;
		}
		return false;
	}

	uint32_t atlasIndex = InvalidShadowMapIndex;
	TVector<glm::ivec4> tiles;
	bool bAllocatedTiles = TryAllocateLocalShadowTiles(mapCount, desiredResolution, atlasIndex, tiles);
	while (!bAllocatedTiles && EvictLeastRecentlyUsedLocalShadowAllocation(componentIndex, frame))
	{
		bAllocatedTiles = TryAllocateLocalShadowTiles(mapCount, desiredResolution, atlasIndex, tiles);
	}
	if (!bAllocatedTiles)
	{
		if (current.m_componentIndex == componentIndex)
		{
			current.m_lastUsedFrame = frame;
			return true;
		}
		return false;
	}
	if (current.m_componentIndex == componentIndex && current.m_lightType == lightType &&
		static_cast<uint32_t>(tiles[0].z) <= current.m_resolution)
	{
		ReleaseLocalShadowTiles(atlasIndex, tiles);
		current.m_requestedResolution = desiredResolution;
		current.m_lastUsedFrame = frame;
		return true;
	}

	LocalLightShadowAllocation allocation{};
	allocation.m_componentIndex = componentIndex;
	allocation.m_lightType = lightType;
	allocation.m_resolution = static_cast<uint32_t>(tiles[0].z);
	allocation.m_requestedResolution = desiredResolution;
	allocation.m_atlasIndex = atlasIndex;
	allocation.m_lastUsedFrame = frame;
	allocation.m_revision = ++m_localShadowAllocationRevision;
	if (allocation.m_revision == 0ull)
	{
		allocation.m_revision = ++m_localShadowAllocationRevision;
	}
	allocation.m_slots.Reserve(mapCount);
	allocation.m_tiles = std::move(tiles);

	if (current.m_componentIndex == componentIndex && current.m_lightType == lightType &&
		current.m_slots.Num() == mapCount)
	{
		allocation.m_slots = current.m_slots;
		ReleaseLocalShadowTiles(current.m_atlasIndex, current.m_tiles);
	}
	else
	{
		ReleaseLocalShadowAllocation(componentIndex);
		for (uint32_t face = 0; face < mapCount; ++face)
		{
			const uint32_t slot = firstSlot + face;
			m_shadowMapOwners[slot] = componentIndex;
			allocation.m_slots.Add(slot);
		}
	}

	m_localShadowAllocations[componentIndex] = std::move(allocation);
	return true;
}

bool LightingECS::EvictLeastRecentlyUsedLocalShadowAllocation(uint32_t protectedComponentIndex, uint64_t frame)
{
	uint32_t oldestComponentIndex = InvalidShadowMapIndex;
	uint64_t oldestFrame = frame;
	for (uint32_t componentIndex = 0; componentIndex < m_localShadowAllocations.Num(); ++componentIndex)
	{
		const auto& allocation = m_localShadowAllocations[componentIndex];
		if (componentIndex == protectedComponentIndex || allocation.m_componentIndex != componentIndex ||
			allocation.m_lastUsedFrame >= frame || allocation.m_lastUsedFrame > oldestFrame)
		{
			continue;
		}

		oldestFrame = allocation.m_lastUsedFrame;
		oldestComponentIndex = componentIndex;
	}

	if (oldestComponentIndex == InvalidShadowMapIndex)
	{
		return false;
	}

	ReleaseLocalShadowAllocation(oldestComponentIndex);
	return true;
}

void LightingECS::ReleaseUnusedLocalShadowAllocations(uint64_t frame)
{
	for (uint32_t componentIndex = 0; componentIndex < m_localShadowAllocations.Num(); ++componentIndex)
	{
		const auto& allocation = m_localShadowAllocations[componentIndex];
		const bool bComponentCannotCastShadows =
			componentIndex >= m_components.Num() || !m_components[componentIndex].m_bIsActive ||
			!ContributesToRealtimeLighting(m_components[componentIndex].m_globalIlluminationMode) ||
			m_components[componentIndex].m_shadowType == RHI::EShadowType::None;
		const bool bExpired =
			frame > allocation.m_lastUsedFrame && frame - allocation.m_lastUsedFrame > LocalShadowCacheRetentionFrames;
		if (allocation.m_componentIndex == componentIndex && (bComponentCannotCastShadows || bExpired))
		{
			ReleaseLocalShadowAllocation(componentIndex);
		}
	}
}

void LightingECS::ReleaseUnusedLocalShadowAtlases()
{
	for (uint32_t atlasIndex = 0; atlasIndex < m_localShadowAtlases.Num(); ++atlasIndex)
	{
		auto& atlas = m_localShadowAtlases[atlasIndex];
		if (!atlas.m_texture)
		{
			continue;
		}

		bool bUsed = false;
		for (const auto occupied : atlas.m_occupancy)
		{
			if (occupied != 0)
			{
				bUsed = true;
				break;
			}
		}
		if (bUsed)
		{
			continue;
		}

		m_shadowMapTextures[NumCascades + atlasIndex] = m_defaultShadowMap;
		m_bShadowMapBindingsDirty = true;
		const auto latestTexture = atlas.m_texture;
		bool bLatestOwnedByFlight = false;
		size_t numPhysicalTextures = 0u;
		for (auto& flightResources : m_shadowFlightResources)
		{
			if (flightResources.m_localShadowAtlasTextures.Num() <= atlasIndex)
			{
				continue;
			}
			auto& flightTexture = flightResources.m_localShadowAtlasTextures[atlasIndex];
			if (!flightTexture)
			{
				continue;
			}
			bLatestOwnedByFlight |= flightTexture == latestTexture;
			++numPhysicalTextures;
			flightTexture.Clear();
		}
		if (latestTexture && !bLatestOwnedByFlight)
		{
			++numPhysicalTextures;
		}
		atlas = {};
		m_shadowMapsMb =
			(std::max)(0.0f, m_shadowMapsMb - LocalShadowAtlasMemoryMb * static_cast<float>(numPhysicalTextures));
	}
}

void LightingECS::PrepareLocalShadowPasses(const RHI::RHISceneViewPtr& sceneView,
	const TVector<RHI::RHILightProxy>& spotLights,
	const TVector<RHI::RHILightProxy>& pointLights,
	const Math::Transform& cameraTransform,
	const CameraData& cameraData,
	uint32_t viewportHeight,
	uint32_t flightSlot,
	LightingShadowFlightResources& flightResources,
	TVector<uint32_t>& shadowIndices,
	TVector<uint32_t>& shadowAtlasTiles,
	TVector<RHI::RHIUpdateShadowMapCommand>& outUpdateShadowMaps)
{
	const uint64_t frame = GetWorld()->GetCurrentFrame();
	const auto casterSceneVersions = sceneView->GetRetainedSceneVersions();
	const auto submissionToken = sceneView->GetOrCreateSubmissionCompletionToken();
	outUpdateShadowMaps.Reserve(outUpdateShadowMaps.Num() + spotLights.Num() + pointLights.Num() * 6u);

	auto prepareLights = [&](const TVector<RHI::RHILightProxy>& lights)
	{
		for (const auto& lightProxy : lights)
		{
			if (lightProxy.m_index >= m_components.Num() || lightProxy.m_index >= shadowIndices.Num())
			{
				continue;
			}

			const auto& light = m_components[lightProxy.m_index];
			const uint32_t desiredResolution =
				CalculateLocalShadowResolution(light, lightProxy.m_distanceToCamera, cameraData, viewportHeight);
			if (light.m_shadowType == RHI::EShadowType::None ||
				(light.m_type != ELightType::Point && light.m_type != ELightType::Spot) ||
				!EnsureLocalShadowAllocation(lightProxy.m_index, light.m_type, desiredResolution, frame))
			{
				continue;
			}

			auto& allocation = m_localShadowAllocations[lightProxy.m_index];
			GameObject* owner = light.m_owner ? static_cast<GameObject*>(light.m_owner.GetRawPtr()) : nullptr;
			if (!owner)
			{
				continue;
			}

			const auto& transform = owner->GetTransformComponent();
			const glm::vec3 position = transform.GetWorldPosition();
			const float farPlane = (std::max)(light.m_radius, 0.01f);
			if (farPlane <= 0.05f)
			{
				shadowIndices[lightProxy.m_index] = InvalidShadowMapIndex;
				continue;
			}
			if (!EnsureWritableLocalShadowAtlas(allocation.m_atlasIndex, flightSlot, flightResources))
			{
				continue;
			}
			if (flightResources.m_localShadowSnapshots.Num() <= lightProxy.m_index)
			{
				flightResources.m_localShadowSnapshots.Resize(static_cast<size_t>(lightProxy.m_index) + 1u);
			}
			auto& flightSnapshots = flightResources.m_localShadowSnapshots[lightProxy.m_index];
			const uint32_t mapCount = GetLocalShadowMapCount(light.m_type);
			if (flightSnapshots.Num() != mapCount)
			{
				flightSnapshots.Resize(mapCount);
			}
			const uint32_t firstSlot = allocation.m_slots[0];
			shadowIndices[lightProxy.m_index] =
				firstSlot | (light.m_shadowFilter == ELightShadowFilter::Soft &&
										App::GetActiveGraphicsSettings().m_bSupportSoftShadows
									? SoftShadowMapBit
									: 0u);

			const float nearPlane = (std::max)(0.01f, farPlane * 0.001f);
			std::array<glm::mat4, 6u> lightMatrices{};
			uint32_t numLightMatrices = 0u;
			if (light.m_type == ELightType::Spot)
			{
				const float fieldOfView = glm::clamp(glm::abs(light.m_cutOff.y) * 2.0f, 1.0f, 179.0f);
				const glm::mat4 view = glm::lookAtRH(position,
					position + glm::normalize(transform.GetForwardVector()),
					glm::normalize(glm::vec3(transform.GetCachedWorldMatrix() * Math::vec4_Up)));
				lightMatrices[0] = Math::PerspectiveRH(glm::radians(fieldOfView), 1.0f, nearPlane, farPlane) * view;
				numLightMatrices = 1u;
			}
			else
			{
				static constexpr glm::vec3 directions[6] = {{1.0f, 0.0f, 0.0f},
					{-1.0f, 0.0f, 0.0f},
					{0.0f, 1.0f, 0.0f},
					{0.0f, -1.0f, 0.0f},
					{0.0f, 0.0f, 1.0f},
					{0.0f, 0.0f, -1.0f}};
				static constexpr glm::vec3 upVectors[6] = {{0.0f, -1.0f, 0.0f},
					{0.0f, -1.0f, 0.0f},
					{0.0f, 0.0f, 1.0f},
					{0.0f, 0.0f, -1.0f},
					{0.0f, -1.0f, 0.0f},
					{0.0f, -1.0f, 0.0f}};
				const glm::mat4 projection = Math::PerspectiveRH(glm::radians(90.0f), 1.0f, nearPlane, farPlane);
				for (uint32_t face = 0; face < 6; ++face)
				{
					lightMatrices[face] =
						projection * glm::lookAtRH(position, position + directions[face], upVectors[face]);
				}
				numLightMatrices = 6u;
			}

			for (uint32_t face = 0; face < numLightMatrices; ++face)
			{
				const uint32_t shadowSlot = allocation.m_slots[face];
				if (shadowAtlasTiles.Num() <= shadowSlot)
				{
					shadowAtlasTiles.Resize(static_cast<size_t>(shadowSlot) + 1u);
				}
				const glm::ivec4 tile = allocation.m_tiles[face];
				const uint32_t tileX = static_cast<uint32_t>(tile.x) / LocalShadowMinResolution;
				const uint32_t tileY = static_cast<uint32_t>(tile.y) / LocalShadowMinResolution;
				const uint32_t tileCells = static_cast<uint32_t>(tile.z) / LocalShadowMinResolution;
				const uint32_t tileLevel = static_cast<uint32_t>(glm::log2(static_cast<float>(tileCells)));
				shadowAtlasTiles[shadowSlot] =
					tileX | (tileY << 6u) | (tileLevel << 12u) | (allocation.m_atlasIndex << 15u);
				auto& cachedState = flightSnapshots[face];
				Math::Frustum shadowFrustum(lightMatrices[face]);
				if (cachedState.m_resourceRevision == allocation.m_revision && cachedState.CanReuse(lightProxy.m_index,
																				   RHI::EShadowType::PCF,
																				   lightMatrices[face],
																				   sceneView->m_shadowCastersRevision,
																				   casterSceneVersions,
																				   shadowFrustum,
																				   submissionToken,
																				   sceneView->m_animationRevision))
				{
					cachedState.m_sceneRevision = sceneView->m_shadowCastersRevision;
					cachedState.m_animationRevision = sceneView->m_animationRevision;
					cachedState.m_casterSceneVersions = casterSceneVersions;
					continue;
				}

				RHI::RHIUpdateShadowMapCommand shadowPass{};
				shadowPass.m_renderArea = tile;
				shadowPass.m_lightMatrix = lightMatrices[face];
				shadowPass.m_lighMatrixIndex = shadowSlot;
				shadowPass.m_shadowType = RHI::EShadowType::PCF;
				shadowPass.m_meshList =
					sceneView->TraceShadowCasters(shadowFrustum, glm::vec3(cameraTransform.m_position));

				CSMLightState snapshot{};
				snapshot.m_componentIndex = lightProxy.m_index;
				snapshot.m_shadowType = RHI::EShadowType::PCF;
				snapshot.m_lightMatrix = lightMatrices[face];
				snapshot.m_sceneRevision = sceneView->m_shadowCastersRevision;
				snapshot.m_animationRevision = sceneView->m_animationRevision;
				snapshot.m_resourceRevision = allocation.m_revision;
				snapshot.m_casterSceneVersions = casterSceneVersions;
				ResolveShadowCasterUpdatePolicy(
					shadowPass.m_meshList, snapshot.m_bContainsDynamicCasters, snapshot.m_bContainsAnimatedCasters);
				shadowPass.m_shadowMap = m_localShadowAtlases[allocation.m_atlasIndex].m_texture;
				snapshot.m_submissionToken = submissionToken;
				snapshot.m_payloadCompletionToken = AcquireShadowPayloadToken(cachedState.m_payloadCompletionToken);
				shadowPass.m_payloadCompletionToken = snapshot.m_payloadCompletionToken;
				cachedState = std::move(snapshot);
				outUpdateShadowMaps.Emplace(std::move(shadowPass));
			}
		}
	};

	prepareLights(spotLights);
	prepareLights(pointLights);
}
