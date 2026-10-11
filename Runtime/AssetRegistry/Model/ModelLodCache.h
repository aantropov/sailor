#pragma once

#include "AssetRegistry/Model/ModelImporter.h"
#include "Core/FileRevision.h"

#if defined(SAILOR_MODEL_IMPORT_TEST_HOOKS)
#include <functional>
#endif

namespace Sailor::ModelLodCache
{
#if defined(SAILOR_MODEL_IMPORT_TEST_HOOKS)
	using AllocationObserver = std::function<void(uint64_t)>;
	AllocationObserver ExchangeAllocationObserverForTests(AllocationObserver observer);
#endif

	bool Load(const ModelAssetInfo& assetInfo,
		const FileRevision& sourceRevision,
		uint32_t lodLevel,
		TVector<ModelImporter::MeshContext>& meshes);

	void Save(const ModelAssetInfo& assetInfo,
		const FileRevision& sourceRevision,
		uint32_t lodLevel,
		const TVector<ModelImporter::MeshContext>& meshes);
}
