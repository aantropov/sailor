#include "Editor/MaterialPreview.h"
#include "AssetRegistry/AssetRegistry.h"
#include "AssetRegistry/Material/MaterialImporter.h"
#include "AssetRegistry/Model/ModelImporter.h"
#include "AssetRegistry/Shader/ShaderCompiler.h"
#include "AssetRegistry/Texture/TextureImporter.h"
#include "Editor/EditorRuntimeBridge.h"
#include "Editor/EditorScene.h"
#include "Memory/UniquePtr.hpp"
#include "RHI/Renderer.h"
#include "RHI/Mesh.h"
#include "Submodules/Editor.h"
#include "Support/ScopeExit.h"
#include "Tasks/Tasks.h"
#define STB_IMAGE_STATIC
#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG
#define STBI_NO_STDIO
#include <stb_image.h>
#include <nlohmann/json.hpp>

#include <chrono>
#include <array>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <future>
#include <iostream>
#include <stdexcept>
#include <thread>

using namespace Sailor;
using namespace Sailor::EditorRuntime;

namespace
{
	void Require(bool value, const std::string& message)
	{
		if (!value)
		{
			throw std::runtime_error(message);
		}
	}

	std::string CaptureSceneSnapshot()
	{
		char* buffer = nullptr;
		const uint32_t length = EditorRuntime::SerializeCurrentWorld(&buffer);
		TUniquePtr<char[]> ownedBuffer(buffer);
		Require(buffer && length != 0, "the active scene must provide a serialized snapshot");
		return std::string(buffer, length);
	}

	MaterialPreviewResult WaitForPreview(uint64_t session, uint64_t revision)
	{
		const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(60);
		while (std::chrono::steady_clock::now() < deadline)
		{
			auto result = GetMaterialPreview(session);
			if (result.m_revision == revision && result.m_state != EMaterialPreviewState::Pending)
			{
				return result;
			}
			std::this_thread::sleep_for(std::chrono::milliseconds(5));
		}
		throw std::runtime_error("Material preview did not complete within 60 seconds");
	}

	glm::u64vec3 Inspect(const MaterialPreviewResult& result, const char* name, uint32_t minimumChanged = 800)
	{
		Require(result.m_state == EMaterialPreviewState::Ready, std::string(name) + ": " + result.m_diagnostic);
		int width = 0, height = 0, channels = 0;
		auto pixels = stbi_load_from_memory(result.m_png.data(), static_cast<int>(result.m_png.size()), &width, &height, &channels, 4);
		Require(pixels && width == 128 && height == 128, "preview must return its own requested extent and a valid PNG");
		glm::u64vec3 sum(0);
		uint32_t changed = 0;
		for (int i = 0; i < width * height; ++i)
		{
			for (int channel = 0; channel < 3; ++channel)
			{
				sum[channel] += pixels[i * 4 + channel];
			}
			if (std::abs(int(pixels[i * 4]) - pixels[0]) + std::abs(int(pixels[i * 4 + 1]) - pixels[1]) +
				std::abs(int(pixels[i * 4 + 2]) - pixels[2]) > 20)
			{
				++changed;
			}
		}
		stbi_image_free(pixels);
		Require(changed > minimumChanged, std::string(name) + " must contain visible shaded geometry, not only a cleared background");
		if (const char* directory = std::getenv("SAILOR_MATERIAL_PREVIEW_EVIDENCE_DIR"))
		{
			std::filesystem::create_directories(directory);
			std::ofstream output(std::filesystem::path(directory) / (std::string(name) + ".png"), std::ios::binary);
			output.write(reinterpret_cast<const char*>(result.m_png.data()), static_cast<std::streamsize>(result.m_png.size()));
		}
		return sum;
	}

	uint32_t ChangedPixels(const MaterialPreviewResult& first, const MaterialPreviewResult& second)
	{
		int width, height, channels;
		auto a = stbi_load_from_memory(first.m_png.data(), static_cast<int>(first.m_png.size()), &width, &height, &channels, 4);
		auto b = stbi_load_from_memory(second.m_png.data(), static_cast<int>(second.m_png.size()), &width, &height, &channels, 4);
		Require(a && b, "pixel comparison requires decodable previews");
		uint32_t changed = 0;
		for (int i = 0; i < width * height; ++i)
		{
			const int difference = std::abs(int(a[i * 4]) - b[i * 4]) + std::abs(int(a[i * 4 + 1]) - b[i * 4 + 1]) +
				std::abs(int(a[i * 4 + 2]) - b[i * 4 + 2]);
			if (difference > 9)
			{
				++changed;
			}
		}
		stbi_image_free(a);
		stbi_image_free(b);
		return changed;
	}

	void WritePreviewText(const std::filesystem::path& path, const std::string& text)
	{
		std::ofstream output(path, std::ios::binary | std::ios::trunc);
		output << text;
		output.close();
		Require(static_cast<bool>(output), "preview fixture file must be written");
	}

	struct ModelPreviewFixture
	{
		ModelPreviewFixture(const std::filesystem::path& content, const char* name,
			FileId firstMaterial, FileId secondMaterial, bool binary, bool batch, bool wide, bool missing = false) :
			m_path(content / (std::string(name) + (binary ? ".glb" : ".gltf"))),
			m_vertices(content / (std::string(name) + ".bin")), m_binary(binary), m_id(FileId::CreateNewFileId())
		{
			m_document = nlohmann::json::parse(R"({
				"asset":{"version":"2.0"},"scene":0,"scenes":[{"nodes":[0]}],
				"nodes":[{"translation":[17,-9,4],"scale":[7,7,7],"children":[1,2,3]},
					{"mesh":0,"translation":[-1.25,0,0]},
					{"mesh":1,"translation":[1.25,0,0]},
					{"mesh":0,"translation":[-1.25,0,0.05]}],
				"buffers":[{"byteLength":96,"uri":""}],
				"bufferViews":[{"buffer":0,"byteOffset":0,"byteLength":36},
					{"buffer":0,"byteOffset":36,"byteLength":36},
					{"buffer":0,"byteOffset":72,"byteLength":24}],
				"accessors":[{"bufferView":0,"componentType":5126,"count":3,"type":"VEC3","min":[-1,-1,0],"max":[1,1,0]},
					{"bufferView":1,"componentType":5126,"count":3,"type":"VEC3"},
					{"bufferView":2,"componentType":5126,"count":3,"type":"VEC2"}],
				"materials":[],"meshes":[]})");
			m_document["buffers"][0]["uri"] = m_vertices.filename().string();
			const size_t meshCount = wide ? 257 : 2;
			for (size_t i = 0; i < meshCount; ++i)
			{
				m_document["materials"].push_back(nlohmann::json::object());
				auto primitive = nlohmann::json::parse(R"({"attributes":{"POSITION":0,"NORMAL":1,"TEXCOORD_0":2}})");
				if (!missing || i != 1)
				{
					primitive["material"] = i;
				}
				m_document["meshes"].push_back({ { "primitives", nlohmann::json::array({ primitive }) } });
			}
			m_document["nodes"][1]["mesh"] = meshCount - 2;
			m_document["nodes"][2]["mesh"] = meshCount - 1;
			m_document["nodes"][3]["mesh"] = meshCount - 2;
			WriteVertices(0);
			WriteSource();
			m_metadata = CreateAssetInfoMetadata<ModelAssetInfo>(m_id, m_path.filename().string());
			m_metadata["bShouldGenerateMaterials"] = false;
			m_metadata["bShouldBatchByMaterial"] = batch;
			m_metadata["bGenerateBLAS"] = false;
			m_metadata["bGenerateLods"] = false;
			m_metadata["bShouldKeepCpuBuffers"] = false;
			TVector<FileId> slots;
			slots.Resize(meshCount);
			slots[meshCount - 2] = firstMaterial;
			slots[meshCount - 1] = secondMaterial;
			m_metadata["materials"] = slots;
			WriteMetadata();
			auto* registry = App::GetSubmodule<AssetRegistry>();
			Require(registry->GetOrLoadFile(m_path.string()) == m_id, "model preview fixture must register with its supplied metadata");
			m_info = registry->GetAssetInfoPtr<ModelAssetInfoPtr>(m_id);
			Require(m_info != nullptr, "model metadata must resolve");
		}

		void WriteVertices(float tipX) const
		{
			const std::array<float, 24> vertices{ -1,-1,0, 1,-1,0, tipX,1,0,
				0,0,1, 0,0,1, 0,0,1, 0,0, 1,0, 0.5f,1 };
			std::ofstream output(m_vertices, std::ios::binary | std::ios::trunc);
			output.write(reinterpret_cast<const char*>(vertices.data()), sizeof(vertices));
			output.close();
			Require(static_cast<bool>(output), "external model geometry must be written");
		}

		void WriteSource() const
		{
			std::string json = m_document.dump();
			if (!m_binary)
			{
				WritePreviewText(m_path, json);
				return;
			}
			while (json.size() % 4 != 0)
			{
				json.push_back(' ');
			}
			std::ofstream output(m_path, std::ios::binary | std::ios::trunc);
			for (const uint32_t word : { 0x46546c67u, 2u, static_cast<uint32_t>(20 + json.size()),
				static_cast<uint32_t>(json.size()), 0x4e4f534au })
			{
				for (uint32_t shift = 0; shift < 32; shift += 8)
				{
					output.put(static_cast<char>((word >> shift) & 255));
				}
			}
			output << json;
			output.close();
			Require(static_cast<bool>(output), "binary glTF with an external buffer must be written");
		}

		void WriteMetadata() const
		{
			WritePreviewText(m_path.string() + ".asset", YAML::Dump(m_metadata));
		}

		std::filesystem::path m_path, m_vertices;
		bool m_binary;
		FileId m_id;
		ModelAssetInfoPtr m_info = nullptr;
		YAML::Node m_metadata;
		nlohmann::json m_document;
	};

	glm::uvec4 InspectModel(const MaterialPreviewResult& result, const char* name)
	{
		Inspect(result, name, 100);
		int width = 0, height = 0, channels = 0;
		auto pixels = stbi_load_from_memory(result.m_png.data(), static_cast<int>(result.m_png.size()), &width, &height, &channels, 4);
		glm::uvec4 counts(0);
		for (int y = 0; y < height; ++y)
		{
			for (int x = 0; x < width; ++x)
			{
				const auto* pixel = pixels + (y * width + x) * 4;
				const int difference = std::abs(int(pixel[0]) - pixels[0]) + std::abs(int(pixel[1]) - pixels[1]) +
					std::abs(int(pixel[2]) - pixels[2]);
				if (difference <= 20)
				{
					continue;
				}
				Require(x > 3 && y > 3 && x < width - 4 && y < height - 4,
					"normalized aggregate model bounds must fit the default camera with an image margin");
				if (pixel[0] > pixel[1] * 1.3f && pixel[0] > pixel[2] * 1.3f)
				{
					++counts.r;
				}
				else if (pixel[1] > pixel[0] * 1.3f && pixel[1] > pixel[2] * 1.3f)
				{
					++counts.g;
				}
				else if (pixel[2] > pixel[0] * 1.3f && pixel[2] > pixel[1] * 1.3f)
				{
					++counts.b;
				}
				else
				{
					++counts.a;
				}
			}
		}
		stbi_image_free(pixels);
		return counts;
	}

	void RunModelPreviewGpuTests(const std::filesystem::path& workspace)
	{
		auto* materialImporter = App::GetSubmodule<MaterialImporter>();
		auto* modelImporter = App::GetSubmodule<ModelImporter>();
		const auto content = workspace / "Content";
		MaterialAsset::Data data;
		data.m_shader = FileId("1A4BA353-FDA4-4F65-941F-D9FFEE4630A0");
		data.m_renderState = RHI::RenderState(true, true, 0, false, RHI::ECullMode::None,
			RHI::EBlendMode::None, RHI::EFillMode::Fill, "Opaque"_h.GetHash(), true);
		data.m_uniformsVec4["material.baseColorFactor"] = glm::vec4(0.9f, 0.03f, 0.03f, 1);
		data.m_uniformsFloat["material.roughnessFactor"] = 0.6f;
		data.m_uniformsFloat["material.metallicFactor"] = 0;
		const auto redPath = content / "ModelPreviewRed.mat";
		const auto redId = materialImporter->CreateMaterialAsset(redPath.string(), data);
		MaterialPtr cachedMaterial;
		Require(materialImporter->LoadMaterial_Immediate(redId, cachedMaterial), "model material cache fixture must load");
		const auto cachedRevision = cachedMaterial->GetContentRevision();
		data.m_uniformsVec4["material.baseColorFactor"] = glm::vec4(0.03f, 0.03f, 0.9f, 1);
		const auto blueId = materialImporter->CreateMaterialAsset((content / "ModelPreviewBlue.mat").string(), data);
		MaterialPreviewRequest request;
		request.m_width = request.m_height = 128;
		request.m_yaw = 0.45f;
		request.m_pitch = 0.25f;
		request.m_distance = 3;
		request.m_primitive = static_cast<EMaterialPreviewPrimitive>(0);
		MaterialPreviewResult accepted;
		std::string diagnostic;
		const auto render = [&](const char* name)
		{
			++request.m_revision;
			Require(UpdateMaterialPreview(request, accepted, diagnostic), diagnostic);
			auto result = WaitForPreview(request.m_sessionId, request.m_revision);
			InspectModel(result, name);
			return result;
		};

		for (bool batch : { true, false })
		{
			ModelPreviewFixture fixture(content, batch ? "PreviewWideGltf" : "PreviewWideGlb",
				redId, blueId, !batch, batch, true);
			request.m_sessionId = batch ? 500 : 501;
			request.m_revision = 0;
			request.m_modelFileId = fixture.m_id.ToString();
			const auto before = render(batch ? "model-wide-gltf" : "model-wide-glb");
			const auto colors = InspectModel(before, batch ? "model-wide-gltf" : "model-wide-glb");
			Require(colors.r > 100 && colors.b > 100,
				"material slots 255 and 256 must remain distinct with batching enabled and disabled");
			const auto warm = GetMaterialPreviewStatistics().m_initializationCount;
			request.m_yaw += 0.1f;
			render("model-warm-orbit");
			Require(GetMaterialPreviewStatistics().m_initializationCount == warm,
				"model camera updates must reuse imported geometry, materials, lighting and graph");
			request.m_yaw -= 0.1f;
			fixture.WriteVertices(0.75f);
			const auto external = render(batch ? "model-gltf-external-reload" : "model-glb-external-reload");
			Require(GetMaterialPreviewStatistics().m_initializationCount == warm + 1 && ChangedPixels(before, external) > 100,
				"both glTF and GLB external geometry edits must invalidate cached GPU previews and change pixels");
			CloseMaterialPreview(request.m_sessionId);
		}

		ModelPreviewFixture fixture(content, "PreviewFreshSources", redId, blueId, false, true, false);
		ModelPtr cachedModel;
		Require(modelImporter->LoadModel_Immediate(fixture.m_id, cachedModel), "global model cache fixture must load");
		const auto cachedNodes = cachedModel->GetNodes().Num();
		const auto cachedMatrix = cachedModel->GetRenderInstances()[0].m_modelMatrix;
		const auto cachedMetadata = YAML::Dump(fixture.m_info->Serialize());
		fixture.m_metadata["bGenerateLods"] = true;
		fixture.m_metadata["bGenerateBLAS"] = true;
		fixture.m_metadata["bShouldKeepCpuBuffers"] = true;
		fixture.WriteMetadata();
		auto allocator = Memory::ObjectAllocatorPtr::Make();
		ModelPtr privateModel;
		auto load = modelImporter->CreateTransientModel(fixture.m_id, allocator, privateModel);
		Require(static_cast<bool>(load), "private model import must start");
		load->Wait();
		Require(static_cast<bool>(load->GetResult()) && !privateModel->HasBLAS() && !privateModel->HasCpuMeshes() &&
			privateModel->GetRenderInstances().Num() == 3 && privateModel->GetNodes().Num() == 4,
			"preview import must retain hierarchy and repeated instances without retained CPU geometry or a BLAS");
		for (auto mesh : privateModel->GetMeshes())
		{
			Require(mesh->m_lods.IsEmpty(), "preview imports must use original geometry without generating derived LODs");
		}
		App::GetSubmodule<Tasks::Scheduler>()->WaitIdle({ EThreadType::Render, EThreadType::Worker, EThreadType::RHI });
		RHI::Renderer::GetDriver()->TrackResources_ThreadSafe();
		privateModel.DestroyObject(allocator);
		const auto legacy = App::GetWorkspaceContext().GetCache() / "Fingerprints" / (fixture.m_id.ToString() + ".png");
		std::filesystem::create_directories(legacy.parent_path());
		WritePreviewText(legacy, "obsolete CPU fingerprint");
		request.m_sessionId = 502;
		request.m_revision = 0;
		request.m_modelFileId = fixture.m_id.ToString();
		const auto original = render("model-fresh-original");
		data.m_uniformsVec4["material.baseColorFactor"] = glm::vec4(0.03f, 0.9f, 0.03f, 1);
		const auto greenYaml = YAML::Dump(MaterialAsset::Serialize(data));
		WritePreviewText(redPath, greenYaml);
		const auto green = render("model-material-disk-reload");
		Require(InspectModel(green, "model-material-disk-reload").g > 100 && ChangedPixels(original, green) > 100 &&
			cachedMaterial->GetContentRevision() == cachedRevision,
			"fresh material file edits must reach the GPU before asset reload publication without changing cached materials");
		fixture.m_metadata["materials"] = TVector<FileId>{ blueId, redId };
		fixture.WriteMetadata();
		const auto swapped = render("model-metadata-disk-reload");
		Require(ChangedPixels(green, swapped) > 200 && YAML::Dump(fixture.m_info->Serialize()) == cachedMetadata,
			"fresh metadata slots must be consumed even while registered ModelAssetInfo still holds older settings");
		fixture.m_document["nodes"][1]["translation"] = { -1.25, 0.8, 0 };
		fixture.WriteSource();
		const auto moved = render("model-hierarchy-disk-reload");
		Require(ChangedPixels(swapped, moved) > 100 && cachedModel->GetNodes().Num() == cachedNodes &&
			cachedModel->GetRenderInstances()[0].m_modelMatrix == cachedMatrix,
			"source hierarchy edits must load fresh geometry without mutating the cached scene model");

		auto* scheduler = App::GetSubmodule<Tasks::Scheduler>();
		scheduler->WaitIdle({ EThreadType::Background, EThreadType::Render, EThreadType::Worker, EThreadType::RHI });
		std::promise<void> entered, release;
		Sailor::Tests::ScopeExit releaseOnFailure([&]() { release.set_value(); });
		auto gate = release.get_future().share();
		auto blocker = Tasks::CreateTask("Hold model preview source revision"_h,
			[&]() { entered.set_value(); gate.wait(); }, EThreadType::Render);
		blocker->Run();
		entered.get_future().wait();
		Require(scheduler->GetNumTasks(EThreadType::Render) == 0, "source race starts with only the active Render blocker");
		++request.m_revision;
		Require(UpdateMaterialPreview(request, accepted, diagnostic), diagnostic);
		const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
		while (scheduler->GetNumTasks(EThreadType::Render) == 0 && std::chrono::steady_clock::now() < deadline)
		{
			std::this_thread::sleep_for(std::chrono::milliseconds(1));
		}
		Require(GetMaterialPreviewStatistics().m_inFlight && scheduler->GetNumTasks(EThreadType::Render) != 0,
			"source race must reach the queued preview Render task after checking cached source revisions");
		fixture.m_document["nodes"][2]["translation"] = { 1.25, -0.5, 0 };
		fixture.WriteSource();
		releaseOnFailure.Run();
		const auto stale = WaitForPreview(request.m_sessionId, request.m_revision);
		Require(stale.m_state == EMaterialPreviewState::Error && stale.m_diagnostic.find("changed") != std::string::npos,
			"a source changed during rendering must never publish a stale image as current");
		render("model-after-source-race");
		scheduler->WaitIdle({ EThreadType::Background, EThreadType::Render, EThreadType::Worker, EThreadType::RHI });
		std::promise<void> closeEntered, closeRelease;
		Sailor::Tests::ScopeExit closeReleaseOnFailure([&]() { closeRelease.set_value(); });
		auto closeGate = closeRelease.get_future().share();
		auto closeBlocker = Tasks::CreateTask("Hold active private model preview"_h,
			[&]() { closeEntered.set_value(); closeGate.wait(); }, EThreadType::Render);
		closeBlocker->Run();
		closeEntered.get_future().wait();
		const auto beforeClose = GetMaterialPreviewRenderCount();
		++request.m_revision;
		Require(UpdateMaterialPreview(request, accepted, diagnostic), diagnostic);
		const auto closeDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
		while (scheduler->GetNumTasks(EThreadType::Render) == 0 && std::chrono::steady_clock::now() < closeDeadline)
		{
			std::this_thread::sleep_for(std::chrono::milliseconds(1));
		}
		Require(scheduler->GetNumTasks(EThreadType::Render) != 0, "close must overlap an active private model request");
		CloseMaterialPreview(request.m_sessionId);
		closeReleaseOnFailure.Run();
		scheduler->WaitIdle({ EThreadType::Background, EThreadType::Render, EThreadType::Worker, EThreadType::RHI });
		Require(GetMaterialPreview(request.m_sessionId).m_state == EMaterialPreviewState::Closed &&
			GetMaterialPreviewRenderCount() == beforeClose && GetMaterialPreviewStatistics().m_liveJobs == 0,
			"closing an active model request must discard publication and safely release its private model after retirement");

		for (bool missing : { false, true })
		{
			ModelPreviewFixture sparse(content, missing ? "PreviewMissingSlot" : "PreviewSparseSlot", blueId, redId,
				false, true, false, missing);
			sparse.m_metadata["materials"] = TVector<FileId>{ blueId };
			sparse.WriteMetadata();
			request.m_sessionId = missing ? 504 : 503;
			request.m_revision = 0;
			request.m_modelFileId = sparse.m_id.ToString();
			const auto result = render(missing ? "model-missing-material" : "model-sparse-material-slots");
			const auto colors = InspectModel(result, missing ? "model-missing-material" : "model-sparse-material-slots");
			Require(colors.b > 100 && colors.a > 100 && colors.g < 20 && colors.r < 20,
				"missing or out-of-range material slots must use neutral shading without borrowing another slot");
			CloseMaterialPreview(request.m_sessionId);
		}

		data.m_renderState = RHI::RenderState(true, true, 0, false, RHI::ECullMode::None,
			RHI::EBlendMode::None, RHI::EFillMode::Fill, "Masked"_h.GetHash(), true);
		data.m_shaderDefines = { "ALPHA_CUTOUT" };
		data.m_uniformsFloat["material.alphaCutoff"] = 0.5f;
		data.m_uniformsVec4["material.baseColorFactor"] = glm::vec4(0.03f, 0.03f, 0.9f, 1);
		const auto maskedId = materialImporter->CreateMaterialAsset((content / "ModelPreviewMasked.mat").string(), data);
		data.m_renderState = RHI::RenderState(true, false, 0, false, RHI::ECullMode::None,
			RHI::EBlendMode::AlphaBlending, RHI::EFillMode::Fill, "Transparent"_h.GetHash(), true);
		data.m_shaderDefines.Clear();
		data.m_uniformsVec4["material.baseColorFactor"] = glm::vec4(0.9f, 0.03f, 0.03f, 0.5f);
		const auto transparentId = materialImporter->CreateMaterialAsset((content / "ModelPreviewTransparent.mat").string(), data);
		ModelPreviewFixture mixed(content, "PreviewMixedQueues", redId, maskedId, false, true, false);
		mixed.m_document["materials"].push_back(nlohmann::json::object());
		auto transparentMesh = mixed.m_document["meshes"][0];
		transparentMesh["primitives"][0]["material"] = 2;
		mixed.m_document["meshes"].push_back(transparentMesh);
		mixed.m_document["nodes"][3]["mesh"] = 2;
		mixed.WriteSource();
		mixed.m_metadata["materials"] = TVector<FileId>{ redId, maskedId, transparentId };
		mixed.WriteMetadata();
		request.m_sessionId = 505;
		request.m_revision = 0;
		request.m_modelFileId = mixed.m_id.ToString();
		render("model-opaque-masked-transparent");
		CloseMaterialPreview(request.m_sessionId);

		data.m_shader = App::GetSubmodule<AssetRegistry>()->GetAssetInfoPtr("Shaders/ComputeBrdfLut.shader")->GetFileId();
		const auto unsupportedId = materialImporter->CreateMaterialAsset((content / "ModelPreviewUnsupported.mat").string(), data);
		mixed.m_metadata["materials"] = TVector<FileId>{ unsupportedId, maskedId, transparentId };
		mixed.WriteMetadata();
		request.m_sessionId = 506;
		request.m_revision = 1;
		const auto beforeInvalid = GetMaterialPreviewRenderCount();
		Require(UpdateMaterialPreview(request, accepted, diagnostic), diagnostic);
		const auto unsupported = WaitForPreview(request.m_sessionId, request.m_revision);
		Require(unsupported.m_state == EMaterialPreviewState::Error && unsupported.m_diagnostic.find("material") != std::string::npos &&
			GetMaterialPreviewRenderCount() == beforeInvalid,
			"a supplied unsupported model shader must report an error instead of being replaced by a neutral material");
		CloseMaterialPreview(request.m_sessionId);
		request.m_sessionId = 507;
		request.m_modelFileId = FileId::CreateNewFileId().ToString();
		Require(UpdateMaterialPreview(request, accepted, diagnostic), diagnostic);
		Require(WaitForPreview(request.m_sessionId, request.m_revision).m_state == EMaterialPreviewState::Error &&
			GetMaterialPreviewRenderCount() == beforeInvalid, "an unknown model must fail without creating a render submission");
		CloseMaterialPreview(request.m_sessionId);
		for (uint32_t level = 1; level <= 2; ++level)
		{
			Require(!std::filesystem::exists(App::GetWorkspaceContext().GetCache() / "Lods" /
				ModelImporter::GetLodCacheFilename(fixture.m_id, level)), "model previews must not publish derived LOD cache files");
		}
		std::string after;
		Require(AssetRegistry::ReadAllTextFile(redPath.string(), after) && after == greenYaml &&
			AssetRegistry::ReadAllTextFile(fixture.m_path.string() + ".asset", after) && after == YAML::Dump(fixture.m_metadata),
			"model previews must not rewrite saved model metadata or material sources");
		Require(AssetRegistry::ReadAllTextFile(legacy.string(), after) && after == "obsolete CPU fingerprint",
			"old CPU thumbnail cache files must neither replace the GPU result nor be rewritten");
		App::GetSubmodule<Tasks::Scheduler>()->WaitIdle({ EThreadType::Background, EThreadType::Render, EThreadType::Worker, EThreadType::RHI });
		Require(GetMaterialPreviewStatistics().m_liveJobs == 0, "closed model sessions must release private meshes and materials");
		std::cout << "Model GPU preview: hierarchy, repeated instances, slots 255/256, neutral fallback, GLTF/GLB external buffers, fresh source/material/metadata, source race, original LOD and isolation passed\n";
	}
}

namespace Sailor::Tests
{
void RunMaterialPreviewCommandTests(const std::filesystem::path& workspace)
{
	auto* renderer = App::GetSubmodule<RHI::Renderer>();
	auto* editor = App::GetSubmodule<Editor>();
	const auto world = editor ? editor->GetWorld() : nullptr;
	const auto sceneGraph = renderer->GetFrameGraph();
	const auto sceneExtent = App::GetMainWindow()->GetRenderArea();
	const auto sceneSamples = renderer->GetMsaaSamples();
	const auto renderMode = App::GetEditorRenderMode();
	const auto readback = renderer->GetEditorReadback();
	const auto serializedScene = editor && world ? CaptureSceneSnapshot() : std::string{};

	MaterialAsset::Data data;
	data.m_shader = FileId("1A4BA353-FDA4-4F65-941F-D9FFEE4630A0");
	data.m_renderState = RHI::RenderState(true, true, 0, false, RHI::ECullMode::Back, RHI::EBlendMode::None, RHI::EFillMode::Fill, "Opaque"_h.GetHash(), true);
	data.m_uniformsVec4["material.baseColorFactor"] = glm::vec4(0.9f, 0.1f, 0.05f, 1);
	data.m_uniformsVec4["material.emissiveFactor"] = glm::vec4(0);
	data.m_uniformsFloat["material.roughnessFactor"] = 0.6f;
	data.m_uniformsFloat["material.metallicFactor"] = 0;
	const auto path = workspace / "Content" / "PreviewSource.mat";
	auto* importer = App::GetSubmodule<MaterialImporter>();
	const auto materialId = importer->CreateMaterialAsset(path.string(), data);
	MaterialPtr savedMaterial;
	Require(importer->LoadMaterial_Immediate(materialId, savedMaterial), "saved material fixture must load");
	auto savedRevision = savedMaterial->GetContentRevision();
	std::string source;
	Require(AssetRegistry::ReadAllTextFile(path.string(), source), "source material fixture must exist");
	MaterialPreviewRequest request;
	request.m_sessionId = 216;
	request.m_revision = 1;
	request.m_materialYaml = source;
	request.m_width = request.m_height = 128;
	request.m_yaw = 0.45f;
	request.m_pitch = 0.2f;
	request.m_distance = 3.0f;
	MaterialPreviewResult accepted;
	std::string diagnostic;
	const auto render = [&](const char* name)
	{
		Require(UpdateMaterialPreview(request, accepted, diagnostic), diagnostic);
		auto result = WaitForPreview(request.m_sessionId, request.m_revision);
		Inspect(result, name);
		return result;
	};
	const auto sphere = render("sphere-red");
	Require(GetMaterialPreview(216, 1).m_png.empty(), "unchanged preview polling must not resend PNG bytes");
	request.m_revision++;
	request.m_primitive = EMaterialPreviewPrimitive::Plane;
	const auto plane = render("plane-red");
	request.m_revision++;
	request.m_primitive = EMaterialPreviewPrimitive::Box;
	const auto box = render("box-red");
	Require(sphere.m_png != plane.m_png && plane.m_png != box.m_png, "the three real primitive meshes must produce different silhouettes");
	data.m_uniformsVec4["material.baseColorFactor"] = glm::vec4(0.05f, 0.1f, 0.9f, 1);
	request.m_materialYaml = YAML::Dump(MaterialAsset::Serialize(data));
	request.m_revision++;
	const auto blue = render("box-blue-unsaved");
	Require(Inspect(blue, "box-blue-unsaved").b > Inspect(box, "box-red").b * 2,
		"unsaved shader uniforms must change actual GPU output");
	request.m_revision++;
	request.m_yaw += 1.0f;
	request.m_distance = 3.5f;
	const auto beforeOrbit = GetMaterialPreviewStatistics().m_initializationCount;
	const auto orbit = render("box-orbit-zoom");
	Require(orbit.m_png != blue.m_png, "orbit and zoom must change the private camera image");
	Require(GetMaterialPreviewStatistics().m_initializationCount == beforeOrbit,
		"camera-only changes must reuse the private material, geometry, lighting, targets and graph");
	const auto warmStart = std::chrono::steady_clock::now();
	const auto beforeWarmDispatches = GetMaterialPreviewStatistics().m_renderDispatchCount;
	for (int i = 0; i < 8; ++i)
	{
		++request.m_revision;
		request.m_yaw += 0.1f;
		Require(UpdateMaterialPreview(request, accepted, diagnostic), diagnostic);
		Require(WaitForPreview(request.m_sessionId, request.m_revision).m_state == EMaterialPreviewState::Ready,
			"consecutive camera updates must complete using a retired private submission context");
	}
	Require(GetMaterialPreviewStatistics().m_initializationCount == beforeOrbit, "warm orbit must not rebuild GPU resources");
	Require(GetMaterialPreviewStatistics().m_renderDispatchCount == beforeWarmDispatches + 8,
		"each warm camera update must prepare and submit in a single Render dispatch");
	std::cout << "Material preview warm orbit: " <<
		std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - warmStart).count() / 8.0 << " ms/update\n";
	request.m_primitive = EMaterialPreviewPrimitive::Sphere;
	request.m_distance = 3;
	request.m_yaw = 0.45f;
	data.m_uniformsVec4["material.baseColorFactor"] = glm::vec4(0.8f, 0.8f, 0.8f, 1);
	data.m_uniformsFloat["material.metallicFactor"] = 1;
	data.m_uniformsFloat["material.roughnessFactor"] = 0.05f;
	request.m_revision++;
	request.m_materialYaml = YAML::Dump(MaterialAsset::Serialize(data));
	const auto polished = render("sphere-polished");
	data.m_uniformsFloat["material.roughnessFactor"] = 0.9f;
	request.m_revision++;
	request.m_materialYaml = YAML::Dump(MaterialAsset::Serialize(data));
	const auto rough = render("sphere-rough");
	Require(ChangedPixels(polished, rough) > 100, "roughness edits must change shaded GPU pixels under studio lighting");
	std::array<uint8_t, 21> textureBytes{};
	textureBytes[2] = 2;
	textureBytes[12] = textureBytes[14] = 1;
	textureBytes[16] = 24;
	textureBytes[19] = 255;
	const auto texturePath = workspace / "Content" / "PreviewGreen.tga";
	std::ofstream textureFile(texturePath, std::ios::binary);
	textureFile.write(reinterpret_cast<const char*>(textureBytes.data()), textureBytes.size());
	textureFile.close();
	const auto textureId = App::GetSubmodule<AssetRegistry>()->GetOrLoadFile(texturePath.string());
	Require(static_cast<bool>(textureId), "texture fixture must register");
	data.m_uniformsVec4["material.baseColorFactor"] = glm::vec4(1);
	data.m_uniformsFloat["material.metallicFactor"] = 0;
	data.m_samplers["baseColorSampler"] = textureId;
	request.m_revision++;
	request.m_materialYaml = YAML::Dump(MaterialAsset::Serialize(data));
	const auto textured = render("sphere-texture-green");
	const auto textureSum = Inspect(textured, "sphere-texture-green");
	Require(textureSum.g > textureSum.r * 2 && textureSum.g > textureSum.b * 2,
		"unsaved texture references must reach the shader's actual texture binding table");
	const auto beforeTextureReload = GetMaterialPreviewStatistics().m_initializationCount;
	textureBytes[19] = 0;
	textureBytes[20] = 255;
	std::ofstream redTextureFile(texturePath, std::ios::binary | std::ios::trunc);
	redTextureFile.write(reinterpret_cast<const char*>(textureBytes.data()), textureBytes.size());
	redTextureFile.close();
	auto* textureImporter = App::GetSubmodule<TextureImporter>();
	textureImporter->OnUpdateAssetInfo(App::GetSubmodule<AssetRegistry>()->GetAssetInfoPtr(textureId), true);
	auto reloadTexture = textureImporter->GetLoadPromise(textureId);
	Require(static_cast<bool>(reloadTexture), "texture reload must be scheduled");
	reloadTexture->Wait();
	Require(static_cast<bool>(reloadTexture->GetResult()), "texture reload must publish successfully");
	++request.m_revision;
	const auto reloadedTexture = render("sphere-texture-reloaded-red");
	const auto reloadedSum = Inspect(reloadedTexture, "sphere-texture-reloaded-red");
	Require(reloadedSum.r > reloadedSum.g * 2 && GetMaterialPreviewStatistics().m_initializationCount == beforeTextureReload + 1,
		"a texture publication must invalidate the warm cache even with identical material YAML");
	Require(savedMaterial->GetContentRevision() == savedRevision, "preview reuse and texture reload must preserve unrelated saved materials");
	auto* compiler = App::GetSubmodule<ShaderCompiler>();
	const auto beforeShaderReload = GetMaterialPreviewStatistics().m_initializationCount;
	auto reloadShader = ShaderCompilerTestAccess::ReloadShaderResources(*compiler, data.m_shader);
	reloadShader->Wait();
	Require(reloadShader->GetResult(), "surface shader reload must publish successfully");
	App::GetSubmodule<Tasks::Scheduler>()->WaitIdle({ EThreadType::Render, EThreadType::Worker, EThreadType::RHI });
	// The explicit shader reload also refreshes the registered fixture material.
	savedRevision = savedMaterial->GetContentRevision();
	++request.m_revision;
	render("sphere-shader-reloaded");
	Require(GetMaterialPreviewStatistics().m_initializationCount == beforeShaderReload + 1,
		"a surface shader publication must invalidate the warm cache with unchanged YAML");
	request.m_revision++;
	request.m_materialYaml = "shaderUid: [broken";
	Require(UpdateMaterialPreview(request, accepted, diagnostic), diagnostic);
	Require(WaitForPreview(216, request.m_revision).m_state == EMaterialPreviewState::Error,
		"invalid unsaved material must report an error without modifying the saved asset");
	const auto computeShader = App::GetSubmodule<AssetRegistry>()->GetAssetInfoPtr("Shaders/ComputeBrdfLut.shader");
	Require(computeShader != nullptr, "non-surface shader fixture must exist");
	data.m_shader = computeShader->GetFileId();
	request.m_revision++;
	request.m_materialYaml = YAML::Dump(MaterialAsset::Serialize(data));
	const auto beforeUnsupported = GetMaterialPreviewRenderCount();
	Require(UpdateMaterialPreview(request, accepted, diagnostic), diagnostic);
	const auto unsupported = WaitForPreview(216, request.m_revision);
	Require(unsupported.m_state == EMaterialPreviewState::Error && unsupported.m_diagnostic.find("surface") != std::string::npos &&
		GetMaterialPreviewRenderCount() == beforeUnsupported,
		"unsupported shader must report a diagnostic instead of publishing an empty ready image");

	// Hold Background to make replacement and close ordering deterministic.
	std::promise<void> entered, release;
	ScopeExit releaseOnFailure([&]() { release.set_value(); });
	auto gate = release.get_future().share();
	auto blocked = Tasks::CreateTask("Hold preview background for lifecycle test"_h, [&]() { entered.set_value(); gate.wait(); }, EThreadType::Background);
	blocked->Run();
	entered.get_future().wait();
	const auto beforeClosed = GetMaterialPreviewRenderCount();
	request.m_revision++;
	request.m_materialYaml = source;
	Require(UpdateMaterialPreview(request, accepted, diagnostic), diagnostic);
	Require(CloseMaterialPreview(216).m_state == EMaterialPreviewState::Closed, "close must synchronously invalidate pending output");
	releaseOnFailure.Run();
	App::GetSubmodule<Tasks::Scheduler>()->WaitIdle({ EThreadType::Background, EThreadType::Render, EThreadType::Worker, EThreadType::RHI });
	Require(GetMaterialPreview(216).m_state == EMaterialPreviewState::Closed && GetMaterialPreviewRenderCount() == beforeClosed,
		"closed previews must not publish late results or submit new render work");
	for (int i = 0; i < 10; ++i)
	{
		GetMaterialPreview(216);
	}
	Require(GetMaterialPreviewRenderCount() == beforeClosed, "status polling must not render hidden previews");
	request.m_revision++;
	Require(UpdateMaterialPreview(request, accepted, diagnostic) && accepted.m_state == EMaterialPreviewState::Closed,
		"late updates must not reopen a closed session");

	std::promise<void> replacementEntered, releaseReplacements;
	ScopeExit releaseReplacementsOnFailure([&]() { releaseReplacements.set_value(); });
	auto replacementGate = releaseReplacements.get_future().share();
	auto replacementBlocker = Tasks::CreateTask("Hold preview replacements"_h,
		[&]() { replacementEntered.set_value(); replacementGate.wait(); }, EThreadType::Background);
	replacementBlocker->Run();
	replacementEntered.get_future().wait();
	request.m_sessionId = 217;
	for (uint64_t revision = 1; revision <= 8; ++revision)
	{
		request.m_revision = revision;
		Require(UpdateMaterialPreview(request, accepted, diagnostic), diagnostic);
	}
	releaseReplacementsOnFailure.Run();
	const auto newest = WaitForPreview(217, 8);
	Inspect(newest, "coalesced-latest");
	Require(GetMaterialPreviewRenderCount() == beforeClosed + 1,
		"rapid queued edits must render only the latest pending revision");
	CloseMaterialPreview(217);

	std::promise<void> queueEntered, releaseQueue;
	ScopeExit releaseQueueOnFailure([&]() { releaseQueue.set_value(); });
	auto queueGate = releaseQueue.get_future().share();
	auto queueBlocker = Tasks::CreateTask("Hold independent preview sessions"_h,
		[&]() { queueEntered.set_value(); queueGate.wait(); }, EThreadType::Background);
	queueBlocker->Run();
	queueEntered.get_future().wait();
	request.m_sessionId = 220;
	request.m_revision = 1;
	Require(UpdateMaterialPreview(request, accepted, diagnostic), diagnostic);
	auto thumbnail = request;
	thumbnail.m_sessionId = 221;
	thumbnail.m_primitive = EMaterialPreviewPrimitive::Box;
	Require(UpdateMaterialPreview(thumbnail, accepted, diagnostic), diagnostic);
	for (uint64_t revision = 2; revision <= 8; ++revision)
	{
		request.m_revision = revision;
		Require(UpdateMaterialPreview(request, accepted, diagnostic), diagnostic);
	}
	const auto queued = GetMaterialPreviewStatistics();
	Require(queued.m_sessions == 2 && queued.m_pendingRequests == 2,
		"independent sessions must retain only one pending request each");
	auto queueObserver = Tasks::CreateTask<bool>("Observe preview FIFO and Background yield"_h, []()
	{
		const auto interactive = GetMaterialPreview(220);
		return interactive.m_revision == 8 && interactive.m_state == EMaterialPreviewState::Ready &&
			GetMaterialPreview(221).m_state == EMaterialPreviewState::Pending;
	}, EThreadType::Background);
	queueObserver->Run();
	releaseQueueOnFailure.Run();
	queueObserver->Wait();
	Require(queueObserver->GetResult(), "preview FIFO must preserve fairness and yield to unrelated Background tasks between requests");
	Inspect(WaitForPreview(221, 1), "independent-thumbnail");
	const auto beforeInterleavedOrbit = GetMaterialPreviewStatistics().m_initializationCount;
	++request.m_revision;
	request.m_yaw += 0.4f;
	render("interleaved-orbit");
	Require(GetMaterialPreviewStatistics().m_initializationCount == beforeInterleavedOrbit &&
		GetMaterialPreview(221, 0).m_state == EMaterialPreviewState::Ready,
		"another transient material must neither invalidate the warm interactive job nor close its session");
	CloseMaterialPreview(221);
	Require(GetMaterialPreview(220).m_state == EMaterialPreviewState::Ready,
		"closing a thumbnail must leave the interactive session available");
	for (uint64_t id = 222; id <= 226; ++id)
	{
		thumbnail.m_sessionId = id;
		Require(UpdateMaterialPreview(thumbnail, accepted, diagnostic), diagnostic);
		Require(WaitForPreview(id, 1).m_state == EMaterialPreviewState::Ready, "bounded thumbnail fixture must render");
		Require(GetMaterialPreviewStatistics().m_liveJobs <= 4, "retained private GPU jobs must stay bounded to four");
	}
	Require(GetMaterialPreview(220).m_state == EMaterialPreviewState::Ready,
		"LRU GPU eviction must retain the session's completed image and status");
	const auto beforeEvictedOrbit = GetMaterialPreviewStatistics().m_initializationCount;
	++request.m_revision;
	render("rebuilt-after-lru");
	Require(GetMaterialPreviewStatistics().m_initializationCount == beforeEvictedOrbit + 1,
		"an evicted session must rebuild its private resources when requested again");
	for (uint64_t id = 222; id <= 226; ++id)
	{
		CloseMaterialPreview(id);
	}

	std::promise<void> renderEntered, releaseRender;
	ScopeExit releaseRenderOnFailure([&]() { releaseRender.set_value(); });
	auto renderGate = releaseRender.get_future().share();
	auto renderBlocker = Tasks::CreateTask("Hold Render before preview reuse"_h,
		[&]() { renderEntered.set_value(); renderGate.wait(); }, EThreadType::Render);
	renderBlocker->Run();
	renderEntered.get_future().wait();
	const auto beforeInFlightClose = GetMaterialPreviewRenderCount();
	++request.m_revision;
	Require(UpdateMaterialPreview(request, accepted, diagnostic), diagnostic);
	const auto startDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
	while (!GetMaterialPreviewStatistics().m_inFlight && std::chrono::steady_clock::now() < startDeadline)
	{
		std::this_thread::sleep_for(std::chrono::milliseconds(1));
	}
	Require(GetMaterialPreviewStatistics().m_inFlight, "close test must reach an active native request");
	CloseMaterialPreview(220);
	releaseRenderOnFailure.Run();
	App::GetSubmodule<Tasks::Scheduler>()->WaitIdle({ EThreadType::Background, EThreadType::Render, EThreadType::Worker, EThreadType::RHI });
	Require(GetMaterialPreview(220).m_state == EMaterialPreviewState::Closed &&
		GetMaterialPreviewRenderCount() == beforeInFlightClose && GetMaterialPreviewStatistics().m_liveJobs == 0,
		"closing active reuse must discard publication and release resources after retirement");

	std::promise<void> capacityEntered, releaseCapacity;
	ScopeExit releaseCapacityOnFailure([&]() { releaseCapacity.set_value(); });
	auto capacityGate = releaseCapacity.get_future().share();
	auto capacityBlocker = Tasks::CreateTask("Hold preview session capacity"_h,
		[&]() { capacityEntered.set_value(); capacityGate.wait(); }, EThreadType::Background);
	capacityBlocker->Run();
	capacityEntered.get_future().wait();
	for (uint64_t id = 300; id < 316; ++id)
	{
		thumbnail.m_sessionId = id;
		Require(UpdateMaterialPreview(thumbnail, accepted, diagnostic), diagnostic);
	}
	thumbnail.m_sessionId = 316;
	Require(!UpdateMaterialPreview(thumbnail, accepted, diagnostic) && diagnostic.find("limit") != std::string::npos &&
		GetMaterialPreviewStatistics().m_sessions == 16 && GetMaterialPreviewStatistics().m_pendingRequests == 16,
		"session admission must be bounded and must not evict another caller's active session");
	for (uint64_t id = 300; id < 316; ++id)
	{
		CloseMaterialPreview(id);
	}
	releaseCapacityOnFailure.Run();
	App::GetSubmodule<Tasks::Scheduler>()->WaitIdle({ EThreadType::Background, EThreadType::Render, EThreadType::Worker, EThreadType::RHI });
	Require(GetMaterialPreviewStatistics().m_sessions == 0 && GetMaterialPreviewStatistics().m_liveJobs == 0 &&
		GetMaterialPreviewRenderCount() == beforeInFlightClose, "closed queued sessions must leave no GPU work or cached resources");
	RunModelPreviewGpuTests(workspace);

	std::string after;
	Require(AssetRegistry::ReadAllTextFile(path.string(), after) && after == source &&
		savedMaterial->GetContentRevision() == savedRevision, "unsaved previews must preserve the source file and loaded material");
	Require(renderer->GetFrameGraph() == sceneGraph && App::GetMainWindow()->GetRenderArea() == sceneExtent &&
		renderer->GetMsaaSamples() == sceneSamples && App::GetEditorRenderMode() == renderMode &&
		renderer->GetEditorReadback() == readback && (!editor || editor->GetWorld() == world),
		"private preview must preserve the active scene graph, extent, MSAA, mode, readback and world");
	Require(!editor || !world || CaptureSceneSnapshot() == serializedScene,
		"private preview must not edit the scene serialization");
	std::cout << "Material preview GPU: primitives, unsaved uniforms/textures, roughness, warm orbit, dependency reload, independent sessions, FIFO, bounded LRU, errors, close and isolation passed\n";
}
}
