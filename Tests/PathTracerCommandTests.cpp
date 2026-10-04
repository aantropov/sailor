#include "Sailor.h"
#include "Core/FileRevision.h"
#include "AssetRegistry/AssetRegistry.h"
#include "AssetRegistry/Material/MaterialImporter.h"
#include "AssetRegistry/Model/ModelImporter.h"
#include "AssetRegistry/Texture/TextureImporter.h"
#include "AssetRegistry/Shader/ShaderCompiler.h"
#include "ECS/CameraECS.h"
#include "ECS/LightingECS.h"
#include "ECS/TransformECS.h"
#include "Components/MeshRendererComponent.h"
#include "Components/PathTracerProxyComponent.h"
#include "Components/CameraComponent.h"
#include "Components/LightComponent.h"
#include "Engine/Frame.h"
#include "Engine/GameObject.h"
#include "Engine/World.h"
#include "FrameGraph/CPUPathTracerNode.h"
#include "RHI/Buffer.h"
#include "RHI/CommandList.h"
#include "RHI/Cubemap.h"
#include "RHI/Material.h"
#include "RHI/Mesh.h"
#include "RHI/RenderTarget.h"
#include "RHI/RenderSubmission.h"
#include "RHI/SceneView.h"
#include "RHI/Shader.h"
#include "RHI/VertexDescription.h"
#include "Raytracing/PathTracer.h"
#include "Support/TempDirectory.h"
#include <glm/gtc/packing.hpp>

#include <array>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string_view>
#include <utility>
#include <vector>

using namespace Sailor;
using namespace Sailor::Raytracing;

namespace Sailor::Tests
{
	void RunGIProbesCommandTests(const std::filesystem::path& workspace);
	int RunGIShutdownCommandTests(int argc, const char** argv, const std::filesystem::path& workspace);
	void RunTextureImporterCommandTests(const std::filesystem::path& workspace);
	void RunMaterialImporterCommandTests(const std::filesystem::path& workspace);
	void RunAudioCommandTests(const std::filesystem::path& workspace);
	void RunLoggingCommandTests();
	void RunLoggingShutdownTests();
	void RunModelLodCommandTests(const std::filesystem::path& workspace);
	void RunPrefabImporterCommandTests(const std::filesystem::path& workspace);
	void RunFrameGraphNodeCommandTests(const std::filesystem::path& workspace);
	void RunCloudNoiseCommandTests(const std::filesystem::path& workspace);
}

namespace
{
	void Require(bool condition, const char* message)
	{
		if (!condition) throw std::runtime_error(message);
	}

	void WriteScene(const Tests::TempDirectory& workspace, const std::string& enginePath, uint32_t msaaSamples)
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
		YAML::Node settings = YAML::LoadFile((std::filesystem::path(enginePath) / "ProjectSettings.yaml").string());
		settings["settingsVersion"] = 1;
		settings["graphics"]["defaultQuality"] = "High";
		for (const char* preset : { "Ultra", "High", "Medium", "Low", "VeryLow" })
		{
			auto profile = settings["graphics"]["presets"][preset];
			profile["msaaSamples"] = msaaSamples;
			profile["enableGlobalIllumination"] = true;
			profile["maxGiProbeStatesPerSnapshot"] = 2;
			auto gi = profile["runtimeGIProbes"];
			gi["version"] = 1;
			gi["maxActiveProbes"] = 8;
			gi["initialSamplesPerProbe"] = 16;
			gi["targetSamplesPerProbe"] = 16;
			gi["workerCount"] = 1;
			gi["cpuDutyFraction"] = 1;
			gi["cpuBudgetMilliseconds"] = 4;
			gi["maxPublicationsPerSecond"] = 60;
		}
		std::ofstream(workspace.Path("ProjectSettings.yaml")) << settings;
		const std::array<const char*, 2> probeNames{ "Retry.probes", "RetryAfterGc.probes" };
		for (uint32_t i = 0; i < probeNames.size(); ++i)
		{
			const auto path = std::filesystem::path("Content") / probeNames[i];
			std::ofstream(workspace.Path(path)) << "incomplete probe payload";
			YAML::Node probes;
			probes["assetInfoType"] = "Sailor::GIProbesAssetInfo";
			probes["fileId"] = "{00000000-0000-0000-0000-000000000" + std::to_string(125 + i) + "}";
			probes["filename"] = probeNames[i];
			std::ofstream(workspace.Path(path.string() + ".asset")) << probes;
		}

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

	class ImageNode final : public Framegraph::CPUPathTracerNode
	{
	public:
		using CPUPathTracerNode::m_pShader;
		using CPUPathTracerNode::ApplyCompletedReadback;
		CameraState& Camera(uint32_t index = 0) { return GetCameraState(index); }
		size_t NumCameras() const { return m_cameras.Num(); }
		TRefPtr<SubmissionResources> Resources(const RHI::RHISceneViewSnapshot& scene)
		{
			return scene.m_submissionContext->GetOrAddFrameGraphResources<SubmissionResources>(this, scene.m_cameraIndex, 0);
		}
	};

	class ImageGraph final : public RHI::RHIFrameGraph
	{
	public:
		ImageGraph()
		{
			using namespace RHI;
			auto& driver = Renderer::GetDriver();
			VertexP3N3UV2C4 vertices[4]{};
			for (uint32_t i = 0u; i < 4u; ++i)
			{
				vertices[i].m_texcoord = vec2(i % 2u, i / 2u);
				vertices[i].m_position = vec3(vertices[i].m_texcoord * 2.0f - 1.0f, 0);
			}
			const uint32_t indices[] = { 0, 1, 2, 2, 1, 3 };
			const auto memory = EMemoryPropertyBit::HostVisible | EMemoryPropertyBit::HostCoherent;
			m_postEffectPlane = RHIMeshPtr::Make();
			m_postEffectPlane->m_vertexDescription = driver->GetOrAddVertexDescription<VertexP3N3UV2C4>();
			m_postEffectPlane->m_vertexBuffer = driver->CreateBuffer(sizeof(vertices), EBufferUsageBit::VertexBuffer_Bit, memory);
			m_postEffectPlane->m_indexBuffer = driver->CreateBuffer(sizeof(indices), EBufferUsageBit::IndexBuffer_Bit, memory);
			std::memcpy(m_postEffectPlane->m_vertexBuffer->GetPointer(), vertices, sizeof(vertices));
			std::memcpy(m_postEffectPlane->m_indexBuffer->GetPointer(), indices, sizeof(indices));
		}
	};

	struct RecordedComposite
	{
		RHI::RHICommandListPtr command;
		RHI::RHIBufferPtr readback;
		RHI::RHIRenderSubmissionContextPtr context;
		ivec2 extent{ 32 };
	};

	class TracerWorld final : public World
	{
	public:
		TracerWorld() : World("Tracer accumulation", 0, CreateEcs()) {}
		~TracerWorld() override { Clear(); }
		void Publish(RHI::RHISceneViewPtr view)
		{
			++m_currentFrame;
			GetECS<TransformECS>()->Tick(0);
			GetECS<LightingECS>()->Tick(0);
			GetECS<PathTracerECS>()->Tick(0);
			GetECS<PathTracerECS>()->CopySceneView(view);
			view->PrepareSnapshots();
		}

	private:
		static TVector<ECS::TBaseSystemPtr> CreateEcs()
		{
			TVector<ECS::TBaseSystemPtr> systems;
			systems.Add(TUniquePtr<TransformECS>::Make());
			systems.Add(TUniquePtr<StaticMeshRendererECS>::Make());
			systems.Add(TUniquePtr<LightingECS>::Make());
			systems.Add(TUniquePtr<PathTracerECS>::Make());
			return systems;
		}
	};

	RHI::RHISceneViewPtr CreateTracerView(World& world, uint32_t cameras = 1)
	{
		using namespace RHI;
		auto view = RHISceneViewPtr::Make();
		view->m_world = &world;
		view->m_submissionContext = RHIRenderSubmissionContextPtr::Make();
		view->m_cameras.Resize(cameras);
		view->m_cameraTransforms.Resize(cameras);
		view->m_shadowMapsToUpdate.Resize(cameras);
		view->m_shadowMapsToBlit.Resize(cameras);
		view->m_shadowIndices.Resize(cameras);
		view->m_shadowAtlasTiles.Resize(cameras);
		view->m_shadowMatrices.Resize(cameras);
		for (uint32_t i = 0; i < cameras; ++i)
		{
			view->m_cameras[i].SetAspect(1);
			view->m_cameras[i].SetFov(glm::degrees(0.8f));
			view->m_cameraTransforms[i].m_position = vec4(0, 0, 3, 1);
		}
		return view;
	}

	void TestTracerSceneDemand(ModelPtr model)
	{
		TracerWorld world;
		auto object = world.Instantiate("Tracer scene publication");
		auto mesh = object->AddComponent<MeshRendererComponent>();
		mesh->GetData().SetModel(model);
		auto proxy = object->AddComponent<PathTracerProxyComponent>();
		proxy->SetEnabled(true);
		auto material = MaterialPtr::Make(world.GetAllocator(), FileId::Invalid);
		material->SetUniform("material.baseColorFactor", vec4(0, 0, 0, 1));
		material->SetUniform("material.emissiveFactor", vec4(2, 0.5f, 0.25f, 0));
		mesh->GetMaterials() = { material };
		auto light = world.Instantiate("Tracer sun")->AddComponent<LightComponent>();
		light->SetLightType(ELightType::Directional);
		light->SetIntensity(vec3(1));
		auto view = CreateTracerView(world, 2);
		auto* ecs = world.GetECS<PathTracerECS>();
		world.Publish(view);
		Require(!view->m_snapshots[0].m_pathTracerScene && !view->m_snapshots[1].m_pathTracerScene,
			"a populated raster-only world must not publish tracer payload");
		ecs->SetPathTracingEnabled(true);
		world.Publish(view);
		Require(view->m_snapshots[0].m_pathTracerScene && view->m_snapshots[0].m_pathTracerScene->m_instances.Num() == 1,
			"the demand fixture must contain a ready, populated tracer scene before disabling it");
		const auto original = view->m_pathTracerScene;
		Require(original->m_materials.Num() == 1 && original->m_lights.Num() == 1,
			"the publication must include actual material and lighting data");
		for (uint32_t frame = 0; frame < 64; ++frame)
		{
			view->m_cameraTransforms[1].m_position.x += 0.01f;
			world.Publish(view);
			for (const auto& snapshot : view->m_snapshots)
			{
				Require(snapshot.m_pathTracerScene == original &&
					snapshot.m_pathTracerScene->m_instances.GetData() == original->m_instances.GetData() &&
					snapshot.m_pathTracerScene->m_materials.GetData() == original->m_materials.GetData() &&
					snapshot.m_pathTracerScene->m_lights.GetData() == original->m_lights.GetData(),
					"unchanged scenes and camera-only motion must reuse all published arrays across frames and cameras");
			}
		}
		ecs->SetPathTracingEnabled(false);
		world.Publish(view);
		Require(!view->m_snapshots[0].m_pathTracerScene && !view->m_snapshots[1].m_pathTracerScene,
			"disabled tracing must not prepare or copy tracer payload into raster camera snapshots");
		ecs->SetPathTracingEnabled(true);
		world.Publish(view);
		Require(view->m_pathTracerScene == original, "re-enabling unchanged input must reuse its publication");

		auto publishChange = [&]()
		{
			const auto previous = view->m_pathTracerScene;
			world.Publish(view);
			Require(view->m_pathTracerScene && view->m_pathTracerScene != previous &&
				view->m_snapshots[0].m_pathTracerScene == view->m_pathTracerScene &&
				view->m_snapshots[1].m_pathTracerScene == view->m_pathTracerScene,
				"a scene edit must publish one new generation to both cameras in the same frame");
			return view->m_pathTracerScene;
		};
		material->SetUniform("material.emissiveFactor", vec4(0.25f, 2, 0.5f, 0));
		const auto edited = publishChange();
		Require(original->m_materials[0]->m_parameters.m_emissiveFactor == vec3(2, 0.5f, 0.25f) &&
			edited->m_materials[0]->m_parameters.m_emissiveFactor == vec3(0.25f, 2, 0.5f),
			"material edits must not mutate the retained frame's material parameters");
		auto replacement = MaterialPtr::Make(world.GetAllocator(), FileId::Invalid);
		replacement->SetUniform("material.emissiveFactor", vec4(3, 1, 0.5f, 0));
		mesh->GetMaterials()[0] = replacement;
		Require(publishChange()->m_materials[0]->m_parameters.m_emissiveFactor == vec3(3, 1, 0.5f),
			"replacing a material must reach both cameras");
		light->SetIntensity(vec3(3));
		Require(publishChange()->m_lights[0].m_intensity == vec3(3) && original->m_lights[0].m_intensity == vec3(1),
			"light edits must publish new values without modifying retained lights");

		ecs->SetPathTracingEnabled(false);
		object->GetTransformComponent().SetPosition(vec3(5, 0, 0));
		world.Publish(view);
		Require(!view->m_pathTracerScene, "changes while disabled must not publish payload");
		ecs->SetPathTracingEnabled(true);
		Require(publishChange()->m_instances[0].m_worldMatrix[3].x == 5 && original->m_instances[0].m_worldMatrix[3].x == 0,
			"the first enabled frame must include changes made while tracing was disabled");
		object->GetTransformComponent().SetPosition(vec3(0));
		publishChange();
		mesh->SetMeshIndex(0);
		Require(publishChange()->m_instances[0].m_meshIndex == 0, "mesh selection must invalidate the shared scene");
		mesh->SetMeshIndex(Model::AllMeshes);
		publishChange();
		auto* importer = App::GetSubmodule<ModelImporter>();
		const auto otherInfo = App::GetSubmodule<AssetRegistry>()->GetAssetInfoPtr<ModelAssetInfoPtr>("NoView.gltf");
		ModelPtr otherModel;
		Require(otherInfo && importer->LoadModel_Immediate(otherInfo->GetFileId(), otherModel) && otherModel->IsReady(),
			"the alternate fixture model must have completed its earlier CLI load");
		mesh->GetData().SetModel(otherModel);
		Require(publishChange()->m_instances[0].m_model == otherModel && original->m_instances[0].m_model == model,
			"model replacement must not change an old frame's model selection");
		mesh->GetData().SetModel(model);
		publishChange();

		const auto vertices = model->GetCpuMeshes()[0].m_vertices;
		for (auto& vertex : model->GetCpuMeshes()[0].m_vertices) vertex.m_position.x *= 0.25f;
		Require(model->BuildBLAS(), "the fixture's narrowed geometry must build");
		const auto narrowed = publishChange();
		Require(narrowed->m_instances[0].m_modelGeometry != original->m_instances[0].m_modelGeometry,
			"a new BLAS generation must replace the scene publication");
		model->GetCpuMeshes()[0].m_vertices = vertices;
		Require(model->BuildBLAS(), "the fixture geometry must be restored");
		publishChange();

		PathTracer tracer;
		PathTracer::PreparedRaySample sample;
		for (const auto& scene : { original, narrowed })
		{
			Require(tracer.InitializeSceneSnapshot(scene->m_instances, scene->m_materials, scene->m_lights),
				"retained publications must remain independently traceable");
			Require(tracer.SamplePreparedSceneVisibility(vec3(0.75f, 0, 3), vec3(0, 0, -1), 10, sample) &&
				sample.m_bHit == (scene == original), "retained geometry must not follow later changes to the live model");
		}
		PathTracer::Params params{};
		params.m_maxBounces = 1;
		params.m_bIncludeDirectLighting = false;
		params.m_bIncludeEnvironment = false;
		for (const auto& scene : { original, edited })
		{
			Require(tracer.InitializeSceneSnapshot(scene->m_instances, scene->m_materials, scene->m_lights) &&
				tracer.SamplePreparedSceneRay(vec3(0, 0, 3), vec3(0, 0, -1), 10, params, 1, sample) && sample.m_bHit &&
				length(sample.m_radiance - scene->m_materials[0]->m_parameters.m_emissiveFactor) < 1e-5f,
				"old and new material publications must produce their own radiance after further edits");
		}
		proxy->SetEnabled(false);
		Require(publishChange()->m_instances.IsEmpty(), "removing the last proxy must publish an empty scene");
		proxy->SetEnabled(true);
		Require(publishChange()->m_instances.Num() == 1, "re-enabling a proxy must republish its instance");
		proxy->SetRebuildEveryFrame(true);
		publishChange();
		publishChange();
		proxy->SetRebuildEveryFrame(false);
		const auto stable = publishChange();
		world.Publish(view);
		Require(view->m_pathTracerScene == stable, "disabling forced rebuild must restore publication reuse");
		std::cout << "Tracer scene publication: 64 warm frames, two cameras, off/on, materials, lights, models and retained rays passed\n";
	}

	void SetTracerMaterial(RHI::RHISceneViewSnapshot& snapshot, MaterialPtr material)
	{
		auto scene = TSharedPtr<RHI::RHIPathTracerScene>::Make(*snapshot.m_pathTracerScene);
		scene->m_materials = PathTracer::CaptureMaterials({ material });
		++scene->m_revision;
		snapshot.m_pathTracerScene = std::move(scene);
	}

	class ScenePublicationNode final : public Framegraph::RHINodeDefault
	{
	public:
		std::array<RHI::RHIPathTracerScenePtr, 2> m_scenes;
		std::array<uint32_t, 2> m_visits{};

		void Process(RHI::RHIFrameGraphPtr, RHI::RHICommandListPtr, RHI::RHICommandListPtr,
			const RHI::RHISceneViewSnapshot& scene) override
		{
			m_scenes.at(scene.m_cameraIndex) = scene.m_pathTracerScene;
			++m_visits.at(scene.m_cameraIndex);
		}
	};

	void TestRendererTracerDemand(ModelPtr model)
	{
		using namespace RHI;
		auto* renderer = App::GetSubmodule<Renderer>();
		auto* scheduler = App::GetSubmodule<Tasks::Scheduler>();
		Require(renderer->EnsureFrameGraph(), "the real renderer must have a graph for demand testing");
		auto graph = renderer->GetFrameGraph()->GetRHI();
		auto originalNodes = std::move(graph->GetGraph());
		const auto originalMode = App::GetEditorRenderMode();
		auto observer = TRefPtr<ScenePublicationNode>::Make();
		auto tracer = TRefPtr<Framegraph::CPUPathTracerNode>::Make();
		tracer->SetTag("AuthoredTracer");
		graph->GetGraph() = { observer };
		auto world = TSharedPtr<World>::Make("Tracer demand", static_cast<uint8_t>(EWorldBehaviourBit::EcsTickable));
		world->Instantiate("First camera")->AddComponent<CameraComponent>();
		world->Instantiate("Second camera")->AddComponent<CameraComponent>();
		auto object = world->Instantiate("Traced quad");
		object->AddComponent<MeshRendererComponent>()->GetData().SetModel(model);
		object->AddComponent<PathTracerProxyComponent>()->SetEnabled(true);
		uint64_t tick = 0;
		auto push = [&](bool expected)
		{
			observer->m_scenes = {};
			observer->m_visits = {};
			Sailor::FrameState frame(world.GetRawPtr(), static_cast<int64_t>(++tick * 16), {}, { 32, 24 });
			world->Tick(frame);
			Require(renderer->PushFrame(frame), "the real renderer must accept the demand test frame");
			scheduler->WaitIdle({ EThreadType::Worker, EThreadType::RHI, EThreadType::Render });
			scheduler->ProcessTasksOnMainThread();
			for (size_t i = 0; i < 2; ++i)
			{
				Require(observer->m_visits[i] == 1 && static_cast<bool>(observer->m_scenes[i]) == expected,
					"renderer demand must reach both camera snapshots in the current frame");
			}
			Require(observer->m_scenes[0] == observer->m_scenes[1], "actual camera submissions must share tracer data");
			return observer->m_scenes[0];
		};
		App::SetEditorRenderMode(ESceneViewRenderMode::Lit);
		push(false);
		graph->GetGraph() = { tracer, observer };
		push(false);
		tracer->SetFloat("enabled", 1);
		const auto original = push(true);
		Require(original->m_instances.Num() == 1 && push(true) == original,
			"the renderer must prepare the first enabled frame and reuse unchanged data");
		App::SetEditorRenderMode(ESceneViewRenderMode::Cascades);
		push(false);
		App::SetEditorRenderMode(ESceneViewRenderMode::Lit);
		Require(push(true) == original, "returning from debug visualization must reuse an unchanged scene");
		tracer->SetFloat("enabled", 0);
		object->GetTransformComponent().SetPosition(vec3(7, 0, 0));
		push(false);
		tracer->SetFloat("enabled", 1);
		const auto edited = push(true);
		Require(edited != original && edited->m_instances[0].m_worldMatrix[3].x == 7 &&
			original->m_instances[0].m_worldMatrix[3].x == 0,
			"reenabling the actual graph must capture deferred changes without altering in-flight data");
		auto disabled = TRefPtr<Framegraph::CPUPathTracerNode>::Make();
		graph->GetGraph() = { disabled, tracer, observer };
		Require(push(true) == edited, "an inactive first tracer must not hide another enabled consumer");
		graph->GetGraph() = { disabled, observer };
		push(false);
		graph->GetGraph() = std::move(originalNodes);
		App::SetEditorRenderMode(originalMode);
		world->Clear();
		std::cout << "Tracer renderer demand: absent, disabled, enabled, debug, deferred changes and multiple nodes passed\n";
	}

	RecordedComposite RecordComposite(ImageNode& node, RHI::RHIFrameGraphPtr graph, RHI::RHISceneViewSnapshot& scene,
		ivec2 extent = ivec2(32))
	{
		using namespace RHI;
		auto& driver = Renderer::GetDriver();
		auto commands = Renderer::GetDriverCommands();
		RecordedComposite result;
		result.extent = extent;
		result.context = scene.m_submissionContext;
		result.command = driver->CreateCommandList(false, ECommandListQueue::Graphics);
		commands->BeginCommandList(result.command, true);
		auto target = driver->CreateRenderTarget(result.command, extent, 1, ETextureFormat::R16G16B16A16_SFLOAT);
		commands->ImageMemoryBarrier(result.command, target, EImageLayout::ColorAttachmentOptimal);
		commands->BeginRenderPass(result.command, TVector<RHITexturePtr>{ target }, nullptr, ivec4(0, 0, extent.x, extent.y),
			ivec2(0), true, vec4(0), 0, false);
		commands->EndRenderPass(result.command);
		node.SetRHIResource("color", target);
		node.Process(graph, result.command, result.command, scene);
		result.readback = driver->CreateBuffer(extent.x * extent.y * 8, EBufferUsageBit::BufferTransferDst_Bit,
			EMemoryPropertyBit::HostVisible | EMemoryPropertyBit::HostCoherent);
		commands->ImageMemoryBarrier(result.command, target, EImageLayout::TransferSrcOptimal);
		commands->CopyImageToBuffer(result.command, target, result.readback);
		commands->MemoryBarrier(result.command, static_cast<EAccessFlags>(EAccessBit::TransferWrite_Bit),
			static_cast<EAccessFlags>(EAccessBit::HostRead_Bit));
		commands->EndCommandList(result.command);
		return result;
	}

	vec3 ReadComposite(const RecordedComposite& frame)
	{
		const auto* pixels = static_cast<const uint32_t*>(frame.readback->GetPointer());
		const size_t center = (frame.extent.y / 2) * frame.extent.x + frame.extent.x / 2;
		return vec3(vec4(glm::unpackHalf2x16(pixels[2 * center]), glm::unpackHalf2x16(pixels[2 * center + 1])));
	}

	void RequireComposite(const RecordedComposite& frame, const vec3& expected,
		const char* message = "each recorded flight must retain its own image bytes")
	{
		const vec3 color = ReadComposite(frame);
		if (!(length(color - expected) < 0.001f))
			std::cerr << "Composite RGB " << color.r << ", " << color.g << ", " << color.b
				<< "; expected " << expected.r << ", " << expected.g << ", " << expected.b << '\n';
		Require(length(color - expected) < 0.001f, message);
	}

	void TestOverlappingTracerFlights(ImageNode& node, RHI::RHIFrameGraphPtr graph, RHI::RHISceneViewSnapshot& scene,
		MaterialPtr firstMaterial, MaterialPtr secondMaterial)
	{
		using namespace RHI;
		auto& driver = Renderer::GetDriver();
		auto firstContext = RHIRenderSubmissionContextPtr::Make();
		auto secondContext = RHIRenderSubmissionContextPtr::Make();
		firstContext->BeginSubmission(3, 0);
		secondContext->BeginSubmission(4, 1);
		scene.m_cameraIndex = 0;
		scene.m_cameraTransform.m_position.x = 0;
		SetTracerMaterial(scene, firstMaterial);
		scene.m_submissionContext = firstContext;
		auto first = RecordComposite(node, graph, scene);
		auto firstUpload = node.Resources(scene)->m_uploadBuffer;
		scene.m_cameraTransform.m_position.x = 0.125f;
		SetTracerMaterial(scene, secondMaterial);
		scene.m_submissionContext = secondContext;
		auto second = RecordComposite(node, graph, scene);
		Require(node.Resources(scene)->m_uploadBuffer != firstUpload, "unretired flights must not share mutable upload storage");
		auto firstFence = RHIFencePtr::Make();
		auto secondFence = RHIFencePtr::Make();
		Require(driver->SubmitCommandList(first.command, firstFence) && driver->SubmitCommandList(second.command, secondFence),
			"both recorded flights must submit before either is awaited");
		Require(firstFence->Wait(5000000000ull) == EFenceStatus::Finished && secondFence->Wait(5000000000ull) == EFenceStatus::Finished,
			"both flight images must complete");
		RequireComposite(first, vec3(2, 0.5f, 0.125f));
		RequireComposite(second, vec3(0.25f, 2, 0.5f));

		firstContext->BeginSubmission(5, 0);
		scene.m_submissionContext = firstContext;
		node.SetFloat("maxAccumulatedSamples", 1);
		const auto samples = node.Camera().m_accumulatedSamples;
		auto reused = RecordComposite(node, graph, scene);
		Require(node.Resources(scene)->m_uploadBuffer == firstUpload && node.Camera().m_accumulatedSamples == samples,
			"a completed flight must reuse capacity and upload the capped camera image without retracing");
		Require(driver->SubmitCommandList_Immediate(reused.command), "the reused flight must complete");
		RequireComposite(reused, vec3(0.25f, 2, 0.5f));

		firstContext->BeginSubmission(6, 0);
		scene.m_cameraTransform.m_position.x = 0.25f;
		SetTracerMaterial(scene, firstMaterial);
		auto refused = RecordComposite(node, graph, scene);
		firstContext->InvalidateSubmissionResources();
		firstContext->BeginSubmission(7, 0);
		auto retry = RecordComposite(node, graph, scene);
		Require(driver->SubmitCommandList_Immediate(retry.command), "the refused upload must be recorded again on retry");
		RequireComposite(retry, vec3(2, 0.5f, 0.125f));
	}

	void TestTracerEnvironmentReadback(ImageNode& node, RHI::RHIFrameGraphPtr graph, RHI::RHISceneViewSnapshot& scene)
	{
		using namespace RHI;
		auto& driver = Renderer::GetDriver();
		auto commands = Renderer::GetDriverCommands();
		auto makeCube = [&](const vec3& color)
		{
			auto cube = driver->CreateCubemap(ivec2(8), 1, ETextureFormat::R16G16B16A16_SFLOAT);
			auto command = driver->CreateCommandList(false, ECommandListQueue::Graphics);
			commands->BeginCommandList(command, true);
			commands->ImageMemoryBarrier(command, cube, EImageLayout::TransferDstOptimal);
			commands->ClearImage(command, cube, vec4(color, 1));
			commands->ImageMemoryBarrier(command, cube, EImageLayout::ShaderReadOnlyOptimal);
			commands->EndCommandList(command);
			Require(driver->SubmitCommandList_Immediate(command), "the known HDR cubemap must initialize on the GPU");
			return cube;
		};
		auto first = makeCube(vec3(2, 1, 0.5f));
		auto second = makeCube(vec3(0.25f, 4, 2));
		auto diffuse = makeCube(vec3(0.125f, 0.5f, 3));
		auto requireEnvironment = [&](const vec3& expected)
		{
			PathTracer::PreparedRaySample sample;
			PathTracer::Params params{};
			Require(node.Camera().m_pathTracer.SamplePreparedSceneRay(vec3(10, 0, 3), vec3(0, 0, -1), 10, params, 1, sample) &&
				!sample.m_bHit && length(sample.m_radiance - expected) < 0.001f,
				"CPU tracing must use only the latest completed environment");
		};
		uint64_t submission = 8;
		auto record = [&](RHICubemapPtr raw, RHICubemapPtr irradiance)
		{
			scene.m_frame += 8;
			scene.m_submissionContext->BeginSubmission(submission++, 0);
			graph->SetSampler("g_rawEnvCubemap", raw);
			graph->SetSampler("g_irradianceCubemap", irradiance);
			return RecordComposite(node, graph, scene);
		};
		auto complete = [&](const RecordedComposite& frame)
		{
			auto fence = frame.context->GetFrameCompletion();
			Require(fence && driver->SubmitCommandList(frame.command, fence) && fence->Wait(5000000000ull) == EFenceStatus::Finished,
				"the readback must use the recorded frame's actual GPU completion");
		};

		auto initial = record(first, diffuse);
		const auto beforeEnvironment = node.Camera().m_imageRevision;
		for (auto& buffer : node.Camera().m_pendingReadback->m_environment.m_faceBuffers)
		{
			auto* bytes = static_cast<uint32_t*>(buffer->GetPointer());
			for (size_t i = 0; i < buffer->GetSize() / sizeof(uint32_t); ++i) bytes[i] = glm::packHalf2x16(vec2(7));
		}
		Require(!node.ApplyCompletedReadback(node.Camera(), first, diffuse), "recording alone must not make readback available");
		requireEnvironment(vec3(0));
		complete(initial);
		Require(node.ApplyCompletedReadback(node.Camera(), first, diffuse), "completed face buffers must publish together");
		requireEnvironment(vec3(2, 1, 0.5f));
		auto withEnvironment = record(first, diffuse);
		Require(node.Camera().m_imageRevision > beforeEnvironment,
			"completed environment content must restart a capped image");
		complete(withEnvironment);
		Require(node.ApplyCompletedReadback(node.Camera(), first, diffuse), "an identical refresh must complete");
		const auto unchangedRevision = node.Camera().m_imageRevision;
		auto unchanged = record(first, diffuse);
		Require(node.Camera().m_imageRevision == unchangedRevision,
			"an identical environment refresh must not restart accumulation");
		complete(unchanged);
		Require(node.ApplyCompletedReadback(node.Camera(), first, diffuse), "the unchanged refresh must remain readable");

		auto obsolete = record(second, diffuse);
		Require(node.Camera().m_imageRevision == unchangedRevision,
			"pending environment content must not invalidate the last completed image");
		Require(!node.ApplyCompletedReadback(node.Camera(), second, diffuse), "pending replacement must not replace the old environment");
		requireEnvironment(vec3(2, 1, 0.5f));
		complete(obsolete);
		Require(!node.ApplyCompletedReadback(node.Camera(), first, diffuse), "a completed obsolete source must not publish");
		requireEnvironment(vec3(2, 1, 0.5f));

		auto refused = record(second, diffuse);
		refused.context->GetFrameCompletion()->MarkSubmissionFailed();
		Require(!node.ApplyCompletedReadback(node.Camera(), second, diffuse), "failed submission must not publish its mapped bytes");
		requireEnvironment(vec3(2, 1, 0.5f));
		auto retry = record(second, diffuse);
		complete(retry);
		Require(node.ApplyCompletedReadback(node.Camera(), second, diffuse), "a fresh readback must recover from refusal");
		requireEnvironment(vec3(0.25f, 4, 2));
		auto changed = record(second, diffuse);
		Require(node.Camera().m_imageRevision > unchangedRevision, "the changed completed environment must invalidate capped samples");
		complete(changed);
		Require(node.ApplyCompletedReadback(node.Camera(), second, diffuse), "the replacement refresh must complete");
		auto diffuseOnly = record({}, diffuse);
		complete(diffuseOnly);
		Require(node.ApplyCompletedReadback(node.Camera(), {}, diffuse), "removing the raw map must publish the diffuse-only environment");
		requireEnvironment(vec3(0.125f, 0.5f, 3));
		const auto beforeRemoval = node.Camera().m_imageRevision;
		auto removed = record({}, {});
		Require(driver->SubmitCommandList_Immediate(removed.command), "removing environment inputs must still composite");
		Require(node.Camera().m_imageRevision > beforeRemoval, "removing the environment must invalidate the image");
		requireEnvironment(vec3(0));

		auto discarded = record(first, diffuse);
		node.Clear();
		complete(discarded);
		Require(node.NumCameras() == 0 && !node.ApplyCompletedReadback(node.Camera(), first, diffuse),
			"Clear must prevent an old GPU completion from resurrecting camera readback state");
	}

	void TestCappedSceneChanges(ModelPtr model, ShaderSetPtr shader, RHI::RHIFrameGraphPtr graph,
		RHI::RHIShaderBindingSetPtr frameBindings)
	{
		using namespace RHI;
		TracerWorld world;
		auto object = world.Instantiate("Emissive quad");
		auto mesh = object->AddComponent<MeshRendererComponent>();
		mesh->GetData().SetModel(model);
		auto material = MaterialPtr::Make(world.GetAllocator(), FileId::Invalid);
		material->SetUniform("material.baseColorFactor", vec4(0, 0, 0, 1));
		material->SetUniform("material.emissiveFactor", vec4(2, 0.5f, 0.125f, 0));
		mesh->GetMaterials() = { material };
		auto proxy = object->AddComponent<PathTracerProxyComponent>();
		proxy->SetEnabled(true);
		world.GetECS<PathTracerECS>()->SetPathTracingEnabled(true);
		auto view = CreateTracerView(world);
		auto node = TRefPtr<ImageNode>::Make();
		node->m_pShader = shader;
		node->SetFloat("enabled", 1);
		node->SetFloat("maxBounces", 1);
		node->SetFloat("samplesPerFrame", 2);
		node->SetFloat("maxAccumulatedSamples", 2);
		uint64_t submission = 100;
		ivec2 extent(32);
		LightProxy sun;
		sun.m_type = ELightType::Directional;
		sun.m_direction = vec3(0, 0, -1);
		sun.m_intensity = vec3(0);
		auto draw = [&]()
		{
			view->m_submissionContext->BeginSubmission(submission++, 0);
			world.Publish(view);
			auto& scene = view->m_snapshots[0];
			scene.m_frameBindings = frameBindings;
			auto traced = TSharedPtr<RHIPathTracerScene>::Make(*scene.m_pathTracerScene);
			traced->m_lights = { sun };
			scene.m_pathTracerScene = std::move(traced);
			auto frame = RecordComposite(*node, graph, scene, extent);
			Require(Renderer::GetDriver()->SubmitCommandList_Immediate(frame.command), "the capped image must complete");
			return frame;
		};
		RequireComposite(draw(), vec3(2, 0.5f, 0.125f));
		Require(view->m_snapshots[0].m_pathTracerScene->m_instances.Num() == 1, "the real ECS must publish the fixture model");
		const auto revision = node->Camera().m_imageRevision;
		RequireComposite(draw(), vec3(2, 0.5f, 0.125f));
		Require(node->Camera().m_imageRevision == revision, "unchanged input must preserve capped accumulation");
		material->SetUniform("material.emissiveFactor", vec4(0.25f, 2, 0.5f, 0));
		RequireComposite(draw(), vec3(0.25f, 2, 0.5f), "the ECS material edit must replace a capped image");
		Require(node->Camera().m_imageRevision > revision && node->Camera().m_accumulatedSamples == 2,
			"editing a material must restart capped accumulation without moving the camera");

		auto unchanged = node->Camera().m_imageRevision;
		auto unrelated = MaterialPtr::Make(world.GetAllocator(), FileId::Invalid);
		unrelated->SetUniform("material.emissiveFactor", vec4(5));
		RequireComposite(draw(), vec3(0.25f, 2, 0.5f));
		Require(node->Camera().m_imageRevision == unchanged, "an unrelated material must not reset the traced scene");
		auto replacement = MaterialPtr::Make(world.GetAllocator(), FileId::Invalid);
		replacement->SetUniform("material.baseColorFactor", vec4(0, 0, 0, 1));
		replacement->SetUniform("material.emissiveFactor", vec4(0));
		replacement->SetUniform("material.emissiveFactor", vec4(4, 1, 0.5f, 0));
		Require(replacement->GetContentRevision() == material->GetContentRevision(),
			"replacement fixture must distinguish material identity, not only its revision number");
		mesh->GetMaterials()[0] = replacement;
		RequireComposite(draw(), vec3(4, 1, 0.5f));
		Require(node->Camera().m_accumulatedSamples == 2, "material replacement must not mix old radiance");

		node->SetFloat("maxAccumulatedSamples", 6);
		RequireComposite(draw(), vec3(4, 1, 0.5f));
		Require(node->Camera().m_accumulatedSamples == 4, "raising the stopping budget must resume valid accumulation");
		replacement->SetUniform("material.emissiveFactor", vec4(1, 3, 0.25f, 0));
		RequireComposite(draw(), vec3(1, 3, 0.25f));
		Require(node->Camera().m_accumulatedSamples == 2, "a pre-limit edit must discard old samples, not average scene states");
		node->SetFloat("maxAccumulatedSamples", 2);
		unchanged = node->Camera().m_imageRevision;
		extent = ivec2(64);
		RequireComposite(draw(), vec3(1, 3, 0.25f));
		Require(node->Camera().m_extent == uvec2(64) && node->Camera().m_imageRevision > unchanged,
			"same-aspect output resize must restart a capped image");

		object->GetTransformComponent().SetPosition(vec3(5, 0, 0));
		RequireComposite(draw(), vec3(0));
		object->GetTransformComponent().SetPosition(vec3(0));
		RequireComposite(draw(), vec3(1, 3, 0.25f));
		proxy->SetEnabled(false);
		RequireComposite(draw(), vec3(0));
		TVector<u8vec4> display;
		uvec2 displayExtent;
		Require(!node->GetLastRenderedImage(display, displayExtent), "removing the last tracer must discard stale display output");
		proxy->SetEnabled(true);
		RequireComposite(draw(), vec3(1, 3, 0.25f));
		auto otherObject = world.Instantiate("Other tracer subset");
		otherObject->GetTransformComponent().SetPosition(vec3(5, 0, 0));
		auto otherMesh = otherObject->AddComponent<MeshRendererComponent>();
		otherMesh->GetData().SetModel(model);
		otherMesh->GetMaterials() = { replacement };
		auto otherProxy = otherObject->AddComponent<PathTracerProxyComponent>();
		otherProxy->SetEnabled(true);
		proxy->SetEnabled(false);
		RequireComposite(draw(), vec3(0));
		Require(view->m_snapshots[0].m_pathTracerScene->m_instances.Num() == 1,
			"replacing the active tracer subset must keep the same instance count");
		otherProxy->SetEnabled(false);
		proxy->SetEnabled(true);
		RequireComposite(draw(), vec3(1, 3, 0.25f));

		node->SetFloat("maxAccumulatedSamples", 1);
		for (const auto& setting : std::array<std::pair<const char*, float>, 4>{ {
			{ "samplesPerFrame", 1 }, { "maxBounces", 2 }, { "rayBiasBase", 0.01f }, { "rayBiasScale", 0.001f } } })
		{
			unchanged = node->Camera().m_imageRevision;
			node->SetFloat(setting.first, setting.second);
			RequireComposite(draw(), vec3(1, 3, 0.25f));
			Require(node->Camera().m_imageRevision > unchanged && node->Camera().m_accumulatedSamples == 1,
				"a tracing setting change must restart accumulation before checking the cap");
		}
		unchanged = node->Camera().m_imageRevision;
		RequireComposite(draw(), vec3(1, 3, 0.25f));
		Require(node->Camera().m_imageRevision == unchanged, "unchanged settings must preserve the result");
		view->m_cameraTransforms[0].m_position.x += 6e-5f;
		RequireComposite(draw(), vec3(1, 3, 0.25f));
		Require(node->Camera().m_imageRevision == unchanged, "sub-tolerance camera jitter must preserve the image");
		view->m_cameraTransforms[0].m_position.x += 6e-5f;
		RequireComposite(draw(), vec3(1, 3, 0.25f));
		Require(node->Camera().m_imageRevision > unchanged,
			"slow camera drift must be compared with the accumulated view, not the previous frame");
		replacement->SetUniform("material.baseColorFactor", vec4(0.5f, 0.5f, 0.5f, 1));
		replacement->SetUniform("material.emissiveFactor", vec4(0));
		RequireComposite(draw(), vec3(0));
		unchanged = node->Camera().m_imageRevision;
		sun.m_intensity = vec3(3);
		++view->m_lightingRevision;
		const auto lit = draw();
		Require(length(ReadComposite(lit)) > 0.05f && node->Camera().m_imageRevision > unchanged,
			"a new lighting revision must illuminate the capped image without a camera edit");
		sun.m_intensity = vec3(0);
		++view->m_lightingRevision;
		RequireComposite(draw(), vec3(0));
	}

	void TestHdrCompositing()
	{
		using namespace RHI;
		auto* registry = App::GetSubmodule<AssetRegistry>();
		const auto info = registry->GetAssetInfoPtr<ModelAssetInfoPtr>("Quad.gltf");
		ModelPtr model;
		Require(info && App::GetSubmodule<ModelImporter>()->LoadModel_Immediate(info->GetFileId(), model),
			"the HDR fixture must reuse the temporary model");
		ShaderSetPtr shader;
		const auto shaderInfo = registry->GetAssetInfoPtr("Shaders/PathTracerComposite.shader");
		Require(shaderInfo && App::GetSubmodule<ShaderCompiler>()->LoadShader_Immediate(shaderInfo->GetFileId(), shader),
			"the actual path tracer composite shader must load");
		auto task = Tasks::CreateTaskWithResult<std::string>("Path tracer HDR composite validation", [model, shader]()
			{
				try
				{
					constexpr uint32_t side = 512;
					auto& driver = Renderer::GetDriver();
					auto commands = Renderer::GetDriverCommands();
					auto graph = TRefPtr<ImageGraph>::Make();
					auto node = TRefPtr<ImageNode>::Make();
					node->m_pShader = shader;
					node->SetFloat("enabled", 1);
					node->SetFloat("maxBounces", 1);
					node->SetFloat("maxAccumulatedSamples", 1);
					auto allocator = Memory::ObjectAllocatorPtr::Make();
					auto material = MaterialPtr::Make(allocator, FileId::Invalid);
					material->SetUniform("material.baseColorFactor", vec4(0, 0, 0, 1));
					material->SetUniform("material.emissiveFactor", vec4(2, 0.5f, 0.125f, 0));
					RHISceneViewSnapshot scene;
					scene.m_submissionContext = RHIRenderSubmissionContextPtr::Make();
					scene.m_submissionContext->BeginSubmission(1, 0);
					scene.m_camera = TUniquePtr<CameraData>::Make();
					scene.m_camera->SetAspect(1);
					scene.m_camera->SetFov(glm::degrees(0.8f));
					scene.m_cameraTransform.m_position = vec4(0, 0, 3, 1);
					auto traced = TSharedPtr<RHIPathTracerScene>::Make();
					PathTracer::TLASInstance instance;
					instance.m_model = model;
					instance.m_worldBounds = Math::AABB(vec3(0), vec3(1, 1, 0));
					traced->m_instances.Add(instance);
					traced->m_materials = PathTracer::CaptureMaterials({ material });
					LightProxy light;
					light.m_type = ELightType::Directional;
					light.m_direction = vec3(0, 0, -1);
					light.m_intensity = vec3(0);
					traced->m_lights.Add(light);
					scene.m_pathTracerScene = std::move(traced);
					scene.m_frameBindings = driver->CreateShaderBindings();
					auto command = driver->CreateCommandList(false, ECommandListQueue::Graphics);
					commands->BeginCommandList(command, true);
					UboFrameData frame{};
					for (uint32_t i = 0; i < 2; ++i)
					{
						auto binding = driver->AddBufferToShaderBindings(scene.m_frameBindings,
							i ? "previousFrameData" : "frameData", sizeof(frame), i, EShaderBindingType::UniformBuffer);
						commands->UpdateShaderBinding(command, binding, &frame, sizeof(frame));
					}
					auto target = driver->CreateRenderTarget(command, ivec2(side), 1, ETextureFormat::R16G16B16A16_SFLOAT);
					node->SetRHIResource("color", target);
					commands->ImageMemoryBarrier(command, target, EImageLayout::ColorAttachmentOptimal);
					commands->BeginRenderPass(command, TVector<RHITexturePtr>{ target }, nullptr, ivec4(0, 0, side, side),
						ivec2(0), true, vec4(0.125f, 1.5f, 4, 1), 0, false);
					commands->EndRenderPass(command);
					node->Process(graph, command, command, scene);
					auto readback = driver->CreateBuffer(side * side * 8u, EBufferUsageBit::BufferTransferDst_Bit,
						EMemoryPropertyBit::HostVisible | EMemoryPropertyBit::HostCoherent);
					commands->ImageMemoryBarrier(command, target, EImageLayout::TransferSrcOptimal);
					commands->CopyImageToBuffer(command, target, readback);
					command->m_vulkan.m_commandBuffer->MemoryBarrier(VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_HOST_READ_BIT);
					commands->EndCommandList(command);
					Require(driver->SubmitCommandList_Immediate(command), "HDR composite and readback must complete");
					const size_t center = (side / 2u) * side + side / 2u;
					const size_t sourceCenter = (node->Camera().m_extent.y / 2u) * node->Camera().m_extent.x + node->Camera().m_extent.x / 2u;
					const vec4 expected(2, 0.5f, 0.125f, 1);
					Require(node->Camera().m_accumulatedImage.Num() > sourceCenter &&
						length(node->Camera().m_accumulatedImage[sourceCenter] - expected) < 0.001f,
						"CPU accumulation must retain linear HDR radiance instead of clamped sRGB bytes");
#ifdef __APPLE__
					Require(node->Camera().m_extent == uvec2(474), "float output must preserve the macOS pixel budget rather than reduce resolution");
#else
					Require(node->Camera().m_extent == uvec2(side), "float output must preserve the requested resolution");
#endif
					Require(node->Resources(scene)->m_uploadBuffer->GetSize() == node->Camera().m_accumulatedImage.Num() * sizeof(vec4) &&
						node->Resources(scene)->m_uploadBuffer->GetSize() > 900000u, "the HDR upload must contain full float pixels, including transfers above 900KB");
					const auto* pixels = static_cast<const uint32_t*>(readback->GetPointer());
					auto pixel = [&](size_t i) { return vec4(glm::unpackHalf2x16(pixels[2 * i]), glm::unpackHalf2x16(pixels[2 * i + 1])); };
					// HDR target alpha carries renderer metadata, not source coverage.
					Require(length(vec3(pixel(center)) - vec3(expected)) < 0.001f, "GPU composite must receive the same linear HDR color");
					Require(length(vec3(pixel(0)) - vec3(0.125f, 1.5f, 4)) < 0.001f, "transparent tracer pixels must preserve the HDR background");
					Require(node->GetDrawCallStats().m_numBatches == 1u, "HDR validation must execute the real composite draw");

					for (uint32_t i = 0; i < 32; ++i)
					{
						const float blend = i % 2 ? 0.75f : 0.25f;
						const vec3 expectedBlend = mix(vec3(0.125f, 1.5f, 4), vec3(expected), blend);
						scene.m_submissionContext->BeginSubmission(i + 2, 0);
						node->SetFloat("blend", blend);
						command = driver->CreateCommandList(false, ECommandListQueue::Graphics);
						commands->BeginCommandList(command, true);
						commands->ImageMemoryBarrier(command, target, EImageLayout::ColorAttachmentOptimal);
						commands->BeginRenderPass(command, TVector<RHITexturePtr>{ target }, nullptr, ivec4(0, 0, side, side),
							ivec2(0), true, vec4(0.125f, 1.5f, 4, 1), 0, false);
						commands->EndRenderPass(command);
						node->Process(graph, command, command, scene);
						commands->ImageMemoryBarrier(command, target, EImageLayout::TransferSrcOptimal);
						commands->CopyImageToBuffer(command, target, readback);
						command->m_vulkan.m_commandBuffer->MemoryBarrier(VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_HOST_READ_BIT);
						commands->EndCommandList(command);
						Require(driver->SubmitCommandList_Immediate(command), "partial HDR blending must complete");
						const vec3 blended(pixel(center));
						if (!(length(blended - expectedBlend) < 0.001f))
							std::cerr << "HDR blend " << blend << " RGB: " << blended.r << ", " << blended.g << ", " << blended.b
								<< "; batches: " << node->GetDrawCallStats().m_numBatches << '\n';
						Require(length(blended - expectedBlend) < 0.001f,
							"partial blending must mix source and background in linear HDR space");
						Require(node->Camera().m_accumulatedSamples == 1u && node->GetDrawCallStats().m_numBatches == 1u,
							"the sample limit must reuse the float image while still drawing the composite");
					}

					node->SetFloat("blend", 1);
					node->SetFloat("maxAccumulatedSamples", 0);
					scene.m_submissionContext->BeginSubmission(2, 0);
					command = driver->CreateCommandList(false, ECommandListQueue::Graphics);
					commands->BeginCommandList(command, true);
					node->Process(graph, command, command, scene);
					commands->ImageMemoryBarrier(command, target, EImageLayout::TransferSrcOptimal);
					commands->CopyImageToBuffer(command, target, readback);
					auto secondTarget = driver->CreateRenderTarget(command, ivec2(side), 1, ETextureFormat::R16G16B16A16_SFLOAT);
					auto secondReadback = driver->CreateBuffer(side * side * 8u, EBufferUsageBit::BufferTransferDst_Bit,
						EMemoryPropertyBit::HostVisible | EMemoryPropertyBit::HostCoherent);
					auto secondMaterial = MaterialPtr::Make(allocator, FileId::Invalid);
					secondMaterial->SetUniform("material.baseColorFactor", vec4(0, 0, 0, 1));
					secondMaterial->SetUniform("material.emissiveFactor", vec4(0.25f, 2, 0.5f, 0));
					SetTracerMaterial(scene, secondMaterial);
					scene.m_cameraIndex = 1;
					scene.m_cameraTransform.m_position.x = 0.1f;
					node->SetRHIResource("color", secondTarget);
					commands->ImageMemoryBarrier(command, secondTarget, EImageLayout::ColorAttachmentOptimal);
					commands->BeginRenderPass(command, TVector<RHITexturePtr>{ secondTarget }, nullptr, ivec4(0, 0, side, side),
						ivec2(0), true, vec4(0), 0, false);
					commands->EndRenderPass(command);
					node->Process(graph, command, command, scene);
					commands->ImageMemoryBarrier(command, secondTarget, EImageLayout::TransferSrcOptimal);
					commands->CopyImageToBuffer(command, secondTarget, secondReadback);
					commands->MemoryBarrier(command, static_cast<EAccessFlags>(EAccessBit::TransferWrite_Bit),
						static_cast<EAccessFlags>(EAccessBit::HostRead_Bit));
					commands->EndCommandList(command);
					Require(driver->SubmitCommandList_Immediate(command), "both recorded cameras must complete together");
					Require(length(vec3(pixel(center)) - vec3(expected)) < 0.001f,
						"recording a second camera must not overwrite the first camera's pending upload");
					const auto* secondPixels = static_cast<const uint32_t*>(secondReadback->GetPointer());
					const vec4 secondColor(glm::unpackHalf2x16(secondPixels[2 * center]), glm::unpackHalf2x16(secondPixels[2 * center + 1]));
					Require(length(vec3(secondColor) - vec3(0.25f, 2, 0.5f)) < 0.001f,
						"the second camera must composite its own accumulated image");
					TestOverlappingTracerFlights(*node, graph, scene, material, secondMaterial);
					TestCappedSceneChanges(model, shader, graph, scene.m_frameBindings);
					TestTracerEnvironmentReadback(*node, graph, scene);
					return std::string{};
				}
				catch (const std::exception& error) { return std::string(error.what()); }
			}, EThreadType::Render);
		task->Run();
		task->Wait();
		if (!task->GetResult().empty()) throw std::runtime_error(task->GetResult());
		TestTracerSceneDemand(model);
		TestRendererTracerDemand(model);
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
		instance.m_worldBounds = Math::AABB(bounds.m_center, vec3(bounds.m_radius));
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
	int RunCloudNoiseGpu(int argc, const char** argv)
	{
		TempDirectory workspace("cloud-noise");
		int result = 1;
		try
		{
			std::string enginePath = std::filesystem::current_path().string();
			for (int i = 1; i + 1 < argc; ++i)
				if (std::string_view(argv[i]) == "--workspace") enginePath = argv[i + 1];
			WriteScene(workspace, enginePath, 1);
			const auto settingsPath = workspace.Path("ProjectSettings.yaml");
			auto settings = YAML::LoadFile(settingsPath.string());
			for (const char* preset : { "Ultra", "High", "Medium", "Low", "VeryLow" })
			{
				auto profile = settings["graphics"]["presets"][preset];
				profile["skyResolution"] = 64;
				profile["cloudsResolutionMultiplier"] = 0.125;
				profile["resolutionFactor"] = 0.25;
				profile["cloudsDithering"] = false;
			}
			std::ofstream(settingsPath) << settings;
			const auto root = workspace.Get().string();
			std::vector<const char*> arguments(argv, argv + argc);
			arguments.insert(arguments.end(), { "--workspace", root.c_str(), "--editor", "--port", "0", "--world", "" });
			Require(App::Initialize(arguments.data(), static_cast<int32_t>(arguments.size())) == EAppInitializationResult::Ready &&
				App::IsRendererInitialized(), "the cloud fixture must initialize a hidden native renderer");
			RunCloudNoiseCommandTests(workspace.Get());
			result = 0;
		}
		catch (const std::exception& error) { std::cerr << error.what() << '\n'; }
		App::Stop();
		if (!App::Shutdown()) result = 1;
		return result;
	}

	int RunPathTracerCommandTests(int argc, const char** argv)
	{
		Tests::TempDirectory workspace("pathtracer-command");
		int result = 1;
		try
		{
			std::string enginePath = std::filesystem::current_path().string();
			uint32_t msaaSamples = 2;
			bool giShutdown = false;
			for (int i = 1; i < argc; ++i)
			{
				if (std::string_view(argv[i]) == "--workspace" && i + 1 < argc) enginePath = argv[i + 1];
				if (std::string_view(argv[i]) == "--gpu-pathtracer-1x") msaaSamples = 1;
				if (std::string_view(argv[i]) == "--gpu-gi-shutdown") giShutdown = true;
			}
			WriteScene(workspace, enginePath, msaaSamples);
			const std::string root = workspace.Get().string();
			const std::string output = workspace.Path("command.png").string();
			std::vector<const char*> arguments(argv, argv + argc);
			if (giShutdown)
			{
				arguments.insert(arguments.end(), { "--workspace", root.c_str(), "--editor", "--port", "0", "--world", "" });
				return RunGIShutdownCommandTests(static_cast<int>(arguments.size()), arguments.data(), workspace.Get());
			}
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
			for (int32_t y : { height / 3, 2 * height / 3 })
				for (int32_t x : { width / 3, 2 * width / 3 })
					Require(pixels[x + y * width].a == 255, "CLI instance bounds must retain all four quadrants of the model");
			TestPreparedParity(workspace, pixels);
			TestHdrCompositing();
			RunGIProbesCommandTests(workspace.Get());
			RunTextureImporterCommandTests(workspace.Get());
			RunMaterialImporterCommandTests(workspace.Get());
			RunAudioCommandTests(workspace.Get());
			RunModelLodCommandTests(workspace.Get());
			RunPrefabImporterCommandTests(workspace.Get());
			RunFrameGraphNodeCommandTests(workspace.Get());
			RunLoggingCommandTests();
			std::cout << "PathTracer CLI/prepared pixel parity test passed\n";
			result = 0;
		}
		catch (const std::exception& error) { std::cerr << error.what() << '\n'; }
		App::Stop();
		try
		{
			if (result == 0) RunLoggingShutdownTests();
			else if (!App::Shutdown()) result = 1;
		}
		catch (const std::exception& error)
		{
			std::cerr << error.what() << '\n';
			App::Shutdown();
			result = 1;
		}
		return result;
	}
}
