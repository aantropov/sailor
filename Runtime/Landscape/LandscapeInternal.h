#pragma once

#include "Landscape/LandscapeData.h"

namespace Sailor::LandscapeInternal
{
	struct LandscapeCpuTexture final
	{
		TSharedPtr<const TextureImporter::ByteCode> m_pixels;
		int32_t m_width = 0;
		int32_t m_height = 0;
		bool m_bFloat = false;

		LandscapeCpuTexture() = default;
		explicit LandscapeCpuTexture(const TextureImporter::CpuTextureSnapshot& texture) :
			m_pixels(texture.m_pixels), m_width(texture.m_width), m_height(texture.m_height),
			m_bFloat(texture.m_source.m_bDecodeAsFloat) {}

		bool IsValid() const
		{
			return m_pixels && m_width > 0 && m_height > 0 &&
				m_pixels->Num() >= static_cast<size_t>(m_width) * m_height * (m_bFloat ? 16u : 4u);
		}
	};

	struct LandscapeChunkCpuData final
	{
		uint32_t m_chunkX = 0u;
		uint32_t m_chunkZ = 0u;
		TVector<RHI::VertexP3N3T3B3UV2C4> m_vertices{};
		TVector<uint32_t> m_indices{};
		TVector<uint32_t> m_collisionIndices{};
		TVector<uint32_t> m_lodFirstIndices{};
		TVector<uint32_t> m_lodIndexCounts{};
		TVector<LandscapeVegetationInstance> m_vegetation{};
		TVector<Math::Triangle> m_bakeTriangles{};
		Math::AABB m_localBounds{};
	};

	enum class EVegetationProxyBuildResult : uint8_t
	{
		Success,
		Pending,
		NoRenderData
	};

	uint32_t HashVegetationSeed(uint32_t value);
	float VegetationRandom01(uint32_t value);
	LandscapeVegetationInstance BuildProceduralVegetationInstance(const LandscapeData& data,
		uint32_t chunkX,
		uint32_t chunkZ,
		size_t profileIndex,
		uint32_t instanceIndex,
		float height);
	bool IsVegetationAssetCompatible(const LandscapeData& data);
	const LandscapeVegetationChunkData* GetAuthoredVegetationChunk(const LandscapeData& data,
		uint32_t chunkX,
		uint32_t chunkZ);
	void AppendRenderInstance(const LandscapeVegetationInstance& instance, LandscapeVegetationRenderInstances& result);
	uint32_t GetVegetationInstanceCapacity(const LandscapeData& data, const LandscapeChunk& chunk, size_t profileIndex);
	void AppendVegetationInstances(const LandscapeData& data, const LandscapeCpuTexture& heightmap,
		uint32_t chunkX, uint32_t chunkZ, size_t profileIndex, TVector<LandscapeVegetationInstance>& instances);
	LandscapeChunkCpuData BuildChunk(const LandscapeData& data,
		const LandscapeCpuTexture& heightmap,
		const TVector<LandscapeCpuTexture>& materialMasks,
		uint32_t chunkX,
		uint32_t chunkZ);
	void GetOctreeBounds(const Math::AABB& bounds, glm::ivec3& center, glm::ivec3& extents);
	size_t LandscapeProxyId(size_t componentIndex, size_t chunkIndex);
	size_t LandscapeVegetationProxyId(size_t componentIndex, size_t chunkIndex, size_t profileIndex);
	// Pending leaves the caller's instances intact for a later retry.
	EVegetationProxyBuildResult BuildLandscapeVegetationProxy(size_t profileIndex,
		const LandscapeVegetationProfile& profile,
		LandscapeVegetationRenderInstances&& instances,
		EMobilityType mobility,
		uint64_t revision,
		LandscapeVegetationRenderProxy& result);
	uint64_t CalculateVegetationRenderRevision(const LandscapeVegetationProfile& profile);
	bool TryLoadVegetationAsset(LandscapeData& data);
	bool TrySaveVegetationAsset(LandscapeData& data);
	uint64_t CalculateGrassViewRevision(const LandscapeData& data,
		uint32_t instanceCapacity,
		const glm::mat4& inverseOwnerMatrix,
		const TVector<glm::vec3>& cameraPositions);
	LandscapeVegetationRenderInstances BuildGrassInstanceTransforms(const LandscapeData& data,
		const LandscapeChunk& chunk,
		size_t profileIndex,
		uint32_t instanceCount,
		const glm::mat4& ownerMatrix,
		const TVector<glm::vec3>& cameraPositions);
}
