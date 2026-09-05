#include "AssetRegistry/Model/GltfImporterUtils.h"

#include "Math/Math.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <string>
#include <utility>

#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>
#include <tiny_gltf.h>

using namespace Sailor;

bool GltfImporterUtils::TryComposeNodeMatrix(const tinygltf::Node& node, glm::mat4& outMatrix)
{
	outMatrix = glm::mat4(1.0f);
	auto tryConvert = [](double value, float& outValue)
	{
		if (!std::isfinite(value) || value > std::numeric_limits<float>::max() ||
			value < -std::numeric_limits<float>::max())
		{
			return false;
		}

		outValue = static_cast<float>(value);
		return true;
	};
	if (!node.matrix.empty())
	{
		if (node.matrix.size() != 16)
		{
			return false;
		}

		for (int32_t column = 0; column < 4; ++column)
		{
			for (int32_t row = 0; row < 4; ++row)
			{
				if (!tryConvert(node.matrix[static_cast<size_t>(column * 4 + row)], outMatrix[column][row]))
				{
					return false;
				}
			}
		}

		return Math::AllFinite(outMatrix);
	}

	if ((!node.translation.empty() && node.translation.size() != 3) ||
		(!node.rotation.empty() && node.rotation.size() != 4) || (!node.scale.empty() && node.scale.size() != 3))
	{
		return false;
	}

	glm::vec3 translation(0.0f);
	glm::vec3 scale(1.0f);
	glm::quat rotation(1.0f, 0.0f, 0.0f, 0.0f);
	for (int32_t component = 0; component < 3; ++component)
	{
		if ((!node.translation.empty() &&
				!tryConvert(node.translation[static_cast<size_t>(component)], translation[component])) ||
			(!node.scale.empty() && !tryConvert(node.scale[static_cast<size_t>(component)], scale[component])))
		{
			return false;
		}
	}

	if (!node.rotation.empty())
	{
		if (!tryConvert(node.rotation[3], rotation.w) || !tryConvert(node.rotation[0], rotation.x) ||
			!tryConvert(node.rotation[1], rotation.y) || !tryConvert(node.rotation[2], rotation.z))
		{
			return false;
		}

		const float lengthSquared = glm::dot(rotation, rotation);
		if (!std::isfinite(lengthSquared) || lengthSquared <= std::numeric_limits<float>::epsilon())
		{
			return false;
		}
		rotation *= glm::inversesqrt(lengthSquared);
	}

	outMatrix =
		glm::translate(glm::mat4(1.0f), translation) * glm::mat4_cast(rotation) * glm::scale(glm::mat4(1.0f), scale);
	return Math::AllFinite(outMatrix);
}

bool GltfImporterUtils::IsMaterialUsedBySkinnedMesh(const tinygltf::Model& model, size_t materialIndex)
{
	for (const tinygltf::Node& node : model.nodes)
	{
		if (node.skin < 0 || node.mesh < 0 || static_cast<size_t>(node.mesh) >= model.meshes.size())
		{
			continue;
		}

		for (const tinygltf::Primitive& primitive : model.meshes[static_cast<size_t>(node.mesh)].primitives)
		{
			if (primitive.material >= 0 && static_cast<size_t>(primitive.material) == materialIndex &&
				primitive.attributes.find("JOINTS_0") != primitive.attributes.end() &&
				primitive.attributes.find("WEIGHTS_0") != primitive.attributes.end())
			{
				return true;
			}
		}
	}

	return false;
}

bool GltfImporterUtils::CollectSceneNodes(const tinygltf::Model& model, float unitScale, TVector<SceneNode>& outNodes)
{
	outNodes.Clear();
	if (!std::isfinite(unitScale))
	{
		return false;
	}

	if (model.meshes.size() > static_cast<size_t>(std::numeric_limits<int32_t>::max()) ||
		model.nodes.size() > static_cast<size_t>(std::numeric_limits<int32_t>::max()))
	{
		return false;
	}

	if (model.nodes.empty())
	{
		outNodes.Reserve(model.meshes.size());
		for (size_t meshIndex = 0; meshIndex < model.meshes.size(); ++meshIndex)
		{
			SceneNode node{};
			node.m_name = model.meshes[meshIndex].name.empty() ? "Mesh_" + std::to_string(meshIndex)
															   : model.meshes[meshIndex].name;
			node.m_meshIndex = static_cast<int32_t>(meshIndex);
			outNodes.Add(std::move(node));
		}
		return true;
	}

	TVector<uint8_t> traversalState(model.nodes.size());
	struct PendingNode
	{
		int32_t m_nodeIndex = -1;
		int32_t m_parentIndex = -1;
		glm::mat4 m_parentWorldMatrix{1.0f};
		bool m_bExit = false;
	};
	auto traverseRoot = [&](int32_t rootNode) -> bool
	{
		TVector<PendingNode> pendingNodes;
		pendingNodes.Add({rootNode, -1, glm::mat4(1.0f), false});
		while (!pendingNodes.IsEmpty())
		{
			PendingNode pending = std::move(*pendingNodes.Last());
			pendingNodes.RemoveLast();
			if (pending.m_nodeIndex < 0 || static_cast<size_t>(pending.m_nodeIndex) >= model.nodes.size())
			{
				return false;
			}

			uint8_t& state = traversalState[static_cast<size_t>(pending.m_nodeIndex)];
			if (pending.m_bExit)
			{
				state = 2;
				continue;
			}
			if (state == 1)
			{
				return false;
			}
			if (state == 2)
			{
				return false;
			}

			state = 1;
			const tinygltf::Node& node = model.nodes[static_cast<size_t>(pending.m_nodeIndex)];
			glm::mat4 localTransform(1.0f);
			if (!TryComposeNodeMatrix(node, localTransform))
			{
				return false;
			}

			glm::mat4 scaledLocalMatrix = localTransform;
			scaledLocalMatrix[3].x *= unitScale;
			scaledLocalMatrix[3].y *= unitScale;
			scaledLocalMatrix[3].z *= unitScale;
			const glm::mat4 worldMatrix = pending.m_parentWorldMatrix * scaledLocalMatrix;
			if (!Math::AllFinite(scaledLocalMatrix) || !Math::AllFinite(worldMatrix))
			{
				return false;
			}

			if (node.mesh < -1 || (node.mesh >= 0 && static_cast<size_t>(node.mesh) >= model.meshes.size()) ||
				node.skin < -1 || (node.skin >= 0 && static_cast<size_t>(node.skin) >= model.skins.size()))
			{
				return false;
			}

			SceneNode sceneNode{};
			sceneNode.m_name = node.name.empty() ? "Node_" + std::to_string(pending.m_nodeIndex) : node.name;
			sceneNode.m_sourceNodeIndex = pending.m_nodeIndex;
			sceneNode.m_parentIndex = pending.m_parentIndex;
			sceneNode.m_meshIndex = node.mesh;
			sceneNode.m_skinIndex = node.skin;
			sceneNode.m_localMatrix = scaledLocalMatrix;
			sceneNode.m_worldMatrix = worldMatrix;
			sceneNode.m_localTransform = Math::Transform::FromMatrix(scaledLocalMatrix);

			const glm::mat4 reconstructed = sceneNode.m_localTransform.Matrix();
			float maxDifference = 0.0f;
			float maxMagnitude = 1.0f;
			for (int32_t column = 0; column < 4; ++column)
			{
				for (int32_t row = 0; row < 4; ++row)
				{
					maxDifference = (std::max)(maxDifference,
						std::abs(reconstructed[column][row] - scaledLocalMatrix[column][row]));
					maxMagnitude = (std::max)(maxMagnitude, std::abs(scaledLocalMatrix[column][row]));
				}
			}
			sceneNode.m_bTransformDecomposable = maxDifference <= maxMagnitude * 1e-4f;

			const int32_t outputNodeIndex = static_cast<int32_t>(outNodes.Num());
			outNodes.Add(std::move(sceneNode));

			pendingNodes.Add({pending.m_nodeIndex, pending.m_parentIndex, glm::mat4(1.0f), true});
			for (size_t child = node.children.size(); child > 0; --child)
			{
				pendingNodes.Add({node.children[child - 1], outputNodeIndex, worldMatrix, false});
			}
		}

		return true;
	};

	bool bTraversedRoot = false;
	if (!model.scenes.empty())
	{
		const int32_t sceneIndex = model.defaultScene >= 0 ? model.defaultScene : 0;
		if (sceneIndex < 0 || static_cast<size_t>(sceneIndex) >= model.scenes.size())
		{
			return false;
		}

		bTraversedRoot = true;
		for (int32_t rootNode : model.scenes[static_cast<size_t>(sceneIndex)].nodes)
		{
			if (!traverseRoot(rootNode))
			{
				outNodes.Clear();
				return false;
			}
		}
	}
	else
	{
		TVector<uint8_t> hasParent(model.nodes.size());
		for (const tinygltf::Node& node : model.nodes)
		{
			for (int32_t childIndex : node.children)
			{
				if (childIndex < 0 || static_cast<size_t>(childIndex) >= model.nodes.size() ||
					hasParent[static_cast<size_t>(childIndex)] != 0)
				{
					return false;
				}
				hasParent[static_cast<size_t>(childIndex)] = 1;
			}
		}

		for (size_t nodeIndex = 0; nodeIndex < model.nodes.size(); ++nodeIndex)
		{
			if (hasParent[nodeIndex] == 0)
			{
				bTraversedRoot = true;
				if (!traverseRoot(static_cast<int32_t>(nodeIndex)))
				{
					outNodes.Clear();
					return false;
				}
			}
		}
	}

	if (!bTraversedRoot)
	{
		outNodes.Clear();
		return false;
	}

	return true;
}

bool GltfImporterUtils::CollectMeshInstances(const tinygltf::Model& model, TVector<MeshInstance>& outInstances)
{
	outInstances.Clear();
	TVector<SceneNode> nodes;
	if (!CollectSceneNodes(model, 1.0f, nodes))
	{
		return false;
	}

	outInstances.Reserve(nodes.Num());
	for (const SceneNode& node : nodes)
	{
		if (node.m_meshIndex < 0)
		{
			continue;
		}

		MeshInstance instance{};
		instance.m_nodeIndex = node.m_sourceNodeIndex;
		instance.m_meshIndex = node.m_meshIndex;
		instance.m_skinIndex = node.m_skinIndex;
		instance.m_worldTransform = node.m_worldMatrix;
		outInstances.Add(std::move(instance));
	}

	return true;
}
