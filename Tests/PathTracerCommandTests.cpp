#include "Sailor.h"
#include "AssetRegistry/AssetRegistry.h"
#include "AssetRegistry/Material/MaterialImporter.h"
#include "AssetRegistry/Model/ModelImporter.h"
#include "AssetRegistry/Texture/TextureImporter.h"
#include "Raytracing/PathTracer.h"
#include "Support/TempDirectory.h"

#include <array>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string_view>
#include <vector>

using namespace Sailor;
using namespace Sailor::Raytracing;

namespace
{
	void Require(bool condition, const char* message)
	{
		if (!condition) throw std::runtime_error(message);
	}

	void WriteScene(const Tests::TempDirectory& workspace, const std::string& enginePath)
	{
		std::filesystem::create_directory(workspace.Path("Content"));
		YAML::Node manifest;
		manifest["manifestVersion"] = 1;
		manifest["workspaceId"] = "00000000-0000-0000-0000-000000000121";
		manifest["name"] = "Path tracer command test";
		manifest["enginePath"] = enginePath;
		manifest["engineReferenceKind"] = "source";
		manifest["contentPath"] = "Content";
		manifest["sourcePath"] = "Source";
		manifest["generatedProjectPath"] = "Generated";
		manifest["cachePath"] = "Cache";
		manifest["buildPath"] = "Cache/Build";
		manifest["logicOutputPath"] = "Binaries";
		manifest["logicModuleName"] = "PathTracerCommandTest";
		std::ofstream(workspace.Path("workspace.sailor")) << manifest;

		const std::array<float, 24> vertices{
			-1, -1, 0, 1, -1, 0, 1, 1, 0, -1, 1, 0,
			0, 0, 1, 0, 0, 1, 0, 0, 1, 0, 0, 1 };
		const std::array<uint32_t, 6> indices{ 0, 1, 2, 0, 2, 3 };
		std::ofstream geometry(workspace.Path("Content/Quad.bin"), std::ios::binary);
		geometry.write(reinterpret_cast<const char*>(vertices.data()), sizeof(vertices));
		geometry.write(reinterpret_cast<const char*>(indices.data()), sizeof(indices));
		const std::string scene = R"({
	"asset": {"version": "2.0"},
	"buffers": [{"uri": "Quad.bin", "byteLength": 120}],
	"bufferViews": [
		{"buffer": 0, "byteOffset": 0, "byteLength": 48},
		{"buffer": 0, "byteOffset": 48, "byteLength": 48},
		{"buffer": 0, "byteOffset": 96, "byteLength": 24}],
	"accessors": [
		{"bufferView": 0, "componentType": 5126, "count": 4, "type": "VEC3", "min": [-1,-1,0], "max": [1,1,0]},
		{"bufferView": 1, "componentType": 5126, "count": 4, "type": "VEC3"},
		{"bufferView": 2, "componentType": 5125, "count": 6, "type": "SCALAR"}],
	"materials": [{"name": "Copper", "doubleSided": true,
		"pbrMetallicRoughness": {"baseColorFactor": [0.6,0.08,0.02,1], "metallicFactor": 0, "roughnessFactor": 1},
		"emissiveFactor": [0.12,0.01,0]}],
	"meshes": [{"primitives": [{"attributes": {"POSITION": 0, "NORMAL": 1}, "indices": 2, "material": 0}]}],
	"cameras": [
		{"name": "Wide", "type": "perspective", "perspective": {"aspectRatio": 2, "yfov": 0.8, "znear": 0.1}},
		{"name": "Front", "type": "perspective", "perspective": {"aspectRatio": 1.5, "yfov": 0.8, "znear": 0.1}},
		{"name": "Miss", "type": "perspective", "perspective": {"aspectRatio": 1, "yfov": 0.8, "znear": 0.1}}],
	"extensionsUsed": ["KHR_lights_punctual"],
	"extensions": {"KHR_lights_punctual": {"lights": [{"type": "directional", "color": [1,1,1], "intensity": 2}]}},
	"nodes": [
		{"mesh": 0},
		{"camera": 0, "translation": [0,0,3]},
		{"name": "FrontNode", "camera": 1, "translation": [0,0,3], "children": [4]},
		{"camera": 2, "translation": [10,0,3]},
		{"extensions": {"KHR_lights_punctual": {"light": 0}}}],
)";
		std::ofstream(workspace.Path("Content/Quad.gltf")) << scene << R"("scenes": [{"nodes": [0,1,2,3]}], "scene": 0})";
		std::ofstream(workspace.Path("Content/NoView.gltf")) << scene << R"("scenes": [{"nodes": [0]}], "scene": 0})";
		YAML::Node model;
		model["assetInfoType"] = "Sailor::ModelAssetInfo";
		model["fileId"] = "{00000000-0000-0000-0000-000000000121}";
		model["filename"] = "Quad.gltf";
		model["bShouldGenerateMaterials"] = true;
		model["bShouldKeepCpuBuffers"] = true;
		model["bGenerateBLAS"] = true;
		model["unitScale"] = 1;
		std::ofstream(workspace.Path("Content/Quad.gltf.asset")) << model;
		model["fileId"] = "{00000000-0000-0000-0000-000000000122}";
		model["filename"] = "NoView.gltf";
		std::ofstream(workspace.Path("Content/NoView.gltf.asset")) << model;
	}

	void CheckCoverage(const TVector<u8vec4>& pixels)
	{
		size_t covered = 0u, colored = 0u, transparent = 0u;
		for (const auto& pixel : pixels)
		{
			covered += pixel.a == 255;
			colored += pixel.a == 255 && pixel.r > 50 && pixel.r > pixel.g && pixel.g > pixel.b;
			transparent += pixel == u8vec4(0);
		}
		Require(covered > 10 && colored > 10, "CLI PNG must contain the visible copper material, not an empty image");
		Require(transparent > 10, "CLI background must remain transparent black");
	}

	void RequireSameImage(const TVector<u8vec4>& first, const TVector<u8vec4>& second)
	{
		Require(first.Num() == second.Num(), "CLI and prepared image dimensions must match");
		for (size_t i = 0u; i < first.Num(); ++i)
			Require(first[i] == second[i], "CLI and prepared paths must produce the same RGBA pixels");
	}

	void TestPreparedParity(const Tests::TempDirectory& workspace, const TVector<u8vec4>& png)
	{
		auto* registry = App::GetSubmodule<AssetRegistry>();
		auto* importer = App::GetSubmodule<ModelImporter>();
		const auto info = registry->GetAssetInfoPtr<ModelAssetInfoPtr>("Quad.gltf");
		ModelPtr model;
		Require(info && importer->LoadModel_Immediate(info->GetFileId(), model), "the CLI's imported model must remain available");
		TVector<MaterialPtr> materials;
		auto loadMaterials = importer->LoadDefaultMaterials(info->GetFileId(), materials);
		loadMaterials->Wait();
		Require(loadMaterials->GetResult() && materials.Num() == 1, "the glTF material must be loaded");

		PathTracer::TLASInstance instance;
		instance.m_model = model;
		const auto bounds = model->GetBoundsSphere();
		instance.m_worldBounds = Math::AABB(bounds.m_center - vec3(bounds.m_radius), bounds.m_center + vec3(bounds.m_radius));
		LightProxy sun;
		sun.m_type = ELightType::Directional;
		sun.m_direction = vec3(0, 0, -1);
		sun.m_intensity = vec3(2);
		PathTracer prepared;
		Require(prepared.InitializeScene({ instance }, materials, { sun }), "prepared geometry and materials must initialize");
		PathTracer::Params params{};
		params.m_pathToModel = "Quad.gltf";
		params.m_height = 16;
		params.m_numSamples = params.m_numAmbientSamples = params.m_maxBounces = params.m_msaa = 1;
		params.m_ambient = vec3(0);
		params.m_rayBiasScale = 3e-4f;
		params.m_bUseRuntimeCamera = true;
		params.m_runtimeCameraPos = vec3(0, 0, 3);
		params.m_runtimeAspectRatio = 1.5f;
		params.m_runtimeHFov = 2.0f * std::atan(std::tan(0.4f) * params.m_runtimeAspectRatio);
		params.m_bRunTasksInline = true;
		Require(prepared.RenderPreparedScene(params), "the prepared scene must render");
		RequireSameImage(png, prepared.GetLastRenderedImage());

		PathTracer cli;
		params.m_bUseRuntimeCamera = false;
		for (const char* camera : { "Front", "FrontNode" })
		{
			params.m_camera = camera;
			cli.Run(params);
			Require(cli.GetLastRenderedExtent() == uvec2(24, 16), "camera and node names must select the authored view");
			RequireSameImage(png, cli.GetLastRenderedImage());
			Require(cli.GetLastScenePreparationStats().m_triangleCount == 2u, "CLI must use the shared scene preparation");
		}
		params.m_camera = "Miss";
		cli.Run(params);
		Require(cli.GetLastRenderedExtent() == uvec2(16), "the off-model camera must retain its aspect ratio");
		for (const auto& pixel : cli.GetLastRenderedImage())
			Require(pixel == u8vec4(0), "an authored camera that misses the model must produce transparent black");

		params.m_camera.clear();
		cli.Run(params);
		Require(cli.GetLastRenderedExtent() == uvec2(32, 16), "without a name CLI must use the first glTF camera");
		CheckCoverage(cli.GetLastRenderedImage());
		params.m_camera = "UnknownCamera";
		cli.Run(params);
		Require(cli.GetLastRenderedExtent() == uvec2(21, 16), "an unknown camera must use default model framing");
		CheckCoverage(cli.GetLastRenderedImage());

		params.m_camera = "Front";
		params.m_height = 48;
		cli.Run(params);
		const auto inlinePixels = cli.GetLastRenderedImage();
		params.m_bRunTasksInline = false;
		params.m_output = workspace.Path("workers.png");
		cli.Run(params);
		RequireSameImage(inlinePixels, cli.GetLastRenderedImage());
		CheckCoverage(cli.GetLastRenderedImage());

		params.m_height = 16;
		params.m_output.clear();
		params.m_pathToModel = "Missing.gltf";
		cli.Run(params);
		Require(cli.GetLastRenderedExtent() == uvec2(0) && cli.GetLastRenderedImage().IsEmpty(),
			"a failed CLI load must not publish the preceding image");
		params.m_pathToModel = "Quad.gltf";
		cli.Run(params);
		RequireSameImage(png, cli.GetLastRenderedImage());

		params.m_pathToModel = "NoView.gltf";
		params.m_camera.clear();
		cli.Run(params);
		CheckCoverage(cli.GetLastRenderedImage());
		Require(prepared.InitializeScene({ instance }, materials, {}), "the prepared scene must accept the default sun");
		params.m_bUseRuntimeCamera = true;
		params.m_runtimeCameraPos = bounds.m_center + vec3(0, bounds.m_radius * 0.6f, bounds.m_radius * 2.5f);
		params.m_runtimeCameraForward = normalize(bounds.m_center - params.m_runtimeCameraPos);
		params.m_runtimeAspectRatio = 4.0f / 3.0f;
		params.m_runtimeHFov = glm::radians(60.0f);
		Require(prepared.RenderPreparedScene(params), "default framing and lighting must render");
		RequireSameImage(cli.GetLastRenderedImage(), prepared.GetLastRenderedImage());
	}
}

namespace Sailor::Tests
{
	int RunPathTracerCommandTests(int argc, const char** argv)
	{
		Tests::TempDirectory workspace("pathtracer-command");
		int result = 1;
		try
		{
			std::string enginePath = std::filesystem::current_path().string();
			for (int i = 1; i + 1 < argc; ++i)
				if (std::string_view(argv[i]) == "--workspace") enginePath = argv[i + 1];
			WriteScene(workspace, enginePath);
			const std::string root = workspace.Get().string();
			const std::string output = workspace.Path("command.png").string();
			std::vector<const char*> arguments(argv, argv + argc);
			arguments.insert(arguments.end(), { "--workspace", root.c_str(), "--editor", "--port", "0", "--pathtracer",
				"--in", "Quad.gltf", "--out", output.c_str(), "--camera", "Front", "--height", "16",
				"--samples", "1", "--ambientSamples", "1", "--bounces", "1", "--ambient", "000000" });
			Require(App::Initialize(arguments.data(), static_cast<int32_t>(arguments.size())) == EAppInitializationResult::Completed,
				"the offline command must complete");
			TextureImporter::CpuDecodeRequest request;
			request.m_filepath = output;
			FileRevision outputRevision;
			Require(Utils::TryGetFileRevision(output, outputRevision), "CLI must write its output PNG");
			request.m_sourceRevisions.Add(output, outputRevision);
			TextureImporter::ByteCode bytes;
			int32_t width = 0, height = 0;
			uint32_t mips = 0;
			Require(TextureImporter::DecodeTextureCpu(request, bytes, width, height, mips) && width == 24 && height == 16 &&
				bytes.Num() == 24u * 16u * 4u, "CLI must write a decodable RGBA PNG with the selected camera's aspect ratio");
			TVector<u8vec4> pixels;
			for (size_t i = 0; i < bytes.Num(); i += 4)
				pixels.Add(u8vec4(bytes[i], bytes[i + 1], bytes[i + 2], bytes[i + 3]));
			CheckCoverage(pixels);
			TestPreparedParity(workspace, pixels);
			std::cout << "PathTracer CLI/prepared pixel parity test passed\n";
			result = 0;
		}
		catch (const std::exception& error) { std::cerr << error.what() << '\n'; }
		App::Stop();
		if (!App::Shutdown()) result = 1;
		return result;
	}
}
