#pragma once

#include "AssetRegistry/Model/ModelImporter.h"

namespace Sailor::ModelLodGeneration
{
	void Generate(TVector<ModelImporter::MeshContext>& meshes, uint32_t numLods, float reductionFactor);
	void Prepare(const ModelAssetInfo& assetInfo, TVector<ModelImporter::MeshContext>& meshes);
}
