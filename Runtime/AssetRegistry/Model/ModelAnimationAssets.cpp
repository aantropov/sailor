#include "AssetRegistry/Model/ModelImporter.h"

#include "AssetRegistry/AssetRegistry.h"
#include "AssetRegistry/Model/GeneratedModelAssetMetadata.h"
#include "AssetRegistry/Model/GltfImporterUtils.h"
#include "Core/Utils.h"
#include "Workspace/WorkspaceCacheContract.h"

#include <filesystem>
#include <sstream>
#include <string>

#include <tiny_gltf.h>

using namespace Sailor;

bool ModelImporter::GenerateAnimationAssets(ModelAssetInfoPtr assetInfo, AssetRegistry& assetRegistry, bool& outChanged)
{
	SAILOR_PROFILE_FUNCTION();
	outChanged = false;

	tinygltf::Model gltfModel;
	std::string err, warn;
	const bool bGltfParsed = GltfImporterUtils::LoadModel(assetInfo->GetAssetFilepath(), true, gltfModel, err, warn);

	if (!bGltfParsed)
	{
		return false;
	}

	if (gltfModel.animations.empty())
	{
		outChanged = assetInfo->GetAnimations().Num() > 0;
		assetInfo->GetAnimations().Clear();
		return true;
	}

	const std::string animationsFolder = Utils::GetFileFolder(assetInfo->GetRelativeAssetFilepath());
	TVector<FileId> registeredAnimations;
	assetRegistry.GetAssetInfoIdsByTypeAndSource(
		"Sailor::AnimationAssetInfo", assetInfo->GetAssetFilepath(), registeredAnimations);
	TVector<FileId> knownAnimations = assetInfo->GetAnimations();
	for (const FileId& fileId : registeredAnimations)
	{
		if (!knownAnimations.Contains(fileId))
		{
			knownAnimations.Add(fileId);
		}
	}

	TVector<FileId> generatedAnimations;
	generatedAnimations.Reserve(gltfModel.animations.size());

	for (size_t i = 0; i < gltfModel.animations.size(); ++i)
	{
		auto matchesClip = [&](AnimationAssetInfoPtr animation)
		{
			std::error_code error;
			return animation != nullptr && animation->GetAnimationIndex() == static_cast<int32_t>(i) &&
				animation->GetSkinIndex() == 0 &&
				std::filesystem::equivalent(animation->GetAssetFilepath(), assetInfo->GetAssetFilepath(), error);
		};
		AnimationAssetInfoPtr existingAnimation = nullptr;
		for (const FileId& fileId : knownAnimations)
		{
			auto* animation = assetRegistry.GetAssetInfoPtr<AnimationAssetInfoPtr>(fileId);
			if (matchesClip(animation))
			{
				existingAnimation = animation;
				break;
			}
		}

		std::filesystem::path outputPath;
		if (!assetRegistry.ResolveWorkspaceContentPathForWrite(
			animationsFolder + assetInfo->GetAssetFilename() + "_animation_" + std::to_string(i) + ".anim.asset", outputPath))
		{
			SAILOR_LOG_ERROR(
				"Cannot resolve generated animation output for %s.", assetInfo->GetAssetFilepath().c_str());
			return false;
		}
		FileId fileId = existingAnimation ? existingAnimation->GetFileId() :
			(i < assetInfo->GetAnimations().Num() ? assetInfo->GetAnimations()[i] : FileId::Invalid);
		if (fileId)
		{
			auto* animation = assetRegistry.GetAssetInfoPtr<AnimationAssetInfoPtr>(fileId);
			if ((animation != nullptr && !matchesClip(animation)) ||
				!assetRegistry.CanReuseSecondaryAssetId(fileId, "Sailor::AnimationAssetInfo", assetInfo->GetAssetFilepath(), outputPath))
			{
				fileId = FileId::Invalid;
			}
		}
		std::error_code error;
		const bool bMetadataExists = std::filesystem::exists(outputPath, error);
		if (error)
		{
			SAILOR_LOG_ERROR("Cannot inspect animation metadata '%s': %s", outputPath.string().c_str(), error.message().c_str());
			return false;
		}

		if (!bMetadataExists)
		{
			if (!fileId)
			{
				fileId = FileId::CreateNewFileId();
			}
			const auto sourceFilename = std::filesystem::relative(assetInfo->GetAssetFilepath(), outputPath.parent_path(), error);
			if (error)
			{
				SAILOR_LOG_ERROR("Cannot resolve the model source for animation metadata: %s", outputPath.string().c_str());
				return false;
			}
			const YAML::Node metadata = GeneratedModelAssetMetadata::CreateAnimation(
				fileId, sourceFilename.generic_string(), static_cast<uint32_t>(i), 0);
			std::ostringstream serialized;
			serialized << metadata;
			if (!serialized)
			{
				SAILOR_LOG_ERROR("Cannot serialize generated animation metadata: %s", outputPath.string().c_str());
				return false;
			}

			const std::string text = serialized.str();
			std::string diagnostic;
			if (!Workspace::AtomicReplaceWorkspaceCacheBinary(outputPath, text.data(), text.size(), diagnostic,
				Workspace::EWorkspaceCacheAtomicWriteFailurePoint::None, Workspace::EWorkspaceCacheAtomicWriteMode::FailIfExists))
			{
				SAILOR_LOG_ERROR("Cannot create animation metadata '%s': %s", outputPath.string().c_str(), diagnostic.c_str());
				return false;
			}
		}

		const FileId registeredId = assetRegistry.RegisterGeneratedSecondaryAssetInfo(outputPath);
		if (!registeredId || (!bMetadataExists && fileId != registeredId) ||
			!matchesClip(assetRegistry.GetAssetInfoPtr<AnimationAssetInfoPtr>(registeredId)))
		{
			SAILOR_LOG_ERROR("Animation metadata does not match its model, clip and skin: %s", outputPath.string().c_str());
			return false;
		}
		generatedAnimations.Add(registeredId);
	}

	outChanged = generatedAnimations != assetInfo->GetAnimations();
	assetInfo->GetAnimations() = std::move(generatedAnimations);
	return true;
}
