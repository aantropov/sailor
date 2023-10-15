#include "MaterialUtils.h"
#include "BVH.h"

#include "stb/stb_image.h"

#include "assimp/scene.h"
#include "assimp/postprocess.h"
#include "assimp/Importer.hpp"
#include "assimp/DefaultLogger.hpp"
#include "assimp/LogStream.hpp"

#include "nlohmann_json/include/nlohmann/json.hpp"

using namespace Sailor;
using namespace Sailor::Math;
using namespace Sailor::Raytracing;

uint Raytracing::PackVec3ToByte(vec3 v)
{
	vec3 clamped = clamp(v, 0.0f, 1.0f);

	uint r = uint(clamped.r * 3.0f + 0.5f);
	uint g = uint(clamped.g * 3.0f + 0.5f);
	uint b = uint(clamped.b * 3.0f + 0.5f);

	// bbbb gggg rrrr
	return (b << 4) | (g << 2) | r;
}

vec3 Raytracing::UnpackByteToVec3(uint byte)
{
	uint b = (byte >> 4) & 0x3;
	uint g = (byte >> 2) & 0x3;
	uint r = byte & 0x3;

	return vec3(float(r) / 3.0f, float(g) / 3.0f, float(b) / 3.0f);
}

void Raytracing::GenerateTangentBitangent(
	vec3& outTangent,
	vec3& outBitangent,
	const vec3* vert,
	const vec2* uv)
{
	vec3 edge1 = vert[1] - vert[0];
	vec3 edge2 = vert[2] - vert[0];

	vec2 deltaUV1 = uv[1] - uv[0];
	vec2 deltaUV2 = uv[2] - uv[0];

	float denominator = deltaUV1.x * deltaUV2.y - deltaUV2.x * deltaUV1.y;
	if (abs(denominator) < 1e-6f)
	{
		return;
	}

	float f = 1.0f / denominator;

	outTangent = vec3(
		f * (deltaUV2.y * edge1.x - deltaUV1.y * edge2.x),
		f * (deltaUV2.y * edge1.y - deltaUV1.y * edge2.y),
		f * (deltaUV2.y * edge1.z - deltaUV1.y * edge2.z)
	);

	vec3 normal = cross(edge1, edge2);
	outBitangent = normalize(cross(normal, outTangent));
	outTangent = normalize(outTangent);
}

void Raytracing::ProcessMesh_Assimp(aiMesh* mesh, TVector<Triangle>& outScene, const aiScene* scene, const glm::mat4& matrix)
{
	SAILOR_PROFILE_FUNCTION();

	check(mesh->HasPositions());
	check(mesh->HasNormals());

	const size_t startIndex = outScene.Num();

	outScene.AddDefault(mesh->mNumFaces);
	for (uint32_t i = 0; i < mesh->mNumFaces; i++)
	{
		aiFace face = mesh->mFaces[i];
		if (face.mNumIndices != 3)
		{
			continue;
		}

		Math::Triangle& tri = outScene[startIndex + i];

		// TODO: use pointers instead of memcpy
		memcpy(&tri.m_vertices[0], &mesh->mVertices[face.mIndices[0]], sizeof(float) * 3);
		memcpy(&tri.m_vertices[1], &mesh->mVertices[face.mIndices[1]], sizeof(float) * 3);
		memcpy(&tri.m_vertices[2], &mesh->mVertices[face.mIndices[2]], sizeof(float) * 3);

		memcpy(&tri.m_normals[0], &mesh->mNormals[face.mIndices[0]], sizeof(float) * 3);
		memcpy(&tri.m_normals[1], &mesh->mNormals[face.mIndices[1]], sizeof(float) * 3);
		memcpy(&tri.m_normals[2], &mesh->mNormals[face.mIndices[2]], sizeof(float) * 3);

		tri.m_normals[0] = vec3(vec4(tri.m_normals[0].x, tri.m_normals[0].y, tri.m_normals[0].z, 0.0f) * matrix);
		tri.m_normals[1] = vec3(vec4(tri.m_normals[1].x, tri.m_normals[1].y, tri.m_normals[1].z, 0.0f) * matrix);
		tri.m_normals[2] = vec3(vec4(tri.m_normals[2].x, tri.m_normals[2].y, tri.m_normals[2].z, 0.0f) * matrix);

		vec4 temp{};

		temp = vec4(tri.m_vertices[0].x, tri.m_vertices[0].y, tri.m_vertices[0].z, 1.0f) * matrix;
		tri.m_vertices[0] = vec3(temp.xyz) / temp.w;

		temp = vec4(tri.m_vertices[1].x, tri.m_vertices[1].y, tri.m_vertices[1].z, 1.0f) * matrix;
		tri.m_vertices[1] = vec3(temp.xyz) / temp.w;

		temp = vec4(tri.m_vertices[2].x, tri.m_vertices[2].y, tri.m_vertices[2].z, 1.0f) * matrix;
		tri.m_vertices[2] = vec3(temp.xyz) / temp.w;

		tri.m_centroid = (tri.m_vertices[0] + tri.m_vertices[1] + tri.m_vertices[2]) * 0.333f;

		if (mesh->HasTextureCoords(0))
		{
			memcpy(&tri.m_uvs[0], &mesh->mTextureCoords[0][face.mIndices[0]], sizeof(float) * 2);
			memcpy(&tri.m_uvs[1], &mesh->mTextureCoords[0][face.mIndices[1]], sizeof(float) * 2);
			memcpy(&tri.m_uvs[2], &mesh->mTextureCoords[0][face.mIndices[2]], sizeof(float) * 2);
		}

		if (mesh->HasTextureCoords(1))
		{
			memcpy(&tri.m_uvs2[0], &mesh->mTextureCoords[1][face.mIndices[0]], sizeof(float) * 2);
			memcpy(&tri.m_uvs2[1], &mesh->mTextureCoords[1][face.mIndices[1]], sizeof(float) * 2);
			memcpy(&tri.m_uvs2[2], &mesh->mTextureCoords[1][face.mIndices[2]], sizeof(float) * 2);
		}

		if (mesh->HasTangentsAndBitangents())
		{
			memcpy(&tri.m_tangent[0], &mesh->mTangents[face.mIndices[0]], sizeof(float) * 3);
			memcpy(&tri.m_tangent[1], &mesh->mTangents[face.mIndices[1]], sizeof(float) * 3);
			memcpy(&tri.m_tangent[2], &mesh->mTangents[face.mIndices[2]], sizeof(float) * 3);

			memcpy(&tri.m_bitangent[0], &mesh->mBitangents[face.mIndices[0]], sizeof(float) * 3);
			memcpy(&tri.m_bitangent[1], &mesh->mBitangents[face.mIndices[1]], sizeof(float) * 3);
			memcpy(&tri.m_bitangent[2], &mesh->mBitangents[face.mIndices[2]], sizeof(float) * 3);

			tri.m_tangent[0] = vec3(vec4(tri.m_tangent[0].x, tri.m_tangent[0].y, tri.m_tangent[0].z, 0.0f) * matrix);
			tri.m_tangent[1] = vec3(vec4(tri.m_tangent[1].x, tri.m_tangent[1].y, tri.m_tangent[1].z, 0.0f) * matrix);
			tri.m_tangent[2] = vec3(vec4(tri.m_tangent[2].x, tri.m_tangent[2].y, tri.m_tangent[2].z, 0.0f) * matrix);

			tri.m_bitangent[0] = vec3(vec4(tri.m_bitangent[0].x, tri.m_bitangent[0].y, tri.m_bitangent[0].z, 0.0f) * matrix);
			tri.m_bitangent[1] = vec3(vec4(tri.m_bitangent[1].x, tri.m_bitangent[1].y, tri.m_bitangent[1].z, 0.0f) * matrix);
			tri.m_bitangent[2] = vec3(vec4(tri.m_bitangent[2].x, tri.m_bitangent[2].y, tri.m_bitangent[2].z, 0.0f) * matrix);

		}
		else
		{
			vec3 t = vec3(0.0f, 0.0f, 0.0f);
			vec3 b = vec3(0.0f, 0.0f, 0.0f);

			Raytracing::GenerateTangentBitangent(t, b, &tri.m_vertices[0], &tri.m_uvs[0]);

			memcpy(&tri.m_tangent[0], &t, sizeof(float) * 3);
			memcpy(&tri.m_tangent[1], &t, sizeof(float) * 3);
			memcpy(&tri.m_tangent[2], &t, sizeof(float) * 3);

			memcpy(&tri.m_bitangent[0], &b, sizeof(float) * 3);
			memcpy(&tri.m_bitangent[1], &b, sizeof(float) * 3);
			memcpy(&tri.m_bitangent[2], &b, sizeof(float) * 3);
		}

		tri.m_materialIndex = mesh->mMaterialIndex;
		//memcpy(&mesh->mVertices[face.mIndices[0]], tri.m_vertices, sizeof(float) * 3);
	}
}

SAILOR_API mat4 Raytracing::GetWorldTransformMatrix(const aiScene* scene, const char* name)
{
	mat4 matrix{ 1 };
	aiMatrix4x4 aiMatrix{};

	aiNode* pRootNode = scene->mRootNode;
	aiNode* cur = pRootNode->FindNode(name);

	while (cur != nullptr)
	{
		aiMatrix = aiMatrix * cur->mTransformation;
		cur = cur->mParent;
	}
	::memcpy(&matrix, &aiMatrix, sizeof(float) * 16);
	return matrix;
}

void Raytracing::ProcessNode_Assimp(TVector<Triangle>& outScene, aiNode* node, const aiScene* scene, const glm::mat4& parentMatrix)
{
	SAILOR_PROFILE_FUNCTION();

	glm::mat4 nodeMatrix{};
	::memcpy(&nodeMatrix, &node->mTransformation, sizeof(float) * 16);

	for (uint32_t i = 0; i < node->mNumMeshes; i++)
	{
		aiMesh* mesh = scene->mMeshes[node->mMeshes[i]];
		Raytracing::ProcessMesh_Assimp(mesh, outScene, scene, nodeMatrix * parentMatrix);
	}

	for (uint32_t i = 0; i < node->mNumChildren; i++)
	{
		Raytracing::ProcessNode_Assimp(outScene, node->mChildren[i], scene, nodeMatrix * parentMatrix);
	}
}
