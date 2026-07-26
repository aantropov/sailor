#pragma once

#include "AssetRegistry/Animation/AnimationAssetInfo.h"
#include "AssetRegistry/Texture/TextureAssetInfo.h"

namespace Sailor::GeneratedModelAssetMetadata
{
	inline YAML::Node CreateTexture(
		const FileId& fileId,
		const std::string& glbFilename,
		uint32_t glbTextureIndex,
		bool bShouldGenerateMips = true,
		RHI::ETextureFormat format = RHI::ETextureFormat::R8G8B8A8_SRGB,
		RHI::ETextureClamping clamping = RHI::ETextureClamping::Repeat,
		RHI::ETextureFiltration filtration = RHI::ETextureFiltration::Linear,
		bool bShouldKeepCpuBuffers = false)
	{
		YAML::Node result = CreateAssetInfoMetadata<TextureAssetInfo>(fileId, glbFilename);
		result["glbTextureIndex"] = glbTextureIndex;
		result["bShouldGenerateMips"] = bShouldGenerateMips;
		result["clamping"] = clamping;
		result["filtration"] = filtration;
		result["format"] = format;
		result["bShouldKeepCpuBuffers"] = bShouldKeepCpuBuffers;
		return result;
	}

	inline YAML::Node CreateAnimation(
		const FileId& fileId,
		const std::string& glbFilename,
		uint32_t animationIndex,
		uint32_t skinIndex)
	{
		YAML::Node result = CreateAssetInfoMetadata<AnimationAssetInfo>(fileId, glbFilename);
		result["animationIndex"] = animationIndex;
		result["skinIndex"] = skinIndex;
		return result;
	}
}
