#include "AssetRegistry/Model/ModelImporter.h"

#include "AssetRegistry/Model/ModelGeometry.h"
#include "Math/Math.h"

#include <algorithm>
#include <cmath>
#include <limits>

using namespace Sailor;

YAML::Node Model::Serialize() const
{
	YAML::Node res;
	SERIALIZE_PROPERTY(res, m_fileId);
	return res;
}

void Model::Deserialize(const YAML::Node& inData)
{
	DESERIALIZE_PROPERTY(inData, m_fileId);
}

bool Model::IsSourceMeshIndexValid(int32_t meshIndex) const
{
	return meshIndex >= 0 && static_cast<size_t>(meshIndex) < m_sourceMeshes.Num() &&
		   !m_sourceMeshes[static_cast<size_t>(meshIndex)].m_renderMeshIndices.IsEmpty();
}

const Math::AABB& Model::GetBoundsAABB(int32_t meshIndex) const
{
	if (meshIndex == AllMeshes)
	{
		return m_boundsAabb;
	}

	static const Math::AABB emptyBounds{};
	return IsSourceMeshIndexValid(meshIndex) ? m_sourceMeshes[static_cast<size_t>(meshIndex)].m_bounds : emptyBounds;
}

bool Model::HasBLAS(int32_t meshIndex) const
{
	return !GetBLASInstances(meshIndex).IsEmpty();
}

const TVector<Model::BLASInstance>& Model::BLASGeometry::GetInstances(int32_t meshIndex) const
{
	if (meshIndex == AllMeshes)
	{
		return m_instances;
	}

	static const TVector<BLASInstance> empty;
	return meshIndex >= 0 && static_cast<size_t>(meshIndex) < m_sourceMeshes.Num() ? m_sourceMeshes[meshIndex] : empty;
}

const TVector<Model::BLASInstance>& Model::GetBLASInstances(int32_t meshIndex) const
{
	static const TVector<BLASInstance> empty;
	return m_blasGeometry ? m_blasGeometry->GetInstances(meshIndex) : empty;
}

bool Model::CollectRenderData(int32_t meshIndex,
	TVector<RHI::RHIMeshPtr>& outMeshes,
	TVector<glm::mat4>& outModelMatrices,
	Math::AABB& outBounds) const
{
	outMeshes.Clear();
	outModelMatrices.Clear();
	outBounds = Math::AABB();

	if (meshIndex == AllMeshes)
	{
		outMeshes.Reserve(m_renderInstances.Num());
		outModelMatrices.Reserve(m_renderInstances.Num());
		for (const RenderInstance& instance : m_renderInstances)
		{
			if (instance.m_renderMeshIndex >= m_meshes.Num() || !m_meshes[instance.m_renderMeshIndex])
			{
				continue;
			}

			const RHI::RHIMeshPtr& mesh = m_meshes[instance.m_renderMeshIndex];
			outMeshes.Add(mesh);
			outModelMatrices.Add(instance.m_modelMatrix);
			Math::AABB instanceBounds = mesh->m_bounds;
			instanceBounds.Apply(instance.m_modelMatrix);
			outBounds.Extend(instanceBounds);
		}
	}
	else if (IsSourceMeshIndexValid(meshIndex))
	{
		const SourceMesh& sourceMesh = m_sourceMeshes[static_cast<size_t>(meshIndex)];
		outMeshes.Reserve(sourceMesh.m_renderMeshIndices.Num());
		outModelMatrices.Reserve(sourceMesh.m_renderMeshIndices.Num());
		for (uint32_t renderMeshIndex : sourceMesh.m_renderMeshIndices)
		{
			if (renderMeshIndex >= m_meshes.Num() || !m_meshes[renderMeshIndex])
			{
				continue;
			}

			outMeshes.Add(m_meshes[renderMeshIndex]);
			outModelMatrices.Add(glm::mat4(1.0f));
		}
		outBounds = sourceMesh.m_bounds;
	}

	return !outMeshes.IsEmpty() && outMeshes.Num() == outModelMatrices.Num() && outBounds.IsValid();
}

void Model::Flush()
{
	const uint64_t inverseBindHash = HashBytes(m_inverseBind.GetData(), m_inverseBind.Num() * sizeof(glm::mat4));
	if (inverseBindHash != m_inverseBindHash)
	{
		m_inverseBindHash = inverseBindHash;
		++m_skeletonRevision;
	}
	m_bGpuReady.store(false, std::memory_order_release);

	if (m_meshes.Num() == 0)
	{
		m_bIsReady.store(false, std::memory_order_release);
		return;
	}

	for (const auto& mesh : m_meshes)
	{
		if (!mesh)
		{
			m_bIsReady.store(false, std::memory_order_release);
			return;
		}
	}

	// Flush publishes a structurally complete model. GPU uploads are asynchronous,
	// so IsReady() also checks every RHIMesh until its upload fence is finished.
	m_bIsReady.store(true, std::memory_order_release);
}

bool Model::BuildBLASData(const TVector<RenderInstance>& blasInstances, BLASData& outData) const
{
	outData = BLASData{};

	if (m_cpuMeshes.Num() == 0 || blasInstances.IsEmpty())
	{
		return false;
	}

	size_t expectedNumTriangles = 0;
	constexpr size_t maxNumTriangles = (static_cast<size_t>(std::numeric_limits<uint32_t>::max()) + 1) / 2;
	for (const RenderInstance& instance : blasInstances)
	{
		if (instance.m_renderMeshIndex >= m_cpuMeshes.Num())
		{
			return false;
		}
		const MeshCpuData& mesh = m_cpuMeshes[instance.m_renderMeshIndex];
		if (mesh.m_indices.Num() % 3 != 0)
		{
			return false;
		}

		const size_t meshTriangles = mesh.m_indices.Num() / 3;
		if (meshTriangles > maxNumTriangles - expectedNumTriangles)
		{
			return false;
		}

		expectedNumTriangles += meshTriangles;
	}

	if (expectedNumTriangles == 0)
	{
		return false;
	}

	outData.m_triangles = TSharedPtr<TVector<Math::Triangle>>::Make();
	outData.m_triangles->Reserve(expectedNumTriangles);

	for (const RenderInstance& instance : blasInstances)
	{
		const MeshCpuData& mesh = m_cpuMeshes[instance.m_renderMeshIndex];
		const glm::mat3 linearMatrix(instance.m_modelMatrix);
		glm::mat3 normalMatrix = linearMatrix;
		const float determinant = glm::determinant(linearMatrix);
		if (std::isfinite(determinant) && determinant != 0.0f)
		{
			const glm::mat3 inverseTranspose = glm::transpose(glm::inverse(linearMatrix));
			if (Math::AllFinite(inverseTranspose))
			{
				normalMatrix = inverseTranspose;
			}
		}

		for (size_t i = 0; i + 2 < mesh.m_indices.Num(); i += 3)
		{
			const uint32_t i0 = mesh.m_indices[i + 0];
			const uint32_t i1 = mesh.m_indices[i + 1];
			const uint32_t i2 = mesh.m_indices[i + 2];
			if (i0 >= mesh.m_vertices.Num() || i1 >= mesh.m_vertices.Num() || i2 >= mesh.m_vertices.Num())
			{
				outData.m_triangles->Clear();
				return false;
			}

			auto v0 = mesh.m_vertices[i0];
			auto v1 = mesh.m_vertices[i1];
			auto v2 = mesh.m_vertices[i2];
			auto applyInstanceTransform = [&](auto& vertex)
			{
				vertex.m_position = glm::vec3(instance.m_modelMatrix * glm::vec4(vertex.m_position, 1.0f));
				vertex.m_normal = normalMatrix * vertex.m_normal;
				vertex.m_tangent = linearMatrix * vertex.m_tangent;
				vertex.m_bitangent = linearMatrix * vertex.m_bitangent;
			};
			applyInstanceTransform(v0);
			applyInstanceTransform(v1);
			applyInstanceTransform(v2);
			if (!Math::AllFinite(v0.m_position) || !Math::AllFinite(v1.m_position) || !Math::AllFinite(v2.m_position))
			{
				outData.m_triangles->Clear();
				return false;
			}

			const glm::vec3 edge1 = v1.m_position - v0.m_position;
			const glm::vec3 edge2 = v2.m_position - v0.m_position;
			const glm::vec3 triangleNormal = glm::cross(edge1, edge2);
			if (!Math::AllFinite(edge1) || !Math::AllFinite(edge2) || !Math::AllFinite(triangleNormal) ||
				!std::isfinite(glm::dot(triangleNormal, triangleNormal)))
			{
				outData.m_triangles->Clear();
				return false;
			}

			ModelGeometry::SanitizeVertexFrame(v0);
			ModelGeometry::SanitizeVertexFrame(v1);
			ModelGeometry::SanitizeVertexFrame(v2);
			if (!Math::AllFinite(v0.m_normal) || !Math::AllFinite(v1.m_normal) || !Math::AllFinite(v2.m_normal) ||
				!Math::AllFinite(v0.m_tangent) || !Math::AllFinite(v1.m_tangent) || !Math::AllFinite(v2.m_tangent) ||
				!Math::AllFinite(v0.m_bitangent) || !Math::AllFinite(v1.m_bitangent) ||
				!Math::AllFinite(v2.m_bitangent))
			{
				outData.m_triangles->Clear();
				return false;
			}

			Math::Triangle tri{};
			tri.m_vertices[0] = v0.m_position;
			tri.m_vertices[1] = v1.m_position;
			tri.m_vertices[2] = v2.m_position;

			tri.m_normals[0] = v0.m_normal;
			tri.m_normals[1] = v1.m_normal;
			tri.m_normals[2] = v2.m_normal;

			tri.m_tangent[0] = v0.m_tangent;
			tri.m_tangent[1] = v1.m_tangent;
			tri.m_tangent[2] = v2.m_tangent;

			tri.m_bitangent[0] = v0.m_bitangent;
			tri.m_bitangent[1] = v1.m_bitangent;
			tri.m_bitangent[2] = v2.m_bitangent;

			tri.m_uvs[0] = Math::AllFinite(v0.m_texcoord) ? v0.m_texcoord : glm::vec2(0.0f);
			tri.m_uvs[1] = Math::AllFinite(v1.m_texcoord) ? v1.m_texcoord : glm::vec2(0.0f);
			tri.m_uvs[2] = Math::AllFinite(v2.m_texcoord) ? v2.m_texcoord : glm::vec2(0.0f);
			tri.m_uvs2[0] = tri.m_uvs[0];
			tri.m_uvs2[1] = tri.m_uvs[1];
			tri.m_uvs2[2] = tri.m_uvs[2];
			tri.m_colors[0] = Math::AllFinite(v0.m_color) ? v0.m_color : glm::vec4(1.0f);
			tri.m_colors[1] = Math::AllFinite(v1.m_color) ? v1.m_color : glm::vec4(1.0f);
			tri.m_colors[2] = Math::AllFinite(v2.m_color) ? v2.m_color : glm::vec4(1.0f);

			tri.m_materialIndex = static_cast<uint32_t>((std::max)(0, mesh.m_materialIndex));
			tri.m_centroid = tri.m_vertices[0] / 3.0f + tri.m_vertices[1] / 3.0f + tri.m_vertices[2] / 3.0f;
			if (!Math::AllFinite(tri.m_centroid))
			{
				outData.m_triangles->Clear();
				return false;
			}

			outData.m_bounds.Extend(v0.m_position);
			outData.m_bounds.Extend(v1.m_position);
			outData.m_bounds.Extend(v2.m_position);
			outData.m_materialSlots = (std::max)(outData.m_materialSlots, tri.m_materialIndex + 1u);
			outData.m_triangles->Add(tri);
		}
	}

	if (outData.m_triangles->Num() == 0)
	{
		return false;
	}

	outData.m_blas = TSharedPtr<Raytracing::BVH>::Make(static_cast<uint32_t>(outData.m_triangles->Num()));
	outData.m_blas->BuildBVH(*outData.m_triangles);
	return true;
}

bool Model::BuildBLAS()
{
	m_blasGeometry.Clear();
	auto geometry = TSharedPtr<BLASGeometry>::Make();
	TVector<TSharedPtr<const BLASData>> meshes(m_cpuMeshes.Num());
	for (size_t index = 0; index < m_cpuMeshes.Num(); ++index)
	{
		if (m_cpuMeshes[index].m_indices.IsEmpty())
		{
			continue;
		}
		RenderInstance source;
		source.m_renderMeshIndex = static_cast<uint32_t>(index);
		auto data = TSharedPtr<BLASData>::Make();
		if (BuildBLASData({ source }, *data))
		{
			meshes[index] = std::move(data);
		}
	}

	auto appendInstance = [&](const RenderInstance& source) -> bool
	{
		if (source.m_renderMeshIndex >= meshes.Num())
		{
			return false;
		}
		if (m_cpuMeshes[source.m_renderMeshIndex].m_indices.IsEmpty())
		{
			return true;
		}
		BLASInstance instance;
		instance.m_geometry = meshes[source.m_renderMeshIndex];
		if (!instance.m_geometry)
		{
			return false;
		}
		instance.m_modelMatrix = source.m_modelMatrix;
		instance.m_inverseModelMatrix = glm::inverse(source.m_modelMatrix);
		if (!Math::AllFinite(instance.m_inverseModelMatrix))
		{
			// A flattened axis can still leave a valid surface. Such geometry cannot use an inverse ray transform.
			auto baked = TSharedPtr<BLASData>::Make();
			if (!BuildBLASData({ source }, *baked))
			{
				return false;
			}
			instance.m_geometry = std::move(baked);
			instance.m_modelMatrix = instance.m_inverseModelMatrix = glm::mat4(1);
		}
		geometry->m_instances.Add(std::move(instance));
		return true;
	};
	if (m_renderInstances.IsEmpty())
	{
		for (size_t index = 0; index < m_cpuMeshes.Num(); ++index)
		{
			RenderInstance instance;
			instance.m_renderMeshIndex = static_cast<uint32_t>(index);
			if (!appendInstance(instance))
			{
				return false;
			}
		}
	}
	else
	{
		geometry->m_instances.Reserve(m_renderInstances.Num());
		for (const auto& instance : m_renderInstances)
		{
			if (!appendInstance(instance))
			{
				return false;
			}
		}
	}
	if (geometry->m_instances.IsEmpty())
	{
		return false;
	}

	geometry->m_sourceMeshes.Resize(m_sourceMeshes.Num());
	for (size_t index = 0; index < m_sourceMeshes.Num(); ++index)
	{
		auto& instances = geometry->m_sourceMeshes[index];
		for (uint32_t renderMesh : m_sourceMeshes[index].m_renderMeshIndices)
		{
			if (renderMesh >= meshes.Num() || (!meshes[renderMesh] && !m_cpuMeshes[renderMesh].m_indices.IsEmpty()))
			{
				instances.Clear();
				break;
			}
			if (meshes[renderMesh])
			{
				instances.Add({ meshes[renderMesh] });
			}
		}
	}
	m_blasGeometry = std::move(geometry);
	return true;
}

void Model::ProceedCpuMeshes(bool bShouldGenerateBLAS, bool bShouldKeepCpuBuffers)
{
	if (bShouldGenerateBLAS)
	{
		BuildBLAS();
	}
	else
	{
		m_blasGeometry.Clear();
	}

	if (!bShouldKeepCpuBuffers)
	{
		m_cpuMeshes.Clear();
	}
}

bool Model::IsReady() const
{
	if (!IsStructurallyReady())
	{
		return false;
	}

	// GPU upload completion is monotonic until the next Flush(). Large editable
	// model hierarchies can reference one Model from thousands of renderers, so
	// rescanning every RHIMesh for every component would be O(N^2) per frame.
	if (m_bGpuReady.load(std::memory_order_acquire))
	{
		return true;
	}

	for (const auto& mesh : m_meshes)
	{
		if (!mesh || !mesh->IsReady())
		{
			return false;
		}
	}

	m_bGpuReady.store(true, std::memory_order_release);
	return true;
}

bool Model::IsStructurallyReady() const
{
	return m_bIsReady.load(std::memory_order_acquire);
}
