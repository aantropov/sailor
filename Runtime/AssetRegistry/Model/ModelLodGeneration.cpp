#include "AssetRegistry/Model/ModelLodGeneration.h"

#include "AssetRegistry/Model/ModelGeometry.h"
#include "AssetRegistry/Model/ModelLodCache.h"
#include "Core/Utils.h"
#include "Math/Math.h"

#include <algorithm>
#include <cmath>

#if defined(SAILOR_HAS_MESHOPT)
#include <meshoptimizer.h>
#endif

using namespace Sailor;

namespace
{
	constexpr uint32_t MaxGeneratedModelLods = 8u;

	void RecalculateShadingBasis(ModelImporter::MeshContext::LodGeometry& geometry)
	{
		const size_t vertexCount = geometry.m_vertices.Num();
		if (vertexCount == 0u || geometry.m_indices.Num() < 3u)
		{
			return;
		}

		TVector<glm::vec3> normals(vertexCount, glm::vec3(0.0f));
		TVector<glm::vec3> tangents(vertexCount, glm::vec3(0.0f));
		TVector<glm::vec3> bitangents(vertexCount, glm::vec3(0.0f));
		for (size_t index = 0u; index + 2u < geometry.m_indices.Num(); index += 3u)
		{
			const uint32_t i0 = geometry.m_indices[index];
			const uint32_t i1 = geometry.m_indices[index + 1u];
			const uint32_t i2 = geometry.m_indices[index + 2u];
			if (i0 >= vertexCount || i1 >= vertexCount || i2 >= vertexCount)
			{
				continue;
			}

			const auto& v0 = geometry.m_vertices[i0];
			const auto& v1 = geometry.m_vertices[i1];
			const auto& v2 = geometry.m_vertices[i2];
			const glm::vec3 edge1 = v1.m_position - v0.m_position;
			const glm::vec3 edge2 = v2.m_position - v0.m_position;
			const glm::vec3 faceNormal = glm::cross(edge1, edge2);
			if (Math::AllFinite(faceNormal))
			{
				normals[i0] += faceNormal;
				normals[i1] += faceNormal;
				normals[i2] += faceNormal;
			}

			const glm::vec2 uv1 = v1.m_texcoord - v0.m_texcoord;
			const glm::vec2 uv2 = v2.m_texcoord - v0.m_texcoord;
			const float determinant = uv1.x * uv2.y - uv1.y * uv2.x;
			if (!std::isfinite(determinant) || std::abs(determinant) <= 1e-8f)
			{
				continue;
			}

			const float reciprocal = 1.0f / determinant;
			const glm::vec3 tangent = (edge1 * uv2.y - edge2 * uv1.y) * reciprocal;
			const glm::vec3 bitangent = (edge2 * uv1.x - edge1 * uv2.x) * reciprocal;
			if (Math::AllFinite(tangent) && Math::AllFinite(bitangent))
			{
				tangents[i0] += tangent;
				tangents[i1] += tangent;
				tangents[i2] += tangent;
				bitangents[i0] += bitangent;
				bitangents[i1] += bitangent;
				bitangents[i2] += bitangent;
			}
		}

		for (size_t vertexIndex = 0u; vertexIndex < vertexCount; ++vertexIndex)
		{
			auto& vertex = geometry.m_vertices[vertexIndex];
			const glm::vec3& normal = normals[vertexIndex];
			if (Math::AllFinite(normal) && glm::dot(normal, normal) > 1e-12f)
			{
				vertex.m_normal = normal;
			}

			const glm::vec3& tangent = tangents[vertexIndex];
			const glm::vec3 unitNormal = Math::SafeNormalize(vertex.m_normal, glm::vec3(0.0f, 1.0f, 0.0f));
			const glm::vec3 projectedTangent = tangent - unitNormal * glm::dot(tangent, unitNormal);
			if (Math::AllFinite(projectedTangent) && glm::dot(projectedTangent, projectedTangent) > 1e-12f)
			{
				vertex.m_tangent = tangent;
			}

			const glm::vec3& bitangent = bitangents[vertexIndex];
			if (Math::AllFinite(bitangent) && glm::dot(bitangent, bitangent) > 1e-12f)
			{
				vertex.m_bitangent = bitangent;
			}
			ModelGeometry::SanitizeVertexFrame(vertex);
		}
	}

	ModelImporter::MeshContext::LodGeometry Build(const ModelImporter::MeshContext& mesh, float targetRatio)
	{
		ModelImporter::MeshContext::LodGeometry result{};
		if (!mesh.HasGeometry())
		{
			return result;
		}

		const size_t triangleIndexCount = mesh.outIndices.Num() - mesh.outIndices.Num() % 3u;
		const size_t targetIndexCount =
			(std::max)(size_t{3u}, static_cast<size_t>(triangleIndexCount * targetRatio) / 3u * 3u);

#if defined(SAILOR_HAS_MESHOPT)
		result.m_indices.Resize(triangleIndexCount);
		float simplificationError = 0.0f;
		const size_t simplifiedIndexCount = meshopt_simplify(result.m_indices.GetData(),
			mesh.outIndices.GetData(),
			triangleIndexCount,
			&mesh.outVertices[0].m_position.x,
			mesh.outVertices.Num(),
			sizeof(RHI::VertexP3N3T3B3UV2C4I4W4),
			(std::min)(targetIndexCount, triangleIndexCount),
			0.01f,
			meshopt_SimplifyLockBorder,
			&simplificationError);
		result.m_indices.Resize(simplifiedIndexCount);
		if (simplifiedIndexCount >= 3u)
		{
			meshopt_optimizeVertexCache(
				result.m_indices.GetData(), result.m_indices.GetData(), result.m_indices.Num(), mesh.outVertices.Num());
			result.m_vertices.Resize(mesh.outVertices.Num());
			const size_t compactedVertexCount = meshopt_optimizeVertexFetch(result.m_vertices.GetData(),
				result.m_indices.GetData(),
				result.m_indices.Num(),
				mesh.outVertices.GetData(),
				mesh.outVertices.Num(),
				sizeof(RHI::VertexP3N3T3B3UV2C4I4W4));
			result.m_vertices.Resize(compactedVertexCount);
			RecalculateShadingBasis(result);
		}
#endif

		if (result.m_vertices.IsEmpty() || result.m_indices.Num() < 3u)
		{
			result.m_vertices = mesh.outVertices;
			result.m_indices = mesh.outIndices;
		}
		return result;
	}
}

void Sailor::ModelLodGeneration::Generate(TVector<ModelImporter::MeshContext>& meshes,
	uint32_t numLods,
	float reductionFactor)
{
	const uint32_t clampedNumLods = (std::min)(numLods, MaxGeneratedModelLods);
	const float clampedReductionFactor = (std::clamp)(reductionFactor, 0.05f, 0.95f);
	for (auto& mesh : meshes)
	{
		mesh.lods.Clear();
		mesh.lods.Reserve(clampedNumLods);
		for (uint32_t lodLevel = 1u; lodLevel <= clampedNumLods; ++lodLevel)
		{
			mesh.lods.Add(Build(mesh, std::pow(clampedReductionFactor, static_cast<float>(lodLevel))));
		}
	}
}

void Sailor::ModelLodGeneration::Prepare(const ModelAssetInfo& assetInfo, TVector<ModelImporter::MeshContext>& meshes)
{
	if (!assetInfo.ShouldGenerateLods() || meshes.IsEmpty())
	{
		return;
	}

	const uint32_t numLods = (std::min)(assetInfo.GetNumGeneratedLods(), MaxGeneratedModelLods);
	FileRevision sourceRevision{};
	if (numLods == 0u || !Utils::TryGetFileRevision(assetInfo.GetAssetFilepath(), sourceRevision))
	{
		return;
	}

	const float reductionFactor = (std::clamp)(assetInfo.GetLodReductionFactor(), 0.05f, 0.95f);
	for (uint32_t lodLevel = 1u; lodLevel <= numLods; ++lodLevel)
	{
		if (ModelLodCache::Load(assetInfo, sourceRevision, lodLevel, meshes))
		{
			continue;
		}

		const size_t lodIndex = static_cast<size_t>(lodLevel - 1u);
		const float targetRatio = std::pow(reductionFactor, static_cast<float>(lodLevel));
		for (auto& mesh : meshes)
		{
			mesh.lods.Resize((std::max)(mesh.lods.Num(), lodIndex + 1u));
			mesh.lods[lodIndex] = Build(mesh, targetRatio);
		}
		ModelLodCache::Save(assetInfo, sourceRevision, lodLevel, meshes);
	}
}
