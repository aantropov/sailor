#pragma once

#include "ECS/LandscapeECS.h"

namespace Sailor::LandscapeECSInternal
{
	struct LandscapeCpuTexture final
	{
		TVector<uint8_t> m_pixels{};
		int32_t m_width = 0;
		int32_t m_height = 0;
		bool m_bFloat = false;

		bool IsValid() const
		{
			return m_width > 0 && m_height > 0 &&
				   m_pixels.Num() >= static_cast<size_t>(m_width) * m_height * (m_bFloat ? 16u : 4u);
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
	bool DecodeCpuTexture(const FileId& fileId, LandscapeCpuTexture& result);
	LandscapeChunkCpuData BuildChunk(const LandscapeData& data,
		const LandscapeCpuTexture& heightmap,
		const TVector<LandscapeCpuTexture>& materialMasks,
		uint32_t chunkX,
		uint32_t chunkZ);
	float GetProfileValue(const TVector<float>& values, size_t index, float fallback);
	void AppendShadowMesh(RHI::RHIShadowCasterProxy& shadowCaster,
		const RHI::RHIMeshPtr& mesh,
		const glm::mat4& worldMatrix,
		const MaterialPtr& material,
		float maxCameraDistance);
	void AppendDepthMaterialMetadata(RHI::RHISceneViewProxy& proxy, const MaterialPtr& material);
	void GetOctreeBounds(const Math::AABB& bounds, glm::ivec3& center, glm::ivec3& extents);
	size_t LandscapeProxyId(size_t componentIndex, size_t chunkIndex);
	EVegetationProxyBuildResult BuildLandscapeVegetationProxy(size_t componentIndex,
		size_t chunkIndex,
		size_t profileIndex,
		const LandscapeVegetationProfile& profile,
		const glm::mat4& ownerMatrix,
		uint64_t frame,
		LandscapeVegetationRenderInstances instances,
		EMobilityType mobility,
		uint64_t revision,
		LandscapeVegetationRenderProxy& result);
	bool AreVegetationProfileSettingsEqual(const LandscapeVegetationProfile& lhs,
		const LandscapeVegetationProfile& rhs);
	uint64_t CalculateVegetationMaterialRenderMetadataRevision(const LandscapeVegetationProfile& profile);
	void MarkChunksAffectedByStampChanges(LandscapeData& data,
		const TVector<float>& previous,
		const TVector<float>& current,
		float margin);
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
