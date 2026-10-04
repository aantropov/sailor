#include "AssetRegistry/Model/ModelImporter.h"
#include "Raytracing/PathTracer.h"
#include <glm/gtc/matrix_transform.hpp>

#include <chrono>
#include <cstddef>
#include <iostream>
#include <stdexcept>
#include <string>

using namespace Sailor;

namespace
{
	void Require(bool condition, const char* message)
	{
		if (!condition) throw std::runtime_error(message);
	}

	glm::vec3 NodePosition(uint32_t index)
	{
		return { float(index % 64) * 2.0f, float(index / 64) * 2.0f, 0 };
	}

	class RepeatedModel final : public Model
	{
	public:
		RepeatedModel(uint32_t numNodes, bool* destroyed = nullptr) : Model(FileId{}), m_destroyed(destroyed)
		{
			constexpr uint32_t Cells = 8;
			MeshCpuData mesh;
			mesh.m_materialIndex = 1;
			mesh.m_bounds = Math::AABB(glm::vec3(0.5f, 0.5f, 0), glm::vec3(0.5f, 0.5f, 0));
			for (uint32_t y = 0; y <= Cells; ++y)
				for (uint32_t x = 0; x <= Cells; ++x)
				{
					RHI::VertexP3N3T3B3UV2C4I4W4 vertex{};
					vertex.m_position = { float(x) / Cells, float(y) / Cells, 0 };
					vertex.m_normal = { 0, 0, 1 };
					vertex.m_tangent = { 1, 0, 0 };
					vertex.m_bitangent = { 0, 1, 0 };
					vertex.m_color = glm::vec4(1);
					vertex.m_texcoord = glm::vec2(vertex.m_position);
					mesh.m_vertices.Add(vertex);
				}
			for (uint32_t y = 0; y < Cells; ++y)
				for (uint32_t x = 0; x < Cells; ++x)
				{
					const uint32_t first = y * (Cells + 1) + x;
					for (uint32_t index : { first, first + 1, first + Cells + 2, first, first + Cells + 2, first + Cells + 1 })
						mesh.m_indices.Add(index);
				}
			m_sourceMeshes.Resize(1);
			m_sourceMeshes[0].m_renderMeshIndices.Add(0);
			m_sourceMeshes[0].m_bounds = mesh.m_bounds;
			m_cpuMeshes.Add(std::move(mesh));
			m_renderInstances.Reserve(numNodes);
			for (uint32_t index = 0; index < numNodes; ++index)
			{
				RenderInstance instance;
				instance.m_renderMeshIndex = 0;
				instance.m_nodeIndex = static_cast<int32_t>(index);
				instance.m_modelMatrix = glm::translate(glm::mat4(1), NodePosition(index));
				auto bounds = m_cpuMeshes[0].m_bounds;
				bounds.Apply(instance.m_modelMatrix);
				m_boundsAabb.Extend(bounds);
				m_renderInstances.Add(instance);
			}
		}
		~RepeatedModel() override { if (m_destroyed) *m_destroyed = true; }

		void SetNodeTransform(const glm::mat4& matrix)
		{
			m_renderInstances[0].m_modelMatrix = matrix;
			UpdateBounds();
		}

		void OffsetVertices(const glm::vec3& offset)
		{
			for (auto& vertex : m_cpuMeshes[0].m_vertices) vertex.m_position += offset;
			m_cpuMeshes[0].m_bounds.m_min += offset;
			m_cpuMeshes[0].m_bounds.m_max += offset;
			m_sourceMeshes[0].m_bounds = m_cpuMeshes[0].m_bounds;
			UpdateBounds();
		}

		void AddPrimitive()
		{
			auto mesh = m_cpuMeshes[0];
			mesh.m_materialIndex = 0;
			for (auto& vertex : mesh.m_vertices) vertex.m_position.x += 2;
			mesh.m_bounds.m_min.x += 2;
			mesh.m_bounds.m_max.x += 2;
			m_sourceMeshes[0].m_bounds.Extend(mesh.m_bounds);
			m_sourceMeshes[0].m_renderMeshIndices.Add(1);
			m_cpuMeshes.Add(std::move(mesh));
			auto instance = m_renderInstances[0];
			instance.m_renderMeshIndex = 1;
			m_renderInstances.Add(instance);
			UpdateBounds();
		}

	private:
		void UpdateBounds()
		{
			m_boundsAabb = {};
			for (const auto& instance : m_renderInstances)
			{
				auto bounds = m_cpuMeshes[instance.m_renderMeshIndex].m_bounds;
				bounds.Apply(instance.m_modelMatrix);
				m_boundsAabb.Extend(bounds);
			}
		}
		bool* m_destroyed;
	};

	class RayProbe final : public Raytracing::PathTracer
	{
	public:
		using PathTracer::IntersectScene;
		using PathTracer::TLASHit;
		using PathTracer::GetShadingBasis;
		using PathTracer::GetMaterialData;
		size_t InstanceCount() const { return m_geometry->m_tlasInstances.Num(); }
	};

	void VerifyHits(const ModelPtr& model, uint32_t numNodes, int32_t selection)
	{
		RayProbe tracer;
		Raytracing::PathTracer::TLASInstance instance;
		instance.m_model = model;
		instance.m_meshIndex = selection;
		instance.m_worldBounds = model->GetBoundsAABB(selection);
		auto material = TSharedPtr<Raytracing::PathTracer::MaterialSnapshot>::Make();
		Require(tracer.InitializeSceneSnapshot({ instance }, { material, material }, {}, false), "model must prepare for tracing");
		const auto& stats = tracer.GetLastScenePreparationStats();
		Require(stats.m_builtBlasCount == 0 && stats.m_reusedBlasCount == (selection == Model::AllMeshes ? numNodes : 1),
			"scene preparation must reuse model BLAS, including source selection");
		for (uint32_t index : { 0u, numNodes / 2, numNodes - 1 })
		{
			const glm::vec3 point = glm::vec3(0.217f, 0.331f, 0) + (selection == Model::AllMeshes ? NodePosition(index) : glm::vec3(0));
			RayProbe::TLASHit hit;
			Require(tracer.IntersectScene(Math::Ray(point + glm::vec3(0, 0, 2), glm::vec3(0, 0, -1)), hit), "full/subset ray must hit");
			Require(glm::length(hit.m_hit.m_point - point) < 1e-5f && hit.m_materialIndex == 1 &&
				glm::length(hit.m_hit.m_normal - glm::vec3(0, 0, 1)) < 1e-5f, "instancing must retain hit positions, normals and materials");
		}
	}

	void VerifyTransforms(const Memory::ObjectAllocatorPtr& allocator)
	{
		auto source = TObjectPtr<RepeatedModel>::Make(allocator, 1);
		const glm::mat4 outer = glm::translate(glm::mat4(1), glm::vec3(2, -1, 6)) *
			glm::rotate(glm::mat4(1), 0.4f, glm::normalize(glm::vec3(1, 2, 3))) *
			glm::scale(glm::mat4(1), glm::vec3(1.2f, 0.8f, 2));
		for (const glm::vec3 scale : { glm::vec3(2, 0.5f, 1.5f), glm::vec3(-1.5f, 2, 0.75f), glm::vec3(2, 1, 0) })
		{
			const glm::mat4 node = glm::translate(glm::mat4(1), glm::vec3(-4, 2, 0)) *
				glm::rotate(glm::mat4(1), 0.61f, glm::normalize(glm::vec3(2, 1, 3))) * glm::scale(glm::mat4(1), scale);
			source->SetNodeTransform(node);
			Require(source->BuildBLAS(), "scaled, mirrored and flattened surfaces must build");
			for (int32_t selection : { Model::AllMeshes, 0 })
			{
				RayProbe tracer;
				RayProbe::TLASInstance instance;
				instance.m_model = source.StaticCast<Model>();
				instance.m_meshIndex = selection;
				instance.m_worldMatrix = outer;
				instance.m_inverseWorldMatrix = glm::inverse(outer);
				instance.m_worldBounds = source->GetBoundsAABB(selection);
				instance.m_worldBounds.Apply(outer);
				instance.m_materialBaseOffset = 2;
				auto material = TSharedPtr<RayProbe::MaterialSnapshot>::Make();
				material->m_parameters.m_emissiveFactor = glm::vec3(1);
				Require(tracer.InitializeSceneSnapshot({ instance }, { material, material, material, material }, {}, false),
					"transformed model must prepare");
				const glm::mat4 matrix = selection == Model::AllMeshes ? outer * node : outer;
				const float area = glm::length(glm::cross(glm::vec3(matrix[0]), glm::vec3(matrix[1])));
				Require(tracer.GetLastScenePreparationStats().m_emissiveTriangleCount == 128 &&
					std::abs(tracer.GetLastScenePreparationStats().m_emissiveSamplingWeight - area) < 1e-4f,
					"emissive sampling must use each selected instance's world-space area");
				const glm::vec3 point = glm::vec3(matrix * glm::vec4(0.217f, 0.331f, 0, 1));
				const glm::vec3 geometricNormal = glm::normalize(glm::cross(glm::vec3(matrix[0]), glm::vec3(matrix[1])));
				const Math::Ray ray(point + 3.0f * geometricNormal, -geometricNormal);
				RayProbe::TLASHit hit;
				Require(tracer.IntersectScene(ray, hit, 3.1f) && glm::length(hit.m_hit.m_point - point) < 1e-4f &&
					hit.m_materialIndex == 3 && std::abs(hit.m_hit.m_rayLenght - 3.0f) < 1e-4f &&
					glm::length(hit.m_geometricNormal - geometricNormal) < 1e-4f,
					"combined node and world transforms must retain hit position, distance, winding and material offset");
				Require(!tracer.IntersectScene(ray, hit, 2.9f), "world-space ray length must clip scaled instances");
				if (scale.z != 0 || selection == 0)
				{
					Require(tracer.IntersectScene(ray, hit), "shading probe must hit");
					glm::vec3 normal, tangent, bitangent;
					tracer.GetShadingBasis(hit, normal, tangent, bitangent);
					const glm::vec3 expectedNormal = glm::normalize(glm::transpose(glm::inverse(glm::mat3(matrix))) * glm::vec3(0, 0, 1));
					const glm::vec3 expectedTangent = glm::normalize(glm::vec3(matrix[0]) - expectedNormal * glm::dot(expectedNormal, glm::vec3(matrix[0])));
					glm::vec3 expectedBitangent = glm::cross(expectedNormal, expectedTangent);
					if (glm::dot(expectedBitangent, glm::vec3(matrix[1])) < 0) expectedBitangent *= -1;
					Require(glm::length(normal - expectedNormal) < 1e-4f && glm::length(tangent - expectedTangent) < 1e-4f &&
						glm::length(bitangent - expectedBitangent) < 1e-4f, "instancing must preserve the transformed tangent frame");
				}
			}
		}
	}

	void VerifySnapshotLifetime(const Memory::ObjectAllocatorPtr& allocator)
	{
		bool destroyed = false;
		auto source = TObjectPtr<RepeatedModel>::Make(allocator, 1, &destroyed);
		Require(source->BuildBLAS(), "snapshot fixture must build");
		RayProbe original;
		RayProbe::TLASInstance instance;
		instance.m_model = source.StaticCast<Model>();
		instance.m_worldBounds = source->GetBoundsAABB();
		auto material = TSharedPtr<RayProbe::MaterialSnapshot>::Make();
		Require(original.InitializeSceneSnapshot({ instance }, { material, material }, {}, false), "live model must prepare");
		instance.m_modelGeometry = source->GetBLASGeometry();
		instance.m_model.Clear();
		source->OffsetVertices(glm::vec3(0, 0, 3));
		Require(source->BuildBLAS() && source->GetBLASGeometry() != instance.m_modelGeometry,
			"rebuilding must publish a new geometry without changing a frozen capture");
		RayProbe rebuilt;
		auto replacement = instance;
		replacement.m_modelGeometry = source->GetBLASGeometry();
		replacement.m_worldBounds = source->GetBoundsAABB();
		Require(rebuilt.InitializeSceneSnapshot({ replacement }, { material, material }, {}, false), "replacement geometry must prepare");
		source->GetCpuMeshes()[0].m_indices[0] = UINT32_MAX;
		Require(!source->BuildBLAS() && !source->HasBLAS(), "failed rebuild must clear only the model's current geometry");
		source->GetCpuMeshes().Clear();
		Require(!source->HasCpuMeshes(), "CPU buffers must be released");
		source.Clear();
		Require(destroyed, "prepared tracers and frozen captures must not retain the live model");
		RayProbe delayed;
		Require(delayed.InitializeSceneSnapshot({ instance }, { material, material }, {}, false),
			"a captured model must still prepare after reload failure, CPU release and model destruction");
		for (const RayProbe* tracer : { &original, &delayed, &rebuilt })
		{
			RayProbe::TLASHit hit;
			const float height = tracer == &rebuilt ? 3.0f : 0.0f;
			Require(tracer->IntersectScene(Math::Ray(glm::vec3(0.217f, 0.331f, 5), glm::vec3(0, 0, -1)), hit) &&
				std::abs(hit.m_hit.m_point.z - height) < 1e-5f && hit.m_materialIndex == 1,
				"old, delayed and replacement tracers must retain their own geometry and materials");
		}
	}

	void VerifyCancellationAndSelection(const Memory::ObjectAllocatorPtr& allocator)
	{
		auto source = TObjectPtr<RepeatedModel>::Make(allocator, 256);
		Require(source->BuildBLAS(), "cancellation fixture must build");
		RayProbe tracer;
		RayProbe::TLASInstance instance;
		instance.m_model = source.StaticCast<Model>();
		instance.m_meshIndex = 4;
		instance.m_worldBounds = source->GetBoundsAABB();
		auto material = TSharedPtr<RayProbe::MaterialSnapshot>::Make();
		uint32_t warnings = 0;
		Require(!tracer.InitializeSceneSnapshot({ instance }, { material, material }, {}, false, {}, true,
			[&](const std::string&) { ++warnings; }) && warnings == 1 &&
			tracer.GetLastScenePreparationStats().m_skippedInstanceCount == 1,
			"an absent source selection must keep its skipped-instance diagnostic");
		instance.m_meshIndex = Model::AllMeshes;
		bool cancelled = false;
		Require(!tracer.InitializeSceneSnapshot({ instance }, { material, material }, {}, false,
			[&](const RayProbe::ScenePreparationProgress& progress)
			{
				if (tracer.InstanceCount() == 64)
				{
					cancelled = progress.m_stage == RayProbe::EScenePreparationStage::Geometry && progress.m_total == 256;
					return false;
				}
				return true;
			}) && cancelled && tracer.InstanceCount() == 64 && tracer.GetLastScenePreparationStats().m_uniqueMaterialCount == 0,
			"cancellation must stop instance expansion before copying all nodes or preparing materials");
		Require(tracer.InitializeSceneSnapshot({ instance }, { material, material }, {}, false) &&
			tracer.GetLastScenePreparationStats().m_geometryInstanceCount == 256 &&
			tracer.GetLastScenePreparationStats().m_builtBlasCount == 0,
			"cancelled expansion must be retryable without rebuilding model BLAS");
	}

	void VerifyMultiPrimitiveSelection(const Memory::ObjectAllocatorPtr& allocator)
	{
		auto source = TObjectPtr<RepeatedModel>::Make(allocator, 1);
		source->AddPrimitive();
		Require(source->BuildBLAS(), "multi-primitive source mesh must build");
		const auto& full = source->GetBLASInstances();
		const auto& subset = source->GetBLASInstances(0);
		Require(full.Num() == 2 && subset.Num() == 2 && full[0].m_geometry == subset[0].m_geometry &&
			full[1].m_geometry == subset[1].m_geometry && full[0].m_geometry != full[1].m_geometry,
			"a source mesh must select and share all of its primitives");
		for (int32_t selection : { Model::AllMeshes, 0 })
		{
			RayProbe tracer;
			RayProbe::TLASInstance instance;
			instance.m_model = source.StaticCast<Model>();
			instance.m_meshIndex = selection;
			instance.m_worldBounds = source->GetBoundsAABB(selection);
			auto material = TSharedPtr<RayProbe::MaterialSnapshot>::Make();
			Require(tracer.InitializeSceneSnapshot({ instance }, { material, material }, {}, false), "multi-primitive selection must prepare");
			for (uint32_t index = 0; index < 2; ++index)
			{
				RayProbe::TLASHit hit;
				Require(tracer.IntersectScene(Math::Ray(glm::vec3(0.217f + 2 * index, 0.331f, 2), glm::vec3(0, 0, -1)), hit) &&
					hit.m_materialIndex == 1 - index && hit.m_instanceIndex == index,
					"each selected primitive must retain its own material and TLAS identity");
			}
		}
	}

	void VerifyWideMaterialSlots(const Memory::ObjectAllocatorPtr& allocator)
	{
		constexpr uint32_t SlotsPerInstance = 257;
		auto source = TObjectPtr<RepeatedModel>::Make(allocator, 1);
		source->AddPrimitive();
		source->GetCpuMeshes()[0].m_materialIndex = 255;
		source->GetCpuMeshes()[1].m_materialIndex = 256;
		Require(source->BuildBLAS(), "a model using material slots 255 and 256 must build");
		RayProbe::MaterialSnapshots materials;
		materials.Resize(2 * SlotsPerInstance);
		auto neutral = TSharedPtr<RayProbe::MaterialSnapshot>::Make();
		for (auto& material : materials) material = neutral;
		for (uint32_t i = 0; i < 4; ++i)
		{
			auto material = TSharedPtr<RayProbe::MaterialSnapshot>::Make();
			material->m_parameters.m_baseColorFactor = glm::vec4(0.1f * (i + 1), 0.25f, 0.7f, 1);
			material->m_parameters.m_roughnessFactor = 0.2f + 0.15f * i;
			material->m_parameters.m_metallicFactor = 0.1f + 0.2f * i;
			material->m_parameters.m_emissiveFactor = glm::vec3(i + 1, 4 - i, 0.25f * i);
			materials[(i / 2) * SlotsPerInstance + 255 + i % 2] = material;
		}
		RayProbe::Params params{};
		params.m_maxBounces = 1;
		params.m_bIncludeDirectLighting = params.m_bIncludeEnvironment = false;
		for (int32_t selection : { Model::AllMeshes, 0 })
		{
			RayProbe tracer;
			TVector<RayProbe::TLASInstance> instances;
			for (uint32_t index = 0; index < 2; ++index)
			{
				RayProbe::TLASInstance instance;
				instance.m_model = source.StaticCast<Model>();
				instance.m_meshIndex = selection;
				instance.m_materialBaseOffset = static_cast<int32_t>(index * SlotsPerInstance);
				instance.m_worldMatrix = glm::translate(glm::mat4(1), glm::vec3(0, 4 * index, 0));
				instance.m_inverseWorldMatrix = glm::inverse(instance.m_worldMatrix);
				instance.m_worldBounds = source->GetBoundsAABB(selection);
				instance.m_worldBounds.Apply(instance.m_worldMatrix);
				instances.Add(instance);
			}
			Require(tracer.InitializeSceneSnapshot(instances, materials, {}, false, {}, true),
				"wide material slots must resolve for both instances and source selections");
			Require(tracer.GetLastScenePreparationStats().m_materialSlotCount == materials.Num() &&
				tracer.GetLastScenePreparationStats().m_skippedInstanceCount == 0,
				"all 514 material slots must remain available without skipping an instance");
			for (uint32_t index = 0; index < 2; ++index)
			{
				for (uint32_t primitive = 0; primitive < 2; ++primitive)
				{
					const uint32_t expectedIndex = index * SlotsPerInstance + 255 + primitive;
					const glm::vec3 origin(0.217f + 2 * primitive, 0.331f + 4 * index, 2);
					const glm::vec3 direction(0, 0, -1);
					RayProbe::TLASHit hit;
					Require(tracer.IntersectScene(Math::Ray(origin, direction), hit), "each wide-slot primitive must be hit");
					if (hit.m_materialIndex != expectedIndex)
					{
						std::cerr << "Expected material " << expectedIndex << ", got " << hit.m_materialIndex << '\n';
						throw std::runtime_error("ray hits must preserve material slots above 255 and each instance base");
					}
					const auto& expected = materials[expectedIndex]->m_parameters;
					const auto sampled = tracer.GetMaterialData(hit.m_materialIndex, hit.m_hit.m_textureCoord);
					Require(glm::length(sampled.m_baseColor - expected.m_baseColorFactor) < 1e-5f &&
						std::abs(sampled.m_orm.y - expected.m_roughnessFactor) < 1e-5f &&
						std::abs(sampled.m_orm.z - expected.m_metallicFactor) < 1e-5f &&
						glm::length(sampled.m_emissive - expected.m_emissiveFactor) < 1e-5f,
						"the selected material must retain its own base color, roughness, metalness and emission");
					RayProbe::PreparedRaySample sample;
					Require(tracer.SamplePreparedSceneRay(origin, direction, 3, params, 17, sample) && sample.m_bHit &&
						std::abs(sample.m_distance - 2) < 1e-5f && glm::length(sample.m_radiance - expected.m_emissiveFactor) < 1e-5f,
						"the prepared ray must return the selected high-slot material's emitted radiance");
				}
			}
		}
		const auto& geometry = source->GetBLASInstances();
		Require(geometry[0].m_geometry->m_materialSlots == 256 && geometry[1].m_geometry->m_materialSlots == 257,
			"BLAS material-slot counts must include the full local material index");
	}

	void VerifyMaterialSlotBounds(const Memory::ObjectAllocatorPtr& allocator)
	{
		auto source = TObjectPtr<RepeatedModel>::Make(allocator, 1);
		for (int32_t slot : { -1, 0, 255, 256, 65535, INT32_MAX })
		{
			source->GetCpuMeshes()[0].m_materialIndex = slot;
			Require(source->BuildBLAS(), "every nonnegative int32 source slot must fit in CPU geometry");
			const uint32_t expected = slot < 0 ? 0 : static_cast<uint32_t>(slot);
			const auto& geometry = *source->GetBLASInstances()[0].m_geometry;
			Require(geometry.m_materialSlots == expected + 1, "the unassigned slot must keep its existing zero fallback");
			for (const auto& triangle : *geometry.m_triangles)
				Require(triangle.m_materialIndex == expected, "BLAS construction must retain the complete source slot");
		}
		auto material = TSharedPtr<RayProbe::MaterialSnapshot>::Make();
		for (int32_t base : { -INT32_MAX, INT32_MAX })
		{
			RayProbe tracer;
			RayProbe::TLASInstance instance;
			instance.m_model = source.StaticCast<Model>();
			instance.m_worldBounds = source->GetBoundsAABB();
			instance.m_materialBaseOffset = base;
			Require(tracer.InitializeSceneSnapshot({ instance }, { material, material }, {}, false),
				"the existing unresolved-slot fallback must remain available");
			RayProbe::TLASHit hit;
			Require(tracer.IntersectScene(Math::Ray(glm::vec3(0.217f, 0.331f, 2), glm::vec3(0, 0, -1)), hit) &&
				hit.m_materialIndex == (base < 0 ? 0u : 1u),
				"adding a wide local slot and signed instance base must not overflow before clamping");
		}
	}
}

int main(int argc, char** argv)
{
	try
	{
		const uint32_t numNodes = argc == 2 ? static_cast<uint32_t>(std::stoul(argv[1])) : 64u;
		Require(numNodes > 0 && numNodes <= 4096, "node count must be 1..4096");
		auto allocator = Memory::ObjectAllocatorPtr::Make(Memory::EAllocationPolicy::LocalMemory_SingleThread);
		auto source = TObjectPtr<RepeatedModel>::Make(allocator, numNodes);
		ModelPtr model = source.StaticCast<Model>();
		const auto start = std::chrono::steady_clock::now();
		Require(model->BuildBLAS(), "repeated model must build BLAS");
		const auto elapsed = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
		size_t storedTriangles = 0;
		TSet<const Model::BLASData*> uniqueGeometry;
		for (int32_t selection : { Model::AllMeshes, 0 })
			for (const auto& instance : model->GetBLASInstances(selection))
				if (!uniqueGeometry.Contains(instance.m_geometry.GetRawPtr()))
				{
					uniqueGeometry.Insert(instance.m_geometry.GetRawPtr());
					storedTriangles += instance.m_geometry->m_triangles->Num();
				}
		VerifyHits(model, numNodes, Model::AllMeshes);
		VerifyHits(model, numNodes, 0);
		std::cout << "nodes=" << numNodes << " base_triangles=128 stored_triangles=" << storedTriangles
			<< " build_ms=" << elapsed << std::endl;
		Require(storedTriangles == 128, "repeated nodes and source selection must share one geometry, not duplicate its triangles");
		if (argc == 1)
		{
			VerifyTransforms(allocator);
			VerifySnapshotLifetime(allocator);
			VerifyCancellationAndSelection(allocator);
			VerifyMultiPrimitiveSelection(allocator);
			std::cout << "cpu_triangle_bytes=" << sizeof(Math::Triangle) << " alignment=" << alignof(Math::Triangle)
				<< " material_offset=" << offsetof(Math::Triangle, m_materialIndex) << std::endl;
			VerifyWideMaterialSlots(allocator);
			VerifyMaterialSlotBounds(allocator);
		}
		return 0;
	}
	catch (const std::exception& error)
	{
		std::cerr << error.what() << std::endl;
		return 1;
	}
}
