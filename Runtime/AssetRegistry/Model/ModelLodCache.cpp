#include "AssetRegistry/Model/ModelLodCache.h"

#include "AssetRegistry/AssetRegistry.h"
#include "Containers/Concepts.h"
#include "RHI/VertexDescription.h"
#include "Sailor.h"
#include "Workspace/WorkspaceCacheContract.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <limits>

using namespace Sailor;

namespace
{
	constexpr uint32_t Version = 1u;
	constexpr uint64_t MaxBytes = 1024ull * 1024ull * 1024ull;
	constexpr std::array<char, 8> Magic = {'S', 'A', 'I', 'L', 'L', 'O', 'D', '\0'};

	struct Header final
	{
		std::array<char, 8> m_magic{};
		uint32_t m_version = 0u;
		uint32_t m_vertexStride = 0u;
		uint32_t m_meshCount = 0u;
		uint32_t m_lodLevel = 0u;
		int64_t m_sourceModificationTime = 0;
		uint64_t m_sourceSize = 0u;
		uint64_t m_sourceContentHash = 0u;
		float m_unitScale = 1.0f;
		float m_reductionFactor = 0.5f;
		uint32_t m_bBatchByMaterial = 0u;
		uint32_t m_bFlipTexcoordY = 0u;
	};

	struct MeshHeader final
	{
		uint64_t m_vertexCount = 0u;
		uint64_t m_indexCount = 0u;
	};

	template <IsTriviallyCopyable Type> void Append(std::string& bytes, const Type& value)
	{
		bytes.append(reinterpret_cast<const char*>(&value), sizeof(Type));
	}

	void Append(std::string& bytes, const void* data, size_t size)
	{
		if (size > 0u)
		{
			bytes.append(static_cast<const char*>(data), size);
		}
	}

	template <IsTriviallyCopyable Type> bool Read(const std::string& bytes, size_t& offset, Type& value)
	{
		if (offset > bytes.size() || sizeof(Type) > bytes.size() - offset)
		{
			return false;
		}

		std::memcpy(&value, bytes.data() + offset, sizeof(Type));
		offset += sizeof(Type);
		return true;
	}

	bool Read(const std::string& bytes, size_t& offset, void* data, size_t size)
	{
		if (offset > bytes.size() || size > bytes.size() - offset || (data == nullptr && size > 0u))
		{
			return false;
		}

		if (size > 0u)
		{
			std::memcpy(data, bytes.data() + offset, size);
		}
		offset += size;
		return true;
	}

	std::filesystem::path GetPath(const FileId& fileId, uint32_t lodLevel)
	{
		const std::filesystem::path filename = ModelImporter::GetLodCacheFilename(fileId, lodLevel);
		return filename.empty() ? std::filesystem::path{}
								: std::filesystem::path(AssetRegistry::GetCacheFolder()) / "Lods" / filename;
	}
}

bool Sailor::ModelLodCache::Load(const ModelAssetInfo& assetInfo,
	const FileRevision& sourceRevision,
	uint32_t lodLevel,
	TVector<ModelImporter::MeshContext>& meshes)
{
	const std::filesystem::path path = GetPath(assetInfo.GetFileId(), lodLevel);
	std::error_code error;
	const uint64_t fileSize = path.empty() ? 0u : std::filesystem::file_size(path, error);
	if (error || fileSize < sizeof(Header) || fileSize > MaxBytes)
	{
		return false;
	}

	std::ifstream input(path, std::ios::binary);
	const std::string bytes{std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
	if (input.bad() || bytes.size() != fileSize)
	{
		return false;
	}

	size_t offset = 0u;
	Header header{};
	if (!Read(bytes, offset, header) || header.m_magic != Magic || header.m_version != Version ||
		header.m_vertexStride != sizeof(RHI::VertexP3N3T3B3UV2C4I4W4) || header.m_meshCount != meshes.Num() ||
		header.m_lodLevel != lodLevel ||
		header.m_sourceModificationTime != sourceRevision.m_modificationTimeNanoseconds ||
		header.m_sourceSize != sourceRevision.m_fileSize ||
		header.m_sourceContentHash != sourceRevision.m_contentHash || header.m_unitScale != assetInfo.GetUnitScale() ||
		header.m_reductionFactor != assetInfo.GetLodReductionFactor() ||
		header.m_bBatchByMaterial != static_cast<uint32_t>(assetInfo.ShouldBatchByMaterial()) ||
		header.m_bFlipTexcoordY != static_cast<uint32_t>(assetInfo.ShouldFlipTexcoordY()))
	{
		return false;
	}

	TVector<ModelImporter::MeshContext::LodGeometry> loaded;
	loaded.Resize(meshes.Num());
	for (auto& lod : loaded)
	{
		MeshHeader meshHeader{};
		if (!Read(bytes, offset, meshHeader) || meshHeader.m_vertexCount > std::numeric_limits<uint32_t>::max() ||
			meshHeader.m_indexCount > std::numeric_limits<uint32_t>::max())
		{
			return false;
		}

		const uint64_t vertexBytes = meshHeader.m_vertexCount * sizeof(RHI::VertexP3N3T3B3UV2C4I4W4);
		const uint64_t indexBytes = meshHeader.m_indexCount * sizeof(uint32_t);
		if (vertexBytes + indexBytes > MaxBytes)
		{
			return false;
		}

		lod.m_vertices.Resize(static_cast<size_t>(meshHeader.m_vertexCount));
		lod.m_indices.Resize(static_cast<size_t>(meshHeader.m_indexCount));
		if (!Read(bytes, offset, lod.m_vertices.GetData(), static_cast<size_t>(vertexBytes)) ||
			!Read(bytes, offset, lod.m_indices.GetData(), static_cast<size_t>(indexBytes)))
		{
			return false;
		}
		for (uint32_t index : lod.m_indices)
		{
			if (index >= lod.m_vertices.Num())
			{
				return false;
			}
		}
	}

	if (offset != bytes.size())
	{
		return false;
	}

	const size_t lodIndex = static_cast<size_t>(lodLevel - 1u);
	for (size_t meshIndex = 0; meshIndex < meshes.Num(); ++meshIndex)
	{
		meshes[meshIndex].lods.Resize((std::max)(meshes[meshIndex].lods.Num(), lodIndex + 1u));
		meshes[meshIndex].lods[lodIndex] = std::move(loaded[meshIndex]);
	}
	return true;
}

void Sailor::ModelLodCache::Save(const ModelAssetInfo& assetInfo,
	const FileRevision& sourceRevision,
	uint32_t lodLevel,
	const TVector<ModelImporter::MeshContext>& meshes)
{
	Header header{};
	header.m_magic = Magic;
	header.m_version = Version;
	header.m_vertexStride = sizeof(RHI::VertexP3N3T3B3UV2C4I4W4);
	header.m_meshCount = static_cast<uint32_t>(meshes.Num());
	header.m_lodLevel = lodLevel;
	header.m_sourceModificationTime = sourceRevision.m_modificationTimeNanoseconds;
	header.m_sourceSize = sourceRevision.m_fileSize;
	header.m_sourceContentHash = sourceRevision.m_contentHash;
	header.m_unitScale = assetInfo.GetUnitScale();
	header.m_reductionFactor = assetInfo.GetLodReductionFactor();
	header.m_bBatchByMaterial = static_cast<uint32_t>(assetInfo.ShouldBatchByMaterial());
	header.m_bFlipTexcoordY = static_cast<uint32_t>(assetInfo.ShouldFlipTexcoordY());

	std::string bytes;
	Append(bytes, header);
	const size_t lodIndex = static_cast<size_t>(lodLevel - 1u);
	for (const auto& mesh : meshes)
	{
		const auto* lod = lodIndex < mesh.lods.Num() ? &mesh.lods[lodIndex] : nullptr;
		const MeshHeader meshHeader{lod ? lod->m_vertices.Num() : 0u, lod ? lod->m_indices.Num() : 0u};
		Append(bytes, meshHeader);
		if (lod)
		{
			Append(bytes, lod->m_vertices.GetData(), lod->m_vertices.Num() * sizeof(RHI::VertexP3N3T3B3UV2C4I4W4));
			Append(bytes, lod->m_indices.GetData(), lod->m_indices.Num() * sizeof(uint32_t));
		}
	}

	if (bytes.size() > MaxBytes)
	{
		return;
	}

	const std::filesystem::path path = GetPath(assetInfo.GetFileId(), lodLevel);
	std::string diagnostic;
	if (!path.empty() && !Workspace::AtomicReplaceWorkspaceCacheBinary(path, bytes.data(), bytes.size(), diagnostic))
	{
		SAILOR_LOG("Cannot save model LOD cache %s: %s", path.string().c_str(), diagnostic.c_str());
	}
}
