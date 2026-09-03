#pragma once

#include "AssetRegistry/Model/ModelImporter.h"
#include "Core/FileRevision.h"

#include <array>

namespace Sailor
{
	class ModelLodCache final
	{
	public:
		static bool Load(
			const ModelAssetInfo& assetInfo,
			const FileRevision& sourceRevision,
			uint32_t lodLevel,
			TVector<ModelImporter::MeshContext>& meshes);

		static void Save(
			const ModelAssetInfo& assetInfo,
			const FileRevision& sourceRevision,
			uint32_t lodLevel,
			const TVector<ModelImporter::MeshContext>& meshes);

	private:
		static constexpr uint32_t Version = 1u;
		static constexpr uint64_t MaxBytes = 1024ull * 1024ull * 1024ull;
		static constexpr std::array<char, 8> Magic = {
			'S', 'A', 'I', 'L', 'L', 'O', 'D', '\0'
		};
	};
}
