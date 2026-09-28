#include "Sailor.h"
#include "AssetRegistry/AssetRegistry.h"
#include "AssetRegistry/Model/ModelImporter.h"
#include "AssetRegistry/Model/ModelLodCache.h"
#include "GraphicsDriver/Vulkan/VulkanApi.h"
#include "RHI/Renderer.h"

#include <array>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>

using namespace Sailor;

namespace
{
	using Geometry = ModelImporter::MeshContext::LodGeometry;

	void Require(bool value, const char* message)
	{
		if (!value) throw std::runtime_error(message);
	}

	void Drain()
	{
		auto* scheduler = App::GetSubmodule<Tasks::Scheduler>();
		scheduler->WaitIdle({ EThreadType::Worker, EThreadType::Render, EThreadType::RHI });
		scheduler->ProcessTasksOnMainThread();
		scheduler->WaitIdle({ EThreadType::Worker, EThreadType::Render, EThreadType::RHI });
		Require(GraphicsDriver::Vulkan::VulkanApi::GetInstance()->GetMainDevice()->WaitIdle() == VK_SUCCESS,
			"model LOD fixture GPU work must finish");
		RHI::Renderer::GetDriver()->TrackResources_ThreadSafe();
	}

	class FreshImporter final
	{
	public:
		FreshImporter() : m_importer(App::GetSubmodule<ModelAssetInfoHandler>(),
			App::GetSubmodule<Tasks::Scheduler>(), App::GetSubmodule<AssetRegistry>()) {}
		~FreshImporter()
		{
			App::GetSubmodule<Tasks::Scheduler>()->WaitIdle(
				{ EThreadType::Worker, EThreadType::Render, EThreadType::RHI });
			App::GetSubmodule<Tasks::Scheduler>()->ProcessTasksOnMainThread();
			App::GetSubmodule<ModelAssetInfoHandler>()->Unsubscribe(&m_importer);
		}

		ModelImporter m_importer;
	};

	struct ModelFixture
	{
		explicit ModelFixture(const std::filesystem::path& workspace) :
			m_path(workspace / "Content" / "ExternalLod.gltf"),
			m_verticesPath(workspace / "Content" / "Lod vertices.bin"),
			m_indicesPath(workspace / "Content" / "LodIndices.bin")
		{
			WriteVertices(0.0f);
			WriteIndices(false);
			{
				std::ofstream source(m_path);
				source << R"({
	"asset": {"version": "2.0"},
	"buffers": [{"uri": "Lod%20vertices.bin", "byteLength": 128}, {"uri": "LodIndices.bin", "byteLength": 24}],
	"bufferViews": [{"buffer": 0, "byteOffset": 0, "byteLength": 48},
		{"buffer": 0, "byteOffset": 48, "byteLength": 48},
		{"buffer": 0, "byteOffset": 96, "byteLength": 32}, {"buffer": 1, "byteLength": 24}],
	"accessors": [{"bufferView": 0, "componentType": 5126, "count": 4, "type": "VEC3"},
		{"bufferView": 1, "componentType": 5126, "count": 4, "type": "VEC3"},
		{"bufferView": 2, "componentType": 5126, "count": 4, "type": "VEC2"},
		{"bufferView": 3, "componentType": 5125, "count": 6, "type": "SCALAR"}],
	"meshes": [{"primitives": [{"attributes": {"POSITION": 0, "NORMAL": 1, "TEXCOORD_0": 2}, "indices": 3}]}],
	"nodes": [{"mesh": 0}], "scenes": [{"nodes": [0]}], "scene": 0
})";
			}
			ModelAssetInfo defaults;
			auto metadata = defaults.Serialize();
			m_id = FileId::CreateNewFileId();
			metadata["fileId"] = m_id;
			metadata["filename"] = m_path.filename().string();
			metadata["bGenerateLods"] = true;
			metadata["numGeneratedLods"] = 2;
			metadata["bShouldGenerateMaterials"] = false;
			metadata["bGenerateBLAS"] = false;
			metadata["bShouldKeepCpuBuffers"] = true;
			{
				std::ofstream sidecar(m_path.string() + ".asset");
				sidecar << metadata;
			}
			auto* registry = App::GetSubmodule<AssetRegistry>();
			Require(registry->GetOrLoadFile(m_path.string()) == m_id, "external-buffer model must register");
			m_info = registry->GetAssetInfoPtr<ModelAssetInfoPtr>(m_id);
			Require(m_info != nullptr, "model fixture metadata must resolve");
		}

		void WriteVertices(float offset) const
		{
			const std::array<float, 32> data{
				-1 + offset, -1, 0, 1 + offset, -1, 0, 1 + offset, 1, 0, -1 + offset, 1, 0,
				0, 0, 1, 0, 0, 1, 0, 0, 1, 0, 0, 1,
				0, 0.25f, 1, 0.25f, 1, 0.75f, 0, 0.75f };
			std::ofstream output(m_verticesPath, std::ios::binary);
			output.write(reinterpret_cast<const char*>(data.data()), sizeof(data));
			output.close();
			Require(static_cast<bool>(output), "external vertex buffer must be written");
		}

		void WriteIndices(bool otherDiagonal) const
		{
			const std::array<uint32_t, 6> indices = otherDiagonal ?
				std::array<uint32_t, 6>{ 0, 1, 3, 1, 2, 3 } : std::array<uint32_t, 6>{ 0, 1, 2, 0, 2, 3 };
			std::ofstream output(m_indicesPath, std::ios::binary);
			output.write(reinterpret_cast<const char*>(indices.data()), sizeof(indices));
			output.close();
			Require(static_cast<bool>(output), "external index buffer must be written");
		}

		std::filesystem::path CachePath(uint32_t level) const
		{
			return App::GetWorkspaceContext().GetCache() / "Lods" / ModelImporter::GetLodCacheFilename(m_id, level);
		}

		std::filesystem::path m_path, m_verticesPath, m_indicesPath;
		FileId m_id;
		ModelAssetInfoPtr m_info = nullptr;
	};

	TVector<Geometry> LoadGeometry(const ModelFixture& fixture, ModelImporter& importer)
	{
		ModelPtr model;
		Require(importer.LoadModel_Immediate(fixture.m_id, model) && model,
			"a fresh importer must load the external-buffer model");
		Drain();
		Require(model->IsReady() && model->GetMeshes().Num() == 1,
			"external-buffer model must upload its mesh");
		Require(model->GetCpuMeshes().Num() == 1, "fixture must retain imported source geometry");
		const auto& source = model->GetCpuMeshes()[0];
		TVector<ModelImporter::MeshContext> parsed(1);
		parsed[0].outVertices = source.m_vertices;
		parsed[0].outIndices = source.m_indices;
		FileRevision revision;
		Require(Utils::TryGetFileRevision(fixture.m_path.string(), revision), "the fixture source must have a revision");
		const auto& mesh = model->GetMeshes()[0];
		Require(mesh->GetNumLods() == 3, "source geometry and both generated LODs must upload");
		TVector<Geometry> result{ Geometry{ source.m_vertices, source.m_indices } };
		for (uint32_t level = 1; level < mesh->GetNumLods(); ++level)
		{
			Require(ModelLodCache::Load(*fixture.m_info, revision, level, parsed),
				"the real importer must publish a cache matching its source geometry");
			const auto lod = mesh->GetLod(level);
			Require(lod && lod->GetIndexCount() == parsed[0].lods[level - 1].m_indices.Num(),
				"uploaded LOD views must use the generated geometry's draw counts");
			result.Add(parsed[0].lods[level - 1]);
		}
		return result;
	}

	TVector<Geometry> LoadGeometry(const ModelFixture& fixture)
	{
		FreshImporter fresh;
		return LoadGeometry(fixture, fresh.m_importer);
	}

	void CheckLods(const TVector<Geometry>& geometry)
	{
		TVector<ModelImporter::MeshContext> expected(1);
		expected[0].outVertices = geometry[0].m_vertices;
		expected[0].outIndices = geometry[0].m_indices;
		ModelImporter::GenerateLods(expected, 2, 0.5f);
		for (size_t level = 1; level < geometry.Num(); ++level)
		{
			Require(geometry[level].m_vertices == expected[0].lods[level - 1].m_vertices &&
				geometry[level].m_indices == expected[0].lods[level - 1].m_indices,
				"every cached LOD must match fresh generation from the current imported geometry");
		}
	}
}

namespace Sailor::Tests
{
	void RunModelLodCommandTests(const std::filesystem::path& workspace)
	{
		ModelFixture fixture(workspace);
		const auto rootTime = std::filesystem::last_write_time(fixture.m_path);
		const auto verticesTime = std::filesystem::last_write_time(fixture.m_verticesPath);
		const auto indicesTime = std::filesystem::last_write_time(fixture.m_indicesPath);
		const auto first = LoadGeometry(fixture);
		CheckLods(first);
		const auto cacheTime = rootTime - std::chrono::hours(24);
		for (uint32_t level : { 1u, 2u }) std::filesystem::last_write_time(fixture.CachePath(level), cacheTime);
		const auto warm = LoadGeometry(fixture);
		CheckLods(warm);
		for (uint32_t level : { 1u, 2u })
			Require(std::filesystem::last_write_time(fixture.CachePath(level)) == cacheTime,
				"unchanged geometry must reuse both existing LOD cache files without rewriting");

		fixture.WriteVertices(11.0f);
		std::filesystem::last_write_time(fixture.m_verticesPath, verticesTime);
		const auto moved = LoadGeometry(fixture);
		Require(moved[0].m_vertices[0].m_position.x == first[0].m_vertices[0].m_position.x + 11.0f,
			"the source LOD must see the changed external vertex buffer");
		CheckLods(moved);
		fixture.WriteIndices(true);
		std::filesystem::last_write_time(fixture.m_indicesPath, indicesTime);
		const auto topology = LoadGeometry(fixture);
		Require(topology[0].m_indices != moved[0].m_indices,
			"the source LOD must see a same-size replacement of the external index buffer");
		CheckLods(topology);
		Require(std::filesystem::last_write_time(fixture.m_path) == rootTime &&
			std::filesystem::last_write_time(fixture.m_verticesPath) == verticesTime &&
			std::filesystem::last_write_time(fixture.m_indicesPath) == indicesTime,
			"external geometry changes must invalidate LODs without relying on any timestamp change");

		auto metadata = fixture.m_info->Serialize();
		metadata["unitScale"] = 2.0f;
		fixture.m_info->Deserialize(metadata);
		const auto scaled = LoadGeometry(fixture);
		Require(scaled[0].m_vertices[0].m_position == topology[0].m_vertices[0].m_position * 2.0f,
			"scale changes must reach source geometry");
		CheckLods(scaled);
		metadata["bFlipTexcoordY"] = true;
		fixture.m_info->Deserialize(metadata);
		const auto flipped = LoadGeometry(fixture);
		Require(flipped[0].m_vertices[0].m_texcoord.y == 1.0f - scaled[0].m_vertices[0].m_texcoord.y,
			"UV flip changes must reach source geometry");
		CheckLods(flipped);
		for (bool collectFailedAttempt : { false, true })
		{
			FreshImporter fresh;
			const auto heldPath = fixture.m_verticesPath.string() + ".hold";
			std::filesystem::rename(fixture.m_verticesPath, heldPath);
			ModelPtr placeholder;
			auto failed = fresh.m_importer.LoadModel(fixture.m_id, placeholder);
			Require(failed.IsValid(), "a registered model with a missing buffer must create an import task");
			failed->Wait();
			Drain();
			Require(!failed->GetResult() && placeholder && !placeholder->IsStructurallyReady(),
				"failed parsing must retain only the caller's unfinished placeholder");
			if (collectFailedAttempt) fresh.m_importer.CollectGarbage();
			std::filesystem::rename(heldPath, fixture.m_verticesPath);
			CheckLods(LoadGeometry(fixture, fresh.m_importer));
			ModelPtr repaired;
			Require(fresh.m_importer.LoadModel_Immediate(fixture.m_id, repaired) && repaired != placeholder && repaired->IsReady(),
				"repairing the buffer must upload a fresh model on the same importer and FileId");
			Require(!placeholder->IsStructurallyReady(), "retired failed placeholders must not turn into the repaired model");
			const auto missingId = FileId::CreateNewFileId();
			TObjectPtr<Object> asset = repaired;
			Require(!fresh.m_importer.LoadAsset(missingId, asset) && !asset,
				"the public immediate asset wrapper must clear its output for an unknown model ID");
			Require(!fresh.m_importer.LoadModel_Immediate(missingId, repaired) && !repaired,
				"the public model loader must clear its output for an unknown ID");
		}
		Drain();
		std::cout << "Model LOD external buffers, warm cache, native import and repaired model retry passed\n";
	}
}
