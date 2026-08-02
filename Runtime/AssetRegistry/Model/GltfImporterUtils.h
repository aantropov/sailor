#pragma once

#include "Containers/Vector.h"
#include "Core/Defines.h"

#include <glm/mat4x4.hpp>

#include <cstdint>
#include <string>

namespace tinygltf
{
	class Model;
	class Node;
}

namespace Sailor::GltfImporterUtils
{
	struct MeshInstance
	{
		int32_t m_nodeIndex = -1;
		int32_t m_meshIndex = -1;
		int32_t m_skinIndex = -1;
		glm::mat4 m_worldTransform{ 1.0f };
	};

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
}
