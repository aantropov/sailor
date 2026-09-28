#pragma once

#include "AssetRegistry/Model/ModelImporter.h"
#include "Core/FileRevision.h"

namespace Sailor::ModelLodCache
{
	bool Load(const ModelAssetInfo& assetInfo,
		const FileRevision& sourceRevision,
		uint32_t lodLevel,
		TVector<ModelImporter::MeshContext>& meshes);

	void Save(const ModelAssetInfo& assetInfo,
		const FileRevision& sourceRevision,
		uint32_t lodLevel,
		const TVector<ModelImporter::MeshContext>& meshes);
}
