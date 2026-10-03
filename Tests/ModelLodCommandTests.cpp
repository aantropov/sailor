#include "Sailor.h"
#include "Core/FileRevision.h"
#include "Core/YamlUtils.h"
#include "AssetRegistry/AssetRegistry.h"
#include "AssetRegistry/Material/MaterialImporter.h"
#include "AssetRegistry/Texture/TextureAssetInfo.h"
#include "AssetRegistry/Model/ModelImporter.h"
#include "AssetRegistry/Model/ModelLodCache.h"
#include "GraphicsDriver/Vulkan/VulkanApi.h"
#include "RHI/Renderer.h"
#include "RHI/Buffer.h"
#include "Raytracing/PathTracer.h"
#include "Support/EditorProtocolWire.h"
#include "Support/SurfaceRender.h"
#include "EditorEngineProtocolLifecycle.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <future>
#include <iostream>
#include <map>
#include <stdexcept>
#include <nlohmann/json.hpp>

using namespace Sailor;

extern "C" SAILOR_SHARED_API void SailorProtocolFreeBuffer(uint8_t* buffer) noexcept;

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

	bool UpdateAssetThroughEditor(const std::string& fileId, bool reimport)
	{
		using namespace Tests::ProtocolWire;
		Protocol::TEditorEngineProtocolLifecycleGate gate;
		std::string error;
		Require(gate.TryBeginInitialization(error), "the native editor protocol fixture must initialize");
		gate.CompleteInitialization(true);
		Protocol::EditorEngineProtocolDependencies dependencies;
		dependencies.m_lifecycleGate = &gate;
		std::string command;
		AppendBytesField(command, 1u, fileId);
		AppendVarintField(command, 2u, reimport);
		const auto request = MakeRequest(148u, 57u, command);
		uint8_t* bytes = nullptr;
		uint32_t size = 0;
		const auto status = Protocol::InvokeEditorEngineProtocol(reinterpret_cast<const uint8_t*>(request.data()),
			static_cast<uint32_t>(request.size()), &bytes, &size, dependencies);
		const std::string payload(bytes ? reinterpret_cast<const char*>(bytes) : "", size);
		SailorProtocolFreeBuffer(bytes);
		TProtocolResponseWire response;
		Require(status == static_cast<int32_t>(Protocol::EEditorEngineTransportStatus::Ok) &&
			ParseResponse(payload, response) && response.m_bSuccess && response.m_protocolVersion == 1 &&
			response.m_requestId == 148u && response.m_resultField == 11u,
			"the native editor update command must return its typed bool result");
		if (response.m_resultPayload.empty()) return false;
		size_t offset = 0;
		uint64_t key = 0, value = 0;
		Require(ReadVarint(response.m_resultPayload, offset, key) && key == 8u &&
			ReadVarint(response.m_resultPayload, offset, value) && offset == response.m_resultPayload.size(),
			"the native editor bool result must decode");
		return value != 0;
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

		void WriteGeometry(const Geometry& geometry) const
		{
			TVector<float> attributes;
			for (const auto& vertex : geometry.m_vertices)
				attributes.AddRange({ vertex.m_position.x, vertex.m_position.y, vertex.m_position.z });
			for (const auto& vertex : geometry.m_vertices)
				attributes.AddRange({ vertex.m_normal.x, vertex.m_normal.y, vertex.m_normal.z });
			for (const auto& vertex : geometry.m_vertices)
				attributes.AddRange({ vertex.m_texcoord.x, vertex.m_texcoord.y });
			std::ofstream vertices(m_verticesPath, std::ios::binary);
			vertices.write(reinterpret_cast<const char*>(attributes.GetData()), attributes.Num() * sizeof(float));
			vertices.close();
			std::ofstream indices(m_indicesPath, std::ios::binary);
			indices.write(reinterpret_cast<const char*>(geometry.m_indices.GetData()), geometry.m_indices.Num() * sizeof(uint32_t));
			indices.close();
			Require(vertices && indices, "LOD geometry buffers must be written");
			std::ifstream input(m_path);
			auto document = nlohmann::json::parse(input);
			input.close();
			const size_t positionBytes = geometry.m_vertices.Num() * 3 * sizeof(float);
			document["buffers"][0]["byteLength"] = attributes.Num() * sizeof(float);
			document["buffers"][1]["byteLength"] = geometry.m_indices.Num() * sizeof(uint32_t);
			for (uint32_t i = 0; i < 3; ++i)
			{
				document["bufferViews"][i]["byteOffset"] = i * positionBytes;
				document["bufferViews"][i]["byteLength"] = geometry.m_vertices.Num() * (i == 2 ? 2 : 3) * sizeof(float);
				document["accessors"][i]["count"] = geometry.m_vertices.Num();
			}
			document["bufferViews"][3]["byteLength"] = geometry.m_indices.Num() * sizeof(uint32_t);
			document["accessors"][3]["count"] = geometry.m_indices.Num();
			std::ofstream output(m_path);
			output << document;
			output.close();
			Require(static_cast<bool>(output), "LOD glTF buffer ranges must match their geometry");
		}

		std::filesystem::path CachePath(uint32_t level) const
		{
			return App::GetWorkspaceContext().GetCache() / "Lods" / ModelImporter::GetLodCacheFilename(m_id, level);
		}

		std::filesystem::path m_path, m_verticesPath, m_indicesPath;
		FileId m_id;
		ModelAssetInfoPtr m_info = nullptr;
	};

	struct ContentFile
	{
		std::string m_bytes;
		std::filesystem::file_time_type m_time;
		bool operator==(const ContentFile&) const = default;
	};

	std::map<std::filesystem::path, ContentFile> ReadContent(const std::filesystem::path& folder)
	{
		std::map<std::filesystem::path, ContentFile> files;
		for (const auto& entry : std::filesystem::recursive_directory_iterator(folder))
		{
			if (!entry.is_regular_file()) continue;
			std::ifstream stream(entry.path(), std::ios::binary);
			Require(stream.is_open(), "Content snapshot must read each file");
			files.emplace(entry.path(), ContentFile{
				std::string(std::istreambuf_iterator<char>(stream), {}), entry.last_write_time() });
		}
		return files;
	}

	void TestColdModelMaterialPublication(const std::filesystem::path& workspace, bool worker)
	{
		const auto folder = workspace / "Content" / (worker ? "ColdModelWorker" : "ColdModelMain");
		std::filesystem::create_directories(folder / "Content/materials");
		ModelFixture fixture(folder, true);
		const auto modelPath = fixture.m_path.parent_path() / "ColdModel.gltf";
		const auto modelId = FileId::CreateNewFileId();
		nlohmann::json source;
		{
			std::ifstream input(fixture.m_path);
			input >> source;
		}
		source["extensionsUsed"] = { "KHR_materials_emissive_strength" };
		source["materials"][0]["emissiveFactor"] = { 0.25f, 0.5f, 0.75f };
		source["materials"][0]["extensions"]["KHR_materials_emissive_strength"]["emissiveStrength"] = 4.0f;
		{
			std::ofstream output(modelPath);
			output << source;
			Require(static_cast<bool>(output), "the cold glTF source must be written");
		}
		{
			auto metadata = fixture.m_info->Serialize();
			metadata["fileId"] = modelId;
			metadata["filename"] = modelPath.filename().string();
			metadata["bShouldGenerateMaterials"] = true;
			std::ofstream output(modelPath.string() + ".asset");
			output << metadata;
			Require(static_cast<bool>(output), "the cold model metadata must be written");
		}

		auto* registry = App::GetSubmodule<AssetRegistry>();
		auto* scheduler = App::GetSubmodule<Tasks::Scheduler>();
		const auto materialPath = modelPath.parent_path() / "materials/ColdModel.gltf_material_0.mat";
		const auto materialId = FileId::CreateNewFileId();
		MaterialAsset::Data data;
		data.m_shader = registry->GetOrLoadFile("Shaders/Standard_glTF.shader");
		data.m_uniformsVec4["material.emissiveFactor"] = glm::vec4(0.125f, 0.25f, 0.5f, 0);
		{
			std::ofstream output(materialPath);
			output << MaterialAsset::Serialize(data);
			Require(static_cast<bool>(output), "the existing generated material must be written");
		}
		{
			auto metadata = CreateAssetInfoMetadata<MaterialAssetInfo>(materialId, materialPath.filename().string());
			metadata["sourceModel"] = modelId;
			metadata["sourceMaterialIndex"] = 0;
			std::ofstream output(materialPath.string() + ".asset");
			output << metadata;
			Require(static_cast<bool>(output), "the generated material ownership must be written");
		}
		Require(registry->GetOrLoadFile(materialPath.string()) == materialId,
			"the owned material must register before its source model");
		MaterialPtr material;
		Require(App::GetSubmodule<MaterialImporter>()->LoadMaterial_Immediate(materialId, material) && material,
			"the owned material must already be live");
		Drain();
		const auto revision = material->GetContentRevision();
		if (worker)
		{
			auto lookup = Tasks::CreateTask<FileId>("Register model with a live generated material", [registry, modelPath]()
				{
					return registry->GetOrLoadFile(modelPath.string());
				}, EThreadType::Worker);
			lookup->Run();
			lookup->Wait();
			Require(lookup->GetResult() == modelId, "cold model registration must return its authored identity");
			scheduler->WaitIdle({ EThreadType::Worker, EThreadType::Render, EThreadType::RHI });
			const bool deferred = material->GetContentRevision() == revision;
			Drain();
			Require(deferred, "cold model registration must not publish a live generated material before Main handles the update");
		}
		else
		{
			Require(registry->GetOrLoadFile(modelPath.string()) == modelId,
				"Main registration must return the authored model identity");
		}
		glm::vec4 emission;
		Require(material->GetUniformsVec4().TryGet("material.emissiveFactor", emission) && emission == glm::vec4(1, 2, 3, 0),
			"Main must publish the generated emission to the existing material");
		auto* model = registry->GetAssetInfoPtr<ModelAssetInfoPtr>(modelId);
		Require(model && model->GetDefaultMaterials().Num() == 2 && model->GetDefaultMaterials()[0] == materialId,
			"cold generation must retain owned material identity and populate model slots");
		Require(!registry->IsAssetExpired(registry->GetAssetInfoPtr(materialId)),
			"the generated material must be acknowledged after publication");
		Drain();

		const auto publishedRevision = material->GetContentRevision();
		const auto published = YAML::LoadFile(materialPath.string());
		{
			auto invalid = YAML::Clone(published);
			invalid["uniformsFloat"]["material.roughnessFactor"] = "invalid";
			std::ofstream output(materialPath);
			output << invalid;
		}
		Require(!UpdateAssetThroughEditor(modelId.ToString(), true),
			"reimport must report a generated material publication failure");
		Require(material->GetContentRevision() == publishedRevision && registry->IsAssetExpired(model) &&
			registry->IsAssetExpired(registry->GetAssetInfoPtr(materialId)),
			"failed publication must keep the live material and leave the import retryable");
		Require(!UpdateAssetThroughEditor(modelId.ToString(), true),
			"unchanged invalid material data must fail again rather than being acknowledged");
		{
			std::ofstream output(materialPath);
			output << published;
		}
		Require(UpdateAssetThroughEditor(modelId.ToString(), true) && !registry->IsAssetExpired(model) &&
			!registry->IsAssetExpired(registry->GetAssetInfoPtr(materialId)),
			"repair must publish the material and acknowledge both assets without changing the glTF");
		Drain();
		const auto repairedRevision = material->GetContentRevision();
		const auto content = ReadContent(folder);
		Require(registry->GetOrLoadFile(modelPath.string()) == modelId, "warm model lookup must keep its identity");
		Drain();
		Require(material->GetContentRevision() == repairedRevision && ReadContent(folder) == content,
			"warm model lookup must not replay material publication or rewrite Content");
		std::cout << (worker ? "Cold model Worker" : "Cold model Main")
			<< ": owner publication, retained identity, failure/retry and warm lookup passed\n";
	}

	void TestModelLoadDoesNotWriteMaterials(const std::filesystem::path& workspace)
	{
		const auto folder = workspace / "Content" / "MaterialImport";
		std::filesystem::create_directories(folder / "Content");
		ModelFixture fixture(folder, true);
		nlohmann::json source;
		{
			std::ifstream input(fixture.m_path);
			input >> source;
		}
		auto& sourceMaterial = source["materials"][0];
		sourceMaterial["alphaMode"] = "MASK";
		sourceMaterial["alphaCutoff"] = 0.4f;
		sourceMaterial["emissiveFactor"] = { 0.25f, 0.5f, 1.0f };
		sourceMaterial["extensions"]["KHR_materials_emissive_strength"]["emissiveStrength"] = 5.0f;
		{
			std::ofstream output(fixture.m_path);
			output << source.dump();
		}
		auto settings = fixture.m_info->Serialize();
		settings["bShouldGenerateMaterials"] = true;
		fixture.m_info->Deserialize(settings);
		Require(fixture.m_info->SaveMetaFile(), "explicit material import settings must save");
		const auto modelId = fixture.m_id.ToString();
		const auto materialsFolder = fixture.m_path.parent_path() / "materials";
		std::filesystem::create_directories(materialsFolder);
		const auto collisionPath = materialsFolder / "CpuHierarchy.gltf_material_0.mat";
		{
			std::ofstream output(collisionPath);
			output << "renderQueue: Authored\n";
		}
		const auto collisionTime = std::filesystem::last_write_time(collisionPath);
		const auto pendingPath = materialsFolder / "CpuHierarchy.gltf_material_1.mat";
		const auto pendingId = FileId::CreateNewFileId();
		{
			auto metadata = CreateAssetInfoMetadata<MaterialAssetInfo>(pendingId, pendingPath.filename().string());
			metadata["sourceModel"] = fixture.m_id;
			metadata["sourceMaterialIndex"] = 1;
			std::ofstream output(pendingPath.string() + ".asset");
			output << metadata;
		}
		Require(UpdateAssetThroughEditor(modelId, true), "the editor reimport command must generate model materials");
		Drain();
		auto materialIds = fixture.m_info->GetDefaultMaterials();
		Require(materialIds.Num() == 2, "explicit import must register both generated materials");
		const auto generatedIds = materialIds;
		auto* registry = App::GetSubmodule<AssetRegistry>();
		Require(generatedIds[1] == pendingId && std::filesystem::last_write_time(collisionPath) == collisionTime &&
			YAML::LoadFile(collisionPath.string())["renderQueue"].as<std::string>() == "Authored" &&
			registry->GetAssetInfoPtr(generatedIds[0])->GetAssetFilepath() != collisionPath.string(),
			"import must reuse pending ownership but never overwrite an unowned file with a canonical name");
		{
			AssetRegistry reopened(App::GetWorkspaceContext(), nullptr);
			MaterialAssetInfoHandler handler(&reopened);
			for (uint32_t index = 0; index < generatedIds.Num(); ++index)
			{
				const auto id = reopened.GetOrLoadFile(registry->GetAssetInfoPtr(generatedIds[index])->GetAssetFilepath());
				const auto* info = reopened.GetAssetInfoPtr<MaterialAssetInfoPtr>(id);
				Require(info && id == generatedIds[index] && info->GetSourceModel() == fixture.m_id &&
					info->GetSourceMaterialIndex() == static_cast<int32_t>(index),
					"a fresh registry must recover typed material ownership from disk");
			}
		}
		const auto unusedGeneratedPath = registry->GetAssetInfoPtr(generatedIds[1])->GetAssetFilepath();
		MaterialPtr liveGenerated;
		Require(App::GetSubmodule<MaterialImporter>()->LoadMaterial_Immediate(generatedIds[1], liveGenerated) && liveGenerated,
			"the generated material must load before its live reimport");
		const auto authoredPath = fixture.m_path.parent_path() / "Authored.mat";
		const auto authoredId = FileId::CreateNewFileId();
		{
			std::ifstream original(unusedGeneratedPath);
			std::ofstream replacement(authoredPath);
			replacement << original.rdbuf();
		}
		{
			auto metadata = registry->GetAssetInfoPtr(generatedIds[1])->Serialize();
			metadata["fileId"] = authoredId;
			metadata["filename"] = authoredPath.filename().string();
			metadata.remove("sourceModel");
			metadata.remove("sourceMaterialIndex");
			std::ofstream output(authoredPath.string() + ".asset");
			output << metadata;
		}
		Require(registry->GetOrLoadFile(authoredPath.string()) == authoredId, "an authored replacement must register");
		materialIds[1] = authoredId;
		fixture.m_info->GetDefaultMaterials() = materialIds;
		Require(fixture.m_info->SaveMetaFile(), "authoring a material replacement must save");
		const auto authoredTime = std::filesystem::last_write_time(authoredPath);
		const auto authoredDocument = YAML::LoadFile(authoredPath.string());
		const auto* materialInfo = registry->GetAssetInfoPtr(materialIds[0]);
		Require(materialInfo != nullptr, "the generated material must be registered immediately");
		const auto materialPath = materialInfo->GetAssetFilepath();
		auto material = YAML::LoadFile(materialPath);
		material["uniformsFloat"]["material.roughnessFactor"] = 0.37f;
		material["uniformsVec4"]["material.baseColorFactor"] = glm::vec4(0.1f, 0.2f, 0.3f, 1.0f);
		material["uniformsVec4"]["material.emissiveFactor"] = glm::vec4(99.0f);
		const auto shaderId = registry->GetOrLoadFile("Shaders/Unlit.shader");
		Require(static_cast<bool>(shaderId), "the authored shader replacement must exist");
		material["shaderUid"] = shaderId;
		material["defines"].push_back("AUTHORED_FEATURE");
		material["bCustomDepthShader"] = true;
		{
			std::ofstream output(materialPath);
			output << material;
		}
		const auto before = ReadContent(workspace / "Content");
		{
			FreshImporter fresh;
			ModelPtr model;
			Require(fresh.m_importer.LoadModel_Immediate(fixture.m_id, model) && model,
				"ordinary model loading must succeed with existing generated materials");
			Drain();
			Require(model->IsReady() && model->GetMeshes().Num() == 2,
				"read-only material handling must still upload the model geometry");
		}
		Require(ReadContent(workspace / "Content") == before,
			"LoadModel must not change Content bytes, timestamps or file inventory, including deferred Main work");

		auto reimport = [&]()
		{
			{
				std::ofstream output(fixture.m_path);
				output << source.dump();
			}
			Require(UpdateAssetThroughEditor(modelId, true), "the editor command must reimport changed glTF properties");
			Drain();
			Require(fixture.m_info->GetDefaultMaterials() == materialIds &&
				YAML::LoadFile(materialPath + ".asset")["fileId"].as<FileId>() == materialIds[0],
				"explicit reimport must preserve generated material identities and references");
			const auto updated = YAML::LoadFile(materialPath);
			Require(updated["shaderUid"].as<FileId>() == shaderId &&
				updated["uniformsFloat"]["material.roughnessFactor"].as<float>() == 0.37f &&
				updated["uniformsVec4"]["material.baseColorFactor"].as<glm::vec4>() == glm::vec4(0.1f, 0.2f, 0.3f, 1.0f) &&
				updated["bCustomDepthShader"].as<bool>(),
				"reimport must preserve authored shader, roughness, base color and custom depth state");
			bool authoredDefine = false;
			for (const auto& define : updated["defines"]) authoredDefine |= define.as<std::string>() == "AUTHORED_FEATURE";
			Require(authoredDefine, "reimport must preserve unrelated authored shader defines");
			const auto unused = YAML::LoadFile(unusedGeneratedPath);
			Require(unused["uniformsVec4"]["material.emissiveFactor"].as<glm::vec4>() == glm::vec4(1, 2, 4, 0),
				"reimport must update its owned material even when a draw slot has an authored replacement");
			glm::vec4 radiance;
			Require(liveGenerated->IsReady() && liveGenerated->GetUniformsVec4().TryGet("material.emissiveFactor", radiance) &&
				radiance == glm::vec4(1, 2, 4, 0),
				"explicit generation must also publish the changed radiance to an already loaded material");
			Require(Utils::AreYamlNodesEqual(YAML::LoadFile(authoredPath.string()), authoredDocument) &&
				std::filesystem::last_write_time(authoredPath) == authoredTime,
				"reimport must not modify an independently authored replacement");
			return updated;
		};
		source["materials"][1]["emissiveFactor"] = { 0.25f, 0.5f, 1.0f };
		source["materials"][1]["extensions"]["KHR_materials_emissive_strength"]["emissiveStrength"] = 4.0f;
		sourceMaterial["alphaMode"] = "BLEND";
		sourceMaterial["extensions"]["KHR_materials_transmission"]["transmissionFactor"] = 0.7f;
		std::filesystem::copy_file(App::GetWorkspaceContext().GetEngineContent() / "Textures" / "DitherPattern.png",
			fixture.m_path.parent_path() / "Surface.png");
		source["images"] = {{{ "uri", "Surface.png" }}};
		source["textures"] = {{{ "source", 0 }}};
		sourceMaterial["extensions"]["KHR_materials_transmission"]["transmissionTexture"]["index"] = 0;
		sourceMaterial["extensions"]["KHR_materials_volume"]["thicknessTexture"]["index"] = 0;
		sourceMaterial["extensions"]["KHR_materials_volume"]["thicknessFactor"] = 0.8f;
		sourceMaterial["extensions"]["KHR_materials_ior"]["ior"] = 1.4f;
		sourceMaterial["extensions"]["KHR_materials_emissive_strength"]["emissiveStrength"] = 8.0f;
		const auto transparentSource = source;
		const auto transparent = reimport();
		Require(transparent["renderQueue"].as<std::string>() == "Transparent" &&
			!transparent["bEnableZWrite"].as<bool>() &&
			transparent["uniformsFloat"]["material.transmissionFactor"].as<float>() == 0.7f &&
			transparent["uniformsFloat"]["material.thicknessFactor"].as<float>() == 0.8f &&
			transparent["uniformsFloat"]["material.indexOfRefraction"].as<float>() == 1.4f &&
			transparent["uniformsVec4"]["material.emissiveFactor"].as<glm::vec4>() == glm::vec4(2, 4, 8, 0),
			"explicit reimport must refresh transmission, alpha state, IOR and HDR emission");
		const auto textureId = transparent["samplers"]["transmissionSampler"].as<FileId>();
		Require(textureId && transparent["samplers"]["thicknessSampler"].as<FileId>() == textureId,
			"equal generated texture requests must reuse one source texture identity");
		const auto repeated = reimport();
		Require(repeated["samplers"]["transmissionSampler"].as<FileId>() == textureId,
			"reimport must keep the generated sampler FileId");
		{
			const auto freshFolder = folder / "Fresh";
			std::filesystem::create_directories(freshFolder / "Content");
			ModelFixture fresh(freshFolder, true);
			std::filesystem::copy_file(fixture.m_path.parent_path() / "Surface.png", fresh.m_path.parent_path() / "Surface.png");
			{
				std::ofstream output(fresh.m_path);
				output << transparentSource.dump();
			}
			auto metadata = fresh.m_info->Serialize();
			metadata["bShouldGenerateMaterials"] = true;
			fresh.m_info->Deserialize(metadata);
			Require(fresh.m_info->SaveMetaFile() && UpdateAssetThroughEditor(fresh.m_id.ToString(), true),
				"the comparison model must generate its materials from scratch");
			const auto freshId = fresh.m_info->GetDefaultMaterials()[0];
			const auto generated = YAML::LoadFile(registry->GetAssetInfoPtr(freshId)->GetAssetFilepath());
			for (const char* property : { "renderQueue", "bEnableZWrite", "blendMode" })
			{
				Require(Utils::AreYamlNodesEqual(generated[property], transparent[property]),
					"fresh generation and reimport must produce identical managed render state");
			}
			for (const char* property : { "material.alphaCutoff", "material.transmissionFactor", "material.thicknessFactor",
				"material.attenuationDistance", "material.indexOfRefraction" })
			{
				Require(Utils::AreYamlNodesEqual(generated["uniformsFloat"][property], transparent["uniformsFloat"][property]),
					"fresh generation and reimport must agree on managed optical parameters");
			}
			for (const char* property : { "material.attenuationColor", "material.emissiveFactor" })
			{
				Require(Utils::AreYamlNodesEqual(generated["uniformsVec4"][property], transparent["uniformsVec4"][property]),
					"fresh generation and reimport must agree on managed color and radiance");
			}
			for (const char* name : { "transmissionSampler", "thicknessSampler" })
			{
				const auto* originalTexture = registry->GetAssetInfoPtr<TextureAssetInfoPtr>(transparent["samplers"][name].as<FileId>());
				const auto* freshTexture = registry->GetAssetInfoPtr<TextureAssetInfoPtr>(generated["samplers"][name].as<FileId>());
				Require(originalTexture && freshTexture && originalTexture->GetGlbTextureIndex() == freshTexture->GetGlbTextureIndex() &&
					originalTexture->GetFormat() == freshTexture->GetFormat(),
					"fresh generation and reimport samplers must reference the same glTF texture with the same color space");
			}
		}
		sourceMaterial["alphaMode"] = "MASK";
		sourceMaterial["alphaCutoff"] = 0.6f;
		sourceMaterial.erase("extensions");
		const auto masked = reimport();
		Require(masked["renderQueue"].as<std::string>() == "Masked" && masked["bEnableZWrite"].as<bool>() &&
			masked["uniformsFloat"]["material.alphaCutoff"].as<float>() == 0.6f &&
			!masked["uniformsFloat"]["material.transmissionFactor"] &&
			!masked["uniformsFloat"]["material.indexOfRefraction"] &&
			!masked["uniformsVec4"]["material.attenuationColor"] &&
			!masked["samplers"]["transmissionSampler"] && !masked["samplers"]["thicknessSampler"],
			"reimport must remove stale extension properties when returning to a masked material");
		sourceMaterial["alphaMode"] = "OPAQUE";
		const auto opaque = reimport();
		Require(opaque["renderQueue"].as<std::string>() == "Opaque" && opaque["bEnableZWrite"].as<bool>(),
			"explicit reimport must restore opaque rendering when alpha masking is removed");
		const auto unchanged = ReadContent(workspace / "Content");
		Require(UpdateAssetThroughEditor(modelId, true), "reimporting unchanged source must succeed");
		Drain();
		Require(ReadContent(workspace / "Content") == unchanged, "an unchanged reimport must not rewrite Content");
		{
			auto stale = YAML::Clone(opaque);
			stale["uniformsVec4"]["material.emissiveFactor"] = glm::vec4(99.0f);
			std::ofstream output(materialPath);
			output << stale;
		}
		const auto sourceTime = std::filesystem::last_write_time(fixture.m_path);
		std::string sourceBytes;
		Require(AssetRegistry::ReadAllTextFile(fixture.m_path.string(), sourceBytes), "the source glTF must remain readable");
		const auto staleContent = ReadContent(workspace / "Content");
		Require(UpdateAssetThroughEditor(modelId, false), "ordinary update of unchanged glTF must succeed");
		Drain();
		Require(ReadContent(workspace / "Content") == staleContent,
			"an ordinary update must not force regeneration of unchanged model assets");
		Require(UpdateAssetThroughEditor(modelId, true), "explicit reimport of unchanged glTF must succeed");
		Drain();
		Require(YAML::LoadFile(materialPath)["uniformsVec4"]["material.emissiveFactor"].as<glm::vec4>() ==
			glm::vec4(0.25f, 0.5f, 1.0f, 0.0f),
			"explicit reimport must repair stale generated properties without modifying the source model");
		{
			std::ofstream output(materialPath);
			output << "uniformsFloat: [incomplete";
		}
		Require(!UpdateAssetThroughEditor(modelId, true) && registry->IsAssetExpired(fixture.m_info),
			"failed material regeneration must fail the editor command and retain the need to retry");
		Require(std::filesystem::remove(materialPath), "the fixture must remove only its own broken generated material");
		Require(UpdateAssetThroughEditor(modelId, true), "explicit reimport must restore a missing owned material");
		Drain();
		Require(std::filesystem::is_regular_file(materialPath) &&
			YAML::LoadFile(materialPath + ".asset")["fileId"].as<FileId>() == generatedIds[0] &&
			fixture.m_info->GetDefaultMaterials() == materialIds && !registry->IsAssetExpired(fixture.m_info),
			"successful retry must retain generated identity and authored slots and acknowledge the model");
		std::string sourceAfter;
		Require(AssetRegistry::ReadAllTextFile(fixture.m_path.string(), sourceAfter) && sourceAfter == sourceBytes &&
			std::filesystem::last_write_time(fixture.m_path) == sourceTime,
			"forced reimport must never manufacture a source change to trigger processing");
		const auto repaired = ReadContent(workspace / "Content");
		Require(UpdateAssetThroughEditor(modelId, true), "a repaired reimport must succeed again");
		Drain();
		Require(ReadContent(workspace / "Content") == repaired, "a completed repair must become a Content no-op");

		const auto engineFolder = App::GetWorkspaceContext().GetEngineContent() / "Models" / "Box";
		const auto engineBefore = ReadContent(engineFolder);
		const auto engineId = registry->GetOrLoadFile("Models/Box/Box.gltf");
		Require(engineId && !registry->GetAssetInfoPtr(engineId)->IsWritable(), "the engine model must be read-only");
		Require(UpdateAssetThroughEditor(engineId.ToString(), true), "explicit reimport may refresh a read-only engine resource");
		{
			FreshImporter fresh;
			ModelPtr model;
			Require(fresh.m_importer.LoadModel_Immediate(engineId, model), "read-only engine models must still load");
			Drain();
		}
		Require(ReadContent(engineFolder) == engineBefore, "engine Content must remain unchanged during load");
		std::cout << "Read-only model loading, explicit material reimport and authored-property preservation passed\n";
	}

	class ModelRayProbe final : public Raytracing::PathTracer
	{
	public:
		using PathTracer::IntersectScene;
		using PathTracer::TLASHit;
	};

	void CheckImportedRayHits(const ModelPtr& model)
	{
		auto verifyHit = [&](int32_t selection, glm::vec3 point, uint32_t materialIndex, uint32_t instanceIndex = 0)
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
				hit.m_materialIndex == materialIndex && hit.m_instanceIndex == instanceIndex,
				"full and subset imports must retain the expected hit position and material");
		};
		verifyHit(Model::AllMeshes, { 4.25f, 0.125f, 0 }, 0);
		verifyHit(Model::AllMeshes, { -3.75f, 0.125f, 0 }, 1, 1);
		verifyHit(Model::AllMeshes, { 4.25f, 3.125f, 0 }, 0, 2);
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

	void TestImportedWideMaterialSlots(const ModelFixture& fixture)
	{
		std::ifstream input(fixture.m_path);
		auto document = nlohmann::json::parse(input);
		input.close();
		document["materials"] = nlohmann::json::array();
		for (uint32_t i = 0; i < 257; ++i)
			document["materials"].push_back({ { "name", "Material " + std::to_string(i) } });
		document["meshes"][1]["primitives"][0]["material"] = 255;
		document["meshes"][2]["primitives"][0]["material"] = 256;
		std::ofstream output(fixture.m_path);
		output << document;
		output.close();
		Require(static_cast<bool>(output), "the 257-material glTF fixture must be written");
		for (bool batchByMaterial : { true, false })
		{
			auto settings = fixture.m_info->Serialize();
			settings["bGenerateBLAS"] = true;
			settings["bShouldKeepCpuBuffers"] = !batchByMaterial;
			settings["bShouldBatchByMaterial"] = batchByMaterial;
			fixture.m_info->Deserialize(settings);
			FreshImporter fresh;
			ModelPtr model;
			Require(fresh.m_importer.LoadModel_Immediate(fixture.m_id, model) && model,
				"the model with 257 glTF materials must import");
			Drain();
			const uint32_t firstSlot = batchByMaterial ? 255 : 0;
			const uint32_t slotsPerInstance = firstSlot + 2;
			Require(model->IsReady() && model->HasBLAS() && model->HasCpuMeshes() == !batchByMaterial &&
				model->GetMeshes().Num() == 2 && model->GetMeshes()[0]->m_materialIndex == firstSlot &&
				model->GetMeshes()[1]->m_materialIndex == firstSlot + 1,
				"GPU material slots must preserve the selected batching policy");
			ModelRayProbe::MaterialSnapshots materials;
			materials.Resize(2 * slotsPerInstance);
			auto neutral = TSharedPtr<ModelRayProbe::MaterialSnapshot>::Make();
			for (auto& material : materials) material = neutral;
			for (uint32_t i = 0; i < 4; ++i)
			{
				auto material = TSharedPtr<ModelRayProbe::MaterialSnapshot>::Make();
				material->m_parameters.m_emissiveFactor = glm::vec3(i + 1, 0.5f * i, 0.25f);
				materials[(i / 2) * slotsPerInstance + firstSlot + i % 2] = material;
			}
			for (int32_t selection : { Model::AllMeshes, 1, 2 })
			{
				ModelRayProbe tracer;
				TVector<ModelRayProbe::TLASInstance> instances;
				for (uint32_t index = 0; index < 2; ++index)
				{
					ModelRayProbe::TLASInstance instance;
					instance.m_model = model;
					instance.m_meshIndex = selection;
					instance.m_materialBaseOffset = static_cast<int32_t>(index * slotsPerInstance);
					instance.m_worldMatrix[3].y = 10.0f * index;
					instance.m_inverseWorldMatrix = glm::inverse(instance.m_worldMatrix);
					instance.m_worldBounds = model->GetBoundsAABB(selection);
					instance.m_worldBounds.Apply(instance.m_worldMatrix);
					instances.Add(instance);
				}
				Require(tracer.InitializeSceneSnapshot(instances, materials, {}, false, {}, true),
					"imported CPU geometry must resolve the same material slots as GPU geometry");
				for (uint32_t index = 0; index < 2; ++index)
				{
					auto verify = [&](glm::vec3 point, uint32_t primitive)
					{
						point.y += 10.0f * index;
						const glm::vec3 origin = point + glm::vec3(0, 0, 2);
						const glm::vec3 direction(0, 0, -1);
						const uint32_t expected = index * slotsPerInstance + firstSlot + primitive;
						ModelRayProbe::TLASHit hit;
						Require(tracer.IntersectScene(Math::Ray(origin, direction), hit) && hit.m_materialIndex == expected,
							"imported full/subset rays must retain high slots and neighboring instance material bases");
						ModelRayProbe::Params params{};
						params.m_maxBounces = 1;
						params.m_bIncludeDirectLighting = params.m_bIncludeEnvironment = false;
						ModelRayProbe::PreparedRaySample sample;
						Require(tracer.SamplePreparedSceneRay(origin, direction, 3, params, 29, sample) && sample.m_bHit &&
							glm::length(sample.m_radiance - materials[expected]->m_parameters.m_emissiveFactor) < 1e-5f,
							"imported geometry must trace the intended material's radiance");
					};
					if (selection == Model::AllMeshes)
					{
						verify({ 4.25f, 0.125f, 0 }, 0);
						verify({ -3.75f, 0.125f, 0 }, 1);
						verify({ 4.25f, 3.125f, 0 }, 0);
					}
					else verify({ 0.25f, 0.125f, 0 }, static_cast<uint32_t>(selection - 1));
				}
			}
			std::cout << "Imported " << (batchByMaterial ? "batched" : "unbatched")
				<< " material slots, full/subset hits and radiance passed\n";
		}
		std::cout << "257-material glTF, CPU/GPU slot parity, full/subset rays and instance radiance passed\n";
	}

	void CheckGpuGeometry(const RHI::RHIMeshPtr& mesh, const Geometry& geometry)
	{
		auto readback = Tasks::CreateTaskWithResult<bool>("Read back selected LOD geometry", [mesh, &geometry]()
			{
				auto& driver = RHI::Renderer::GetDriver();
				const auto memory = RHI::EMemoryPropertyBit::HostVisible | RHI::EMemoryPropertyBit::HostCoherent;
				const auto vertexBytes = geometry.m_vertices.Num() * sizeof(geometry.m_vertices[0]);
				const auto indexBytes = geometry.m_indices.Num() * sizeof(uint32_t);
				auto vertices = driver->CreateBuffer(vertexBytes, RHI::EBufferUsageBit::BufferTransferDst_Bit, memory);
				auto indices = driver->CreateBuffer(indexBytes, RHI::EBufferUsageBit::BufferTransferDst_Bit, memory);
				return driver->CopyBuffer_Immediate(mesh->m_vertexBuffer, vertices, vertexBytes,
					mesh->m_vertexOffset * sizeof(geometry.m_vertices[0])) &&
					driver->CopyBuffer_Immediate(mesh->m_indexBuffer, indices, indexBytes, mesh->m_firstIndex * sizeof(uint32_t)) &&
					std::equal(geometry.m_vertices.begin(), geometry.m_vertices.end(),
						static_cast<const RHI::VertexP3N3T3B3UV2C4I4W4*>(vertices->GetPointer())) &&
					std::equal(geometry.m_indices.begin(), geometry.m_indices.end(),
						static_cast<const uint32_t*>(indices->GetPointer()));
			}, EThreadType::RHI);
		readback->Run();
		readback->Wait();
		Require(readback->GetResult(), "the selected LOD range must contain the expected GPU attributes and indices");
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
		Require(mesh->m_vertexBuffer->GetSize() == source.m_vertices.Num() * sizeof(source.m_vertices[0]) &&
			mesh->m_indexBuffer->GetSize() == source.m_indices.Num() * sizeof(uint32_t),
			"the locked-border panel must upload its vertex and index data only once");
		CheckGpuGeometry(mesh, Geometry{ source.m_vertices, source.m_indices });
		TVector<Geometry> result{ Geometry{ source.m_vertices, source.m_indices } };
		for (uint32_t level = 1; level < mesh->GetNumLods(); ++level)
		{
			Require(ModelLodCache::Load(*fixture.m_info, revision, level, parsed),
				"the real importer must publish a cache matching its source geometry");
			const auto lod = mesh->GetLod(level);
			const auto& cached = parsed[0].lods[level - 1];
			auto geometry = cached.m_indices.IsEmpty() ? *result.Last() : cached;
			Require(lod && lod->GetIndexCount() == geometry.m_indices.Num(),
				"uploaded LOD views must use the generated geometry's draw counts");
			Require(lod->m_vertexBuffer == mesh->m_vertexBuffer && lod->m_indexBuffer == mesh->m_indexBuffer &&
				lod->GetVertexOffset() == mesh->GetVertexOffset() && lod->GetFirstIndex() == mesh->GetFirstIndex(),
				"unreduced LODs must select the exact base vertex and index ranges");
			result.Add(std::move(geometry));
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
			const auto& generated = expected[0].lods[level - 1];
			const auto& selected = generated.m_indices.IsEmpty() ? geometry[level - 1] : generated;
			Require(geometry[level].m_vertices == selected.m_vertices && geometry[level].m_indices == selected.m_indices,
				"every cached LOD must match fresh generation from the current imported geometry");
		}
	}

	Geometry MakeSurfaceGeometry(uint32_t cells)
	{
		Geometry geometry;
		const uint32_t subdivisions = (std::max)(cells, 1u);
		const uint32_t width = subdivisions + 1;
		for (uint32_t y = 0; y < width; ++y)
		{
			for (uint32_t x = 0; x < width; ++x)
			{
				RHI::VertexP3N3T3B3UV2C4I4W4 vertex{};
				vertex.m_texcoord = glm::vec2(x, y) / static_cast<float>(subdivisions);
				vertex.m_position = glm::vec3(vertex.m_texcoord * 1.5f - 0.75f, 0.5f);
				vertex.m_normal = glm::vec3(0, 0, 1);
				vertex.m_tangent = glm::vec3(1, 0, 0);
				vertex.m_bitangent = glm::vec3(0, 1, 0);
				vertex.m_color = glm::vec4(1);
				geometry.m_vertices.Add(vertex);
			}
		}
		if (cells == 0)
		{
			geometry.m_vertices.Resize(3);
			geometry.m_indices = { 0, 1, 2 };
			return geometry;
		}
		for (uint32_t y = 0; y < cells; ++y)
		{
			for (uint32_t x = 0; x < cells; ++x)
			{
				const uint32_t first = y * width + x;
				geometry.m_indices.AddRange({ first, first + 1, first + width,
					first + 1, first + width + 1, first + width });
			}
		}
		return geometry;
	}

	RHI::RHIMeshPtr MakeReferenceMesh(const Geometry& geometry)
	{
		auto task = Tasks::CreateTaskWithResult<RHI::RHIMeshPtr>("Upload independent LOD reference", [&geometry]()
			{
				auto& driver = RHI::Renderer::GetDriver();
				const auto memory = RHI::EMemoryPropertyBit::HostVisible | RHI::EMemoryPropertyBit::HostCoherent;
				auto mesh = RHI::RHIMeshPtr::Make();
				mesh->m_vertexDescription = driver->GetOrAddVertexDescription<RHI::VertexP3N3T3B3UV2C4I4W4>();
				const size_t vertexBytes = geometry.m_vertices.Num() * sizeof(geometry.m_vertices[0]);
				const size_t indexBytes = geometry.m_indices.Num() * sizeof(uint32_t);
				mesh->m_vertexBuffer = driver->CreateBuffer(vertexBytes, RHI::EBufferUsageBit::VertexBuffer_Bit, memory);
				mesh->m_indexBuffer = driver->CreateBuffer(indexBytes, RHI::EBufferUsageBit::IndexBuffer_Bit, memory);
				std::memcpy(mesh->m_vertexBuffer->GetPointer(), geometry.m_vertices.GetData(), vertexBytes);
				std::memcpy(mesh->m_indexBuffer->GetPointer(), geometry.m_indices.GetData(), indexBytes);
				mesh->m_indexCount = static_cast<uint32_t>(geometry.m_indices.Num());
				mesh->m_firstIndex = mesh->m_vertexOffset = 0;
				return mesh;
			}, EThreadType::RHI);
		task->Run();
		task->Wait();
		return task->GetResult();
	}

	void CheckSurfacePixels(const Tests::SurfacePixels& expected, const Tests::SurfacePixels& actual, uint32_t level)
	{
		for (size_t pixel = 0; pixel < expected.size(); ++pixel)
		{
			for (uint32_t channel = 0; channel < 4; ++channel)
			{
				if (!std::isfinite(actual[pixel][channel]) || std::abs(actual[pixel][channel] - expected[pixel][channel]) > 0.002f)
				{
					std::cerr << "LOD " << level << ", pixel " << pixel << ", channel " << channel
						<< ": expected " << expected[pixel][channel] << ", got " << actual[pixel][channel] << '\n';
					throw std::runtime_error("the selected LOD must render the independent geometry reference");
				}
			}
		}
	}

	void CheckLodDrawRanges(const ModelFixture& fixture, MaterialPtr material,
		const Tests::SurfacePixels& pixels, uint32_t numLods, bool reduced)
	{
		FreshImporter fresh;
		ModelPtr model;
		Require(fresh.m_importer.LoadModel_Immediate(fixture.m_id, model) && model,
			"the surface model must load through the real importer");
		Drain();
		Require(model->IsReady() && model->GetMeshes().Num() == 1 && model->GetCpuMeshes().Num() == 1,
			"the surface model must retain source geometry and upload its mesh");
		const auto& cpu = model->GetCpuMeshes()[0];
		const Geometry source{ cpu.m_vertices, cpu.m_indices };
		TVector<ModelImporter::MeshContext> expected(1);
		expected[0].outVertices = source.m_vertices;
		expected[0].outIndices = source.m_indices;
		ModelImporter::GenerateLods(expected, numLods, 0.05f);
		const auto mesh = model->GetMeshes()[0];
		Require(mesh->GetNumLods() == numLods + 1, "geometry reuse must preserve every requested logical level");
		size_t vertices = source.m_vertices.Num(), indices = source.m_indices.Num();
		size_t vertexOffset = 0, firstIndex = 0;
		size_t bytesWithoutAliases = 0;
		uint32_t physicalLevels = 1, nonBaseAliases = 0;
		const Geometry* selected = &source;
		for (uint32_t level = 0; level <= numLods; ++level)
		{
			if (level > 0)
			{
				const auto& lod = expected[0].lods[level - 1];
				if (!lod.m_indices.IsEmpty())
				{
					selected = &lod;
					vertexOffset = vertices;
					firstIndex = indices;
					vertices += lod.m_vertices.Num();
					indices += lod.m_indices.Num();
					++physicalLevels;
				}
				else if (vertexOffset > 0 && firstIndex > 0)
				{
					++nonBaseAliases;
				}
			}
			const auto view = level == 0 ? mesh : mesh->GetLod(level);
			Require(view && view->GetIndexCount() == selected->m_indices.Num() &&
				view->m_vertexBuffer == mesh->m_vertexBuffer && view->m_indexBuffer == mesh->m_indexBuffer &&
				view->GetVertexOffset() == mesh->GetVertexOffset() + vertexOffset &&
				view->GetFirstIndex() == mesh->GetFirstIndex() + firstIndex,
				"each logical LOD must select its physical geometry range in the shared buffers");
			CheckGpuGeometry(view, *selected);
			CheckSurfacePixels(pixels, Tests::RenderSurface(material, view), level);
			bytesWithoutAliases += selected->m_vertices.Num() * sizeof(source.m_vertices[0]) +
				selected->m_indices.Num() * sizeof(uint32_t);
		}
		Require(mesh->m_vertexBuffer->GetSize() == vertices * sizeof(source.m_vertices[0]) &&
			mesh->m_indexBuffer->GetSize() == indices * sizeof(uint32_t),
			"GPU buffers must contain only the base and genuinely reduced geometry");
		Require(reduced ? physicalLevels > 1 && nonBaseAliases > 0 : physicalLevels == 1,
			"the fixtures must exercise both base aliases and aliases of a reduced non-base range");
		const size_t uploadedBytes = vertices * sizeof(source.m_vertices[0]) + indices * sizeof(uint32_t);
		Require(uploadedBytes < bytesWithoutAliases, "aliased levels must reduce the uploaded geometry size");
		std::cout << "LOD surface: " << numLods + 1 << " logical / " << physicalLevels << " physical levels, "
			<< uploadedBytes << " uploaded bytes (" << bytesWithoutAliases << " without aliases)\n";
	}

	void TestLodRendering(const std::filesystem::path& workspace, const ModelFixture& fixture)
	{
		MaterialAsset::Data data;
		data.m_shader = FileId("1A4BA353-FDA4-4F65-941F-D9FFEE4630A0");
		data.m_renderQueue = "SurfaceReference";
		data.m_renderState = RHI::RenderState(false, false, 0, false, RHI::ECullMode::None,
			RHI::EBlendMode::None, RHI::EFillMode::Fill, 0, false);
		data.m_uniformsVec4["material.baseColorFactor"] = glm::vec4(0.3f, 0.6f, 0.85f, 1);
		auto* importer = App::GetSubmodule<MaterialImporter>();
		const auto id = importer->CreateMaterialAsset((workspace / "Content" / "LodSurface.mat").string(), data);
		MaterialPtr material;
		Require(importer->LoadMaterial_Immediate(id, material) && material, "the LOD reference material must load");
		Drain();
		for (uint32_t cells : { 0u, 1u, 8u })
		{
			const uint32_t numLods = cells == 8 ? 8 : 2;
			fixture.WriteGeometry(MakeSurfaceGeometry(cells));
			auto metadata = fixture.m_info->Serialize();
			metadata["unitScale"] = 1.0f;
			metadata["bFlipTexcoordY"] = false;
			metadata["numGeneratedLods"] = numLods;
			metadata["lodReductionFactor"] = 0.05f;
			fixture.m_info->Deserialize(metadata);
			auto reference = MakeSurfaceGeometry(cells == 0 ? 0 : 1);
			const auto pixels = Tests::RenderSurface(material, MakeReferenceMesh(reference));
			const auto covered = std::count_if(pixels.begin(), pixels.end(), [](const auto& color) { return color.r > 0; });
			Require(covered > 0 && covered < static_cast<ptrdiff_t>(pixels.size()),
				"the reference must contain both the lit surface and uncovered pixels");
			for (auto& vertex : reference.m_vertices) vertex.m_position.x += 4.0f;
			const auto outside = Tests::RenderSurface(material, MakeReferenceMesh(reference));
			Require(std::all_of(outside.begin(), outside.end(), [](const auto& color) { return color == glm::vec4(-1); }),
				"moving the independent reference outside the view must clear all coverage");
			CheckLodDrawRanges(fixture, material, pixels, numLods, cells == 8);
			const auto cacheTime = std::filesystem::last_write_time(fixture.m_path) - std::chrono::hours(24);
			for (uint32_t level = 1; level <= numLods; ++level)
				std::filesystem::last_write_time(fixture.CachePath(level), cacheTime);
			CheckLodDrawRanges(fixture, material, pixels, numLods, cells == 8);
			for (uint32_t level = 1; level <= numLods; ++level)
				Require(std::filesystem::last_write_time(fixture.CachePath(level)) == cacheTime,
					"warm LOD rendering must reuse every cache file without rewriting it");
		}
		std::cout << "Cold/warm triangle, locked plane and reduced-grid LOD ranges and pixels passed\n";
	}
}

namespace Sailor::Tests
{
	void TestAssetRelativePaths()
	{
		struct AssetPaths : AssetInfo
		{
			AssetPaths(const std::string& folder, const std::string& filename)
			{
				m_folder = folder;
				m_assetFilename = filename;
			}

			void SetVirtualPaths(const std::string& asset, const std::string& meta)
			{
				m_virtualAssetFilepath = asset;
				m_virtualMetaFilepath = meta;
			}
		};

		const std::string root = AssetRegistry::GetContentFolder();
		Require(!root.empty(), "relative-path checks require the active workspace content root");
		AssetPaths ordinary(root + "Models/", "Ship.glb");
		Require(ordinary.GetRelativeAssetFilepath() == "Models/Ship.glb" &&
			ordinary.GetRelativeMetaFilepath() == "Models/Ship.glb.asset",
			"asset and metadata paths must remove the initial content prefix");

		AssetPaths repeated(root + "Copies" + root, "Ship.glb");
		Require(repeated.GetRelativeAssetFilepath() == "Copies" + root + "Ship.glb" &&
			repeated.GetRelativeMetaFilepath() == "Copies" + root + "Ship.glb.asset",
			"a content-root substring inside the relative name must not be erased");
		AssetPaths outside("Other" + root, "Ship.glb");
		Require(outside.GetRelativeAssetFilepath() == outside.GetAssetFilepath() &&
			outside.GetRelativeMetaFilepath() == outside.GetMetaFilepath(),
			"a non-prefix content-root match must leave both paths unchanged");

		repeated.SetVirtualPaths("Models/Alias.glb", "Metadata/Alias.asset");
		Require(repeated.GetRelativeAssetFilepath() == "Models/Alias.glb" &&
			repeated.GetRelativeMetaFilepath() == "Metadata/Alias.asset",
			"virtual mount paths must take precedence over physical prefix handling");
		std::cout << "Asset relative paths preserve nested names and virtual mount overrides passed\n";
	}

	void RunModelLodCommandTests(const std::filesystem::path& workspace)
	{
		TestAssetRelativePaths();
		TestColdModelMaterialPublication(workspace, false);
		TestColdModelMaterialPublication(workspace, true);
		TestModelLoadDoesNotWriteMaterials(workspace);
		ModelFixture cpuFixture(workspace, true);
		TestCpuPreparationDoesNotUseRhi(cpuFixture);
		TestImportedWideMaterialSlots(cpuFixture);
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
		TestLodRendering(workspace, fixture);
	}
}
