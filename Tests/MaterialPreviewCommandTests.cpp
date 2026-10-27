#include "Editor/MaterialPreview.h"
#include "AssetRegistry/AssetRegistry.h"
#include "AssetRegistry/Material/MaterialImporter.h"
#include "Editor/EditorRuntimeBridge.h"
#include "RHI/Renderer.h"
#include "Submodules/Editor.h"
#include "Support/ScopeExit.h"
#include "Tasks/Tasks.h"
#include <stb_image.h>

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
		if (!value) throw std::runtime_error(message);
	}

	MaterialPreviewResult WaitForPreview(uint64_t session, uint64_t revision)
	{
		const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(60);
		while (std::chrono::steady_clock::now() < deadline)
		{
			auto result = GetMaterialPreview(session);
			if (result.m_revision == revision && result.m_state != EMaterialPreviewState::Pending) return result;
			std::this_thread::sleep_for(std::chrono::milliseconds(5));
		}
		throw std::runtime_error("Material preview did not complete within 60 seconds");
	}

	glm::u64vec3 Inspect(const MaterialPreviewResult& result, const char* name)
	{
		Require(result.m_state == EMaterialPreviewState::Ready, std::string(name) + ": " + result.m_diagnostic);
		int width = 0, height = 0, channels = 0;
		auto pixels = stbi_load_from_memory(result.m_png.data(), static_cast<int>(result.m_png.size()), &width, &height, &channels, 4);
		Require(pixels && width == 128 && height == 128, "preview must return its own requested extent and a valid PNG");
		glm::u64vec3 sum(0);
		uint32_t changed = 0;
		for (int i = 0; i < width * height; ++i)
		{
			for (int channel = 0; channel < 3; ++channel) sum[channel] += pixels[i * 4 + channel];
			if (std::abs(int(pixels[i * 4]) - pixels[0]) + std::abs(int(pixels[i * 4 + 1]) - pixels[1]) +
				std::abs(int(pixels[i * 4 + 2]) - pixels[2]) > 20) ++changed;
		}
		stbi_image_free(pixels);
		Require(changed > 800, std::string(name) + " must contain a visible shaded primitive, not only a cleared background");
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
			if (difference > 9) ++changed;
		}
		stbi_image_free(a);
		stbi_image_free(b);
		return changed;
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
	const auto serializedScene = editor && world ? YAML::Dump(editor->SerializeWorld()) : std::string{};

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
	const auto savedRevision = savedMaterial->GetContentRevision();
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
	const auto orbit = render("box-orbit-zoom");
	Require(orbit.m_png != blue.m_png, "orbit and zoom must change the private camera image");
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
	for (int i = 0; i < 10; ++i) GetMaterialPreview(216);
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

	std::string after;
	Require(AssetRegistry::ReadAllTextFile(path.string(), after) && after == source &&
		savedMaterial->GetContentRevision() == savedRevision, "unsaved previews must preserve the source file and loaded material");
	Require(renderer->GetFrameGraph() == sceneGraph && App::GetMainWindow()->GetRenderArea() == sceneExtent &&
		renderer->GetMsaaSamples() == sceneSamples && App::GetEditorRenderMode() == renderMode &&
		renderer->GetEditorReadback() == readback && (!editor || editor->GetWorld() == world),
		"private preview must preserve the active scene graph, extent, MSAA, mode, readback and world");
	Require(!editor || !world || YAML::Dump(editor->SerializeWorld()) == serializedScene,
		"private preview must not edit the scene serialization");
	std::cout << "Material preview GPU: primitives, unsaved uniforms/textures, roughness, orbit/zoom, errors, close, coalescing and scene/asset isolation passed\n";
}
}
