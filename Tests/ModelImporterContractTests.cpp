#include "AssetRegistry/Model/ModelImporter.h"
#include "AssetRegistry/Model/GltfImporterUtils.h"
#include "Raytracing/MaterialUtils.h"
#include "Raytracing/PathTracer.h"

#include <cmath>
#include <functional>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <utility>

#include <glm/gtc/matrix_transform.hpp>
#include <tiny_gltf.h>

using namespace Sailor;

namespace
{
	class ControllableMesh final : public RHI::RHIMesh
	{
	public:
		bool IsReady() const override
		{
			return m_bReady;
		}

		void SetReady(bool bReady)
		{
			m_bReady = bReady;
		}

	private:
		bool m_bReady = false;
	};

	class PathTracerBasisProbe final : public Raytracing::PathTracer
	{
	public:
		void Evaluate(
			const ModelPtr& model,
			const glm::mat4& worldMatrix,
			glm::vec3& outNormal,
			glm::vec3& outTangent,
			glm::vec3& outBitangent)
		{
			m_tlasInstances.Clear();
			TLASInstance instance{};
			instance.m_model = model;
			instance.m_worldMatrix = worldMatrix;
			instance.m_inverseWorldMatrix = glm::inverse(worldMatrix);
			m_tlasInstances.Add(std::move(instance));

			TLASHit hit{};
			hit.m_instanceIndex = 0;
			hit.m_triangleIndex = 0;
			hit.m_hit.m_barycentricCoordinate = glm::vec3(1.0f, 0.0f, 0.0f);
			GetShadingBasis(hit, outNormal, outTangent, outBitangent);
		}

		bool OrientAgainstRay(
			const glm::vec3& rayDirection,
			glm::vec3& inOutNormal,
			glm::vec3& inOutBitangent)
		{
			return OrientShadingBasisAgainstRay(
				rayDirection,
				inOutNormal,
				inOutBitangent);
		}
	};

	void Require(bool condition, const std::string& message)
	{
		if (!condition)
		{
			throw std::runtime_error(message);
		}
	}

	RHI::VertexP3N3T3B3UV2C4I4W4 MakeVertex(const glm::vec3& position)
	{
		RHI::VertexP3N3T3B3UV2C4I4W4 vertex{};
		vertex.m_position = position;
		vertex.m_normal = glm::vec3(0.0f, 0.0f, 1.0f);
		vertex.m_tangent = glm::vec3(1.0f, 0.0f, 0.0f);
		vertex.m_bitangent = glm::vec3(0.0f, 1.0f, 0.0f);
		return vertex;
	}

	bool NearlyEqual(float lhs, float rhs, float epsilon = 0.0001f)
	{
		return std::abs(lhs - rhs) <= epsilon;
	}

	void RequireVec3Near(
		const glm::vec3& actual,
		const glm::vec3& expected,
		const std::string& message)
	{
		Require(
			NearlyEqual(actual.x, expected.x) &&
				NearlyEqual(actual.y, expected.y) &&
				NearlyEqual(actual.z, expected.z),
			message);
	}

	Model::MeshCpuData MakeTriangleMesh(uint32_t thirdIndex)
	{
		Model::MeshCpuData mesh;
		mesh.m_vertices = {
			MakeVertex(glm::vec3(0.0f, 0.0f, 0.0f)),
			MakeVertex(glm::vec3(1.0f, 0.0f, 0.0f)),
			MakeVertex(glm::vec3(0.0f, 1.0f, 0.0f))
		};
		mesh.m_indices = { 0, 1, thirdIndex };
		return mesh;
	}

	void TestModelReadinessTracksMeshUploads()
	{
		auto firstMesh = TRefPtr<ControllableMesh>::Make();
		auto secondMesh = TRefPtr<ControllableMesh>::Make();
		Model model(
			FileId{},
			TVector<RHI::RHIMeshPtr>{ firstMesh, secondMesh });

		model.Flush();
		Require(!model.IsReady(),
			"a structurally complete model must wait for every mesh upload");

		firstMesh->SetReady(true);
		Require(!model.IsReady(),
			"one completed mesh upload must not publish the whole model");

		secondMesh->SetReady(true);
		Require(model.IsReady(),
			"the model must become ready without another Flush once all uploads finish");

		Model emptyModel(FileId{});
		emptyModel.Flush();
		Require(!emptyModel.IsReady(),
			"an empty model must remain unavailable");

		Model modelWithNullMesh(
			FileId{},
			TVector<RHI::RHIMeshPtr>{ RHI::RHIMeshPtr{} });
		modelWithNullMesh.Flush();
		Require(!modelWithNullMesh.IsReady(),
			"a model with a null mesh must remain unavailable");
	}

	void TestMeshContextRejectsEmptyGpuUploads()
	{
		ModelImporter::MeshContext emptyMesh;
		Require(!emptyMesh.HasGeometry(),
			"an empty mesh context must not reach the GPU upload path");

		emptyMesh.outVertices.Add(MakeVertex(glm::vec3(0.0f)));
		Require(!emptyMesh.HasGeometry(),
			"vertices without indices must not reach the GPU upload path");

		emptyMesh.outIndices = { 0, 0, 0 };
		Require(emptyMesh.HasGeometry(),
			"a mesh context with vertices and indices must remain uploadable");
	}

	void TestCompactedMeshesRetainMaterialSlots()
	{
		RHI::RHIMesh mesh;
		Require(mesh.ResolveMaterialIndex(1, 3) == 1,
			"generated meshes must retain positional material fallback");

		mesh.m_materialIndex = 2;
		Require(mesh.ResolveMaterialIndex(0, 3) == 2,
			"a compacted mesh must retain its original material slot");
		Require(mesh.ResolveMaterialIndex(0, 0) ==
			(std::numeric_limits<size_t>::max)(),
			"material resolution without materials must remain invalid");
	}

	void TestGeneratedTangentsPreserveMirroredUvHandedness()
	{
		const glm::vec3 vertices[] = {
			glm::vec3(0.0f, 0.0f, 0.0f),
			glm::vec3(1.0f, 0.0f, 0.0f),
			glm::vec3(0.0f, 1.0f, 0.0f)
		};
		const glm::vec2 regularUvs[] = {
			glm::vec2(0.0f, 0.0f),
			glm::vec2(1.0f, 0.0f),
			glm::vec2(0.0f, 1.0f)
		};
		const glm::vec2 mirroredUvs[] = {
			glm::vec2(0.0f, 0.0f),
			glm::vec2(-1.0f, 0.0f),
			glm::vec2(0.0f, 1.0f)
		};
		const glm::vec3 normal(0.0f, 0.0f, 1.0f);

		glm::vec3 tangent(0.0f);
		glm::vec3 bitangent(0.0f);
		Raytracing::GenerateTangentBitangent(
			tangent,
			bitangent,
			vertices,
			regularUvs);
		RequireVec3Near(tangent, glm::vec3(1.0f, 0.0f, 0.0f),
			"regular UVs must produce the expected tangent");
		RequireVec3Near(bitangent, glm::vec3(0.0f, 1.0f, 0.0f),
			"regular UVs must produce the expected bitangent");
		Require(glm::dot(glm::cross(normal, tangent), bitangent) > 0.0f,
			"regular UVs must retain positive tangent-space handedness");

		tangent = glm::vec3(0.0f);
		bitangent = glm::vec3(0.0f);
		Raytracing::GenerateTangentBitangent(
			tangent,
			bitangent,
			vertices,
			mirroredUvs);
		RequireVec3Near(tangent, glm::vec3(-1.0f, 0.0f, 0.0f),
			"mirrored UVs must produce the mirrored tangent");
		RequireVec3Near(bitangent, glm::vec3(0.0f, 1.0f, 0.0f),
			"mirrored UVs must retain the UV-derived bitangent");
		Require(glm::dot(glm::cross(normal, tangent), bitangent) < 0.0f,
			"mirrored UVs must retain negative tangent-space handedness");
	}

	void TestPathTracerTransformsShadingBasisCorrectly()
	{
		const glm::vec3 localNormal = glm::normalize(glm::vec3(1.0f, 0.0f, 1.0f));
		const glm::vec3 localTangent = glm::normalize(glm::vec3(1.0f, 0.0f, -1.0f));
		const glm::vec3 localBitangent = -glm::normalize(
			glm::cross(localNormal, localTangent));

		auto allocator = Memory::ObjectAllocatorPtr::Make(
			Memory::EAllocationPolicy::LocalMemory_SingleThread);
		ModelPtr model = ModelPtr::Make(allocator, FileId{});
		Model::MeshCpuData mesh = MakeTriangleMesh(2);
		for (auto& vertex : mesh.m_vertices)
		{
			vertex.m_normal = localNormal;
			vertex.m_tangent = localTangent;
			vertex.m_bitangent = localBitangent;
		}
		model->GetCpuMeshes().Add(std::move(mesh));
		Require(model->BuildBLAS(),
			"path tracer basis fixture must build its BLAS");

		const glm::mat4 worldMatrix = glm::scale(
			glm::mat4(1.0f),
			glm::vec3(2.0f, 1.0f, 0.5f));
		const glm::mat3 linearMatrix(worldMatrix);
		const glm::mat3 normalMatrix = glm::transpose(glm::inverse(linearMatrix));
		const glm::vec3 expectedNormal = glm::normalize(normalMatrix * localNormal);
		const glm::vec3 transformedTangent = linearMatrix * localTangent;
		const glm::vec3 expectedTangent = glm::normalize(
			transformedTangent - expectedNormal *
				glm::dot(expectedNormal, transformedTangent));
		const glm::vec3 transformedBitangent = linearMatrix * localBitangent;
		const float expectedHandedness = glm::dot(
			glm::cross(expectedNormal, expectedTangent),
			transformedBitangent) < 0.0f ? -1.0f : 1.0f;
		const glm::vec3 expectedBitangent = glm::normalize(
			glm::cross(expectedNormal, expectedTangent)) * expectedHandedness;

		PathTracerBasisProbe pathTracer;
		glm::vec3 actualNormal(0.0f);
		glm::vec3 actualTangent(0.0f);
		glm::vec3 actualBitangent(0.0f);
		pathTracer.Evaluate(
			model,
			worldMatrix,
			actualNormal,
			actualTangent,
			actualBitangent);

		RequireVec3Near(actualNormal, expectedNormal,
			"path tracer normals must use the inverse-transpose world transform");
		RequireVec3Near(actualTangent, expectedTangent,
			"path tracer tangents must use the linear world transform and Gram-Schmidt");
		RequireVec3Near(actualBitangent, expectedBitangent,
			"path tracer bitangents must retain tangent-space handedness");
		Require(NearlyEqual(glm::dot(actualNormal, actualTangent), 0.0f) &&
			NearlyEqual(glm::dot(actualNormal, actualBitangent), 0.0f) &&
			NearlyEqual(glm::dot(actualTangent, actualBitangent), 0.0f),
			"path tracer world-space TBN must remain orthogonal under non-uniform scale");

		const glm::vec3 frontNormal = actualNormal;
		const glm::vec3 frontTangent = actualTangent;
		const glm::vec3 frontBitangent = actualBitangent;
		Require(!pathTracer.OrientAgainstRay(
				frontNormal,
				actualNormal,
				actualBitangent),
			"a ray traveling with the shading normal must hit the back face");
		RequireVec3Near(actualNormal, -frontNormal,
			"back-face orientation must flip the shading normal");
		RequireVec3Near(actualTangent, frontTangent,
			"back-face orientation must preserve the tangent direction");
		RequireVec3Near(actualBitangent, -frontBitangent,
			"back-face orientation must flip the bitangent with the normal");
		Require(
			glm::dot(
				glm::cross(actualNormal, actualTangent),
				actualBitangent) *
			glm::dot(
				glm::cross(frontNormal, frontTangent),
				frontBitangent) > 0.0f,
			"back-face orientation must preserve tangent-space handedness");
	}

	void TestBuildBlasRejectsOutOfRangeIndicesAtomically()
	{
		Model model(FileId{});
		model.GetCpuMeshes() = {
			MakeTriangleMesh(2),
			MakeTriangleMesh(3)
		};

		Require(!model.BuildBLAS(),
			"BLAS construction must reject a mesh with an out-of-range index");
		Require(!model.HasBLAS(),
			"a rejected mesh must not retain a BLAS");
		Require(model.GetBLASTriangles().Num() == 0,
			"a rejected mesh must not retain triangles from earlier valid meshes");
		Require(!model.BuildBLAS(),
			"repeated BLAS construction on malformed geometry must remain safe");
		Require(!model.HasBLAS() && model.GetBLASTriangles().Num() == 0,
			"repeated rejection must preserve clean BLAS state");
	}

	void TestBuildBlasRecoversAfterGeometryIsCorrected()
	{
		Model model(FileId{});
		model.GetCpuMeshes() = { MakeTriangleMesh(3) };
		Require(!model.BuildBLAS(),
			"the malformed geometry precondition must be rejected");

		model.GetCpuMeshes() = { MakeTriangleMesh(2), MakeTriangleMesh(2) };
		Require(model.BuildBLAS(),
			"BLAS construction must recover after geometry is corrected");
		Require(model.HasBLAS(),
			"corrected geometry must produce a BLAS");
		Require(model.GetBLASTriangles().Num() == 2,
			"both corrected triangles must be included");
	}

	void TestBuildBlasIgnoresEmptyMeshes()
	{
		Model model(FileId{});
		model.GetCpuMeshes() = {
			Model::MeshCpuData(),
			MakeTriangleMesh(2)
		};
		Require(model.BuildBLAS(),
			"an empty mesh must not prevent valid geometry from building");
		Require(model.GetBLASTriangles().Num() == 1,
			"only the valid mesh must contribute a triangle");

		model.GetCpuMeshes() = { Model::MeshCpuData() };
		Require(!model.BuildBLAS(),
			"a model containing only empty meshes must be rejected");
		Require(!model.HasBLAS() && model.GetBLASTriangles().Num() == 0,
			"an empty model must leave BLAS state clean");
		Require(!model.GetBLAS().IsValid(),
			"a failed rebuild must release the previous BLAS");
	}

	void TestBuildBlasRejectsIncompleteAndNonFiniteGeometry()
	{
		Model model(FileId{});
		Model::MeshCpuData incomplete = MakeTriangleMesh(2);
		incomplete.m_indices.Add(0);
		model.GetCpuMeshes() = { std::move(incomplete) };
		Require(!model.BuildBLAS(),
			"BLAS construction must reject an incomplete triangle");
		Require(!model.HasBLAS() && model.GetBLASTriangles().Num() == 0,
			"an incomplete triangle must leave BLAS state clean");

		Model::MeshCpuData nonFinite = MakeTriangleMesh(2);
		nonFinite.m_vertices[0].m_position.x =
			std::numeric_limits<float>::quiet_NaN();
		model.GetCpuMeshes() = { std::move(nonFinite) };
		Require(!model.BuildBLAS(),
			"BLAS construction must reject non-finite positions");
		Require(!model.HasBLAS() && model.GetBLASTriangles().Num() == 0,
			"non-finite geometry must leave BLAS state clean");
	}

	void TestBuildBlasSanitizesExtremeVertexFrames()
	{
		Model model(FileId{});
		Model::MeshCpuData mesh = MakeTriangleMesh(2);
		const float maxValue = std::numeric_limits<float>::max();
		for (auto& vertex : mesh.m_vertices)
		{
			vertex.m_normal = glm::vec3(maxValue);
			vertex.m_tangent = glm::vec3(maxValue, -maxValue, 0.0f);
			vertex.m_bitangent = glm::vec3(0.0f, maxValue, -maxValue);
		}
		model.GetCpuMeshes() = { std::move(mesh) };

		Require(model.BuildBLAS(),
			"finite extreme vertex frames must be sanitized safely");
		Require(model.GetBLASTriangles().Num() == 1,
			"the sanitized triangle must be retained");
		const auto& triangle = model.GetBLASTriangles()[0];
		for (size_t i = 0; i < 3; ++i)
		{
			Require(
				std::isfinite(triangle.m_normals[i].x) &&
				std::isfinite(triangle.m_normals[i].y) &&
				std::isfinite(triangle.m_normals[i].z) &&
				std::isfinite(triangle.m_tangent[i].x) &&
				std::isfinite(triangle.m_tangent[i].y) &&
				std::isfinite(triangle.m_tangent[i].z) &&
				std::isfinite(triangle.m_bitangent[i].x) &&
				std::isfinite(triangle.m_bitangent[i].y) &&
				std::isfinite(triangle.m_bitangent[i].z),
				"sanitized vertex frames must remain finite");
		}
	}

	void TestBuildBlasHandlesExtremeCentroidRange()
	{
		Model model(FileId{});
		Model::MeshCpuData mesh;
		const float extremes[] = {
			std::numeric_limits<float>::lowest(),
			std::numeric_limits<float>::max(),
			-1.0f,
			0.0f,
			1.0f
		};

		for (float position : extremes)
		{
			const uint32_t firstVertex =
				static_cast<uint32_t>(mesh.m_vertices.Num());
			for (size_t i = 0; i < 3; ++i)
			{
				mesh.m_vertices.Add(MakeVertex(glm::vec3(position, 0.0f, 0.0f)));
				mesh.m_indices.Add(firstVertex + static_cast<uint32_t>(i));
			}
		}
		model.GetCpuMeshes() = { std::move(mesh) };

		Require(model.BuildBLAS(),
			"finite extreme centroid ranges must not corrupt BVH binning");
		Require(model.GetBLASTriangles().Num() == 5,
			"all extreme-range triangles must be retained");
	}

	void TestCollectMeshInstancesUsesActiveSceneHierarchy()
	{
		tinygltf::Model model;
		model.meshes.resize(1);
		model.nodes.resize(4);

		model.nodes[0].translation = { 10.0, 0.0, 0.0 };
		model.nodes[0].children = { 1 };
		model.nodes[1].mesh = 0;
		model.nodes[1].translation = { 0.0, 2.0, 0.0 };
		model.nodes[1].rotation = {
			0.0,
			0.0,
			0.7071067811865475,
			0.7071067811865476
		};
		model.nodes[1].scale = { 2.0, 1.0, 1.0 };

		model.nodes[2].mesh = 0;
		model.nodes[2].translation = { 100.0, 0.0, 0.0 };
		model.nodes[2].matrix = {
			1.0, 0.0, 0.0, 0.0,
			0.0, 1.0, 0.0, 0.0,
			0.0, 0.0, 1.0, 0.0,
			-4.0, 0.0, 0.0, 1.0
		};

		model.nodes[3].mesh = 0;
		model.nodes[3].translation = { 1000.0, 0.0, 0.0 };

		tinygltf::Scene scene;
		scene.nodes = { 0, 2 };
		model.scenes.push_back(std::move(scene));
		model.defaultScene = 0;

		TVector<GltfImporterUtils::MeshInstance> instances;
		Require(
			GltfImporterUtils::CollectMeshInstances(model, instances),
			"valid active-scene hierarchy must be traversed");
		Require(instances.Num() == 2,
			"one mesh resource referenced by two active nodes must emit two instances");
		Require(instances[0].m_nodeIndex == 1 &&
			instances[0].m_meshIndex == 0 &&
			instances[0].m_skinIndex == -1,
			"mesh instance must retain its source node, mesh, and skin indices");
		Require(instances[1].m_nodeIndex == 2 &&
			instances[1].m_meshIndex == 0,
			"matrix-authored mesh node must be emitted in active-scene order");

		const glm::mat4 unitScale =
			glm::scale(glm::mat4(1.0f), glm::vec3(2.0f));
		const glm::mat4 firstGeometryTransform =
			unitScale * instances[0].m_worldTransform;
		RequireVec3Near(
			glm::vec3(firstGeometryTransform *
				glm::vec4(0.0f, 0.0f, 0.0f, 1.0f)),
			glm::vec3(20.0f, 4.0f, 0.0f),
			"parent and child translations must accumulate and receive unit scale");
		RequireVec3Near(
			glm::vec3(firstGeometryTransform *
				glm::vec4(1.0f, 0.0f, 0.0f, 1.0f)),
			glm::vec3(20.0f, 8.0f, 0.0f),
			"node TRS must use glTF translation-rotation-scale order");
		RequireVec3Near(
			glm::vec3(firstGeometryTransform *
				glm::vec4(0.0f, 1.0f, 0.0f, 1.0f)),
			glm::vec3(18.0f, 4.0f, 0.0f),
			"non-uniform node scale and rotation must compose correctly");

		const glm::mat4 secondGeometryTransform =
			unitScale * instances[1].m_worldTransform;
		RequireVec3Near(
			glm::vec3(secondGeometryTransform *
				glm::vec4(0.0f, 0.0f, 0.0f, 1.0f)),
			glm::vec3(-8.0f, 0.0f, 0.0f),
			"node matrix must take precedence over TRS properties");
	}

	void TestUnitScaleDoesNotShrinkImportedDirections()
	{
		GltfImporterUtils::MeshInstance instance;
		glm::mat4 quarterTurn(1.0f);
		quarterTurn[0] = glm::vec4(0.0f, 1.0f, 0.0f, 0.0f);
		quarterTurn[1] = glm::vec4(-1.0f, 0.0f, 0.0f, 0.0f);
		instance.m_worldTransform =
			quarterTurn *
			glm::scale(glm::mat4(1.0f), glm::vec3(2.0f, 1.0f, 0.5f));

		const auto transforms =
			GltfImporterUtils::ResolveMeshInstanceTransforms(instance, 10000.0f);
		RequireVec3Near(
			glm::vec3(transforms.m_geometryTransform * glm::vec4(1.0f, 0.0f, 0.0f, 1.0f)),
			glm::vec3(0.0f, 20000.0f, 0.0f),
			"unit scale must still affect imported positions");

		const glm::mat3 normalTransform =
			glm::transpose(glm::inverse(transforms.m_directionTransform));
		const glm::vec3 importedNormal = normalTransform * glm::vec3(0.0f, 0.0f, 1.0f);
		Require(
			glm::length(importedNormal) > 1.0f,
			"large unit scale must not shrink imported normals below the sanitizer threshold");
		RequireVec3Near(
			glm::normalize(importedNormal),
			glm::vec3(0.0f, 0.0f, 1.0f),
			"node transforms must still be applied to imported normals");

		const auto mirroredTransforms =
			GltfImporterUtils::ResolveMeshInstanceTransforms(instance, -10000.0f);
		Require(
			glm::determinant(mirroredTransforms.m_directionTransform) < 0.0f,
			"negative unit scale must retain mirrored winding without scaling directions");
	}

	void TestCollectMeshInstancesRejectsCyclesAndPreservesLegacyMeshes()
	{
		tinygltf::Model cyclicModel;
		cyclicModel.meshes.resize(1);
		cyclicModel.nodes.resize(2);
		cyclicModel.nodes[0].mesh = 0;
		cyclicModel.nodes[0].children = { 1 };
		cyclicModel.nodes[1].children = { 0 };
		tinygltf::Scene cyclicScene;
		cyclicScene.nodes = { 0 };
		cyclicModel.scenes.push_back(std::move(cyclicScene));
		cyclicModel.defaultScene = 0;

		TVector<GltfImporterUtils::MeshInstance> instances;
		Require(
			!GltfImporterUtils::CollectMeshInstances(cyclicModel, instances),
			"cyclic glTF node hierarchies must be rejected");
		Require(instances.IsEmpty(),
			"failed traversal must not retain partial mesh instances");

		tinygltf::Model legacyModel;
		legacyModel.meshes.resize(2);
		Require(
			GltfImporterUtils::CollectMeshInstances(legacyModel, instances),
			"mesh-only glTF assets must retain legacy import support");
		Require(instances.Num() == 2 &&
			instances[0].m_meshIndex == 0 &&
			instances[1].m_meshIndex == 1,
			"mesh-only assets must emit one identity instance per mesh resource");
	}

	void TestCollectMeshInstancesHandlesDeepHierarchyIteratively()
	{
		constexpr size_t NumNodes = 8192;
		tinygltf::Model model;
		model.meshes.resize(1);
		model.nodes.resize(NumNodes);
		for (size_t nodeIndex = 0; nodeIndex + 1 < NumNodes; ++nodeIndex)
		{
			model.nodes[nodeIndex].children = {
				static_cast<int32_t>(nodeIndex + 1)
			};
		}
		model.nodes[NumNodes - 1].mesh = 0;
		tinygltf::Scene scene;
		scene.nodes = { 0 };
		model.scenes.push_back(std::move(scene));
		model.defaultScene = 0;

		TVector<GltfImporterUtils::MeshInstance> instances;
		Require(
			GltfImporterUtils::CollectMeshInstances(model, instances),
			"deep valid glTF hierarchies must not overflow the native stack");
		Require(instances.Num() == 1 &&
			instances[0].m_nodeIndex == static_cast<int32_t>(NumNodes - 1),
			"deep hierarchy traversal must reach the leaf mesh node");
	}
}

int main()
{
	const std::pair<const char*, std::function<void()>> tests[] = {
		{ "ModelReadinessTracksMeshUploads", TestModelReadinessTracksMeshUploads },
		{ "MeshContextRejectsEmptyGpuUploads", TestMeshContextRejectsEmptyGpuUploads },
		{ "CompactedMeshesRetainMaterialSlots", TestCompactedMeshesRetainMaterialSlots },
		{ "GeneratedTangentsPreserveMirroredUvHandedness", TestGeneratedTangentsPreserveMirroredUvHandedness },
		{ "PathTracerTransformsShadingBasisCorrectly", TestPathTracerTransformsShadingBasisCorrectly },
		{ "BuildBlasRejectsOutOfRangeIndicesAtomically", TestBuildBlasRejectsOutOfRangeIndicesAtomically },
		{ "BuildBlasRecoversAfterGeometryIsCorrected", TestBuildBlasRecoversAfterGeometryIsCorrected },
		{ "BuildBlasIgnoresEmptyMeshes", TestBuildBlasIgnoresEmptyMeshes },
		{ "BuildBlasRejectsIncompleteAndNonFiniteGeometry", TestBuildBlasRejectsIncompleteAndNonFiniteGeometry },
		{ "BuildBlasSanitizesExtremeVertexFrames", TestBuildBlasSanitizesExtremeVertexFrames },
		{ "BuildBlasHandlesExtremeCentroidRange", TestBuildBlasHandlesExtremeCentroidRange },
		{ "CollectMeshInstancesUsesActiveSceneHierarchy", TestCollectMeshInstancesUsesActiveSceneHierarchy },
		{ "UnitScaleDoesNotShrinkImportedDirections", TestUnitScaleDoesNotShrinkImportedDirections },
		{ "CollectMeshInstancesRejectsCyclesAndPreservesLegacyMeshes", TestCollectMeshInstancesRejectsCyclesAndPreservesLegacyMeshes },
		{ "CollectMeshInstancesHandlesDeepHierarchyIteratively", TestCollectMeshInstancesHandlesDeepHierarchyIteratively }
	};

	for (const auto& test : tests)
	{
		try
		{
			test.second();
			std::cout << "[PASS] " << test.first << std::endl;
		}
		catch (const std::exception& error)
		{
			std::cerr << "[FAIL] " << test.first << ": "
				<< error.what() << std::endl;
			return 1;
		}
	}

	return 0;
}
