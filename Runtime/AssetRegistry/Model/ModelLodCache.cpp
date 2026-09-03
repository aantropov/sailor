#include "AssetRegistry/Model/ModelLodCache.h"

#include "AssetRegistry/AssetRegistry.h"
#include "Containers/Concepts.h"
#include "RHI/VertexDescription.h"
#include "Sailor.h"
#include "Workspace/WorkspaceCacheContract.h"

#include <algorithm>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <limits>
#include <string_view>

using namespace Sailor;

namespace
{
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

	class BinaryWriter final
	{
	public:
		template<IsTriviallyCopyable Type>
		void Write(const Type& value)
		{
			Write(&value, sizeof(Type));
		}

		void Write(const void* data, size_t size)
		{
			if (size != 0u)
			{
				m_bytes.append(static_cast<const char*>(data), size);
			}
		}

		const std::string& GetBytes() const noexcept { return m_bytes; }

	private:
		std::string m_bytes;
	};

	class BinaryReader final
	{
	public:
		explicit BinaryReader(std::string_view bytes) : m_bytes(bytes) {}

		template<IsTriviallyCopyable Type>
		bool Read(Type& value)
		{
			return Read(&value, sizeof(Type));
		}

		bool Read(void* data, size_t size)
		{
			if (size > m_bytes.size() - m_offset)
			{
				return false;
			}
			if (size != 0u)
			{
				std::memcpy(data, m_bytes.data() + m_offset, size);
			}
			m_offset += size;
			return true;
		}

		bool IsComplete() const noexcept { return m_offset == m_bytes.size(); }

	private:
		std::string_view m_bytes;
		size_t m_offset = 0u;
	};

	std::filesystem::path GetPath(const FileId& fileId, uint32_t lodLevel)
	{
		const std::filesystem::path filename =
			ModelImporter::GetLodCacheFilename(fileId, lodLevel);
		return filename.empty()
			? std::filesystem::path{}
			: std::filesystem::path(AssetRegistry::GetCacheFolder()) / "Lods" / filename;
	}
}

bool ModelLodCache::Load(
	const ModelAssetInfo& assetInfo,
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
	const std::string bytes{
		std::istreambuf_iterator<char>(input),
		std::istreambuf_iterator<char>() };
	if (input.bad() || bytes.size() != fileSize)
	{
		return false;
	}

	BinaryReader reader(bytes);
	Header header{};
	if (!reader.Read(header) ||
		header.m_magic != Magic ||
		header.m_version != Version ||
		header.m_vertexStride != sizeof(RHI::VertexP3N3T3B3UV2C4I4W4) ||
		header.m_meshCount != meshes.Num() ||
		header.m_lodLevel != lodLevel ||
		header.m_sourceModificationTime != sourceRevision.m_modificationTimeNanoseconds ||
		header.m_sourceSize != sourceRevision.m_fileSize ||
		header.m_sourceContentHash != sourceRevision.m_contentHash ||
		header.m_unitScale != assetInfo.GetUnitScale() ||
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
		if (!reader.Read(meshHeader) ||
			meshHeader.m_vertexCount > std::numeric_limits<uint32_t>::max() ||
			meshHeader.m_indexCount > std::numeric_limits<uint32_t>::max())
		{
			return false;
		}

		const uint64_t vertexBytes = meshHeader.m_vertexCount *
			sizeof(RHI::VertexP3N3T3B3UV2C4I4W4);
		const uint64_t indexBytes = meshHeader.m_indexCount * sizeof(uint32_t);
		if (vertexBytes + indexBytes > MaxBytes)
		{
			return false;
		}

		lod.m_vertices.Resize(static_cast<size_t>(meshHeader.m_vertexCount));
		lod.m_indices.Resize(static_cast<size_t>(meshHeader.m_indexCount));
		if (!reader.Read(lod.m_vertices.GetData(), static_cast<size_t>(vertexBytes)) ||
			!reader.Read(lod.m_indices.GetData(), static_cast<size_t>(indexBytes)))
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

	if (!reader.IsComplete())
	{
		return false;
	}

	const size_t lodIndex = static_cast<size_t>(lodLevel - 1u);
	for (size_t meshIndex = 0; meshIndex < meshes.Num(); ++meshIndex)
	{
		meshes[meshIndex].lods.Resize((std::max)(
			meshes[meshIndex].lods.Num(),
			lodIndex + 1u));
		meshes[meshIndex].lods[lodIndex] = std::move(loaded[meshIndex]);
	}
	return true;
}

void ModelLodCache::Save(
	const ModelAssetInfo& assetInfo,
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

	BinaryWriter writer;
	writer.Write(header);
	const size_t lodIndex = static_cast<size_t>(lodLevel - 1u);
	for (const auto& mesh : meshes)
	{
		const auto* lod = lodIndex < mesh.lods.Num() ? &mesh.lods[lodIndex] : nullptr;
		const MeshHeader meshHeader{
			lod ? lod->m_vertices.Num() : 0u,
			lod ? lod->m_indices.Num() : 0u
		};
		writer.Write(meshHeader);
		if (lod)
		{
			writer.Write(
				lod->m_vertices.GetData(),
				lod->m_vertices.Num() * sizeof(RHI::VertexP3N3T3B3UV2C4I4W4));
			writer.Write(
				lod->m_indices.GetData(),
				lod->m_indices.Num() * sizeof(uint32_t));
		}
	}

	const std::string& bytes = writer.GetBytes();
	if (bytes.size() > MaxBytes)
	{
		return;
	}

	const std::filesystem::path path = GetPath(assetInfo.GetFileId(), lodLevel);
	std::string diagnostic;
	if (!path.empty() &&
		!Workspace::AtomicReplaceWorkspaceCacheBinary(
			path,
			bytes.data(),
			bytes.size(),
			diagnostic))
	{
		SAILOR_LOG(
			"Cannot save model LOD cache %s: %s",
			path.string().c_str(),
			diagnostic.c_str());
	}
}
