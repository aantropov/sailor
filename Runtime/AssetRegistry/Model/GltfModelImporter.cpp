#include "AssetRegistry/Model/ModelImporter.h"

#include "AssetRegistry/Model/GltfImporterUtils.h"
#include "AssetRegistry/Model/GltfModelImporterInternal.h"
#include "AssetRegistry/Model/ModelGeometry.h"
#include "Math/Math.h"
#include "Raytracing/MaterialUtils.h"

#include <cmath>
#include <limits>
#include <string>
#include <utility>

#include <tiny_gltf.h>

using namespace Sailor;
using namespace Sailor::GltfImporterInternal;

static glm::vec3 CalculateNormal(const glm::vec3& v0, const glm::vec3& v1, const glm::vec3& v2)
{
	return Math::SafeNormalize(glm::cross(v1 - v0, v2 - v0));
}

static void GenerateTangents(ModelImporter::MeshContext& meshContext,
	uint32_t vertexOffset,
	uint32_t vertexCount,
	uint32_t indexOffset,
	uint32_t indexCount)
{
	TVector<glm::vec3> tangents(vertexCount, glm::vec3(0.0f));
	TVector<glm::vec3> bitangents(vertexCount, glm::vec3(0.0f));

	auto processTriangle = [&](uint32_t idx0, uint32_t idx1, uint32_t idx2)
	{
		glm::vec3 verts[3] = {meshContext.outVertices[idx0].m_position,
			meshContext.outVertices[idx1].m_position,
			meshContext.outVertices[idx2].m_position};

		glm::vec2 uvs[3] = {meshContext.outVertices[idx0].m_texcoord,
			meshContext.outVertices[idx1].m_texcoord,
			meshContext.outVertices[idx2].m_texcoord};

		glm::vec3 t(0.0f);
		glm::vec3 b(0.0f);
		Raytracing::GenerateTangentBitangent(t, b, verts, uvs);

		tangents[idx0 - vertexOffset] += t;
		tangents[idx1 - vertexOffset] += t;
		tangents[idx2 - vertexOffset] += t;

		bitangents[idx0 - vertexOffset] += b;
		bitangents[idx1 - vertexOffset] += b;
		bitangents[idx2 - vertexOffset] += b;
	};

	if (indexCount > 0 && meshContext.outIndices.Num() > 0)
	{
		for (uint32_t i = 0; i + 2 < indexCount; i += 3)
		{
			uint32_t idx0 = meshContext.outIndices[indexOffset + i];
			uint32_t idx1 = meshContext.outIndices[indexOffset + i + 1];
			uint32_t idx2 = meshContext.outIndices[indexOffset + i + 2];
			processTriangle(idx0, idx1, idx2);
		}
	}
	else
	{
		for (uint32_t i = 0; i + 2 < vertexCount; i += 3)
		{
			uint32_t idx0 = vertexOffset + i;
			uint32_t idx1 = vertexOffset + i + 1;
			uint32_t idx2 = vertexOffset + i + 2;
			processTriangle(idx0, idx1, idx2);
		}
	}

	for (uint32_t i = 0; i < vertexCount; ++i)
	{
		meshContext.outVertices[vertexOffset + i].m_tangent = Math::SafeNormalize(tangents[i]);
		meshContext.outVertices[vertexOffset + i].m_bitangent = Math::SafeNormalize(bitangents[i]);
	}
}

static void GenerateNormals(ModelImporter::MeshContext& meshContext,
	uint32_t vertexOffset,
	uint32_t vertexCount,
	uint32_t indexOffset,
	uint32_t indexCount)
{
	TVector<glm::vec3> normals(vertexCount, glm::vec3(0.0f));

	if (indexCount > 0 && meshContext.outIndices.Num() > 0)
	{
		for (uint32_t i = 0; i + 2 < indexCount; i += 3)
		{
			uint32_t idx0 = meshContext.outIndices[indexOffset + i];
			uint32_t idx1 = meshContext.outIndices[indexOffset + i + 1];
			uint32_t idx2 = meshContext.outIndices[indexOffset + i + 2];

			glm::vec3 normal = CalculateNormal(meshContext.outVertices[idx0].m_position,
				meshContext.outVertices[idx1].m_position,
				meshContext.outVertices[idx2].m_position);

			normals[idx0 - vertexOffset] += normal;
			normals[idx1 - vertexOffset] += normal;
			normals[idx2 - vertexOffset] += normal;
		}
	}
	else
	{
		for (uint32_t i = 0; i + 2 < vertexCount; i += 3)
		{
			uint32_t idx0 = vertexOffset + i;
			uint32_t idx1 = vertexOffset + i + 1;
			uint32_t idx2 = vertexOffset + i + 2;

			glm::vec3 normal = CalculateNormal(meshContext.outVertices[idx0].m_position,
				meshContext.outVertices[idx1].m_position,
				meshContext.outVertices[idx2].m_position);

			normals[i] += normal;
			normals[i + 1] += normal;
			normals[i + 2] += normal;
		}
	}

	for (uint32_t i = 0; i < vertexCount; ++i)
	{
		meshContext.outVertices[vertexOffset + i].m_normal =
			Math::SafeNormalize(normals[i], glm::vec3(0.0f, 1.0f, 0.0f));
	}
}

bool ModelImporter::ImportModel(ModelAssetInfoPtr assetInfo,
	TVector<MeshContext>& outParsedMeshes,
	Math::AABB& outBoundsAabb,
	Math::Sphere& outBoundsSphere,
	TVector<glm::mat4>& outInverseBind)
{
	return assetInfo && ImportModel(assetInfo->GetAssetFilepath(),
							assetInfo->GetUnitScale(),
							assetInfo->ShouldBatchByMaterial(),
							assetInfo->ShouldFlipTexcoordY(),
							outParsedMeshes,
							outBoundsAabb,
							outBoundsSphere,
							outInverseBind);
}

bool ModelImporter::ImportModel(const std::string& assetFilepath,
	float unitScale,
	bool bShouldBatchByMaterial,
	bool bFlipTexcoordY,
	TVector<MeshContext>& outParsedMeshes,
	Math::AABB& outBoundsAabb,
	Math::Sphere& outBoundsSphere,
	TVector<glm::mat4>& outInverseBind,
	tinygltf::Model* outGltfModel)
{
	outParsedMeshes.Clear();
	outBoundsAabb = Math::AABB();
	outBoundsSphere = Math::Sphere();
	outInverseBind.Clear();
	if (outGltfModel != nullptr)
	{
		*outGltfModel = tinygltf::Model();
	}

	if (!std::isfinite(unitScale))
	{
		return false;
	}

	tinygltf::Model gltfModel;
	std::string err;
	std::string warn;
	const bool bGltfParsed = GltfImporterUtils::LoadModel(assetFilepath, true, gltfModel, err, warn);

	if (!err.empty())
	{
		SAILOR_LOG_ERROR("Parsing gltf %s error: %s", assetFilepath.c_str(), err.c_str());
	}

	if (!warn.empty())
	{
		SAILOR_LOG("Parsing gltf %s warning: %s", assetFilepath.c_str(), warn.c_str());
	}

	if (!bGltfParsed)
	{
		return false;
	}

	TVector<glm::mat4> parsedInverseBind;
	if (!gltfModel.skins.empty())
	{
		const auto& gltfSkin = gltfModel.skins[0];
		const size_t numBones = gltfSkin.joints.size();
		parsedInverseBind.Resize(numBones);
		for (size_t i = 0; i < numBones; ++i)
		{
			parsedInverseBind[i] = glm::mat4(1.0f);
		}

		GltfAccessorView inverseBindView;
		if (gltfSkin.inverseBindMatrices >= 0)
		{
			if (!TryGetAccessorView(
					gltfModel, gltfSkin.inverseBindMatrices, TINYGLTF_TYPE_MAT4, numBones, inverseBindView) ||
				!IsFloatAccessor(*inverseBindView.m_accessor))
			{
				SAILOR_LOG_ERROR("Cannot import invalid inverse-bind accessor: %s", assetFilepath.c_str());
				return false;
			}

			for (size_t i = 0; i < numBones; ++i)
			{
				for (size_t component = 0; component < 16; ++component)
				{
					parsedInverseBind[i][static_cast<int32_t>(component / 4)][static_cast<int32_t>(component % 4)] =
						ReadAccessorFloat(inverseBindView, i, component);
				}

				if (!Math::AllFinite(parsedInverseBind[i]))
				{
					SAILOR_LOG_ERROR("Cannot import non-finite inverse-bind matrix: %s", assetFilepath.c_str());
					return false;
				}
			}
		}
	}

	if (gltfModel.materials.size() == std::numeric_limits<size_t>::max())
	{
		return false;
	}

	const bool bHasMeshQuantization = HasGltfExtension(gltfModel, "KHR_mesh_quantization");
	TVector<GltfImporterUtils::SceneNode> sceneNodes;
	if (!GltfImporterUtils::CollectSceneNodes(gltfModel, unitScale, sceneNodes))
	{
		SAILOR_LOG_ERROR("Cannot resolve glTF scene hierarchy: %s", assetFilepath.c_str());
		return false;
	}
	for (const GltfImporterUtils::SceneNode& node : sceneNodes)
	{
		if (node.m_skinIndex > 0)
		{
			SAILOR_LOG_ERROR("Cannot import unsupported active glTF skin index %d; only skin 0 is supported: %s",
				node.m_skinIndex,
				assetFilepath.c_str());
			return false;
		}
	}

	TVector<size_t> meshContextOffsets(gltfModel.meshes.size());
	TVector<size_t> meshContextCounts(gltfModel.meshes.size());
	for (size_t meshIndex = 0; meshIndex < gltfModel.meshes.size(); ++meshIndex)
	{
		const tinygltf::Mesh& mesh = gltfModel.meshes[meshIndex];
		meshContextOffsets[meshIndex] = outParsedMeshes.Num();
		for (size_t primitiveIndex = 0; primitiveIndex < mesh.primitives.size(); ++primitiveIndex)
		{
			const int32_t materialIndex =
				mesh.primitives[primitiveIndex].material >= 0 &&
						static_cast<size_t>(mesh.primitives[primitiveIndex].material) < gltfModel.materials.size()
					? mesh.primitives[primitiveIndex].material
					: -1;
			bool bHasContext = false;
			if (bShouldBatchByMaterial)
			{
				for (size_t contextIndex = meshContextOffsets[meshIndex]; contextIndex < outParsedMeshes.Num();
					++contextIndex)
				{
					if (outParsedMeshes[contextIndex].materialIndex == materialIndex)
					{
						bHasContext = true;
						break;
					}
				}
			}

			if (!bHasContext)
			{
				if (outParsedMeshes.Num() > static_cast<size_t>(std::numeric_limits<uint32_t>::max()))
				{
					return false;
				}

				MeshContext context{};
				context.materialIndex = materialIndex;
				context.materialSlot = bShouldBatchByMaterial
										   ? (materialIndex >= 0 ? static_cast<uint32_t>(materialIndex) : 0u)
										   : static_cast<uint32_t>(outParsedMeshes.Num());
				context.sourceMeshIndex = static_cast<int32_t>(meshIndex);
				outParsedMeshes.Add(std::move(context));
			}
		}

		meshContextCounts[meshIndex] = outParsedMeshes.Num() - meshContextOffsets[meshIndex];
	}

	for (size_t meshIndex = 0; meshIndex < gltfModel.meshes.size(); ++meshIndex)
	{
		const tinygltf::Mesh& mesh = gltfModel.meshes[meshIndex];
		const glm::mat4 geometryTransform = glm::scale(glm::mat4(1.0f), glm::vec3(unitScale));
		const glm::mat3 directionTransform =
			glm::mat3(glm::scale(glm::mat4(1.0f), glm::vec3(unitScale < 0.0f ? -1.0f : 1.0f)));
		const float transformDeterminant = glm::determinant(directionTransform);
		if (!std::isfinite(transformDeterminant))
		{
			SAILOR_LOG_ERROR("Cannot import glTF mesh instance with invalid transform: %s", assetFilepath.c_str());
			return false;
		}

		const bool bReverseWinding = transformDeterminant < 0.0f;
		glm::mat3 normalTransform = directionTransform;
		if (transformDeterminant != 0.0f)
		{
			const glm::mat3 inverseTranspose = glm::transpose(glm::inverse(directionTransform));
			if (Math::AllFinite(inverseTranspose))
			{
				normalTransform = inverseTranspose;
			}
		}

		for (size_t primitiveIndex = 0; primitiveIndex < mesh.primitives.size(); ++primitiveIndex)
		{
			const tinygltf::Primitive& primitive = mesh.primitives[primitiveIndex];
			const int32_t materialIndex =
				primitive.material >= 0 && static_cast<size_t>(primitive.material) < gltfModel.materials.size()
					? primitive.material
					: -1;
			if (primitive.mode != TINYGLTF_MODE_TRIANGLES)
			{
				SAILOR_LOG("Skipping non-triangle glTF primitive in %s.", assetFilepath.c_str());
				continue;
			}

			const auto positionIt = primitive.attributes.find("POSITION");
			GltfAccessorView positionView;
			if (positionIt == primitive.attributes.end() ||
				!TryGetAccessorView(gltfModel, positionIt->second, TINYGLTF_TYPE_VEC3, 1, positionView) ||
				positionView.m_accessor->count > std::numeric_limits<uint32_t>::max() ||
				!IsPositionAccessorSupported(*positionView.m_accessor, bHasMeshQuantization))
			{
				SAILOR_LOG_ERROR(
					"Skipping glTF primitive with unsupported POSITION accessor: %s", assetFilepath.c_str());
				continue;
			}

			const size_t vertexCount = positionView.m_accessor->count;
			auto tryGetAttribute = [&](const char* semantic, int32_t expectedType, GltfAccessorView& outView)
			{
				const auto it = primitive.attributes.find(semantic);
				return it != primitive.attributes.end() &&
					   TryGetAccessorView(gltfModel, it->second, expectedType, vertexCount, outView) &&
					   outView.m_accessor->count == vertexCount;
			};

			GltfAccessorView normalView;
			GltfAccessorView texcoordView;
			GltfAccessorView tangentView;
			GltfAccessorView colorView;
			GltfAccessorView jointsView;
			GltfAccessorView weightsView;

			const bool bNormalsPresent = primitive.attributes.find("NORMAL") != primitive.attributes.end();
			const bool bTexcoordsPresent = primitive.attributes.find("TEXCOORD_0") != primitive.attributes.end();
			const bool bTangentsPresent = primitive.attributes.find("TANGENT") != primitive.attributes.end();
			const bool bJointsPresent = primitive.attributes.find("JOINTS_0") != primitive.attributes.end();
			const bool bWeightsPresent = primitive.attributes.find("WEIGHTS_0") != primitive.attributes.end();

			const bool bHasNormals = bNormalsPresent && tryGetAttribute("NORMAL", TINYGLTF_TYPE_VEC3, normalView);
			const bool bHasTexcoords =
				bTexcoordsPresent && tryGetAttribute("TEXCOORD_0", TINYGLTF_TYPE_VEC2, texcoordView);
			const bool bHasTangents = bTangentsPresent && tryGetAttribute("TANGENT", TINYGLTF_TYPE_VEC4, tangentView);
			const bool bHasJoints = bJointsPresent && tryGetAttribute("JOINTS_0", TINYGLTF_TYPE_VEC4, jointsView);
			const bool bHasWeights = bWeightsPresent && tryGetAttribute("WEIGHTS_0", TINYGLTF_TYPE_VEC4, weightsView);

			const auto colorIt = primitive.attributes.find("COLOR_0");
			const bool bColorPresent = colorIt != primitive.attributes.end();
			const bool bHasColor =
				bColorPresent && TryGetAccessorView(gltfModel, colorIt->second, -1, vertexCount, colorView) &&
				colorView.m_accessor->count == vertexCount &&
				(colorView.m_accessor->type == TINYGLTF_TYPE_VEC3 || colorView.m_accessor->type == TINYGLTF_TYPE_VEC4);

			if ((bNormalsPresent &&
					(!bHasNormals || !IsDirectionAccessorSupported(*normalView.m_accessor, bHasMeshQuantization))) ||
				(bTexcoordsPresent &&
					(!bHasTexcoords || !IsTexcoordAccessorSupported(*texcoordView.m_accessor, bHasMeshQuantization))) ||
				(bTangentsPresent &&
					(!bHasTangents || !IsDirectionAccessorSupported(*tangentView.m_accessor, bHasMeshQuantization))) ||
				(bColorPresent && (!bHasColor || !IsColorAccessorSupported(*colorView.m_accessor))) ||
				bJointsPresent != bWeightsPresent ||
				(bJointsPresent && (!bHasJoints || !bHasWeights || !IsJointsAccessorSupported(*jointsView.m_accessor) ||
									   !IsWeightsAccessorSupported(*weightsView.m_accessor))))
			{
				SAILOR_LOG_ERROR("Skipping glTF primitive with invalid vertex attributes: %s", assetFilepath.c_str());
				continue;
			}

			TVector<uint32_t> localIndices;
			if (primitive.indices >= 0)
			{
				GltfAccessorView indexView;
				if (!TryGetAccessorView(gltfModel, primitive.indices, TINYGLTF_TYPE_SCALAR, 1, indexView) ||
					indexView.m_accessor->count > std::numeric_limits<uint32_t>::max() ||
					indexView.m_accessor->normalized)
				{
					SAILOR_LOG_ERROR(
						"Skipping glTF primitive with unsupported index accessor: %s", assetFilepath.c_str());
					continue;
				}

				localIndices.Reserve(indexView.m_accessor->count);
				bool bIndicesValid = true;
				for (size_t i = 0; i < indexView.m_accessor->count; ++i)
				{
					uint32_t index = 0;
					if (!TryReadAccessorIndex(indexView, i, index) || index >= vertexCount)
					{
						bIndicesValid = false;
						break;
					}

					localIndices.Add(index);
				}

				if (!bIndicesValid)
				{
					SAILOR_LOG_ERROR("Skipping glTF primitive with out-of-range indices: %s", assetFilepath.c_str());
					continue;
				}
			}
			else
			{
				localIndices.Reserve(vertexCount);
				for (uint32_t i = 0; i < vertexCount; ++i)
				{
					localIndices.Add(i);
				}
			}

			if (localIndices.Num() == 0 || localIndices.Num() % 3 != 0)
			{
				SAILOR_LOG_ERROR("Skipping glTF primitive with invalid triangle indices: %s", assetFilepath.c_str());
				continue;
			}
			if (bReverseWinding)
			{
				for (size_t i = 0; i < localIndices.Num(); i += 3)
				{
					std::swap(localIndices[i + 1], localIndices[i + 2]);
				}
			}

			TVector<RHI::VertexP3N3T3B3UV2C4I4W4> localVertices;
			localVertices.Reserve(vertexCount);
			bool bVerticesValid = true;
			for (size_t i = 0; i < vertexCount; ++i)
			{
				RHI::VertexP3N3T3B3UV2C4I4W4 vertex{};
				const glm::vec3 sourcePosition(ReadAccessorFloat(positionView, i, 0),
					ReadAccessorFloat(positionView, i, 1),
					ReadAccessorFloat(positionView, i, 2));
				vertex.m_position = glm::vec3(geometryTransform * glm::vec4(sourcePosition, 1.0f));
				const glm::vec3 sourceNormal = bHasNormals ? glm::vec3(ReadAccessorFloat(normalView, i, 0),
																 ReadAccessorFloat(normalView, i, 1),
																 ReadAccessorFloat(normalView, i, 2))
														   : glm::vec3(0.0f);
				vertex.m_normal = normalTransform * sourceNormal;
				vertex.m_texcoord = bHasTexcoords ? glm::vec2(ReadAccessorFloat(texcoordView, i, 0),
														ReadAccessorFloat(texcoordView, i, 1))
												  : glm::vec2(0.0f);
				if (bHasTexcoords && bFlipTexcoordY)
				{
					vertex.m_texcoord.y = 1.0f - vertex.m_texcoord.y;
				}
				const glm::vec3 sourceTangent = bHasTangents ? glm::vec3(ReadAccessorFloat(tangentView, i, 0),
																   ReadAccessorFloat(tangentView, i, 1),
																   ReadAccessorFloat(tangentView, i, 2))
															 : glm::vec3(0.0f);
				vertex.m_tangent = directionTransform * sourceTangent;
				vertex.m_bitangent = bHasTangents && bHasNormals
										 ? directionTransform * (glm::cross(sourceNormal, sourceTangent) *
																	ReadAccessorFloat(tangentView, i, 3))
										 : glm::vec3(0.0f);
				vertex.m_color = glm::vec4(1.0f);
				if (bHasColor)
				{
					vertex.m_color = glm::vec4(ReadAccessorFloat(colorView, i, 0),
						ReadAccessorFloat(colorView, i, 1),
						ReadAccessorFloat(colorView, i, 2),
						colorView.m_accessor->type == TINYGLTF_TYPE_VEC4 ? ReadAccessorFloat(colorView, i, 3) : 1.0f);
				}

				vertex.m_boneIds = glm::ivec4(0);
				if (bHasJoints)
				{
					for (size_t component = 0; component < 4; ++component)
					{
						vertex.m_boneIds[static_cast<int32_t>(component)] =
							static_cast<int32_t>(ReadAccessorFloat(jointsView, i, component));
					}
				}

				vertex.m_boneWeights = glm::vec4(0.0f);
				if (bHasWeights)
				{
					for (size_t component = 0; component < 4; ++component)
					{
						vertex.m_boneWeights[static_cast<int32_t>(component)] =
							ReadAccessorFloat(weightsView, i, component);
					}
				}

				if (!Math::AllFinite(vertex.m_position) || !Math::AllFinite(vertex.m_normal) ||
					!Math::AllFinite(vertex.m_texcoord) || !Math::AllFinite(vertex.m_tangent) ||
					!Math::AllFinite(vertex.m_bitangent) || !Math::AllFinite(vertex.m_color) ||
					!Math::AllFinite(vertex.m_boneWeights))
				{
					bVerticesValid = false;
					break;
				}

				localVertices.Add(vertex);
			}

			if (!bVerticesValid)
			{
				SAILOR_LOG_ERROR("Skipping glTF primitive with non-finite vertices: %s", assetFilepath.c_str());
				continue;
			}

			MeshContext* pMeshContext = nullptr;
			if (bShouldBatchByMaterial)
			{
				const size_t contextEnd = meshContextOffsets[meshIndex] + meshContextCounts[meshIndex];
				for (size_t contextIndex = meshContextOffsets[meshIndex]; contextIndex < contextEnd; ++contextIndex)
				{
					if (outParsedMeshes[contextIndex].materialIndex == materialIndex)
					{
						pMeshContext = &outParsedMeshes[contextIndex];
						break;
					}
				}
			}
			else
			{
				const size_t contextIndex = meshContextOffsets[meshIndex] + primitiveIndex;
				if (contextIndex < outParsedMeshes.Num())
				{
					pMeshContext = &outParsedMeshes[contextIndex];
				}
			}

			if (pMeshContext == nullptr)
			{
				SAILOR_LOG_ERROR("Cannot resolve glTF source mesh context: %s", assetFilepath.c_str());
				return false;
			}

			const size_t existingVertices = pMeshContext != nullptr ? pMeshContext->outVertices.Num() : 0;
			const size_t existingIndices = pMeshContext != nullptr ? pMeshContext->outIndices.Num() : 0;
			const size_t maxMeshElements = std::numeric_limits<uint32_t>::max();
			if (vertexCount > maxMeshElements - existingVertices ||
				localIndices.Num() > maxMeshElements - existingIndices)
			{
				SAILOR_LOG_ERROR("Skipping oversized glTF primitive: %s", assetFilepath.c_str());
				continue;
			}

			const uint32_t startIndex = static_cast<uint32_t>(pMeshContext->outVertices.Num());
			const uint32_t indicesStart = static_cast<uint32_t>(pMeshContext->outIndices.Num());
			for (const auto& vertex : localVertices)
			{
				pMeshContext->outVertices.Add(vertex);
				pMeshContext->bounds.Extend(vertex.m_position);
			}

			for (uint32_t index : localIndices)
			{
				pMeshContext->outIndices.Add(startIndex + index);
			}

			const uint32_t indexCount = static_cast<uint32_t>(localIndices.Num());
			if (!bHasNormals)
			{
				GenerateNormals(
					*pMeshContext, startIndex, static_cast<uint32_t>(vertexCount), indicesStart, indexCount);
			}

			if (!bHasTangents || !bHasNormals)
			{
				GenerateTangents(
					*pMeshContext, startIndex, static_cast<uint32_t>(vertexCount), indicesStart, indexCount);
			}

			for (size_t i = 0; i < vertexCount; ++i)
			{
				ModelGeometry::SanitizeVertexFrame(pMeshContext->outVertices[startIndex + i]);
			}
		}
	}

	TVector<Math::AABB> sourceMeshBounds(gltfModel.meshes.size());
	for (const MeshContext& meshContext : outParsedMeshes)
	{
		if (meshContext.HasGeometry() && meshContext.sourceMeshIndex >= 0 &&
			static_cast<size_t>(meshContext.sourceMeshIndex) < sourceMeshBounds.Num())
		{
			sourceMeshBounds[static_cast<size_t>(meshContext.sourceMeshIndex)].Extend(meshContext.bounds);
		}
	}

	for (const GltfImporterUtils::SceneNode& node : sceneNodes)
	{
		if (node.m_meshIndex < 0 || static_cast<size_t>(node.m_meshIndex) >= sourceMeshBounds.Num())
		{
			continue;
		}

		Math::AABB instanceBounds = sourceMeshBounds[static_cast<size_t>(node.m_meshIndex)];
		if (!instanceBounds.IsValid())
		{
			continue;
		}
		if (node.m_skinIndex < 0)
		{
			instanceBounds.Apply(node.m_worldMatrix);
		}
		outBoundsAabb.Extend(instanceBounds);
	}

	bool bImported = outParsedMeshes.Num() > 0 && outBoundsAabb.IsValid();
	if (bImported)
	{
		outBoundsSphere.m_center = 0.5f * outBoundsAabb.m_min + 0.5f * outBoundsAabb.m_max;
		outBoundsSphere.m_radius = glm::distance(outBoundsAabb.m_max, outBoundsSphere.m_center);
		bImported = Math::AllFinite(outBoundsSphere.m_center) && std::isfinite(outBoundsSphere.m_radius);
	}

	if (!bImported)
	{
		outParsedMeshes.Clear();
		outBoundsAabb = Math::AABB();
		outBoundsSphere = Math::Sphere();
		outInverseBind.Clear();
	}

	if (outGltfModel != nullptr && bImported)
	{
		*outGltfModel = std::move(gltfModel);
	}

	if (bImported)
	{
		outInverseBind = std::move(parsedInverseBind);
	}

	return bImported;
}
