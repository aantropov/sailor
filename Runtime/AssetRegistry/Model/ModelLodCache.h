#pragma once

#include "AssetRegistry/Model/ModelImporter.h"
#include "Core/FileRevision.h"

#include <filesystem>

namespace Sailor::ModelLodCache
{
	SAILOR_SHARED_API bool Load(const std::filesystem::path& cacheFolder,
		const ModelAssetInfo& assetInfo,
		const FileRevision& sourceRevision,
		uint32_t lodLevel,
		TVector<ModelImporter::MeshContext>& meshes);

	SAILOR_SHARED_API void Save(const std::filesystem::path& cacheFolder,
		const ModelAssetInfo& assetInfo,
		const FileRevision& sourceRevision,
		uint32_t lodLevel,
		const TVector<ModelImporter::MeshContext>& meshes);
}
