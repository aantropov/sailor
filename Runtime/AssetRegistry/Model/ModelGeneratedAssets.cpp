#include "AssetRegistry/Model/ModelImporter.h"

#include "AssetRegistry/AssetRegistry.h"
#include "AssetRegistry/Material/MaterialImporter.h"
#include "AssetRegistry/Model/GeneratedModelAssetMetadata.h"
#include "AssetRegistry/Model/GltfImporterUtils.h"
#include "AssetRegistry/Texture/TextureImporter.h"
#include "Core/Utils.h"
#include "YamlExceptionBoundary.h"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <limits>
#include <string>

#include <tiny_gltf.h>

using namespace Sailor;

static bool TryLoadYamlFile(const std::filesystem::path& filepath, YAML::Node& outDocument, std::string& outDiagnostic)
{
	std::string payload;
	if (!AssetRegistry::ReadAllTextFile(filepath.string(), payload))
	{
		outDiagnostic = "cannot read the file";
		return false;
	}

	return External::TryLoadYaml(payload, outDocument, outDiagnostic);
}

FileId ModelImporter::CreateTextureAsset(const std::string& filepath,
	const std::string& sourceFilename,
	uint32_t sourceTextureIndex,
	bool bShouldGenerateMips,
	RHI::EFormat format,
	RHI::ETextureClamping clamping,
	RHI::ETextureFiltration filtration,
	bool bShouldKeepCpuBuffers)
{
	AssetRegistry* assetRegistry = App::GetSubmodule<AssetRegistry>();
	if (assetRegistry == nullptr)
	{
		return FileId::Invalid;
	}

	std::error_code statusError;
	const std::filesystem::file_status metadataStatus = std::filesystem::symlink_status(filepath, statusError);
	if (statusError == std::errc::no_such_file_or_directory || statusError == std::errc::not_a_directory)
	{
		statusError.clear();
	}
	if (statusError)
	{
		SAILOR_LOG_ERROR(
			"Cannot inspect generated texture metadata path '%s': %s", filepath.c_str(), statusError.message().c_str());
		return FileId::Invalid;
	}

	const bool bMetadataExists = std::filesystem::exists(metadataStatus);
	if (bMetadataExists && !std::filesystem::is_regular_file(metadataStatus))
	{
		SAILOR_LOG_ERROR("Generated texture metadata path is not a regular file: %s", filepath.c_str());
		return FileId::Invalid;
	}

	FileId fileId =
		bMetadataExists ? assetRegistry->RegisterGeneratedSecondaryAssetInfo(filepath) : FileId::CreateNewFileId();
	if (!fileId)
	{
		return FileId::Invalid;
	}

	if (bMetadataExists)
	{
		TextureAssetInfoPtr existingTextureInfo = assetRegistry->GetAssetInfoPtr<TextureAssetInfoPtr>(fileId);
		if (existingTextureInfo == nullptr || existingTextureInfo->GetAssetFilename() != sourceFilename ||
			existingTextureInfo->GetGlbTextureIndex() != static_cast<int32_t>(sourceTextureIndex))
		{
			SAILOR_LOG_ERROR("Existing generated texture metadata is incompatible: %s", filepath.c_str());
			return FileId::Invalid;
		}

		if (existingTextureInfo->ShouldGenerateMips() == bShouldGenerateMips &&
			existingTextureInfo->GetFormat() == format && existingTextureInfo->GetClamping() == clamping &&
			existingTextureInfo->GetFiltration() == filtration &&
			existingTextureInfo->ShouldKeepCpuBuffers() == bShouldKeepCpuBuffers)
		{
			return fileId;
		}
	}

	YAML::Node newTexture = GeneratedModelAssetMetadata::CreateTexture(fileId,
		sourceFilename,
		sourceTextureIndex,
		bShouldGenerateMips,
		format,
		clamping,
		filtration,
		bShouldKeepCpuBuffers);

	std::ostringstream serialized;
	serialized << newTexture;
	if (!serialized)
	{
		SAILOR_LOG_ERROR("Cannot serialize generated texture metadata: %s", filepath.c_str());
		return {};
	}

	std::string diagnostic;
	if (!Workspace::AtomicReplaceWorkspaceCacheText(std::filesystem::path(filepath), serialized.str(), diagnostic))
	{
		SAILOR_LOG_ERROR("Cannot save generated texture metadata '%s': %s", filepath.c_str(), diagnostic.c_str());
		return {};
	}

	if (assetRegistry->RegisterGeneratedSecondaryAssetInfo(filepath) != fileId)
	{
		SAILOR_LOG_ERROR(
			"Cannot register generated texture metadata for immediate model processing: %s", filepath.c_str());
		return FileId::Invalid;
	}

	return fileId;
}

bool ModelImporter::GenerateMaterialAssets(ModelAssetInfoPtr assetInfo)
{
	SAILOR_PROFILE_FUNCTION();

	tinygltf::Model gltfModel;
	std::string err;
	std::string warn;
	const bool bGltfParsed = GltfImporterUtils::LoadModel(assetInfo->GetAssetFilepath(), true, gltfModel, err, warn);

	if (!err.empty())
	{
		SAILOR_LOG_ERROR("Parsing gltf %s error: %s", assetInfo->GetAssetFilepath().c_str(), err.c_str());
	}

	if (!warn.empty())
	{
		SAILOR_LOG("Parsing gltf %s warning: %s", assetInfo->GetAssetFilepath().c_str(), warn.c_str());
	}

	if (!bGltfParsed)
	{
		return false;
	}

	const std::string texturesFolder = Utils::GetFileFolder(assetInfo->GetRelativeAssetFilepath());

	TVector<MaterialAsset::Data> materials(gltfModel.materials.size());

	for (size_t i = 0; i < gltfModel.materials.size(); ++i)
	{
		const auto& material = gltfModel.materials[i];

		MaterialAsset::Data& data = materials[i];
		data.m_name = !material.name.empty() ? material.name : ("material" + std::to_string(i));
		if (GltfImporterUtils::IsMaterialUsedBySkinnedMesh(gltfModel, i))
		{
			data.m_shaderDefines.Add("SKINNING");
		}

		std::filesystem::path materialNamePath;
		if (!App::GetSubmodule<AssetRegistry>()->ResolveWorkspaceContentPathForWrite(
				texturesFolder + assetInfo->GetAssetFilename() + "_material_" + std::to_string(i), materialNamePath))
		{
			SAILOR_LOG_ERROR("Cannot resolve generated material output for %s.", assetInfo->GetAssetFilepath().c_str());
			return false;
		}
		const std::string materialName = materialNamePath.string();

		if (material.pbrMetallicRoughness.baseColorTexture.index != -1)
		{
			data.m_samplers.Add("baseColorSampler",
				CreateTextureAsset(materialName + "_baseColorTexture.png.asset",
					assetInfo->GetAssetFilename(),
					material.pbrMetallicRoughness.baseColorTexture.index,
					true,
					RHI::ETextureFormat::R8G8B8A8_SRGB,
					RHI::ETextureClamping::Repeat,
					RHI::ETextureFiltration::Linear,
					assetInfo->ShouldKeepCpuBuffers()));
		}

		if (material.normalTexture.index != -1)
		{
			data.m_samplers.Add("normalSampler",
				CreateTextureAsset(materialName + "_normalTexture.png.asset",
					assetInfo->GetAssetFilename(),
					material.normalTexture.index,
					true,
					RHI::ETextureFormat::R8G8B8A8_UNORM,
					RHI::ETextureClamping::Repeat,
					RHI::ETextureFiltration::Linear,
					assetInfo->ShouldKeepCpuBuffers()));
		}

		if (material.emissiveTexture.index != -1)
		{
			data.m_samplers.Add("emissiveSampler",
				CreateTextureAsset(materialName + "_emissionTexture.png.asset",
					assetInfo->GetAssetFilename(),
					material.emissiveTexture.index,
					true,
					RHI::ETextureFormat::R8G8B8A8_SRGB,
					RHI::ETextureClamping::Repeat,
					RHI::ETextureFiltration::Linear,
					assetInfo->ShouldKeepCpuBuffers()));
		}

		if (material.pbrMetallicRoughness.metallicRoughnessTexture.index != -1)
		{
			data.m_samplers.Add("ormSampler",
				CreateTextureAsset(materialName + "_ormTexture.png.asset",
					assetInfo->GetAssetFilename(),
					material.pbrMetallicRoughness.metallicRoughnessTexture.index,
					true,
					RHI::ETextureFormat::R8G8B8A8_UNORM,
					RHI::ETextureClamping::Repeat,
					RHI::ETextureFiltration::Linear,
					assetInfo->ShouldKeepCpuBuffers()));
		}

		if (material.occlusionTexture.index != -1)
		{
			data.m_samplers.Add("occlusionSampler",
				CreateTextureAsset(materialName + "_occlusionTexture.png.asset",
					assetInfo->GetAssetFilename(),
					material.occlusionTexture.index,
					true,
					RHI::ETextureFormat::R8G8B8A8_UNORM,
					RHI::ETextureClamping::Repeat,
					RHI::ETextureFiltration::Linear,
					assetInfo->ShouldKeepCpuBuffers()));
		}

		auto tryReadNumberProperty = [](const tinygltf::Value& object, const char* property, double& outValue)
		{
			if (!object.IsObject() || !object.Has(property))
			{
				return false;
			}

			const tinygltf::Value& value = object.Get(property);
			if (!value.IsNumber())
			{
				return false;
			}

			const double parsedValue = value.GetNumberAsDouble();
			if (!std::isfinite(parsedValue) || parsedValue > std::numeric_limits<float>::max() ||
				parsedValue < -std::numeric_limits<float>::max())
			{
				return false;
			}

			outValue = parsedValue;
			return true;
		};

		auto tryReadTextureIndex = [&gltfModel](const tinygltf::Value& object, const char* property, int32_t& outIndex)
		{
			if (!object.IsObject() || !object.Has(property))
			{
				return false;
			}

			const tinygltf::Value& textureInfo = object.Get(property);
			if (!textureInfo.IsObject() || !textureInfo.Has("index"))
			{
				return false;
			}

			const tinygltf::Value& indexValue = textureInfo.Get("index");
			if (!indexValue.IsInt())
			{
				return false;
			}

			const int32_t index = indexValue.GetNumberAsInt();
			if (index < 0 || static_cast<size_t>(index) >= gltfModel.textures.size())
			{
				return false;
			}

			outIndex = index;
			return true;
		};

		auto tryReadVec3Property = [](const tinygltf::Value& object, const char* property, glm::vec3& outValue)
		{
			if (!object.IsObject() || !object.Has(property))
			{
				return false;
			}

			const tinygltf::Value& array = object.Get(property);
			if (!array.IsArray() || array.ArrayLen() < 3)
			{
				return false;
			}

			glm::vec3 parsedValue(0.0f);
			for (size_t component = 0; component < 3; ++component)
			{
				const tinygltf::Value& value = array.Get(component);
				if (!value.IsNumber())
				{
					return false;
				}

				const double parsedComponent = value.GetNumberAsDouble();
				if (!std::isfinite(parsedComponent) || parsedComponent > std::numeric_limits<float>::max() ||
					parsedComponent < -std::numeric_limits<float>::max())
				{
					return false;
				}
				parsedValue[static_cast<int32_t>(component)] = static_cast<float>(parsedComponent);
			}

			outValue = parsedValue;
			return true;
		};

		const auto transmissionSettings = GltfImporterUtils::ResolveMaterialTransmission(
			material, gltfModel.textures.size(), assetInfo->GetUnitScale());
		if (transmissionSettings.IsEnabled())
		{
			data.m_uniformsFloat.Add("material.transmissionFactor", transmissionSettings.m_factor);
			data.m_uniformsFloat.Add("material.thicknessFactor", transmissionSettings.m_thicknessFactor);
			data.m_uniformsFloat.Add("material.attenuationDistance", transmissionSettings.m_attenuationDistance);
			data.m_uniformsVec4.Add(
				"material.attenuationColor", glm::vec4(transmissionSettings.m_attenuationColor, 1.0f));
			if (transmissionSettings.m_textureIndex >= 0)
			{
				data.m_samplers.Add("transmissionSampler",
					CreateTextureAsset(materialName + "_transmissionTexture.png.asset",
						assetInfo->GetAssetFilename(),
						transmissionSettings.m_textureIndex,
						true,
						RHI::ETextureFormat::R8G8B8A8_UNORM,
						RHI::ETextureClamping::Repeat,
						RHI::ETextureFiltration::Linear,
						assetInfo->ShouldKeepCpuBuffers()));
			}
			if (transmissionSettings.m_thicknessTextureIndex >= 0)
			{
				data.m_samplers.Add("thicknessSampler",
					CreateTextureAsset(materialName + "_thicknessTexture.png.asset",
						assetInfo->GetAssetFilename(),
						transmissionSettings.m_thicknessTextureIndex,
						true,
						RHI::ETextureFormat::R8G8B8A8_UNORM,
						RHI::ETextureClamping::Repeat,
						RHI::ETextureFiltration::Linear,
						assetInfo->ShouldKeepCpuBuffers()));
			}
			data.m_shaderDefines.Add("TRANSMISSION");
		}
		if (transmissionSettings.IsEnabled() || transmissionSettings.m_bHasIndexOfRefraction)
		{
			data.m_uniformsFloat.Add("material.indexOfRefraction", transmissionSettings.m_indexOfRefraction);
		}
		if (!transmissionSettings.IsEnabled() && transmissionSettings.m_bHasIndexOfRefraction)
		{
			data.m_shaderDefines.Add("MATERIAL_IOR");
		}

		int32_t textureIndex = -1;
		auto ccIt = material.extensions.find("KHR_materials_clearcoat");
		if (ccIt != material.extensions.end() && ccIt->second.IsObject())
		{
			const tinygltf::Value& cc = ccIt->second;

			double ccFactor = 0.0;
			tryReadNumberProperty(cc, "clearcoatFactor", ccFactor);

			double ccRoughness = 0.0;
			tryReadNumberProperty(cc, "clearcoatRoughnessFactor", ccRoughness);

			data.m_uniformsFloat.Add("material.clearcoatFactor", (float)ccFactor);
			data.m_uniformsFloat.Add("material.clearcoatRoughnessFactor", (float)ccRoughness);

			textureIndex = -1;
			if (tryReadTextureIndex(cc, "clearcoatTexture", textureIndex))
			{
				data.m_samplers.Add("clearcoatSampler",
					CreateTextureAsset(materialName + "_clearcoatTexture.png.asset",
						assetInfo->GetAssetFilename(),
						textureIndex,
						true,
						RHI::ETextureFormat::R8G8B8A8_UNORM,
						RHI::ETextureClamping::Repeat,
						RHI::ETextureFiltration::Linear,
						assetInfo->ShouldKeepCpuBuffers()));
			}

			textureIndex = -1;
			if (tryReadTextureIndex(cc, "clearcoatRoughnessTexture", textureIndex))
			{
				data.m_samplers.Add("clearcoatRoughnessSampler",
					CreateTextureAsset(materialName + "_clearcoatRoughnessTexture.png.asset",
						assetInfo->GetAssetFilename(),
						textureIndex,
						true,
						RHI::ETextureFormat::R8G8B8A8_UNORM,
						RHI::ETextureClamping::Repeat,
						RHI::ETextureFiltration::Linear,
						assetInfo->ShouldKeepCpuBuffers()));
			}

			if (cc.Has("clearcoatNormalTexture") && cc.Get("clearcoatNormalTexture").IsObject())
			{
				const tinygltf::Value& tex = cc.Get("clearcoatNormalTexture");
				double scale = 1.0;
				tryReadNumberProperty(tex, "scale", scale);

				textureIndex = -1;
				if (tryReadTextureIndex(cc, "clearcoatNormalTexture", textureIndex))
				{
					data.m_samplers.Add("clearcoatNormalSampler",
						CreateTextureAsset(materialName + "_clearcoatNormalTexture.png.asset",
							assetInfo->GetAssetFilename(),
							textureIndex,
							true,
							RHI::ETextureFormat::R8G8B8A8_UNORM,
							RHI::ETextureClamping::Repeat,
							RHI::ETextureFiltration::Linear,
							assetInfo->ShouldKeepCpuBuffers()));
				}
				data.m_uniformsFloat.Add("material.clearcoatNormalScale", (float)scale);
			}

			data.m_shaderDefines.Add("CLEAR_COAT");
		}

		auto sheenIt = material.extensions.find("KHR_materials_sheen");
		if (sheenIt != material.extensions.end() && sheenIt->second.IsObject())
		{
			const tinygltf::Value& sheen = sheenIt->second;

			glm::vec3 color = glm::vec3(0.0f);
			tryReadVec3Property(sheen, "sheenColorFactor", color);

			double roughness = 0.0;
			tryReadNumberProperty(sheen, "sheenRoughnessFactor", roughness);

			data.m_uniformsVec4.Add("material.sheenColorFactor", glm::vec4(color, 0.0f));
			data.m_uniformsFloat.Add("material.sheenRoughnessFactor", (float)roughness);

			textureIndex = -1;
			if (tryReadTextureIndex(sheen, "sheenColorTexture", textureIndex))
			{
				data.m_samplers.Add("sheenColorSampler",
					CreateTextureAsset(materialName + "_sheenColorTexture.png.asset",
						assetInfo->GetAssetFilename(),
						textureIndex,
						true,
						RHI::ETextureFormat::R8G8B8A8_SRGB,
						RHI::ETextureClamping::Repeat,
						RHI::ETextureFiltration::Linear,
						assetInfo->ShouldKeepCpuBuffers()));
			}

			textureIndex = -1;
			if (tryReadTextureIndex(sheen, "sheenRoughnessTexture", textureIndex))
			{
				data.m_samplers.Add("sheenRoughnessSampler",
					CreateTextureAsset(materialName + "_sheenRoughnessTexture.png.asset",
						assetInfo->GetAssetFilename(),
						textureIndex,
						true,
						RHI::ETextureFormat::R8G8B8A8_UNORM,
						RHI::ETextureClamping::Repeat,
						RHI::ETextureFiltration::Linear,
						assetInfo->ShouldKeepCpuBuffers()));
			}

			data.m_shaderDefines.Add("SHEEN");
		}

		const vec4 baseColor = vec4((float)material.pbrMetallicRoughness.baseColorFactor[0],
			(float)material.pbrMetallicRoughness.baseColorFactor[1],
			(float)material.pbrMetallicRoughness.baseColorFactor[2],
			(float)material.pbrMetallicRoughness.baseColorFactor[3]);

		const vec4 emissiveFactor = vec4(GltfImporterUtils::ResolveMaterialEmissiveFactor(material), 0.0f);

		data.m_uniformsVec4.Add("material.baseColorFactor", baseColor);
		data.m_uniformsVec4.Add("material.emissiveFactor", emissiveFactor);

		data.m_uniformsFloat.Add("material.roughnessFactor", (float)material.pbrMetallicRoughness.roughnessFactor);
		data.m_uniformsFloat.Add("material.metallicFactor", (float)material.pbrMetallicRoughness.metallicFactor);
		data.m_uniformsFloat.Add("material.normalScale", (float)material.normalTexture.scale);
		data.m_uniformsFloat.Add("material.alphaCutoff", (float)material.alphaCutoff);
		data.m_uniformsFloat.Add("material.occlusionStrength", (float)material.occlusionTexture.strength);

		const auto alphaModeSettings =
			GltfImporterUtils::ResolveMaterialAlphaMode(material.alphaMode, transmissionSettings.IsEnabled());
		data.m_renderQueue = alphaModeSettings.m_renderQueue;

		if (alphaModeSettings.m_bAlphaCutout)
		{
			data.m_shaderDefines.Add("ALPHA_CUTOUT");
		}

		data.m_renderState = RHI::RenderState(true,
			alphaModeSettings.m_bEnableZWrite,
			0.0f,
			alphaModeSettings.m_bAlphaCutout,
			material.doubleSided ? RHI::ECullMode::None : RHI::ECullMode::Back,
			alphaModeSettings.m_blendMode,
			RHI::EFillMode::Fill,
			StringHash::Runtime(data.m_renderQueue).GetHash());

		data.m_shader = App::GetSubmodule<AssetRegistry>()->GetOrLoadFile("Shaders/Standard_glTF.shader");
		for (const auto& sampler : data.m_samplers)
		{
			if (sampler.m_second == nullptr || !*sampler.m_second)
			{
				SAILOR_LOG_ERROR(
					"Cannot create generated texture metadata for %s.", assetInfo->GetAssetFilepath().c_str());
				return false;
			}
		}
	}

	std::filesystem::path materialsFolder;
	if (!App::GetSubmodule<AssetRegistry>()->ResolveWorkspaceContentPathForWrite(
			texturesFolder + "materials", materialsFolder))
	{
		SAILOR_LOG_ERROR("Cannot resolve generated materials folder for %s.", assetInfo->GetAssetFilepath().c_str());
		return false;
	}
	std::error_code directoryError;
	std::filesystem::create_directories(materialsFolder, directoryError);
	if (directoryError)
	{
		SAILOR_LOG_ERROR("Cannot create generated materials folder for %s: %s",
			assetInfo->GetAssetFilepath().c_str(),
			directoryError.message().c_str());
		return false;
	}

	TVector<FileId> materialFiles;
	materialFiles.Reserve(materials.Num());
	for (size_t i = 0; i < materials.Num(); ++i)
	{
		const MaterialAsset::Data& material = materials[i];

		const FileId materialFileId = App::GetSubmodule<MaterialImporter>()->CreateMaterialAsset(
			(materialsFolder / (assetInfo->GetAssetFilename() + "_material_" + std::to_string(i) + ".mat")).string(),
			material);
		if (!materialFileId)
		{
			return false;
		}
		materialFiles.Add(materialFileId);
	}

	TVector<FileId> generatedMaterials;
	if (assetInfo->ShouldBatchByMaterial())
	{
		generatedMaterials = std::move(materialFiles);
	}
	else
	{
		for (const tinygltf::Mesh& mesh : gltfModel.meshes)
		{
			for (const tinygltf::Primitive& primitive : mesh.primitives)
			{
				if (primitive.material < 0 || static_cast<size_t>(primitive.material) >= materialFiles.Num())
				{
					SAILOR_LOG_ERROR(
						"Cannot resolve primitive material for %s.", assetInfo->GetAssetFilepath().c_str());
					return false;
				}
				generatedMaterials.Add(materialFiles[primitive.material]);
			}
		}
	}

	assetInfo->GetDefaultMaterials() = std::move(generatedMaterials);
	bool& bMigrationComplete = m_generatedMaterialMigrationComplete.At_Lock(assetInfo->GetFileId(), false);
	bMigrationComplete = true;
	m_generatedMaterialMigrationComplete.Unlock(assetInfo->GetFileId());
	return true;
}

bool ModelImporter::UpdateGeneratedMaterialProperties(ModelAssetInfoPtr assetInfo)
{
	SAILOR_PROFILE_FUNCTION();
	if (assetInfo == nullptr || !assetInfo->IsWritable())
	{
		return false;
	}

	tinygltf::Model gltfModel;
	std::string error;
	std::string warning;
	if (!GltfImporterUtils::LoadModel(assetInfo->GetAssetFilepath(), true, gltfModel, error, warning))
	{
		SAILOR_LOG_ERROR(
			"Cannot update generated materials for %s: %s", assetInfo->GetAssetFilepath().c_str(), error.c_str());
		return false;
	}

	if (!warning.empty())
	{
		SAILOR_LOG("Parsing gltf %s warning: %s", assetInfo->GetAssetFilepath().c_str(), warning.c_str());
	}

	const FileId modelId = assetInfo->GetFileId();
	bool& bMigrationComplete = m_generatedMaterialMigrationComplete.At_Lock(modelId, false);
	const bool bUpdated = UpdateGeneratedMaterialProperties(assetInfo, gltfModel);
	bMigrationComplete = bUpdated;
	m_generatedMaterialMigrationComplete.Unlock(modelId);
	return bUpdated;
}

bool ModelImporter::UpdateGeneratedMaterialPropertiesOnDemand(ModelAssetInfoPtr assetInfo,
	const tinygltf::Model& gltfModel)
{
	if (assetInfo == nullptr || !assetInfo->IsWritable() || !assetInfo->ShouldGenerateMaterials() ||
		assetInfo->GetDefaultMaterials().IsEmpty())
	{
		return true;
	}

	const FileId modelId = assetInfo->GetFileId();
	bool& bMigrationComplete = m_generatedMaterialMigrationComplete.At_Lock(modelId, false);
	if (bMigrationComplete)
	{
		m_generatedMaterialMigrationComplete.Unlock(modelId);
		return true;
	}

	const bool bUpdated = UpdateGeneratedMaterialProperties(assetInfo, gltfModel);
	bMigrationComplete = bUpdated;
	m_generatedMaterialMigrationComplete.Unlock(modelId);
	return bUpdated;
}

bool ModelImporter::UpdateGeneratedMaterialProperties(ModelAssetInfoPtr assetInfo, const tinygltf::Model& gltfModel)
{
	SAILOR_PROFILE_FUNCTION();
	if (assetInfo == nullptr || !assetInfo->IsWritable())
	{
		return false;
	}

	AssetRegistry* assetRegistry = App::GetSubmodule<AssetRegistry>();
	if (assetRegistry == nullptr)
	{
		return false;
	}

	const std::string relativeFolder = Utils::GetFileFolder(assetInfo->GetRelativeAssetFilepath());
	std::filesystem::path materialsFolder;
	if (!assetRegistry->ResolveWorkspaceContentPathForWrite(relativeFolder + "materials", materialsFolder))
	{
		SAILOR_LOG_ERROR("Cannot resolve generated materials folder for %s.", assetInfo->GetAssetFilepath().c_str());
		return false;
	}

	auto sanitizeLegacyMaterialStem = [](const std::string& materialName, size_t materialIndex)
	{
		std::string result = materialName.empty() ? ("material" + std::to_string(materialIndex)) : materialName;
		constexpr const char* InvalidFilenameCharacters = "<>:\"/\\|?*";
		for (char& character : result)
		{
			if (static_cast<unsigned char>(character) < 32 ||
				std::strchr(InvalidFilenameCharacters, character) != nullptr)
			{
				character = '_';
			}
		}
		while (!result.empty() && (result.back() == '.' || result.back() == ' '))
		{
			result.back() = '_';
		}
		return result.empty() ? ("material" + std::to_string(materialIndex)) : result;
	};

	auto findOwnedMaterial = [assetInfo, assetRegistry](size_t materialIndex,
								 const std::filesystem::path& indexedPath,
								 const std::filesystem::path& legacyPath,
								 MaterialAssetInfoPtr& outMaterialInfo)
	{
		auto tryMatch = [assetRegistry, &indexedPath, &legacyPath, &outMaterialInfo](const FileId& materialId)
		{
			MaterialAssetInfoPtr materialInfo = assetRegistry->GetAssetInfoPtr<MaterialAssetInfoPtr>(materialId);
			if (materialInfo == nullptr || !materialInfo->IsWritable())
			{
				return false;
			}

			for (const std::filesystem::path& candidate : {indexedPath, legacyPath})
			{
				std::error_code equivalentError;
				if (std::filesystem::equivalent(candidate, materialInfo->GetAssetFilepath(), equivalentError) &&
					!equivalentError)
				{
					outMaterialInfo = materialInfo;
					return true;
				}
			}
			return false;
		};

		const TVector<FileId>& defaultMaterials = assetInfo->GetDefaultMaterials();
		if (assetInfo->ShouldBatchByMaterial())
		{
			// Batched models retain the direct glTF material ordering. Requiring
			// both the position and a known generated path avoids claiming a
			// separately authored replacement material.
			return materialIndex < defaultMaterials.Num() && tryMatch(defaultMaterials[materialIndex]);
		}

		for (const FileId& materialId : defaultMaterials)
		{
			if (tryMatch(materialId))
			{
				return true;
			}
		}

		return false;
	};

	TVector<FileId> registeredTextureIds;
	assetRegistry->GetAssetInfoIdsByTypeAndSource(
		"Sailor::TextureAssetInfo", assetInfo->GetAssetFilepath(), registeredTextureIds);
	TMap<int32_t, FileId> textureIdsByGltfIndex;
	for (const FileId& registeredTextureId : registeredTextureIds)
	{
		TextureAssetInfoPtr textureInfo = assetRegistry->GetAssetInfoPtr<TextureAssetInfoPtr>(registeredTextureId);
		if (textureInfo == nullptr || textureInfo->GetGlbTextureIndex() < 0 ||
			textureInfo->GetFormat() != RHI::ETextureFormat::R8G8B8A8_UNORM ||
			textureInfo->GetClamping() != RHI::ETextureClamping::Repeat ||
			textureInfo->GetFiltration() != RHI::ETextureFiltration::Linear || !textureInfo->ShouldGenerateMips())
		{
			continue;
		}

		std::error_code sourceError;
		if (std::filesystem::equivalent(textureInfo->GetAssetFilepath(), assetInfo->GetAssetFilepath(), sourceError) &&
			!sourceError)
		{
			textureIdsByGltfIndex.Insert(textureInfo->GetGlbTextureIndex(), registeredTextureId);
		}
	}
	TSet<FileId> updatedMaterialIds;
	bool bSucceeded = true;
	for (size_t materialIndex = 0; materialIndex < gltfModel.materials.size(); ++materialIndex)
	{
		const std::string generatedStem = assetInfo->GetAssetFilename() + "_material_" + std::to_string(materialIndex);
		const std::filesystem::path indexedMaterialPath = materialsFolder / (generatedStem + ".mat");
		const std::string legacyMaterialStem =
			sanitizeLegacyMaterialStem(gltfModel.materials[materialIndex].name, materialIndex);
		const std::filesystem::path legacyMaterialPath = materialsFolder / (legacyMaterialStem + ".mat");
		MaterialAssetInfoPtr materialInfo = nullptr;
		if (!findOwnedMaterial(materialIndex, indexedMaterialPath, legacyMaterialPath, materialInfo))
		{
			// A default material may be replaced with a separately authored asset.
			// Do not infer ownership from its position in the model's material list.
			SAILOR_LOG("Skipped non-generated material while updating %s: %s",
				assetInfo->GetAssetFilepath().c_str(),
				indexedMaterialPath.string().c_str());
			continue;
		}

		const tinygltf::Material& sourceMaterial = gltfModel.materials[materialIndex];
		const auto transmission = GltfImporterUtils::ResolveMaterialTransmission(
			sourceMaterial, gltfModel.textures.size(), assetInfo->GetUnitScale());
		const auto alphaMode =
			GltfImporterUtils::ResolveMaterialAlphaMode(sourceMaterial.alphaMode, transmission.IsEnabled());
		YAML::Node generatedProperties(YAML::NodeType::Map);
		generatedProperties["renderQueue"] = alphaMode.m_renderQueue;
		generatedProperties["bEnableZWrite"] = alphaMode.m_bEnableZWrite;
		generatedProperties["bCustomDepthShader"] = alphaMode.m_bAlphaCutout;
		::Serialize(generatedProperties, "blendMode", alphaMode.m_blendMode);

		YAML::Node generatedDefines(YAML::NodeType::Sequence);
		if (GltfImporterUtils::IsMaterialUsedBySkinnedMesh(gltfModel, materialIndex))
		{
			generatedDefines.push_back("SKINNING");
		}
		if (transmission.IsEnabled())
		{
			generatedDefines.push_back("TRANSMISSION");
		}
		else if (transmission.m_bHasIndexOfRefraction)
		{
			generatedDefines.push_back("MATERIAL_IOR");
		}
		if (alphaMode.m_bAlphaCutout)
		{
			generatedDefines.push_back("ALPHA_CUTOUT");
		}
		generatedProperties["defines"] = generatedDefines;
		generatedProperties["uniformsFloat"]["material.alphaCutoff"] = static_cast<float>(sourceMaterial.alphaCutoff);
		generatedProperties["uniformsVec4"]["material.emissiveFactor"] =
			glm::vec4(GltfImporterUtils::ResolveMaterialEmissiveFactor(sourceMaterial), 0.0f);

		if (transmission.IsEnabled())
		{
			generatedProperties["uniformsFloat"]["material.transmissionFactor"] = transmission.m_factor;
			generatedProperties["uniformsFloat"]["material.thicknessFactor"] = transmission.m_thicknessFactor;
			generatedProperties["uniformsFloat"]["material.attenuationDistance"] = transmission.m_attenuationDistance;
			generatedProperties["uniformsVec4"]["material.attenuationColor"] =
				glm::vec4(transmission.m_attenuationColor, 1.0f);

			auto addGeneratedSampler = [assetInfo,
										   assetRegistry,
										   materialIndex,
										   &relativeFolder,
										   &generatedProperties,
										   &textureIdsByGltfIndex,
										   &bSucceeded](
										   const char* samplerName, const char* assetSuffix, int32_t textureIndex)
			{
				if (textureIndex < 0)
				{
					return;
				}

				const FileId* registeredTextureId = nullptr;
				FileId textureFileId =
					textureIdsByGltfIndex.Find(textureIndex, registeredTextureId) && registeredTextureId != nullptr
						? *registeredTextureId
						: FileId();

				if (!textureFileId)
				{
					std::filesystem::path generatedTexturePath;
					const std::string generatedTextureVirtualPath = relativeFolder + assetInfo->GetAssetFilename() +
																	"_material_" + std::to_string(materialIndex) + "_" +
																	assetSuffix + ".png.asset";
					if (!assetRegistry->ResolveWorkspaceContentPathForWrite(
							generatedTextureVirtualPath, generatedTexturePath))
					{
						SAILOR_LOG_ERROR("Cannot resolve generated glTF %s for %s.",
							samplerName,
							assetInfo->GetAssetFilepath().c_str());
						bSucceeded = false;
						return;
					}

					textureFileId = ModelImporter::CreateTextureAsset(generatedTexturePath.string(),
						assetInfo->GetAssetFilename(),
						static_cast<uint32_t>(textureIndex),
						true,
						RHI::ETextureFormat::R8G8B8A8_UNORM,
						RHI::ETextureClamping::Repeat,
						RHI::ETextureFiltration::Linear,
						assetInfo->ShouldKeepCpuBuffers());
					if (!textureFileId)
					{
						SAILOR_LOG_ERROR("Cannot create generated glTF %s for %s.",
							samplerName,
							assetInfo->GetAssetFilepath().c_str());
						bSucceeded = false;
						return;
					}

					textureIdsByGltfIndex.Insert(textureIndex, textureFileId);
				}

				generatedProperties["samplers"][samplerName] = textureFileId;
			};

			addGeneratedSampler("transmissionSampler", "transmissionTexture", transmission.m_textureIndex);
			addGeneratedSampler("thicknessSampler", "thicknessTexture", transmission.m_thicknessTextureIndex);
		}
		if (transmission.IsEnabled() || transmission.m_bHasIndexOfRefraction)
		{
			generatedProperties["uniformsFloat"]["material.indexOfRefraction"] = transmission.m_indexOfRefraction;
		}

		YAML::Node materialDocument;
		std::string diagnostic;
		if (!TryLoadYamlFile(materialInfo->GetAssetFilepath(), materialDocument, diagnostic))
		{
			SAILOR_LOG_ERROR("Cannot read generated material '%s': %s",
				materialInfo->GetAssetFilepath().c_str(),
				diagnostic.c_str());
			bSucceeded = false;
			continue;
		}

		const YAML::Node previousDocument = YAML::Clone(materialDocument);
		bool bMerged = false;
		const bool bYamlHandled = External::GuardYamlExceptions([&materialDocument, &generatedProperties, &bMerged]()
			{ bMerged = GltfImporterUtils::MergeGeneratedMaterialProperties(materialDocument, generatedProperties); },
			diagnostic);
		if (!bYamlHandled || !bMerged)
		{
			if (diagnostic.empty())
			{
				diagnostic = "material YAML has an incompatible structure";
			}
			SAILOR_LOG_ERROR("Cannot migrate generated material '%s': %s",
				materialInfo->GetAssetFilepath().c_str(),
				diagnostic.c_str());
			bSucceeded = false;
			continue;
		}

		if (Utils::AreYamlNodesEqual(previousDocument, materialDocument))
		{
			continue;
		}

		std::string serializedMaterial;
		if (!External::TryDumpYaml(materialDocument, serializedMaterial, diagnostic) ||
			!Workspace::AtomicReplaceWorkspaceCacheText(
				materialInfo->GetAssetFilepath(), serializedMaterial, diagnostic))
		{
			SAILOR_LOG_ERROR(
				"Cannot save migrated material '%s': %s", materialInfo->GetAssetFilepath().c_str(), diagnostic.c_str());
			bSucceeded = false;
			continue;
		}

		updatedMaterialIds.Insert(materialInfo->GetFileId());
	}

	for (const FileId& materialId : updatedMaterialIds)
	{
		if (!assetRegistry->UpdateAsset(materialId))
		{
			SAILOR_LOG_ERROR("Cannot reload migrated generated material: %s", materialId.ToString().c_str());
			bSucceeded = false;
		}
	}

	return bSucceeded;
}
