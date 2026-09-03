#include "AssetRegistry/Model/GltfImporterUtils.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <string>
#include <utility>

#include <glm/gtc/matrix_transform.hpp>
#include <tiny_gltf.h>

using namespace Sailor;

GltfImporterUtils::MeshInstanceTransforms GltfImporterUtils::ResolveMeshInstanceTransforms(const MeshInstance& instance,
	float unitScale)
{
	const glm::mat4 sourceTransform = instance.m_skinIndex >= 0 ? glm::mat4(1.0f) : instance.m_worldTransform;
	const float directionScale = unitScale < 0.0f ? -1.0f : 1.0f;

	MeshInstanceTransforms result;
	result.m_geometryTransform = glm::scale(glm::mat4(1.0f), glm::vec3(unitScale)) * sourceTransform;
	result.m_directionTransform = glm::mat3(glm::scale(glm::mat4(1.0f), glm::vec3(directionScale)) * sourceTransform);
	const glm::mat3 sourceLinear(sourceTransform);
	result.m_bakedVolumeScale =
		glm::vec3(glm::length(sourceLinear[0]), glm::length(sourceLinear[1]), glm::length(sourceLinear[2]));
	return result;
}

GltfImporterUtils::MaterialAlphaModeSettings GltfImporterUtils::ResolveMaterialAlphaMode(const std::string& alphaMode,
	bool bHasTransmission)
{
	if (bHasTransmission)
	{
		return {"Transparent",
			false,
			alphaMode == "MASK",
			alphaMode == "BLEND" ? RHI::EBlendMode::AlphaBlending : RHI::EBlendMode::None};
	}

	if (alphaMode == "BLEND")
	{
		return {"Transparent", false, false, RHI::EBlendMode::AlphaBlending};
	}

	if (alphaMode == "MASK")
	{
		return {"Masked", true, true, RHI::EBlendMode::None};
	}

	return {};
}

GltfImporterUtils::MaterialTransmissionSettings GltfImporterUtils::ResolveMaterialTransmission(
	const tinygltf::Material& material,
	size_t numTextures,
	float unitScale)
{
	constexpr float InfiniteIndexOfRefraction = 1000000.0f;
	MaterialTransmissionSettings result;
	auto tryReadFiniteNumber = [](const tinygltf::Value& object, const char* property, double& outValue)
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

	auto tryReadTextureIndex = [numTextures](const tinygltf::Value& object, const char* property, int32_t& outIndex)
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
		if (index < 0 || static_cast<size_t>(index) >= numTextures)
		{
			return false;
		}

		outIndex = index;
		return true;
	};

	double parsedValue = 0.0;
	const auto iorIt = material.extensions.find("KHR_materials_ior");
	if (iorIt != material.extensions.end() && tryReadFiniteNumber(iorIt->second, "ior", parsedValue) &&
		(parsedValue == 0.0 || parsedValue >= 1.0))
	{
		result.m_bHasIndexOfRefraction = true;
		result.m_indexOfRefraction = parsedValue == 0.0 ? InfiniteIndexOfRefraction : static_cast<float>(parsedValue);
	}

	const auto extensionIt = material.extensions.find("KHR_materials_transmission");
	if (extensionIt == material.extensions.end() || !extensionIt->second.IsObject())
	{
		return result;
	}

	const tinygltf::Value& extension = extensionIt->second;
	if (tryReadFiniteNumber(extension, "transmissionFactor", parsedValue))
	{
		result.m_factor = static_cast<float>(std::clamp(parsedValue, 0.0, 1.0));
	}

	tryReadTextureIndex(extension, "transmissionTexture", result.m_textureIndex);

	const auto volumeIt = material.extensions.find("KHR_materials_volume");
	if (volumeIt != material.extensions.end() && volumeIt->second.IsObject())
	{
		const tinygltf::Value& volume = volumeIt->second;
		if (tryReadFiniteNumber(volume, "thicknessFactor", parsedValue))
		{
			result.m_thicknessFactor = static_cast<float>((std::max)(0.0, parsedValue));
		}

		tryReadTextureIndex(volume, "thicknessTexture", result.m_thicknessTextureIndex);

		if (tryReadFiniteNumber(volume, "attenuationDistance", parsedValue) && parsedValue > 0.0)
		{
			result.m_attenuationDistance = static_cast<float>(parsedValue);
		}

		if (volume.Has("attenuationColor"))
		{
			const tinygltf::Value& color = volume.Get("attenuationColor");
			if (color.IsArray() && color.ArrayLen() >= 3)
			{
				glm::vec3 parsedColor(1.0f);
				bool bValidColor = true;
				for (size_t component = 0; component < 3; ++component)
				{
					const tinygltf::Value& value = color.Get(component);
					if (!value.IsNumber() || !std::isfinite(value.GetNumberAsDouble()))
					{
						bValidColor = false;
						break;
					}

					parsedColor[static_cast<int32_t>(component)] =
						static_cast<float>(std::clamp(value.GetNumberAsDouble(), 0.0, 1.0));
				}

				if (bValidColor)
				{
					result.m_attenuationColor = parsedColor;
				}
			}
		}
	}

	const double lengthScale = std::isfinite(unitScale) ? std::abs(static_cast<double>(unitScale)) : 1.0;
	auto scaleLength = [lengthScale](float value)
	{
		return static_cast<float>((std::min)(static_cast<double>((std::numeric_limits<float>::max)()),
			static_cast<double>(value) * lengthScale));
	};

	result.m_thicknessFactor = scaleLength(result.m_thicknessFactor);
	if (result.m_attenuationDistance < (std::numeric_limits<float>::max)())
	{
		result.m_attenuationDistance = scaleLength(result.m_attenuationDistance);
	}

	return result;
}

glm::vec3 GltfImporterUtils::ResolveMaterialEmissiveFactor(const tinygltf::Material& material)
{
	double emissiveStrength = 1.0;
	const auto extensionIt = material.extensions.find("KHR_materials_emissive_strength");
	if (extensionIt != material.extensions.end() && extensionIt->second.IsObject() &&
		extensionIt->second.Has("emissiveStrength"))
	{
		const tinygltf::Value& value = extensionIt->second.Get("emissiveStrength");
		if (value.IsNumber())
		{
			const double parsedStrength = value.GetNumberAsDouble();
			if (std::isfinite(parsedStrength) && parsedStrength >= 0.0)
			{
				emissiveStrength = parsedStrength;
			}
		}
	}

	glm::vec3 result(0.0f);
	for (glm::length_t component = 0; component < 3; ++component)
	{
		const double radiance = material.emissiveFactor[component] * emissiveStrength;
		result[component] = static_cast<float>(std::clamp(
			std::isfinite(radiance) ? radiance : 0.0, 0.0, static_cast<double>((std::numeric_limits<float>::max)())));
	}
	return result;
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

	auto isManagedDefine = [](const std::string& define)
	{
		return define == "TRANSMISSION" || define == "MATERIAL_IOR" || define == "ALPHA_CUTOUT" || define == "SKINNING";
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

			const std::string define = defineNode.as<std::string>();
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

		bool bHasTransmission = false;
		bool bHasMaterialIor = false;
		bool bHasAlphaCutout = false;
		bool bHasSkinning = false;
		for (const YAML::Node& defineNode : generatedDefines)
		{
			if (!defineNode.IsScalar())
			{
				return false;
			}

			const std::string define = defineNode.as<std::string>();
			if (define == "TRANSMISSION" && !bHasTransmission)
			{
				mergedDefines.push_back(define);
				bHasTransmission = true;
			}
			else if (define == "MATERIAL_IOR" && !bHasMaterialIor)
			{
				mergedDefines.push_back(define);
				bHasMaterialIor = true;
			}
			else if (define == "ALPHA_CUTOUT" && !bHasAlphaCutout)
			{
				mergedDefines.push_back(define);
				bHasAlphaCutout = true;
			}
			else if (define == "SKINNING" && !bHasSkinning)
			{
				mergedDefines.push_back(define);
				bHasSkinning = true;
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

		if (!targetGroup || targetGroup.IsNull())
		{
			targetGroup = YAML::Node(YAML::NodeType::Map);
			merged[group.m_group] = targetGroup;
		}

		for (size_t index = 0; index < group.m_numProperties; ++index)
		{
			const char* property = group.m_properties[index];
			if (generatedGroup && generatedGroup[property])
			{
				targetGroup[property] = YAML::Clone(generatedGroup[property]);
			}
			else
			{
				targetGroup.remove(property);
			}
		}
	}

	inOutMaterial = std::move(merged);
	return true;
}
