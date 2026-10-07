#include "AssetRegistry/Model/ModelImporter.h"
#include "Platform/AtomicFile.h"

#include "AssetRegistry/AssetRegistry.h"
#include "AssetRegistry/Material/MaterialImporter.h"
#include "AssetRegistry/Model/GeneratedModelAssetMetadata.h"
#include "AssetRegistry/Model/GltfImporterUtils.h"
#include "AssetRegistry/Texture/TextureImporter.h"
#include "Core/Utils.h"
#include "Core/YamlUtils.h"
#include "YamlExceptionBoundary.h"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <limits>
#include <string>
#include <string_view>

#include <tiny_gltf.h>

using namespace Sailor;
using namespace Sailor::Workspace;

static bool TryLoadYamlFile(const std::filesystem::path& filepath, YAML::Node& outDocument, std::string& outDiagnostic)
{
	std::string payload;
	if (!AssetRegistry::ReadAllTextFile(PathToUtf8(filepath), payload))
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
	AssetRegistry* assetRegistry = m_assetRegistry;
	const auto metadataPath = PathFromUtf8(filepath);

	std::error_code statusError;
	const std::filesystem::file_status metadataStatus = std::filesystem::symlink_status(metadataPath, statusError);
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
	if (!bMetadataExists)
	{
		TVector<FileId> textures;
		assetRegistry->GetAssetInfoIdsByTypeAndSource("Sailor::TextureAssetInfo",
			PathToUtf8(metadataPath.parent_path() / PathFromUtf8(sourceFilename)), textures);
		for (const auto& id : textures)
		{
			const auto* texture = assetRegistry->GetAssetInfoPtr<TextureAssetInfoPtr>(id);
			if (texture && texture->GetGlbTextureIndex() == static_cast<int32_t>(sourceTextureIndex) &&
				texture->ShouldGenerateMips() == bShouldGenerateMips && texture->GetFormat() == format &&
				texture->GetClamping() == clamping && texture->GetFiltration() == filtration &&
				texture->ShouldKeepCpuBuffers() == bShouldKeepCpuBuffers &&
				std::filesystem::is_regular_file(PathFromUtf8(texture->GetMetaFilepath())))
			{
				return id;
			}
		}
	}
	if (bMetadataExists && !std::filesystem::is_regular_file(metadataStatus))
	{
		SAILOR_LOG_ERROR("Generated texture metadata path is not a regular file: %s", filepath.c_str());
		return FileId::Invalid;
	}

	FileId fileId =
		bMetadataExists ? assetRegistry->RegisterGeneratedSecondaryAssetInfo(metadataPath) : FileId::CreateNewFileId();
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
	if (!Platform::IsAtomicWriteComplete(Platform::AtomicWriteFile(metadataPath, serialized.str(), diagnostic)))
	{
		SAILOR_LOG_ERROR("Cannot save generated texture metadata '%s': %s", filepath.c_str(), diagnostic.c_str());
		return {};
	}

	if (assetRegistry->RegisterGeneratedSecondaryAssetInfo(metadataPath) != fileId)
	{
		SAILOR_LOG_ERROR(
			"Cannot register generated texture metadata for immediate model processing: %s", filepath.c_str());
		return FileId::Invalid;
	}

	return fileId;
}

bool GltfImporterUtils::MergeGeneratedMaterialProperties(YAML::Node& inOutMaterial,
	const YAML::Node& generatedProperties)
{
	if (!inOutMaterial.IsMap() || !generatedProperties.IsMap())
	{
		return false;
	}

	YAML::Node merged = YAML::Clone(inOutMaterial);
	for (const char* property : {"renderQueue", "bEnableZWrite", "blendMode"})
	{
		if (!generatedProperties[property] || !generatedProperties[property].IsScalar())
		{
			return false;
		}
		merged[property] = YAML::Clone(generatedProperties[property]);
	}

	const YAML::Node generatedCustomDepth = generatedProperties["bCustomDepthShader"];
	if (!generatedCustomDepth || !generatedCustomDepth.IsScalar())
	{
		return false;
	}

	bool bCustomDepthShader = generatedCustomDepth.as<bool>();
	const YAML::Node authoredCustomDepth = merged["bCustomDepthShader"];
	if (authoredCustomDepth)
	{
		if (!authoredCustomDepth.IsScalar())
		{
			return false;
		}
		bCustomDepthShader |= authoredCustomDepth.as<bool>();
	}
	merged["bCustomDepthShader"] = bCustomDepthShader;

	// Reimport owns alpha/skinning and optical properties. Surface authoring stays local.
	static constexpr std::string_view ManagedDefines[] = { "TRANSMISSION", "MATERIAL_IOR", "ALPHA_CUTOUT", "SKINNING" };
	auto isManagedDefine = [](std::string_view define)
	{
		return std::find(std::begin(ManagedDefines), std::end(ManagedDefines), define) != std::end(ManagedDefines);
	};

	YAML::Node mergedDefines(YAML::NodeType::Sequence);
	const YAML::Node existingDefines = merged["defines"];
	if (existingDefines && !existingDefines.IsNull())
	{
		if (!existingDefines.IsSequence())
		{
			return false;
		}

		for (const YAML::Node& defineNode : existingDefines)
		{
			if (!defineNode.IsScalar())
			{
				return false;
			}

			const auto& define = defineNode.Scalar();
			if (!isManagedDefine(define))
			{
				mergedDefines.push_back(define);
			}
		}
	}

	const YAML::Node generatedDefines = generatedProperties["defines"];
	if (generatedDefines && !generatedDefines.IsNull())
	{
		if (!generatedDefines.IsSequence())
		{
			return false;
		}

		TSet<std::string_view> addedDefines;
		for (const YAML::Node& defineNode : generatedDefines)
		{
			if (!defineNode.IsScalar())
			{
				return false;
			}
			const auto& define = defineNode.Scalar();
			if (isManagedDefine(define) && addedDefines.Insert(define))
			{
				mergedDefines.push_back(define);
			}
		}
	}
	merged["defines"] = mergedDefines.size() > 0 ? mergedDefines : YAML::Node();

	struct ManagedPropertyGroup final
	{
		const char* m_group;
		const char* const* m_properties;
		size_t m_numProperties;
	};

	static const char* FloatProperties[] = {"material.alphaCutoff",
		"material.transmissionFactor",
		"material.thicknessFactor",
		"material.attenuationDistance",
		"material.indexOfRefraction"};
	static const char* Vec4Properties[] = {"material.attenuationColor", "material.emissiveFactor"};
	static const char* SamplerProperties[] = {"transmissionSampler", "thicknessSampler"};
	const ManagedPropertyGroup groups[] = {{"uniformsFloat", FloatProperties, std::size(FloatProperties)},
		{"uniformsVec4", Vec4Properties, std::size(Vec4Properties)},
		{"samplers", SamplerProperties, std::size(SamplerProperties)}};

	for (const ManagedPropertyGroup& group : groups)
	{
		YAML::Node targetGroup = merged[group.m_group];
		const YAML::Node generatedGroup = generatedProperties[group.m_group];
		if ((targetGroup && !targetGroup.IsNull() && !targetGroup.IsMap()) ||
			(generatedGroup && !generatedGroup.IsNull() && !generatedGroup.IsMap()))
		{
			return false;
		}

		for (size_t index = 0; index < group.m_numProperties; ++index)
		{
			const char* property = group.m_properties[index];
			if (generatedGroup && generatedGroup[property])
			{
				if (!targetGroup || targetGroup.IsNull())
				{
					targetGroup = YAML::Node(YAML::NodeType::Map);
					merged[group.m_group] = targetGroup;
				}
				targetGroup[property] = YAML::Clone(generatedGroup[property]);
			}
			else if (targetGroup && targetGroup.IsMap())
			{
				targetGroup.remove(property);
			}
		}
	}

	inOutMaterial = std::move(merged);
	return true;
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
		if (!m_assetRegistry->ResolveWorkspaceContentPathForWrite(
				texturesFolder + assetInfo->GetAssetFilename() + "_material_" + std::to_string(i), materialNamePath))
		{
			SAILOR_LOG_ERROR("Cannot resolve generated material output for %s.", assetInfo->GetAssetFilepath().c_str());
			return false;
		}
		const std::string materialName = PathToUtf8(materialNamePath);

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
			HashString(data.m_renderQueue));

		data.m_shader = m_assetRegistry->GetOrLoadFile("Shaders/Standard_glTF.shader");
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
	if (!m_assetRegistry->ResolveWorkspaceContentPathForWrite(
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

	TVector<FileId> registeredMaterials;
	m_assetRegistry->GetAllAssetInfos<MaterialAssetInfo>(registeredMaterials);
	TMap<int32_t, MaterialAssetInfoPtr> ownedMaterials;
	for (const FileId& id : registeredMaterials)
	{
		auto* info = m_assetRegistry->GetAssetInfoPtr<MaterialAssetInfoPtr>(id);
		if (info == nullptr || info->GetSourceModel() != assetInfo->GetFileId() || info->GetSourceMaterialIndex() < 0)
		{
			continue;
		}
		if (ownedMaterials.ContainsKey(info->GetSourceMaterialIndex()))
		{
			SAILOR_LOG_ERROR("Multiple materials claim glTF material %d of %s.",
				info->GetSourceMaterialIndex(), assetInfo->GetAssetFilepath().c_str());
			return false;
		}
		ownedMaterials.Insert(info->GetSourceMaterialIndex(), info);
	}

	TVector<FileId> materialFiles;
	materialFiles.Reserve(materials.Num());
	for (size_t i = 0; i < materials.Num(); ++i)
	{
		const auto owned = ownedMaterials.Find(static_cast<int32_t>(i));
		MaterialAssetInfoPtr info = owned != ownedMaterials.end() ? owned.Value() : nullptr;
		std::filesystem::path materialPath;
		YAML::Node metadata;
		std::string diagnostic;
		FileId fileId;
		if (info != nullptr)
		{
			if (!info->IsWritable() || !m_assetRegistry->ResolveWorkspaceContentPathForWrite(
				info->GetVirtualAssetFilepath(), materialPath))
			{
				SAILOR_LOG_ERROR("Cannot reimport a read-only generated material: %s", info->GetAssetFilepath().c_str());
				return false;
			}
			fileId = info->GetFileId();
			metadata = info->Serialize();
		}
		else
		{
			const std::string stem = assetInfo->GetAssetFilename() + "_material_" + std::to_string(i);
			for (uint32_t suffix = 0;; ++suffix)
			{
				materialPath = materialsFolder / PathFromUtf8(stem + (suffix ? "_" + std::to_string(suffix) : "") + ".mat");
				auto metadataPath = materialPath;
				metadataPath += ".asset";
				if (std::filesystem::exists(metadataPath))
				{
					MaterialAssetInfo candidate;
					if (!TryLoadYamlFile(metadataPath, metadata, diagnostic) ||
						!External::GuardYamlExceptions([&]() { candidate.Deserialize(metadata); }, diagnostic))
					{
						SAILOR_LOG_ERROR("Cannot read generated material metadata '%s': %s", PathToUtf8(metadataPath).c_str(), diagnostic.c_str());
						return false;
					}
					if (candidate.GetSourceModel() == assetInfo->GetFileId() &&
						candidate.GetSourceMaterialIndex() == static_cast<int32_t>(i))
					{
						fileId = candidate.GetFileId();
						break;
					}
				}
				else if (!std::filesystem::exists(materialPath))
				{
					fileId = FileId::CreateNewFileId();
					metadata = CreateAssetInfoMetadata<MaterialAssetInfo>(fileId, PathToUtf8(materialPath.filename()));
					metadata["sourceModel"] = assetInfo->GetFileId();
					metadata["sourceMaterialIndex"] = static_cast<int32_t>(i);
					break;
				}
			}
		}

		auto metadataPath = materialPath;
		metadataPath += ".asset";
		const bool bMetadataExists = std::filesystem::exists(metadataPath);
		if (bMetadataExists && !TryLoadYamlFile(metadataPath, metadata, diagnostic))
		{
			SAILOR_LOG_ERROR("Cannot read generated material metadata '%s': %s", PathToUtf8(metadataPath).c_str(), diagnostic.c_str());
			return false;
		}
		MaterialAssetInfo identity;
		if (!External::GuardYamlExceptions([&]() { identity.Deserialize(metadata); }, diagnostic) ||
			!fileId || identity.GetFileId() != fileId || identity.GetSourceModel() != assetInfo->GetFileId() ||
			identity.GetSourceMaterialIndex() != static_cast<int32_t>(i) ||
			identity.GetAssetFilename() != PathToUtf8(materialPath.filename()))
		{
			SAILOR_LOG_ERROR("Generated material ownership changed: %s", PathToUtf8(metadataPath).c_str());
			return false;
		}

		const YAML::Node generated = MaterialAsset::Serialize(materials[i]);
		YAML::Node document;
		bool bWriteMaterial = true;
		if (std::filesystem::exists(materialPath))
		{
			YAML::Node previous;
			bool merged = false;
			if (!TryLoadYamlFile(materialPath, previous, diagnostic) ||
				!External::GuardYamlExceptions([&]()
					{
						document = YAML::Clone(previous);
						merged = GltfImporterUtils::MergeGeneratedMaterialProperties(document, generated);
					}, diagnostic) || !merged)
			{
				SAILOR_LOG_ERROR("Cannot update generated material '%s': %s", PathToUtf8(materialPath).c_str(), diagnostic.c_str());
				return false;
			}
			bWriteMaterial = !Utils::AreYamlNodesEqual(previous, document);
		}
		else
		{
			document = generated;
		}

		// Persist ownership first so a failed material write can retry with the same FileId.
		if (!bMetadataExists)
		{
			std::string contents;
			if (!External::TryDumpYaml(metadata, contents, diagnostic) ||
				!Platform::IsAtomicWriteComplete(Platform::AtomicWriteFile(
					metadataPath, contents, diagnostic, Platform::EAtomicWriteMode::FailIfExists)))
			{
				SAILOR_LOG_ERROR("Cannot save generated material metadata '%s': %s", PathToUtf8(metadataPath).c_str(), diagnostic.c_str());
				return false;
			}
		}
		if (bWriteMaterial)
		{
			std::string contents;
			if (!External::TryDumpYaml(document, contents, diagnostic) ||
				!Platform::IsAtomicWriteComplete(Platform::AtomicWriteFile(materialPath, contents, diagnostic)))
			{
				SAILOR_LOG_ERROR("Cannot save generated material '%s': %s", PathToUtf8(materialPath).c_str(), diagnostic.c_str());
				return false;
			}
		}
		// Keep standalone/Main error handling synchronous. Off-Main engine
		// callers already requested publication through GetOrLoadFile.
		if (m_assetRegistry->GetOrLoadFile(PathToUtf8(materialPath)) != fileId ||
			((!m_scheduler || m_scheduler->IsMainThread()) && !m_assetRegistry->UpdateAsset(fileId)))
		{
			return false;
		}
		materialFiles.Add(fileId);
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

	const auto& authoredMaterials = assetInfo->GetDefaultMaterials();
	for (size_t slot = 0; slot < generatedMaterials.Num() && slot < authoredMaterials.Num(); ++slot)
	{
		if (authoredMaterials[slot] && m_assetRegistry->GetAssetInfoPtr(authoredMaterials[slot]))
		{
			generatedMaterials[slot] = authoredMaterials[slot];
		}
	}
	assetInfo->GetDefaultMaterials() = std::move(generatedMaterials);
	return true;
}
