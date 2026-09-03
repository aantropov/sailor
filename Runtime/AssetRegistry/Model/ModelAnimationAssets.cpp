#include "AssetRegistry/Model/ModelImporter.h"

#include "AssetRegistry/AssetRegistry.h"
#include "AssetRegistry/Model/GeneratedModelAssetMetadata.h"
#include "AssetRegistry/Model/GltfImporterUtils.h"
#include "Core/Utils.h"
#include "Workspace/WorkspaceCacheContract.h"

#include <filesystem>
#include <string>

#include <tiny_gltf.h>

using namespace Sailor;

static FileId CreateAnimationAsset(const std::string& filepath,
	const std::string& glbFilename,
	uint32_t animationIndex,
	uint32_t skinIndex)
{
	FileId newFileId = FileId::CreateNewFileId();

	YAML::Node newAnimation =
		GeneratedModelAssetMetadata::CreateAnimation(newFileId, glbFilename, animationIndex, skinIndex);

	std::ostringstream serialized;
	serialized << newAnimation;
	if (!serialized)
	{
		SAILOR_LOG_ERROR("Cannot serialize generated animation metadata: %s", filepath.c_str());
		return {};
	}

	std::string diagnostic;
	if (!Workspace::AtomicReplaceWorkspaceCacheText(std::filesystem::path(filepath), serialized.str(), diagnostic))
	{
		SAILOR_LOG_ERROR("Cannot save generated animation metadata '%s': %s", filepath.c_str(), diagnostic.c_str());
		return {};
	}

	return newFileId;
}

bool ModelImporter::GenerateAnimationAssets(ModelAssetInfoPtr assetInfo)
{
	SAILOR_PROFILE_FUNCTION();

	tinygltf::Model gltfModel;
	std::string err, warn;
	const bool bGltfParsed = GltfImporterUtils::LoadModel(assetInfo->GetAssetFilepath(), true, gltfModel, err, warn);

	if (!bGltfParsed)
	{
		return false;
	}

	if (gltfModel.animations.empty())
	{
		const bool bChanged = assetInfo->GetAnimations().Num() > 0;
		assetInfo->GetAnimations().Clear();
		return bChanged;
	}

	const std::string animationsFolder = Utils::GetFileFolder(assetInfo->GetRelativeAssetFilepath());
	TVector<FileId> generatedAnimations;
	generatedAnimations.Reserve(gltfModel.animations.size());

	for (size_t i = 0; i < gltfModel.animations.size(); ++i)
	{
		std::filesystem::path outputPath;
		if (!App::GetSubmodule<AssetRegistry>()->ResolveWorkspaceContentPathForWrite(
				animationsFolder + assetInfo->GetAssetFilename() + "_animation_" + std::to_string(i) + ".anim.asset",
				outputPath))
		{
			SAILOR_LOG_ERROR(
				"Cannot resolve generated animation output for %s.", assetInfo->GetAssetFilepath().c_str());
			return false;
		}
		const FileId id = CreateAnimationAsset(outputPath.string(), assetInfo->GetAssetFilename(), (uint32_t)i, 0);
		if (!id)
		{
			return false;
		}
		generatedAnimations.Add(id);
	}

	assetInfo->GetAnimations() = std::move(generatedAnimations);
	return true;
}
