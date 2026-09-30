#include "Sailor.h"
#include "AssetRegistry/AssetRegistry.h"
#include "AssetRegistry/Model/ModelImporter.h"
#include "AssetRegistry/Model/ModelLodCache.h"
#include "GraphicsDriver/Vulkan/VulkanApi.h"
#include "RHI/Renderer.h"
#include "RHI/Buffer.h"
#include "Raytracing/PathTracer.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <future>
#include <iostream>
#include <stdexcept>

using namespace Sailor;

namespace Sailor
{
	class ModelImporterTestAccess
	{
	public:
		static void BeforeCpuPreparation(ModelImporter& importer, std::function<void()> callback)
		{
			importer.m_beforeCpuPreparationForTests = std::move(callback);
		}
	};
}

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
		explicit ModelFixture(const std::filesystem::path& workspace, bool instanced = false) :
			m_path(workspace / "Content" / (instanced ? "CpuHierarchy.gltf" : "ExternalLod.gltf")),
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
)";
				if (instanced)
				{
					source << R"(
	"materials": [{"name": "First"}, {"name": "Second"}],
	"meshes": [{"name": "Empty", "primitives": []},
		{"name": "First panel", "primitives": [{"attributes": {"POSITION": 0, "NORMAL": 1, "TEXCOORD_0": 2}, "indices": 3, "material": 0}]},
		{"name": "Second panel", "primitives": [{"attributes": {"POSITION": 0, "NORMAL": 1, "TEXCOORD_0": 2}, "indices": 3, "material": 1}]}],
	"nodes": [{"mesh": 1, "translation": [4,0,0]}, {"mesh": 2, "translation": [-4,0,0]},
		{"mesh": 1, "translation": [4,3,0]}], "scenes": [{"nodes": [0,1,2]}], "scene": 0
})";
				}
				else
				{
					source << R"(
	"meshes": [{"primitives": [{"attributes": {"POSITION": 0, "NORMAL": 1, "TEXCOORD_0": 2}, "indices": 3}]}],
	"nodes": [{"mesh": 0}], "scenes": [{"nodes": [0]}], "scene": 0
})";
				}
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

	class ModelRayProbe final : public Raytracing::PathTracer
	{
	public:
		using PathTracer::IntersectScene;
		using PathTracer::TLASHit;
	};

	void CheckImportedRayHits(const ModelPtr& model)
	{
		auto verifyHit = [&](int32_t selection, glm::vec3 point, uint32_t materialIndex)
		{
			ModelRayProbe tracer;
			Raytracing::PathTracer::TLASInstance instance;
			instance.m_model = model;
			instance.m_meshIndex = selection;
			instance.m_worldBounds = model->GetBoundsAABB(selection);
			auto material = TSharedPtr<Raytracing::PathTracer::MaterialSnapshot>::Make();
			Require(tracer.InitializeSceneSnapshot({ instance }, { material, material }, {}, false),
				"imported full and source-mesh BLAS must both prepare for ray tracing");
			ModelRayProbe::TLASHit hit;
			Require(tracer.IntersectScene(Math::Ray(point + glm::vec3(0, 0, 2), glm::vec3(0, 0, -1)), hit) &&
				glm::length(hit.m_hit.m_point - point) < 1e-5f &&
				hit.m_materialIndex == materialIndex && hit.m_instanceIndex == 0,
				"full and subset imports must retain the expected hit position and material");
		};
		verifyHit(Model::AllMeshes, { 4.25f, 0.125f, 0 }, 0);
		verifyHit(Model::AllMeshes, { -3.75f, 0.125f, 0 }, 1);
		verifyHit(Model::AllMeshes, { 4.25f, 3.125f, 0 }, 0);
		verifyHit(1, { 0.25f, 0.125f, 0 }, 0);
		verifyHit(2, { 0.25f, 0.125f, 0 }, 1);
	}

	void TestCpuPreparationDoesNotUseRhi(const ModelFixture& fixture)
	{
		auto* scheduler = App::GetSubmodule<Tasks::Scheduler>();
		const auto original = fixture.m_info->Serialize();
		for (bool generateBlas : { true, false })
		{
			for (bool keepCpu : { false, true })
			{
				auto settings = YAML::Clone(original);
				settings["bGenerateBLAS"] = generateBlas;
				settings["bShouldKeepCpuBuffers"] = keepCpu;
				fixture.m_info->Deserialize(settings);
				std::promise<void> entered, resume, uploadFinished;
				auto started = entered.get_future();
				auto released = resume.get_future();
				auto uploaded = uploadFinished.get_future();
				bool worker = false;
				FreshImporter fresh;
				ModelImporterTestAccess::BeforeCpuPreparation(fresh.m_importer, [&]()
					{
						worker = scheduler->GetCurrentThreadType() == EThreadType::Worker;
						entered.set_value();
						released.wait();
					});
				ModelPtr model;
				auto load = fresh.m_importer.LoadModel(fixture.m_id, model);
				const bool reached = started.wait_for(std::chrono::seconds(5)) == std::future_status::ready;
				const bool pending = !load->IsFinished() && !model->IsStructurallyReady();
				auto upload = Tasks::CreateTask<bool>(*scheduler, "Independent model-test buffer upload", [&]()
					{
						const std::array<uint32_t, 4> data{ 17, 29, 41, 53 };
						auto& driver = RHI::Renderer::GetDriver();
						auto source = driver->CreateBuffer_Immediate(data.data(), sizeof(data), RHI::EBufferUsageBit::BufferTransferSrc_Bit);
						auto readback = driver->CreateBuffer(sizeof(data), RHI::EBufferUsageBit::BufferTransferDst_Bit,
							RHI::EMemoryPropertyBit::HostVisible | RHI::EMemoryPropertyBit::HostCoherent);
						const bool copied = source && readback && driver->CopyBuffer_Immediate(source, readback, sizeof(data)) &&
							std::equal(data.begin(), data.end(), static_cast<const uint32_t*>(readback->GetPointer()));
						uploadFinished.set_value();
						return copied;
					}, EThreadType::RHI);
				upload->Run();
				const bool concurrentUpload = uploaded.wait_for(std::chrono::seconds(5)) == std::future_status::ready;
				resume.set_value();
				load->Wait();
				upload->Wait();
				Drain();
				Require(reached && pending && worker, "model CPU preparation must execute on Worker before RHI publication");
				Require(concurrentUpload && upload->GetResult(), "paused CPU preparation must not delay an unrelated RHI upload/readback");
				Require(load->GetResult() == model && model->IsReady() && model->HasCpuMeshes() == keepCpu &&
					model->HasBLAS() == generateBlas && !model->HasBLAS(0) &&
					model->HasBLAS(1) == generateBlas && model->HasBLAS(2) == generateBlas &&
					model->GetMeshes().Num() == 2 && model->GetRenderInstances().Num() == 3 &&
					model->GetSourceMeshes().Num() == 3 && !model->IsSourceMeshIndexValid(0) &&
					model->GetSourceMeshes()[1].m_name == "First panel" && model->GetSourceMeshes()[2].m_name == "Second panel",
					"all BLAS/CPU-retention combinations must publish the same GPU geometry and hierarchy");
				for (const auto& mesh : model->GetMeshes())
				{
					Require(mesh->GetIndexCount() == 6 && mesh->GetNumLods() == 3,
						"CPU preparation must preserve every uploaded base mesh and its LODs");
				}
				if (generateBlas) CheckImportedRayHits(model);
			}
		}
		fixture.m_info->Deserialize(original);
		std::cout << "Worker CPU preparation, independent RHI upload and full/subset ray hits passed\n";
	}

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
		ModelFixture cpuFixture(workspace, true);
		TestCpuPreparationDoesNotUseRhi(cpuFixture);
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
		auto* scheduler = App::GetSubmodule<Tasks::Scheduler>();
		const auto previewId = fixture.m_id.ToString();
		const auto preview = workspace / "Cache" / "Fingerprints" / (previewId + ".png");
		Require(App::RequestModelFingerprint(previewId.c_str()), "the editor bridge must accept a registered model preview");
		scheduler->WaitIdle({ EThreadType::Background, EThreadType::Main });
		Require(App::GetModelFingerprintStatus(previewId.c_str()) == static_cast<uint32_t>(ModelImporter::EFingerprintStatus::Ready) &&
			std::filesystem::is_regular_file(preview) && std::filesystem::file_size(preview) > 0,
			"the initialized engine must publish the requested preview into the active workspace cache");
		const auto modelTime = std::filesystem::last_write_time(fixture.m_path);
		Require(std::filesystem::remove(preview), "remove only the generated fixture preview");
		Require(App::RequestModelFingerprint(previewId.c_str()), "the editor bridge must permit retrying a missing preview");
		scheduler->WaitIdle({ EThreadType::Background, EThreadType::Main });
		Require(App::GetModelFingerprintStatus(previewId.c_str()) == static_cast<uint32_t>(ModelImporter::EFingerprintStatus::Ready) &&
			std::filesystem::is_regular_file(preview) && std::filesystem::last_write_time(fixture.m_path) == modelTime,
			"native preview retry must complete without another model edit");
		std::cout << "Explicit model preview bridge, workspace cache and missing-output retry passed\n";
		std::cout << "Model LOD external buffers, warm cache, native import and repaired model retry passed\n";
	}
}
