#pragma once

#include "Containers/Vector.h"
#include "Core/Defines.h"
#include "Math/Transform.h"
#include "RHI/Types.h"

#include <glm/mat4x4.hpp>
#include <glm/mat3x3.hpp>
#include <glm/vec3.hpp>

#include <yaml-cpp/yaml.h>

#include <cstddef>
#include <cstdint>
#include <limits>
#include <string>

namespace tinygltf
{
	class Material;
	class Model;
	class Node;
}

namespace Sailor::GltfImporterUtils
{
	struct MaterialAlphaModeSettings
	{
		const char* m_renderQueue = "Opaque";
		bool m_bEnableZWrite = true;
		bool m_bAlphaCutout = false;
		RHI::EBlendMode m_blendMode = RHI::EBlendMode::None;
	};

	struct MaterialTransmissionSettings
	{
		float m_factor = 0.0f;
		int32_t m_textureIndex = -1;
		float m_thicknessFactor = 0.0f;
		int32_t m_thicknessTextureIndex = -1;
		glm::vec3 m_attenuationColor = glm::vec3(1.0f);
		float m_attenuationDistance = (std::numeric_limits<float>::max)();
		float m_indexOfRefraction = 1.5f;

		bool IsEnabled() const { return m_factor > 0.0f; }
	};

	struct MeshInstance
	{
		int32_t m_nodeIndex = -1;
		int32_t m_meshIndex = -1;
		int32_t m_skinIndex = -1;
		glm::mat4 m_worldTransform{ 1.0f };
	};

	struct MeshInstanceTransforms
	{
		glm::mat4 m_geometryTransform{ 1.0f };
		glm::mat3 m_directionTransform{ 1.0f };
		glm::vec3 m_bakedVolumeScale{ 1.0f };
	};

	struct SceneNode
	{
		std::string m_name;
		int32_t m_sourceNodeIndex = -1;
		int32_t m_parentIndex = -1;
		int32_t m_meshIndex = -1;
		int32_t m_skinIndex = -1;
		Math::Transform m_localTransform{};
		glm::mat4 m_localMatrix{ 1.0f };
		glm::mat4 m_worldMatrix{ 1.0f };
		bool m_bTransformDecomposable = true;
	};

	SAILOR_SHARED_API MeshInstanceTransforms ResolveMeshInstanceTransforms(
		const MeshInstance& instance,
		float unitScale);

	SAILOR_SHARED_API MaterialAlphaModeSettings ResolveMaterialAlphaMode(
		const std::string& alphaMode,
		bool bHasTransmission = false);

	SAILOR_SHARED_API MaterialTransmissionSettings ResolveMaterialTransmission(
		const tinygltf::Material& material,
		size_t numTextures,
		float unitScale = 1.0f);

	// Updates only the material properties owned by the glTF alpha/transmission
	// import path. All other authored material properties remain untouched.
	SAILOR_SHARED_API bool MergeGeneratedMaterialProperties(
		YAML::Node& inOutMaterial,
		const YAML::Node& generatedProperties);

	SAILOR_SHARED_API bool LoadModel(
		const std::string& assetFilepath,
		bool bImagesAsIs,
		tinygltf::Model& outModel,
		std::string& outError,
		std::string& outWarning);

	SAILOR_SHARED_API bool TryComposeNodeMatrix(
		const tinygltf::Node& node,
		glm::mat4& outMatrix);

	SAILOR_SHARED_API bool CollectMeshInstances(
		const tinygltf::Model& model,
		TVector<MeshInstance>& outInstances);

	SAILOR_SHARED_API bool CollectSceneNodes(
		const tinygltf::Model& model,
		float unitScale,
		TVector<SceneNode>& outNodes);
}
