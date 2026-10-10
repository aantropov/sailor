#include "Sailor.h"
#include "AssetRegistry/AssetRegistry.h"
#include "AssetRegistry/GlobalIllumination/GIProbesImporter.h"
#include "AssetRegistry/Material/MaterialImporter.h"
#include "AssetRegistry/Model/ModelImporter.h"
#include "AssetRegistry/Prefab/PrefabImporter.h"
#include "AssetRegistry/Texture/TextureImporter.h"
#include "AssetRegistry/World/WorldPrefabImporter.h"
#include "AssetRegistry/FrameGraph/FrameGraphImporter.h"
#include "Components/CameraComponent.h"
#include "Components/LightComponent.h"
#include "Components/MeshRendererComponent.h"
#include "Components/SkyComponent.h"
#include "Components/Tests/BufferReadback.h"
#include "Core/YamlUtils.h"
#include "ECS/GlobalIlluminationECS.h"
#include "ECS/LandscapeECS.h"
#include "ECS/LightingECS.h"
#include "ECS/StaticMeshRendererECS.h"
#include "ECS/TransformECS.h"
#include "Engine/GameObject.h"
#include "Engine/EngineLoop.h"
#include "Engine/World.h"
#include "Editor/GlobalIlluminationBakeController.h"
#include "GlobalIllumination/GIProbesBinary.h"
#include "GlobalIllumination/GIProbesSampling.h"
#include "FrameGraph/RHIFrameGraph.h"
#include "GraphicsDriver/Vulkan/VulkanCommandBuffer.h"
#include "Memory/UniquePtr.hpp"
#include "Memory/WeakPtr.hpp"
#include "RHI/CommandList.h"
#include "RHI/Cubemap.h"
#include "RHI/Renderer.h"
#include "Settings/GraphicsSettings.h"
#include "Submodules/Editor.h"
#if defined(__APPLE__)
#include "Support/VulkanCapabilityOverrides.h"
#endif

#include <array>
#include <barrier>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <latch>
#include <stdexcept>
#include <string_view>
#include <thread>

using namespace Sailor;

namespace Sailor::Tests
{
	void RequireRejectedComputeSubmission(const std::function<bool()>& submit);
	void RunRuntimeGIProbesTaskTests();
	uint64_t CountRuntimeGICompletedSamples(const RuntimeGIProbesService& service);
}

namespace Sailor
{
#if defined(SAILOR_GI_BAKE_TEST_HOOKS)
	class GlobalIlluminationBakeControllerTestAccess
	{
	public:
		static GlobalIlluminationBakeController& Controller(Editor& editor) { return *editor.m_giProbesBakeController; }
		static auto State(Editor& editor) { return Controller(editor).m_state; }
		static TWeakPtr<Tasks::ITask> Task(Editor& editor) { return Controller(editor).m_task; }
		static void Observe(void (*preparation)(), void (*saving)(), void (*waited)())
		{
			GlobalIlluminationBakeController::s_preparationObserver = preparation;
			GlobalIlluminationBakeController::s_savingObserver = saving;
			GlobalIlluminationBakeController::s_waitObserver = waited;
		}
	};
#endif

	class GlobalIlluminationECSTestAccess
	{
	public:
		static const RuntimeGIProbesService& RuntimeProbes(const GlobalIlluminationECS& system)
		{
			return system.m_runtimeProbes;
		}

		static bool FailCurrentPreparation(GlobalIlluminationECS& system)
		{
			if (!system.m_runtimeScenePreparationTask)
			{
				return false;
			}
			system.m_runtimeScenePreparationTask->Wait();
			auto result = system.m_runtimeScenePreparationTask->GetResult();
			if (!result.m_scene)
			{
				return false;
			}
			result.m_scene.Clear();
			result.m_diagnostic = "injected preparation failure";
			system.m_runtimeScenePreparationTask =
				Tasks::TaskPtr<GlobalIlluminationECS::RuntimeScenePreparationResult>::Make(std::move(result));
			return true;
		}

		static uint64_t PreparationCount(const GlobalIlluminationECS& system)
		{
			return system.m_runtimeScenePreparationRequestId;
		}

		static GIProbesPreparedScenePtr WaitPreparation(GlobalIlluminationECS& system)
		{
			if (!system.m_runtimeScenePreparationTask)
			{
				return {};
			}
			system.m_runtimeScenePreparationTask->Wait();
			return system.m_runtimeScenePreparationTask->GetResult().m_scene;
		}

		static GIProbesPreparedScenePtr PreparedScene(const GlobalIlluminationECS& system)
		{
			return system.m_runtimePreparedScene;
		}

		static GIProbesSceneSnapshotPtr CapturedScene(const GlobalIlluminationECS& system)
		{
			return system.m_runtimeSceneSnapshot;
		}
	};
}

namespace
{
	void Require(bool value, std::string_view message)
	{
		if (!value)
		{
			throw std::runtime_error(std::string(message));
		}
	}

	void WaitMaterialReady(const MaterialPtr& material)
	{
		const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
		while (!material->IsReady() && std::chrono::steady_clock::now() < deadline)
		{
			RHI::Renderer::GetDriver()->TrackResources_ThreadSafe();
			std::this_thread::yield();
		}
		Require(material->IsReady(), "GI fixture material upload must complete");
	}

#if defined(SAILOR_GI_BAKE_TEST_HOOKS)
	struct BakeShutdownObservation
	{
		std::latch resume{ 1 };
		std::atomic<bool> entered{ false }, released{ false }, shutdownEntered{ false };
		bool background = false, waitObserved = false, dependenciesAlive = false, taskReleased = false;
		bool acceptedCancellation = false;
		TWeakPtr<Tasks::ITask> task;
		TWeakPtr<World> world;

		void Release()
		{
			if (!released.exchange(true)) resume.count_down();
		}
	};

	BakeShutdownObservation* shutdownObservation = nullptr;

	void PauseBakeForShutdown()
	{
		auto& observation = *shutdownObservation;
		if (observation.entered.load(std::memory_order_acquire)) return;
		observation.background = App::GetSubmodule<Tasks::Scheduler>()->GetCurrentThreadType() == EThreadType::Background;
		observation.entered.store(true, std::memory_order_release);
		observation.resume.wait();
	}

	void ObserveBakeJoinedBeforeDependencies()
	{
		auto& observation = *shutdownObservation;
		if (observation.waitObserved) return;
		observation.waitObserved = true;
		observation.dependenciesAlive = App::GetSubmodule<TextureImporter>() && App::GetSubmodule<ModelImporter>() &&
			App::GetSubmodule<AssetRegistry>() && App::GetSubmodule<EngineLoop>() && observation.world.TryLock();
		observation.taskReleased = !observation.task.TryLock();
	}

	void TestSavedWorldBakePreflight(const std::filesystem::path& path,
		const EditorGIProbesBakeRequest& request, GameObjectPtr receiver, GameObjectPtr linked)
	{
		auto* editor = App::GetSubmodule<Editor>();
		std::string savedText;
		Require(AssetRegistry::ReadTextFile(path.string(), savedText), "preflight fixture must read its saved world");
		const auto saved = YAML::Load(savedText);
		auto write = [](const std::filesystem::path& destination, std::string_view text)
		{
			std::ofstream file(destination);
			file.write(text.data(), static_cast<std::streamsize>(text.size()));
			Require(file.good(), "preflight fixture must write the selected world");
		};
		auto reject = [&](std::string_view reason)
		{
			std::string diagnostic;
			const bool bStarted = editor->StartGIProbesBake(request, diagnostic);
			Require(!bStarted,
				"invalid or unsaved input must fail before starting a bake: " + std::string(reason) + "; " + diagnostic);
			const auto status = editor->GetGIProbesBakeStatus();
			Require(diagnostic.find(reason) != std::string::npos && status.m_diagnostic == diagnostic &&
				status.m_state == EEditorGIProbesBakeState::Failed,
				"preflight must report the actual input failure: " + diagnostic);
			Require(!GlobalIlluminationBakeControllerTestAccess::Task(*editor).TryLock() &&
				!std::filesystem::exists(path.parent_path() / request.m_outputVirtualPath),
				"a rejected preflight must neither schedule a bake task nor publish probes");
		};

		write(path, "prefabs: [");
		reject("asset is invalid");
		auto malformed = YAML::Clone(saved);
		malformed["prefabs"] = YAML::Node(YAML::NodeType::Map);
		write(path, YAML::Dump(malformed));
		reject("no prefab sequence");
		auto mesh = receiver->GetComponent<MeshRendererComponent>();
		malformed = YAML::Clone(saved);
		bool bChangedComponent = false;
		for (YAML::Node prefab : malformed["prefabs"])
		{
			for (YAML::Node component : prefab["components"])
			{
				if (component["overrideProperties"]["instanceId"].as<InstanceId>() == mesh->GetInstanceId())
				{
					component["typename"] = "MissingBakeComponent";
					bChangedComponent = true;
				}
			}
		}
		Require(bChangedComponent, "preflight must change a gameplay component, not the excluded editor camera");
		write(path, YAML::Dump(malformed));
		// Unknown types remain loadable, but changed gameplay data must not match the saved scene.
		reject("unsaved changes");
		for (const char* field : { "instanceIds", "gameObjectOverrides", "componentOverrides" })
		{
			malformed = YAML::Clone(saved);
			bool bRemoved = false;
			for (YAML::Node prefab : malformed["prefabs"])
			{
				if (prefab["fileId"].as<FileId>() == linked->GetFileId()) bRemoved |= prefab.remove(field);
			}
			Require(bRemoved, "the preflight fixture must include a linked record with explicit mappings");
			write(path, YAML::Dump(malformed));
			reject(field);
		}
		write(path, savedText);

		auto& transform = receiver->GetTransformComponent();
		const auto position = transform.GetPosition();
		transform.SetPosition(glm::vec3(position) + glm::vec3(1, 0, 0));
		reject("unsaved changes");
		transform.SetPosition(glm::vec3(position));
		const auto model = mesh->GetModel();
		mesh->SetModel({});
		reject("unsaved changes");
		mesh->SetModel(model);

		auto* materials = App::GetSubmodule<MaterialImporter>();
		MaterialAsset::Data surface;
		surface.m_shader = mesh->GetMaterials()[0]->GetShader()->GetFileId();
		surface.m_uniformsVec4["material.albedo"] = glm::vec4(0, 1, 0, 1);
		const FileId materialId = materials->CreateMaterialAsset((path.parent_path() / "BakeOverride.mat").string(), surface);
		MaterialPtr material;
		Require(materialId && materials->LoadMaterial_Immediate(materialId, material), "preflight fixture must load its second material");
		WaitMaterialReady(material);
		const auto overrides = mesh->GetOverrideMaterials();
		mesh->SetOverrideMaterials({ materialId });
		Require(mesh->GetMaterials()[0] == material, "the material edit must affect the live renderer");
		reject("unsaved changes");
		mesh->SetOverrideMaterials(overrides);
		auto light = receiver->GetComponent<LightComponent>();
		const auto intensity = light->GetIntensity();
		light->SetIntensity(intensity * 2.0f);
		reject("unsaved changes");
		light->SetIntensity(intensity);
		auto linkedMesh = linked->GetComponent<MeshRendererComponent>();
		Require(linkedMesh->GetMinLod() == 0u, "the linked instance must override its source with the class default");
		linkedMesh->SetMinLod(1u);
		reject("unsaved changes");
		linkedMesh->SetMinLod(0u);
		auto loaded = App::GetSubmodule<WorldPrefabImporter>()->Create();
		loaded->Deserialize(saved);
		Require(loaded->IsReady(), loaded->GetLoadDiagnostic());
		bool bCheckedLinkedRecord = false;
		for (const auto& prefab : loaded->GetGameObjects())
		{
			if (!prefab->IsLinkedInstanceRecord()) continue;
			const auto& ids = prefab->GetLinkedInstanceIds();
			const auto& componentOverrides = prefab->GetLinkedComponentOverrides();
			Require(ids.ContainsKey(receiver->GetInstanceId()) && ids[receiver->GetInstanceId()] == linked->GetInstanceId() &&
				componentOverrides.ContainsKey(mesh->GetInstanceId()) &&
				componentOverrides[mesh->GetInstanceId()].GetProperties()["minLod"].as<uint32_t>() == 0u,
				"loading must retain identity mappings and a class-default override of a nondefault source");
			bCheckedLinkedRecord = true;
		}
		Require(bCheckedLinkedRecord, "the saved world must load its linked record");
		const auto sourcePath = path.parent_path() / "BakeReceiver.prefab";
		std::string sourceText;
		Require(AssetRegistry::ReadTextFile(sourcePath.string(), sourceText), "the linked source must remain readable");
		auto editedSource = YAML::Load(sourceText);
		bool bChangedLight = false;
		for (YAML::Node component : editedSource["components"])
		{
			if (component["typename"].Scalar() != "Sailor::LightComponent") continue;
			component["overrideProperties"]["intensity"] = glm::vec3(3.0f);
			bChangedLight = true;
		}
		Require(bChangedLight, "the source edit must change a real inherited light");
		write(sourcePath, YAML::Dump(editedSource));
		reject("unsaved changes");
		write(sourcePath, sourceText);
		std::cout << "Saved-world preflight rejected malformed YAML, missing linked maps, changed source and live transform/model/material/light/override edits without starting a task\n";
	}

	EditorGIProbesBakeRequest CreateShutdownBakeScene(const std::filesystem::path& workspace, const char* output)
	{
		auto world = App::GetSubmodule<EngineLoop>()->GetWorld();
		auto* registry = App::GetSubmodule<AssetRegistry>();
		const auto info = registry->GetAssetInfoPtr<ModelAssetInfoPtr>("Quad.gltf");
		ModelPtr model;
		Require(info && App::GetSubmodule<ModelImporter>()->LoadModel_Immediate(info->GetFileId(), model),
			"shutdown fixture must load real captured geometry");
		TVector<MaterialPtr> materials;
		auto load = App::GetSubmodule<ModelImporter>()->LoadDefaultMaterials(info->GetFileId(), materials);
		load->Wait();
		Require(load->GetResult() && !materials.IsEmpty(), "shutdown fixture must load a real material");
		WaitMaterialReady(materials[0]);
		auto object = world->Instantiate("Shutdown bake receiver");
		object->SetMobilityType(EMobilityType::Static);
		object->GetTransformComponent().SetPosition({ 0, 0, -2 });
		auto renderer = object->AddComponent<MeshRendererComponent>();
		renderer->GetData().SetModel(model);
		renderer->GetMaterials() = { materials[0] };
		object->AddComponent<LightComponent>()->SetIntensity(glm::vec3(2.0f));
		renderer->SetMinLod(1u);
		const auto sourcePath = workspace / "Content" / "BakeReceiver.prefab";
		Require(Prefab::FromGameObject(object)->SaveToFile(sourcePath.string()), "the linked bake source must be saved");
		renderer->SetMinLod(0u);
		const FileId prefabId = registry->GetOrLoadFile(sourcePath.string());
		PrefabPtr prefab;
		Require(prefabId && App::GetSubmodule<PrefabImporter>()->LoadPrefab_Immediate(prefabId, prefab),
			"the bake source prefab must load through its importer");
		auto linked = world->Instantiate(prefab);
		Require(linked && linked->GetFileId() == prefabId, "the bake fixture must contain a real linked prefab instance");
		auto linkedMesh = linked->GetComponent<MeshRendererComponent>();
		Require(linkedMesh && linkedMesh->GetMinLod() == 1u, "the linked source must have a nondefault minimum LOD");
		linkedMesh->SetMinLod(0u);
		linked->GetTransformComponent().SetPosition({ 2, 0, -2 });
		world->GetECS<StaticMeshRendererECS>()->BeginPlay();
		world->GetECS<TransformECS>()->Tick(0.0f);
		world->GetECS<StaticMeshRendererECS>()->Tick(0.0f);
		world->GetECS<LightingECS>()->Tick(0.0f);
		const auto document = WorldPrefab::FromWorld(world.GetRawPtr());
		const auto path = workspace / "Content" / "Shutdown.world";
		Require(document && document->IsReady() && document->SaveToFile(path.string()), "shutdown fixture world must be saved");
		const auto serialized = document->Serialize();
		TVector<YAML::Node> fields;
		for (const auto& field : serialized) fields.Add(field.first);
		YAML::Node authored(YAML::NodeType::Map);
		for (size_t i = fields.Num(); i-- > 0;)
			authored[fields[i]] = YAML::Clone(serialized[fields[i]]);
		const auto defaultRotation = Prefab::ReflectedGameObject{}.Serialize()["rotation"];
		const auto& defaultMinLod = Reflection::GetCDO("Sailor::MeshRendererComponent").GetProperties()["minLod"];
		bool bOmittedRotation = false, bOmittedMinLod = false;
		for (YAML::Node prefab : authored["prefabs"])
		{
			for (YAML::Node object : prefab["gameObjects"])
			{
				if (Utils::AreYamlNodesEqual(object["rotation"], defaultRotation))
					bOmittedRotation |= object.remove("rotation");
			}
			for (YAML::Node component : prefab["components"])
			{
				if (component["typename"].Scalar() == "Sailor::MeshRendererComponent" &&
					Utils::AreYamlNodesEqual(component["overrideProperties"]["minLod"], defaultMinLod))
					bOmittedMinLod |= component["overrideProperties"].remove("minLod");
			}
		}
		Require(bOmittedRotation && bOmittedMinLod, "the native bake must exercise both world and component defaults");
		{
			std::ofstream output(path);
			output << authored;
			Require(output.good(), "the equivalent authored world must be written before preflight");
		}
		EditorGIProbesBakeRequest request;
		request.m_worldAsset = registry->GetOrLoadFile(path.string());
		request.m_stateName = "Shutdown lifecycle";
		request.m_outputVirtualPath = output;
		request.m_settings.m_bIncludeSky = false;
		request.m_settings.m_bounceCount = 1u;
		request.m_settings.m_minProbeSpacing = 8.0f;
		request.m_settings.m_maxSubdivisionLevel = 1u;
		request.m_settings.m_raysPerProbe = 8u;
		Require(static_cast<bool>(request.m_worldAsset), "shutdown fixture world must be registered");
		TestSavedWorldBakePreflight(path, request, object, linked);
		return request;
	}
#endif

	class GIWorld final : public World
	{
	public:
		GIWorld() : World("GI lifecycle", 0, CreateEcs())
		{
			GetECS<StaticMeshRendererECS>()->BeginPlay();
			Instantiate("Camera")->AddComponent<CameraComponent>();
			auto* registry = App::GetSubmodule<AssetRegistry>();
			const auto info = registry->GetAssetInfoPtr<ModelAssetInfoPtr>("Quad.gltf");
			ModelPtr model;
			Require(info && App::GetSubmodule<ModelImporter>()->LoadModel_Immediate(info->GetFileId(), model),
				"GI lifecycle fixture must load its real model");
			auto object = Instantiate("Static quad");
			object->SetMobilityType(EMobilityType::Static);
			auto renderer = object->AddComponent<MeshRendererComponent>();
			renderer->GetData().SetModel(model);
			TVector<MaterialPtr> materials;
			auto loadMaterials = App::GetSubmodule<ModelImporter>()->LoadDefaultMaterials(info->GetFileId(), materials);
			loadMaterials->Wait();
			Require(loadMaterials->GetResult() && !materials.IsEmpty(), "GI fixture material must load");
			WaitMaterialReady(materials[0]);
			m_material = materials[0];
			renderer->GetMaterials() = { m_material };
			object->GetTransformComponent().SetPosition(glm::vec3(0, 0, -2));
			auto back = Instantiate("Back quad");
			back->SetMobilityType(EMobilityType::Static);
			back->GetTransformComponent().SetPosition(glm::vec3(0, 0, -4));
			auto backRenderer = back->AddComponent<MeshRendererComponent>();
			backRenderer->GetData().SetModel(model);
			backRenderer->GetMaterials() = { m_material };
			GISettings settings;
			settings.m_mode = EGlobalIlluminationMode::Runtime;
			settings.m_runtimeProbes.m_bounceCount = 1;
			settings.m_runtimeProbes.m_minProbeSpacing = 8;
			settings.m_runtimeProbes.m_maxRayDistance = 20;
			settings.m_runtimeProbes.m_bIncludeSky = false;
			std::string diagnostic;
			Require(GI().ApplyWorldSettings(settings, diagnostic), diagnostic);
			Require(GI().SetRuntimeGIProbesPreviewEnabled(true, diagnostic), diagnostic);
			Require(GI().SetRuntimeGIProbesEditorBudget(Settings::ERuntimeGIProbesEditorBudget::Balanced, diagnostic), diagnostic);
		}
		~GIWorld() override { Clear(); }
		GlobalIlluminationECS& GI() { return *GetECS<GlobalIlluminationECS>(); }

		void Step(float elapsed = 1.0f / 60.0f)
		{
			++m_currentFrame;
			GetECS<TransformECS>()->Tick(elapsed);
			GetECS<StaticMeshRendererECS>()->Tick(elapsed);
			GetECS<CameraECS>()->Tick(elapsed);
			GI().Tick(elapsed);
		}

		void WaitReady(uint64_t afterRevision = 0)
		{
			const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
			do
			{
				Step();
				const auto status = GI().GetRuntimeGIProbesStatus();
				if (status.m_lifecycle == ERuntimeGIProbesLifecycle::Ready &&
					status.m_refinement == 1.0f && status.m_publishedRevision > afterRevision && GI().GetActiveSnapshot())
				{
					return;
				}
				std::this_thread::yield();
			} while (std::chrono::steady_clock::now() < deadline);
			throw std::runtime_error("GI did not converge: " + GI().GetRuntimeGIProbesStatus().m_diagnostic);
		}

		MaterialPtr m_material;

	private:
		static TVector<ECS::TBaseSystemPtr> CreateEcs()
		{
			TVector<ECS::TBaseSystemPtr> systems;
			systems.Add(TUniquePtr<TransformECS>::Make());
			systems.Add(TUniquePtr<StaticMeshRendererECS>::Make());
			systems.Add(TUniquePtr<CameraECS>::Make());
			systems.Add(TUniquePtr<LightingECS>::Make());
			systems.Add(TUniquePtr<LandscapeECS>::Make());
			systems.Add(TUniquePtr<GlobalIlluminationECS>::Make());
			return systems;
		}
	};

	void TestRestart(GIProbesDataPtr& data, uint64_t& publishedBytes)
	{
		GIWorld world;
		world.WaitReady();
		auto& gi = world.GI();
		const auto published = gi.GetActiveSnapshot();
		data = published->m_layout;
		const auto completed = gi.GetRuntimeGIProbesStatus();
		const auto prepared = GlobalIlluminationECSTestAccess::PreparedScene(gi);
		const auto preparationCount = GlobalIlluminationECSTestAccess::PreparationCount(gi);
		const auto& service = GlobalIlluminationECSTestAccess::RuntimeProbes(gi);
		Require(Tests::CountRuntimeGICompletedSamples(service) > 0, "the initial ECS solve must trace real irradiance samples");
		publishedBytes = completed.m_publishedBytes;
		Require(completed.m_activeProbeCount == 8 && completed.m_readyProbeCount == 8,
			"the ECS fixture must fully refine its eight real probes");
		gi.SetRuntimeGIProbesWorkAllowed(false);
		std::string diagnostic;
		Require(gi.SetRuntimeGIProbesEditorBudget(Settings::ERuntimeGIProbesEditorBudget::Eco, diagnostic), diagnostic);
		Require(gi.GetRuntimeGIProbesStatus().m_readyProbeCount == completed.m_readyProbeCount,
			"ordinary budget changes must retain compatible completed probes");
		Require(gi.RestartRuntimeGIProbes(diagnostic), diagnostic);
		Require(Tests::CountRuntimeGICompletedSamples(service) == 0,
			"explicit ECS restart must clear the actual accumulators, not only the reported progress");
		Require(gi.GetRuntimeGIProbesStatus().m_readyProbeCount == 0 &&
			gi.GetRuntimeGIProbesStatus().m_refinement == 0.0f,
			"explicit ECS restart must discard refined samples even with identical inputs");
		Require(gi.GetActiveSnapshot() == published, "restart must retain the last good publication while warming");
		gi.SetRuntimeGIProbesWorkAllowed(true);
		world.WaitReady(completed.m_publishedRevision);
		Require(gi.GetActiveSnapshot() != published &&
			gi.GetRuntimeGIProbesStatus().m_publishedRevision > completed.m_publishedRevision,
			"the restarted solver must trace and publish a new result");
		Require(Tests::CountRuntimeGICompletedSamples(service) > 0 &&
			GlobalIlluminationECSTestAccess::PreparedScene(gi) == prepared &&
			GlobalIlluminationECSTestAccess::PreparationCount(gi) == preparationCount,
			"unchanged-scene ECS restart must execute new ray work using the existing prepared scene");
		std::cout << "ECS Runtime GI restart: unchanged capture, zeroed samples and new GI work passed\n";
	}

	class ReloadTaskProbe final : public IAssetInfoHandlerListener
	{
	public:
		ReloadTaskProbe(AssetInfo& info) : m_handler(*info.GetHandler()), m_fileId(info.GetFileId())
		{
			m_handler.Subscribe(this);
		}
		~ReloadTaskProbe()
		{
			m_handler.Unsubscribe(this);
		}

		void OnImportAsset(AssetInfoPtr) override {}
		void OnUpdateAssetInfo(AssetInfoPtr info, bool bExpired) override
		{
			if (!bExpired || info->GetFileId() != m_fileId)
			{
				return;
			}
			++m_notifications;
			m_bWaitedForPrevious = m_previousFinished.load();
			auto worker = Tasks::CreateTask("Reload test: worker"_h, []() {});
			auto render = worker->Then([]() {}, "Reload test: render"_h, EThreadType::Render);
			auto rhi = render->Then([]() {}, "Reload test: RHI"_h, EThreadType::RHI);
			m_completion = rhi->Then([this]()
				{
					m_afterEntered = true;
					m_releaseAfter.wait();
					m_completed = true;
				}, "Reload test: final worker"_h, EThreadType::Worker);
			worker->Run();
		}

		template<typename TUpdate>
		void Update(TUpdate&& update)
		{
			std::latch entered(1), release(1);
			auto previous = Tasks::CreateTask("Reload test: previous render reader"_h, [&]()
				{
					entered.count_down();
					release.wait();
					m_previousFinished = true;
				}, EThreadType::Render);
			previous->Run();
			entered.wait();
			std::jthread unblock([&]()
				{
					std::this_thread::sleep_for(std::chrono::milliseconds(50));
					release.count_down();
					const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
					while (!m_afterEntered.load() && std::chrono::steady_clock::now() < deadline)
					{
						std::this_thread::yield();
					}
					std::this_thread::sleep_for(std::chrono::milliseconds(50));
					m_releaseAfter.count_down();
				});
			const bool bUpdated = update();
			const bool bCompletedOnReturn = m_completed.load();
			unblock.join();
			App::GetSubmodule<Tasks::Scheduler>()->WaitIdle(
				{ EThreadType::Worker, EThreadType::Render, EThreadType::RHI });
			Require(bUpdated && m_notifications == 1, "asset update must notify the changed asset once");
			Require(m_bWaitedForPrevious && bCompletedOnReturn,
				"asset update must fence previous readers and finish its cross-queue publication before returning");
		}

		uint32_t m_notifications = 0;

	private:
		IAssetInfoHandler& m_handler;
		FileId m_fileId;
		Tasks::ITaskPtr m_completion;
		std::atomic<bool> m_previousFinished{ false }, m_afterEntered{ false }, m_completed{ false };
		std::latch m_releaseAfter{ 1 };
		bool m_bWaitedForPrevious = false;
	};

	void WriteColorTexture(const std::filesystem::path& path, uint8_t red, uint8_t blue)
	{
		std::array<uint8_t, 21> bytes{};
		bytes[2] = 2;
		bytes[12] = bytes[14] = 1;
		bytes[16] = 24;
		bytes[18] = blue;
		bytes[20] = red;
		std::ofstream output(path, std::ios::binary);
		output.write(reinterpret_cast<const char*>(bytes.data()), bytes.size());
		Require(static_cast<bool>(output), "GI texture fixture must be written");
	}

	TexturePtr LoadColorTexture(const std::filesystem::path& path, uint8_t red, uint8_t blue)
	{
		WriteColorTexture(path, red, blue);
		TextureAssetInfo info;
		auto metadata = info.Serialize();
		const auto id = FileId::CreateNewFileId();
		metadata["fileId"] = id;
		metadata["filename"] = path.filename().string();
		metadata["bShouldKeepCpuBuffers"] = true;
		metadata["bShouldGenerateMips"] = false;
		{
			std::ofstream output(path.string() + ".asset");
			output << metadata;
			Require(static_cast<bool>(output), "GI texture metadata must be written");
		}
		Require(App::GetSubmodule<AssetRegistry>()->GetOrLoadFile(path.string()) == id,
			"GI texture fixture must register");
		TexturePtr texture;
		Require(App::GetSubmodule<TextureImporter>()->LoadTexture_Immediate(id, texture) && texture->HasCpuData(),
			"GI texture fixture must load its real CPU/GPU resources");
		return texture;
	}

	void TestTargetedAssetCapture(const std::filesystem::path& workspace)
	{
		GIWorld world;
		world.WaitReady();
		auto* registry = App::GetSubmodule<AssetRegistry>();
		auto* textures = App::GetSubmodule<TextureImporter>();
		const auto material = world.m_material;
		auto* materialInfo = registry->GetAssetInfoPtr(material->GetFileId());
		const auto materialPath = std::filesystem::path(materialInfo->GetAssetFilepath());
		Require(std::filesystem::canonical(materialPath).generic_string().starts_with(
			std::filesystem::canonical(workspace / "Content").generic_string() + "/"),
			"reload fixture may only change its temporary workspace material");
		const auto oldScene = GlobalIlluminationECSTestAccess::CapturedScene(world.GI());
		const auto oldEmission = oldScene->m_materials[0]->m_parameters.m_emissiveFactor;

		const auto imagePath = workspace / "Content/Reload.tga";
		const auto texture = LoadColorTexture(imagePath, 255, 0);
		const auto textureId = texture->GetFileId();
		const auto textureSlot = textures->GetTextureIndex(textureId);

		auto document = YAML::LoadFile(materialPath.string());
		auto vectors = document["uniformsVec4"].as<TMap<std::string, glm::vec4>>();
		auto scalars = document["uniformsFloat"].as<TMap<std::string, float>>();
		auto samplers = document["samplers"].as<TMap<std::string, FileId>>();
		vectors["material.emissiveFactor"] = glm::vec4(7, 3, 1, 0);
		scalars["material.alphaCutoff"] = 0.375f;
		samplers["baseColorSampler"] = textureId;
		document["uniformsVec4"] = vectors;
		document["uniformsFloat"] = scalars;
		document["samplers"] = samplers;
		{
			std::ofstream output(materialPath);
			output << document;
		}
		{
			ReloadTaskProbe publication(*materialInfo);
			publication.Update([&]() { return App::UpdateAsset(material->GetFileId().ToString().c_str()); });
			const auto unchangedRevision = material->GetContentRevision();
			Require(App::UpdateAsset(material->GetFileId().ToString().c_str()) &&
				publication.m_notifications == 1 && material->GetContentRevision() == unchangedRevision,
				"an unchanged targeted update must not reload or revise the material");
		}
		Require(App::GetSubmodule<MaterialImporter>()->GetLoadedMaterial(material->GetFileId()) == material,
			"targeted reload must preserve material identity");
		const auto captured = Raytracing::PathTracer::CaptureMaterials({ material });
		const auto findTexture = [](const Raytracing::PathTracer::MaterialSnapshots& snapshots)
		{
			for (const auto& sampler : snapshots[0]->m_samplers)
			{
				if (sampler.m_first == "baseColorSampler"_h) return sampler.m_second.m_texture;
			}
			return TSharedPtr<const Raytracing::PathTracer::TextureSnapshot>{};
		};
		const auto redTexture = findTexture(captured);
		Require(captured[0]->m_parameters.m_emissiveFactor == glm::vec3(7, 3, 1) &&
			captured[0]->m_parameters.m_alphaCutoff == 0.375f && redTexture && redTexture->m_data && redTexture->m_data->Num() == 4 &&
			(*redTexture->m_data)[0] == 255 && (*redTexture->m_data)[2] == 0,
			"GI must capture the completed emission, alpha and sampler update together");
		Require(oldScene->m_materials[0]->m_parameters.m_emissiveFactor == oldEmission,
			"in-flight GI must retain its old material values across reload");

		WriteColorTexture(imagePath, 0, 255);
		{
			ReloadTaskProbe publication(*registry->GetAssetInfoPtr(textureId));
			publication.Update([&]() { return App::UpdateAsset(textureId.ToString().c_str()); });
		}
		const auto updated = Raytracing::PathTracer::CaptureMaterials({ material });
		const auto blueTexture = findTexture(updated);
		Require(textures->GetLoadedTexture(textureId) == texture && textures->GetTextureIndex(textureId) == textureSlot &&
			blueTexture && blueTexture->m_data && (*blueTexture->m_data)[0] == 0 && (*blueTexture->m_data)[2] == 255 &&
			(*redTexture->m_data)[0] == 255 && (*redTexture->m_data)[2] == 0 &&
			updated[0]->m_contentRevision > captured[0]->m_contentRevision,
			"texture reload must finish dependent materials, preserve object/slot identity and retain old CPU pixels");
		Require(!App::UpdateAsset(nullptr) && !App::UpdateAsset("") &&
			!App::UpdateAsset(FileId::CreateNewFileId().ToString().c_str()),
			"invalid targeted updates must remain rejected");

		vectors["material.emissiveFactor"] = glm::vec4(9, 5, 2, 0);
		scalars["material.alphaCutoff"] = 0.25f;
		document["uniformsVec4"] = vectors;
		document["uniformsFloat"] = scalars;
		{
			std::ofstream output(materialPath);
			output << document;
		}
		{
			ReloadTaskProbe publication(*materialInfo);
			publication.Update([&]() { return registry->GetOrLoadFile(materialPath.string()) == material->GetFileId(); });
		}
		const auto direct = Raytracing::PathTracer::CaptureMaterials({ material });
		Require(direct[0]->m_parameters.m_emissiveFactor == glm::vec3(9, 5, 2) &&
			direct[0]->m_parameters.m_alphaCutoff == 0.25f &&
			updated[0]->m_parameters.m_emissiveFactor == glm::vec3(7, 3, 1),
			"Main LoadFile must finish material publication and preserve earlier GI snapshots");
		WriteColorTexture(imagePath, 127, 31);
		{
			ReloadTaskProbe publication(*registry->GetAssetInfoPtr(textureId));
			publication.Update([&]() { return registry->GetOrLoadFile(imagePath.string()) == textureId; });
		}
		const auto directTexture = findTexture(Raytracing::PathTracer::CaptureMaterials({ material }));
		Require(directTexture && directTexture->m_data && (*directTexture->m_data)[0] == 127 &&
			(*directTexture->m_data)[2] == 31 && (*blueTexture->m_data)[2] == 255,
			"Main LoadFile must finish texture and dependent material publication without changing retained pixels");
		std::cout << "Direct LoadFile: Main reader fences, material/texture publication and retained GI snapshots passed\n";
	}

	void TestMaterialPreparationStress(const std::filesystem::path& workspace)
	{
		auto* scheduler = App::GetSubmodule<Tasks::Scheduler>();
		Require(scheduler->IsMainThread(), "live material edits must run on the world owner");
		const std::array<TexturePtr, 2> textures{
			LoadColorTexture(workspace / "Content/StressRed.tga", 255, 0),
			LoadColorTexture(workspace / "Content/StressBlue.tga", 0, 255) };
		auto world = TUniquePtr<GIWorld>::Make();
		world->WaitReady();
		world->GI().SetRuntimeGIProbesWorkAllowed(false);
		auto material = world->m_material;
		const auto path = App::GetSubmodule<AssetRegistry>()->GetAssetInfoPtr(material->GetFileId())->GetAssetFilepath();
		Require(std::filesystem::canonical(path).generic_string().starts_with(
			std::filesystem::canonical(workspace / "Content").generic_string() + "/"),
			"stress reload may only modify its temporary workspace");
		auto document = YAML::LoadFile(path);
		GIProbesSceneCaptureRequest request;
		request.m_settings.m_bIncludeSky = false;
		request.m_settings.m_bIncludeDirectLighting = false;
		request.m_settings.m_bounceCount = 1;
		request.m_fallbackEnvironment = glm::vec3(0);
		TVector<GIProbesSceneSnapshotPtr> snapshots;
		TVector<Tasks::TaskPtr<std::string>> preparations;

		auto checkSnapshot = [&](const GIProbesSceneSnapshot& scene, uint32_t step)
		{
			const float value = static_cast<float>(step) / 16.0f;
			Require(scene.m_materials.Num() == 2 && scene.m_materials[0] && scene.m_materials[0] == scene.m_materials[1],
				"both stress instances must retain the same captured material");
			const auto& captured = *scene.m_materials[0];
			Require(captured.m_parameters.m_emissiveFactor == glm::vec3(value, value * 2, value * 4) &&
				captured.m_parameters.m_alphaCutoff == (step % 2 ? 0.25f : 0.75f),
				"each capture must retain a completed emission/alpha state");
			bool found = false;
			for (const auto& sampler : captured.m_samplers)
			{
				if (sampler.m_first != "emissiveSampler"_h) continue;
				const auto texture = sampler.m_second.m_texture;
				found = texture && texture->m_fileId == textures[step % 2]->GetFileId() &&
					texture->m_data && texture->m_data->Num() == 4 &&
					(*texture->m_data)[step % 2 ? 2 : 0] == 255;
			}
			Require(found, "the retained sampler and pixels must belong to the same material state");
		};
		auto prepare = [&](GIProbesSceneSnapshotPtr snapshot, uint32_t step,
			Raytracing::PathTracer::ScenePreparationProgressCallback progress = {})
		{
			auto task = Tasks::CreateTask<std::string>("Stress immutable GI preparation"_h,
				[snapshot, step, settings = request.m_settings, progress]()
				{
					try
					{
						Require(App::GetSubmodule<Tasks::Scheduler>()->GetCurrentThreadType() == EThreadType::Background,
							"stress must use the real Background queue");
						GIProbesPreparedScene prepared;
						std::string diagnostic;
						Require(PrepareGIProbesScene(*snapshot, settings, nullptr, prepared, diagnostic, progress), diagnostic);
						GIProbeBakeRaySample sample;
						Require(prepared.m_sampler->Sample(glm::vec3(0), glm::vec3(0, 0, -1), 20, step, sample, diagnostic), diagnostic);
						const float value = static_cast<float>(step) / 16.0f;
						const auto expected = step % 2 ? glm::vec3(0, 0, value * 4) : glm::vec3(value, 0, 0);
						Require(sample.m_bHit && std::abs(sample.m_distance - 2) < 1e-4f &&
							glm::length(sample.m_radiance - expected) < 1e-4f,
							"Background rays must use the captured emission and sampler, not later live values");
						return std::string{};
					}
					catch (const std::exception& error) { return std::string(error.what()); }
				}, EThreadType::Background);
			task->Run();
			return task;
		};

		constexpr uint32_t iterations = 256;
		for (uint32_t step = 1; step <= iterations; ++step)
		{
			const float value = static_cast<float>(step) / 16.0f;
			const glm::vec4 emission(value, value * 2, value * 4, 0);
			const float cutoff = step % 2 ? 0.25f : 0.75f;
			const auto texture = textures[step % 2];
			if (step % 13 == 0)
			{
				auto vectors = document["uniformsVec4"].as<TMap<std::string, glm::vec4>>();
				auto scalars = document["uniformsFloat"].as<TMap<std::string, float>>();
				auto samplers = document["samplers"].as<TMap<std::string, FileId>>();
				vectors["material.emissiveFactor"] = emission;
				scalars["material.alphaCutoff"] = cutoff;
				samplers["emissiveSampler"] = texture->GetFileId();
				document["uniformsVec4"] = vectors;
				document["uniformsFloat"] = scalars;
				document["samplers"] = samplers;
				{
					std::ofstream output(path);
					output << document;
					Require(static_cast<bool>(output), "stress material update must be written");
				}
				Require(App::UpdateAsset(material->GetFileId().ToString().c_str()), "stress material reload must complete");
				WaitMaterialReady(material);
			}
			else
			{
				material->SetUniform("material.emissiveFactor"_h, emission);
				material->SetUniform("material.alphaCutoff"_h, cutoff);
				material->SetSampler("emissiveSampler"_h, texture);
			}
			auto snapshot = GIProbesSceneSnapshotPtr::Make();
			std::string diagnostic;
			Require(CaptureGIProbesScene(world.GetRawPtr(), request, *snapshot, diagnostic), diagnostic);
			checkSnapshot(*snapshot, step);
			snapshots.Add(snapshot);
			preparations.Add(prepare(snapshot, step));
		}
		for (auto& task : preparations)
		{
			task->Wait();
			Require(task->GetResult().empty(), task->GetResult());
		}
		for (uint32_t step = 1; step <= iterations; ++step) checkSnapshot(*snapshots[step - 1], step);

		std::atomic<bool> entered{ false }, release{ false };
		auto afterClose = prepare(*snapshots.Last(), iterations, [&](const auto&)
			{
				entered = true;
				const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
				while (!release && std::chrono::steady_clock::now() < deadline) std::this_thread::yield();
				return release.load();
			});
		const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
		while (!entered && !afterClose->IsFinished() && std::chrono::steady_clock::now() < deadline) std::this_thread::yield();
		const bool preparingAtClose = entered && !afterClose->IsFinished();
		world.Clear();
		material.Clear();
		release = true;
		afterClose->Wait();
		Require(preparingAtClose && afterClose->GetResult().empty(),
			"retained GI preparation must finish after the captured world is destroyed: " + afterClose->GetResult());
		for (uint32_t step = 1; step <= iterations; ++step) checkSnapshot(*snapshots[step - 1], step);
		std::cout << "GI material stress: 256 owner updates, 19 reloads, coherent Background rays and world-close retention passed\n";
	}

	void TestBackgroundPreparationCancellation()
	{
		GIWorld world;
		world.WaitReady();
		GIProbesSceneCaptureRequest request;
		request.m_settings.m_bIncludeSky = false;
		request.m_settings.m_bounceCount = 1u;
		auto captured = GIProbesSceneSnapshotPtr::Make();
		std::string diagnostic;
		Require(CaptureGIProbesScene(&world, request, *captured, diagnostic), diagnostic);
		GIProbesPreparedScene retained;
		Require(PrepareGIProbesScene(*captured, request.m_settings, nullptr, retained, diagnostic), diagnostic);
		GIProbeBakeRaySample original;
		Require(retained.m_sampler->Sample({ 0, 0, 0 }, { 0, 0, -1 }, 20.0f, 41u, original, diagnostic) && original.m_bHit,
			"the retained Background cancellation reader must hit its captured quad");

		auto pending = GIProbesSceneSnapshotPtr::Make(*captured);
		const auto instance = pending->m_instances[0];
		pending->m_instances.Clear();
		for (uint32_t index = 0u; index < 4096u; ++index) pending->m_instances.Add(instance);
		TWeakPtr<GIProbesSceneSnapshot> pendingInput(pending);
		std::atomic<bool> entered{ false }, cancel{ false };
		std::latch resume(1);
		auto task = Tasks::CreateTask<std::string>("Cancel captured GI preparation"_h,
			[pending, settings = request.m_settings, &entered, &cancel, &resume]()
			{
				try
				{
					Require(App::GetSubmodule<Tasks::Scheduler>()->GetCurrentThreadType() == EThreadType::Background,
						"cancelled preparation must execute on the real Background queue");
					GIProbesPreparedScene prepared;
					std::string error;
					uint32_t reports = 0u;
					const bool completed = PrepareGIProbesScene(*pending, settings, &cancel, prepared, error,
						[&](const Raytracing::PathTracer::ScenePreparationProgress& progress)
						{
							if (progress.m_stage == Raytracing::PathTracer::EScenePreparationStage::Geometry &&
								progress.m_completed == 0u && ++reports == 9u)
							{
								entered.store(true, std::memory_order_release);
								resume.wait();
							}
							return true;
						});
					Require(!completed && !prepared.m_sampler && error.find("cancelled") != std::string::npos,
						"a cancelled Background task must not return a partially prepared scene");
					return std::string{};
				}
				catch (const std::exception& error) { return std::string(error.what()); }
			}, EThreadType::Background);
		task->Run();
		pending.Clear();
		const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
		while (!entered.load(std::memory_order_acquire) && !task->IsFinished() && std::chrono::steady_clock::now() < deadline)
			std::this_thread::yield();
		const bool preparing = entered.load(std::memory_order_acquire) && !task->IsFinished();
		GIProbeBakeRaySample concurrent;
		const bool retainedReadable = retained.m_sampler->Sample({ 0, 0, 0 }, { 0, 0, -1 }, 20.0f, 41u, concurrent, diagnostic);
		cancel.store(true, std::memory_order_release);
		resume.count_down();
		task->Wait();
		Require(preparing && task->GetResult().empty(), "Background preparation cancellation failed: " + task->GetResult());
		Require(retainedReadable && concurrent.m_bHit && concurrent.m_distance == original.m_distance &&
			concurrent.m_radiance == original.m_radiance, "retained rays must remain usable during cancelled preparation");
		task.Clear();
		App::GetSubmodule<Tasks::Scheduler>()->WaitIdle(EThreadType::Background);
		Require(!pendingInput.TryLock(), "the completed cancelled task must release its captured scene");

		auto next = Tasks::CreateTask<std::string>("Bake after cancelled GI preparation"_h,
			[captured, settings = request.m_settings, original]()
			{
				try
				{
					Require(App::GetSubmodule<Tasks::Scheduler>()->GetCurrentThreadType() == EThreadType::Background,
						"the replacement solve must execute on Background");
					GIProbesPreparedScene prepared;
					std::string error;
					Require(PrepareGIProbesScene(*captured, settings, nullptr, prepared, error), error);
					GIProbeBakeRaySample ray;
					Require(prepared.m_sampler->Sample({ 0, 0, 0 }, { 0, 0, -1 }, 20.0f, 41u, ray, error) &&
						ray.m_bHit && ray.m_distance == original.m_distance && ray.m_radiance == original.m_radiance,
						"the replacement preparation must preserve captured ray results");
					GIProbesBakeRequest bake;
					bake.m_stateName = "After cancellation";
					bake.m_volumeMin = glm::vec3(-1.0f);
					bake.m_volumeMax = glm::vec3(1.0f);
					bake.m_settings = settings;
					bake.m_settings.m_minProbeSpacing = 8.0f;
					bake.m_settings.m_maxSubdivisionLevel = 0u;
					bake.m_settings.m_raysPerProbe = 8u;
					const auto result = GIProbesBaker::Bake(bake, *prepared.m_sampler);
					Require(result.IsSuccess() && result.m_data->Validate(error), result.m_diagnostic + error);
					return std::string{};
				}
				catch (const std::exception& error) { return std::string(error.what()); }
			}, EThreadType::Background);
		next->Run();
		next->Wait();
		Require(next->GetResult().empty(), next->GetResult());
		std::cout << "Background GI cancellation: retained rays, released snapshot and replacement bake passed\n";
	}

	void TestContributorMaterialRevision()
	{
		GIWorld world;
		std::string diagnostic;
		Require(world.GI().SetRuntimeGIProbesPreviewEnabled(false, diagnostic), diagnostic);
		auto* meshes = world.GetECS<StaticMeshRendererECS>();
		float emission = 1.0f;
		for (const auto mobility : { EMobilityType::Static, EMobilityType::Stationary, EMobilityType::Dynamic })
		{
			for (auto object : world.GetGameObjects())
			{
				if (object->GetComponent<MeshRendererComponent>())
				{
					object->SetMobilityType(mobility);
				}
			}
			world.Step();
			const auto revision = meshes->GetGlobalIlluminationContributorRevision();
			const auto scene = meshes->GetRHIScene()->GetCurrentVersion();
			world.m_material->SetUniform("material.emissiveFactor"_h, glm::vec4(emission++, 0, 0, 0));
			world.Step();
			Require((meshes->GetGlobalIlluminationContributorRevision() != revision) ==
				(mobility != EMobilityType::Dynamic),
				"only Static and Stationary uniform edits must invalidate GI");
			Require(meshes->GetRHIScene()->GetCurrentVersion() == scene,
				"uniform edits must preserve the published RHI scene for every mobility");
			const auto updatedRevision = meshes->GetGlobalIlluminationContributorRevision();
			world.Step();
			Require(meshes->GetGlobalIlluminationContributorRevision() == updatedRevision,
				"an unchanged material must not invalidate GI again");
		}
	}

	enum class PreparationRecovery { MaterialChange, Restart, Rebuild };

	void TestPreparationRecovery(PreparationRecovery recovery)
	{
		GIWorld world;
		world.Step();
		auto& gi = world.GI();
		Require(GlobalIlluminationECSTestAccess::FailCurrentPreparation(gi),
			"the real initial preparation must complete before injecting its failed outcome");
		world.Step();
		Require(gi.GetRuntimeGIProbesStatus().m_lifecycle == ERuntimeGIProbesLifecycle::Failed && !gi.GetActiveSnapshot(),
			"a failed first preparation must expose failure without publishing a scene");
		const auto attempts = GlobalIlluminationECSTestAccess::PreparationCount(gi);
		for (int i = 0; i < 20; ++i)
		{
			world.Step(0.5f);
		}
		Require(GlobalIlluminationECSTestAccess::PreparationCount(gi) == attempts,
			"unchanged failed inputs must not trigger repeated expensive preparation");
		if (recovery == PreparationRecovery::Restart)
		{
			std::string diagnostic;
			Require(gi.RestartRuntimeGIProbes(diagnostic), diagnostic);
		}
		else if (recovery == PreparationRecovery::Rebuild)
		{
			std::string diagnostic;
			Require(gi.RebuildRuntimeGIProbesScene(diagnostic), diagnostic);
		}
		else
		{
			const auto before = world.GetECS<StaticMeshRendererECS>()->GetGlobalIlluminationContributorRevision();
			const auto scene = world.GetECS<StaticMeshRendererECS>()->GetRHIScene()->GetCurrentVersion();
			world.m_material->SetUniform("material.emissiveFactor"_h, glm::vec4(0.5f, 0.25f, 0.125f, 0));
			world.Step();
			Require(world.GetECS<StaticMeshRendererECS>()->GetGlobalIlluminationContributorRevision() != before,
				"the material edit must reach the real scene revision publisher");
			Require(world.GetECS<StaticMeshRendererECS>()->GetRHIScene()->GetCurrentVersion() == scene,
				"uniform-only GI invalidation must not republish the RHI scene");
		}
		world.WaitReady();
		Require(GlobalIlluminationECSTestAccess::PreparationCount(gi) == attempts + 1,
			"changed inputs or explicit rebuild must retry the failed first preparation once");
		if (recovery == PreparationRecovery::Restart)
		{
			Require(Tests::CountRuntimeGICompletedSamples(GlobalIlluminationECSTestAccess::RuntimeProbes(gi)) > 0,
				"restart after a failed preparation must execute real sample work before reporting success");
			std::cout << "Runtime GI failed preparation: explicit restart retries and publishes passed\n";
		}
	}

	void TestStalePreparationRecovery()
	{
		GIWorld world;
		world.Step();
		auto& gi = world.GI();
		Require(GlobalIlluminationECSTestAccess::FailCurrentPreparation(gi),
			"the initial preparation must complete before injecting its failed outcome");
		const auto attempts = GlobalIlluminationECSTestAccess::PreparationCount(gi);
		world.m_material->SetUniform("material.emissiveFactor"_h, glm::vec4(0.75f, 0.5f, 0.25f, 0));
		world.Step();
		Require(gi.GetRuntimeGIProbesStatus().m_lifecycle != ERuntimeGIProbesLifecycle::Failed,
			"a failed result with changed inputs must request retry instead of a permanent failure");
		world.WaitReady();
		Require(GlobalIlluminationECSTestAccess::PreparationCount(gi) == attempts + 1,
			"a stale first preparation must recover once the scene becomes stable");
	}

	void TestLightingPreparation()
	{
		GIWorld world;
		auto checkDistance = [](const GIProbesPreparedScenePtr& scene, float expected)
		{
			GIProbeBakeRaySample sample;
			std::string diagnostic;
			Require(scene->m_sampler->SampleVisibility(glm::vec3(0), glm::vec3(0, 0, -1), 20, 1, sample, diagnostic), diagnostic);
			Require(sample.m_bHit && std::abs(sample.m_distance - expected) < 1e-4f,
				"each prepared transport must trace its own static transforms");
		};
		auto sky = world.Instantiate("Sky")->AddComponent<SkyComponent>();
		auto settings = world.GI().GetWorldSettings();
		settings.m_runtimeProbes.m_bIncludeSky = true;
		std::string diagnostic;
		Require(world.GI().ApplyWorldSettings(settings, diagnostic), diagnostic);
		world.Step();
		const auto prepared = GlobalIlluminationECSTestAccess::WaitPreparation(world.GI());
		Require(prepared && prepared->m_sampler, "the real background task must prepare the initial sky scene");
		sky->SetSunAngle(25.0f);
		world.Step();
		Require(GlobalIlluminationECSTestAccess::PreparedScene(world.GI()) == prepared,
			"a newer light state must not discard usable prepared geometry or starve the first solve");
		world.WaitReady();
		const auto captured = GlobalIlluminationECSTestAccess::CapturedScene(world.GI());
		const auto firstPublication = world.GI().GetActiveSnapshot();
		Require(prepared->m_sampler->GetLastScenePreparationStats().m_builtBlasCount == 0 &&
			prepared->m_sampler->GetLastScenePreparationStats().m_reusedBlasCount == 2 &&
			captured->m_instances[0].m_modelGeometry &&
			captured->m_instances[0].m_modelGeometry == captured->m_instances[1].m_modelGeometry,
			"the initial capture must share immutable model geometry and reuse its BLAS for both objects");
		checkDistance(prepared, 2);
		world.GI().SetRuntimeGIProbesWorkAllowed(false);
		world.Step(0.6f);
		world.Step();
		const auto relit = GlobalIlluminationECSTestAccess::WaitPreparation(world.GI());
		const auto recaptured = GlobalIlluminationECSTestAccess::CapturedScene(world.GI());
		Require(relit && relit != prepared && relit->m_geometryHash == prepared->m_geometryHash &&
			relit->m_lightingHash != prepared->m_lightingHash &&
			relit->m_sampler->GetLastScenePreparationStats().m_builtBlasCount == 0 &&
			relit->m_sampler->GetLastScenePreparationStats().m_decodedTextureCount == 0,
			"sun-only refresh must retain prepared geometry and skip BLAS and texture decoding");
		Require(recaptured->m_instances[0].m_modelGeometry == captured->m_instances[0].m_modelGeometry &&
			recaptured->m_materials[0] == captured->m_materials[0],
			"light-only owner capture must retain frozen model geometry and material snapshots");
		sky->SetSunAngle(35.0f);
		world.Step();
		Require(GlobalIlluminationECSTestAccess::PreparedScene(world.GI()) == relit,
			"continued sun movement must not discard the light-only result either");
		const auto attempts = GlobalIlluminationECSTestAccess::PreparationCount(world.GI());
		for (uint32_t frame = 0; frame < 8; ++frame)
		{
			sky->SetGiIndirectIntensity(1.0f + 0.1f * frame);
			world.Step(0.6f);
		}
		Require(GlobalIlluminationECSTestAccess::PreparationCount(world.GI()) == attempts &&
			world.GI().GetActiveSnapshot() == firstPublication,
			"moving light must coalesce while a throttled replacement has not published");
		world.GI().SetRuntimeGIProbesWorkAllowed(true);
		const auto beforeRefresh = world.GI().GetRuntimeGIProbesStatus().m_publishedRevision;
		world.WaitReady(beforeRefresh);
		world.Step(0.6f);
		world.Step();
		const auto caughtUp = GlobalIlluminationECSTestAccess::WaitPreparation(world.GI());
		Require(caughtUp && caughtUp->m_lightingHash != relit->m_lightingHash &&
			caughtUp->m_sampler->GetLastScenePreparationStats().m_builtBlasCount == 0,
			"after publishing, the next light-only preparation must catch up to the latest sky");
		world.Step();
		world.WaitReady();

		for (auto object : world.GetGameObjects())
		{
			if (object->GetComponent<MeshRendererComponent>())
			{
				object->GetTransformComponent().SetPosition(glm::vec3(0, 0, -6));
				break;
			}
		}
		world.Step(0.6f);
		world.Step();
		const auto moved = GlobalIlluminationECSTestAccess::WaitPreparation(world.GI());
		Require(moved && moved->m_geometryHash != caughtUp->m_geometryHash &&
			moved->m_sampler->GetLastScenePreparationStats().m_builtBlasCount == 0 &&
			moved->m_sampler->GetLastScenePreparationStats().m_reusedBlasCount == 2,
			"a Static transform change must rebuild transport while reusing immutable model BLAS");
		checkDistance(moved, 4);
		checkDistance(prepared, 2);
		world.Step();
		world.WaitReady();
		world.m_material->SetUniform("material.baseColorFactor"_h, glm::vec4(1, 1, 1, 0.5f));
		world.Step(0.6f);
		world.Step();
		const auto alpha = GlobalIlluminationECSTestAccess::WaitPreparation(world.GI());
		Require(alpha && alpha->m_geometryHash != moved->m_geometryHash &&
			alpha->m_sampler->GetLastScenePreparationStats().m_builtBlasCount == 0 &&
			alpha->m_sampler->GetLastScenePreparationStats().m_reusedBlasCount == 2,
			"material alpha edits must not use the light-only transport shortcut");
		const auto alphaCapture = GlobalIlluminationECSTestAccess::CapturedScene(world.GI());
		Require(alphaCapture->m_materials[0] != captured->m_materials[0] &&
			alphaCapture->m_materials[0]->m_parameters.m_baseColorFactor.a == 0.5f,
			"transport rebuilt for alpha must contain the new material snapshot");
		world.Step();
		world.WaitReady();
		Require(world.GI().RebuildRuntimeGIProbesScene(diagnostic), diagnostic);
		world.Step();
		const auto explicitRebuild = GlobalIlluminationECSTestAccess::WaitPreparation(world.GI());
		Require(explicitRebuild && explicitRebuild != alpha && explicitRebuild->m_sampler != alpha->m_sampler &&
			GlobalIlluminationECSTestAccess::CapturedScene(world.GI()) != alphaCapture &&
			explicitRebuild->m_geometryHash == alpha->m_geometryHash &&
			explicitRebuild->m_sampler->GetLastScenePreparationStats().m_builtBlasCount == 0 &&
			explicitRebuild->m_sampler->GetLastScenePreparationStats().m_reusedBlasCount == 2,
			"explicit rebuild must recapture and prepare transport without duplicating immutable model BLAS");
		checkDistance(explicitRebuild, 4);
	}

	void TestEmissionPreparation()
	{
		GIWorld world;
		world.m_material->SetUniform("material.emissiveFactor"_h, glm::vec4(0));
		world.WaitReady();
		const auto original = GlobalIlluminationECSTestAccess::PreparedScene(world.GI());
		const auto captured = GlobalIlluminationECSTestAccess::CapturedScene(world.GI());
		world.m_material->SetUniform("material.emissiveFactor"_h, glm::vec4(2, 4, 8, 0));
		world.Step(0.6f);
		world.Step();
		const auto updated = GlobalIlluminationECSTestAccess::WaitPreparation(world.GI());
		const auto recaptured = GlobalIlluminationECSTestAccess::CapturedScene(world.GI());
		Require(updated && updated->m_geometryHash == original->m_geometryHash &&
			updated->m_lightingHash != original->m_lightingHash &&
			updated->m_sampler->GetLastScenePreparationStats().m_builtBlasCount == 0 &&
			updated->m_sampler->GetLastScenePreparationStats().m_decodedTextureCount == 0,
			"emission-only updates must change GI lighting without rebuilding geometry or decoding textures");
		Require(captured->m_instances[0].m_modelGeometry &&
			recaptured->m_instances[0].m_modelGeometry == captured->m_instances[0].m_modelGeometry &&
			recaptured->m_materials[0] != captured->m_materials[0] &&
			recaptured->m_materials[0]->m_parameters.m_emissiveFactor == glm::vec3(2, 4, 8) &&
			captured->m_materials[0]->m_parameters.m_emissiveFactor == glm::vec3(0),
			"emission capture must retain geometry without mutating previous material values");
		world.Step();
		world.WaitReady();
	}

	void TestEmissionDuringPreparation()
	{
		GIWorld world;
		world.m_material->SetUniform("material.emissiveFactor"_h, glm::vec4(0));
		world.Step();
		const auto prepared = GlobalIlluminationECSTestAccess::WaitPreparation(world.GI());
		Require(prepared && prepared->m_sampler, "prepare the real initial scene before changing emission");
		world.m_material->SetUniform("material.emissiveFactor"_h, glm::vec4(3, 2, 1, 0));
		world.Step();
		Require(GlobalIlluminationECSTestAccess::PreparedScene(world.GI()) == prepared,
			"emission changes must not discard usable geometry before its first publication");
		world.WaitReady();
		world.Step(0.6f);
		world.Step();
		const auto latest = GlobalIlluminationECSTestAccess::WaitPreparation(world.GI());
		Require(latest && latest->m_lightingHash != prepared->m_lightingHash &&
			latest->m_sampler->GetLastScenePreparationStats().m_builtBlasCount == 0,
			"after first publication the next emission generation must catch up without rebuilding BLAS");
	}

	void TestSelectedSkyLightingSource()
	{
		GIWorld world;
		auto other = world.Instantiate("Other sky", InstanceId("00000000000000000003"))->AddComponent<SkyComponent>();
		auto owner = world.Instantiate("Selected sky", InstanceId("00000000000000000002"));
		auto selected = owner->AddComponent<SkyComponent>();
		selected->SetSunAngle(20);
		selected->SetGiIndirectIntensity(2);
		other->SetSunAngle(70);
		world.Step();
		GIProbesSceneCaptureRequest request;
		GIProbesSceneSnapshot captured;
		GIProbesSceneRevision before, after;
		std::string diagnostic;
		Require(CaptureGIProbesScene(&world, request, captured, diagnostic), diagnostic);
		Require(world.GetECS<LightingECS>()->GetSky() == selected &&
			captured.m_environment.m_type == EEnvironmentSource::Sky &&
			captured.m_environment.m_sky == selected->GetSkyParameters() &&
			captured.m_environment.m_skyIndirectIntensity == 2,
			"GI capture and rendering must select the same sky independently of component insertion order");
		Require(ObserveGIProbesSceneRevision(&world, request, before, diagnostic), diagnostic);
		other->SetSunAngle(5);
		other->SetGiIndirectIntensity(7);
		Require(ObserveGIProbesSceneRevision(&world, request, after, diagnostic), diagnostic);
		Require(before == after, "nonselected sky changes must not invalidate GI lighting");
		Require(owner->RemoveComponent(selected), "the selected sky must be removable");
		Require(CaptureGIProbesScene(&world, request, captured, diagnostic), diagnostic);
		Require(captured.m_environment.m_sky == other->GetSkyParameters() &&
			captured.m_environment.m_skyIndirectIntensity == 7,
			"GI capture must follow the surviving render sky after selected-owner removal");
		Require(ObserveGIProbesSceneRevision(&world, request, after, diagnostic), diagnostic);
		Require(before.m_geometry == after.m_geometry && before.m_lighting != after.m_lighting,
			"sky ownership changes must invalidate lighting without changing geometry");
		std::cout << "GI sky ownership: matching render selection, nonselected edits and selected-owner removal passed\n";
	}

	void TestCloudsDoNotInvalidateGI()
	{
		GIWorld world;
		auto sky = world.Instantiate("Sky")->AddComponent<SkyComponent>();
		GIProbesSceneCaptureRequest request;
		GIProbesSceneRevision before, after;
		std::string diagnostic;
		Require(ObserveGIProbesSceneRevision(&world, request, before, diagnostic), diagnostic);
		sky->SetCloudsDensity(0.8f);
		sky->SetCloudsCoverage(0.2f);
		sky->SetCloudsHorizonBlend(2.0f);
		sky->SetSunShaftsIntensity(0.9f);
		Require(ObserveGIProbesSceneRevision(&world, request, after, diagnostic), diagnostic);
		Require(before == after, "clouds, fog and shafts absent from CPU clear sky must not invalidate GI");
		sky->SetSunAngle(25.0f);
		Require(ObserveGIProbesSceneRevision(&world, request, after, diagnostic), diagnostic);
		Require(before.m_geometry == after.m_geometry && after.HasChanges(before, 30.0f),
			"a changed sun must invalidate lighting without invalidating geometry");
	}

	void RequireGpuEnvironment(const FileId& id, const GIProbesPreparedScene& scene)
	{
		TexturePtr texture;
		Require(App::GetSubmodule<TextureImporter>()->LoadTexture_Immediate(id, texture), "the GPU environment must load");
		const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
		while (!texture->IsReady() && std::chrono::steady_clock::now() < deadline)
		{
			RHI::Renderer::GetDriver()->TrackResources_ThreadSafe();
			std::this_thread::yield();
		}
		Require(texture->IsReady(), "the GPU environment upload must complete");
		auto task = Tasks::CreateTaskWithResult<std::string>("Environment CPU/GPU parity"_h, [&]() -> std::string
		{
			try
			{
				using namespace RHI;
				auto& driver = Renderer::GetDriver();
				auto commands = Renderer::GetDriverCommands();
				constexpr int32_t side = 32;
				auto cube = driver->CreateCubemap(glm::ivec2(side), 1, EFormat::R16G16B16A16_SFLOAT,
					ETextureFiltration::Linear, ETextureClamping::Clamp,
					ETextureUsageBit::Storage_Bit | ETextureUsageBit::Sampled_Bit | ETextureUsageBit::TextureTransferSrc_Bit);
				auto convert = driver->CreateCommandList(false, ECommandListQueue::Graphics);
				commands->BeginCommandList(convert, true);
				commands->ImageMemoryBarrier(convert, cube, EImageLayout::ComputeWrite);
				commands->ConvertEquirect2Cubemap(convert, texture->GetRHI(), cube);
				commands->ImageMemoryBarrier(convert, cube, EImageLayout::ShaderReadOnlyOptimal);
				cube->ForceSetDefaultLayout(EImageLayout::ShaderReadOnlyOptimal);
				commands->EndCommandList(convert);
				Require(driver->SubmitCommandList_Immediate(convert), "the HDR conversion must complete");
				for (uint32_t face = 0u; face < 6u; ++face)
				{
					auto buffer = driver->CreateBuffer(side * side * 8u, EBufferUsageBit::BufferTransferDst_Bit,
						EMemoryPropertyBit::HostVisible | EMemoryPropertyBit::HostCoherent);
					auto read = driver->CreateCommandList(false, ECommandListQueue::Graphics);
					commands->BeginCommandList(read, true);
					commands->ImageMemoryBarrier(read, cube, EImageLayout::TransferSrcOptimal);
					read->m_vulkan.m_commandBuffer->CopyImageToBuffer(*buffer->m_vulkan.m_buffer->Get(),
						cube->m_vulkan.m_image, side, side, 1, 0, face);
					commands->ImageMemoryBarrier(read, cube, EImageLayout::ShaderReadOnlyOptimal);
					commands->MemoryBarrier(read, static_cast<EAccessFlags>(EAccessBit::TransferWrite_Bit),
						static_cast<EAccessFlags>(EAccessBit::HostRead_Bit));
					commands->EndCommandList(read);
					Require(driver->SubmitCommandList_Immediate(read), "the environment readback must complete");
					const auto pixels = static_cast<const uint32_t*>(buffer->GetPointer());
					for (int32_t y : { 0, side / 2 - 1, side / 2, side - 1 })
						for (int32_t x : { 0, side / 2 - 1, side / 2, side - 1 })
						{
							const float u = 2.0f * (x + 0.5f) / side - 1.0f;
							const float v = 1.0f - 2.0f * (y + 0.5f) / side;
							const std::array<glm::vec3, 6> directions{ glm::vec3(1, v, -u), { -1, v, u },
								{ u, 1, -v }, { u, -1, v }, { u, v, 1 }, { -u, v, -1 } };
							GIProbeBakeRaySample ray;
							std::string diagnostic;
							Require(scene.m_sampler->Sample({ 0, 0, 40 }, glm::normalize(directions[face]),
								20.0f, 1u, ray, diagnostic) && !ray.m_bHit, diagnostic);
							const int32_t index = 2 * (y * side + x);
							const glm::vec3 actual(glm::vec4(glm::unpackHalf2x16(pixels[index]), glm::unpackHalf2x16(pixels[index + 1])));
							Require(glm::length(actual - ray.m_radiance) < 0.01f,
								"CPU/GPU environment direction mismatch at face " + std::to_string(face) +
								", pixel " + std::to_string(x) + "," + std::to_string(y) + ": GPU red " +
								std::to_string(actual.r) + ", CPU red " + std::to_string(ray.m_radiance.r));
						}
				}
				return {};
			}
			catch (const std::exception& error) { return error.what(); }
		}, EThreadType::Render);
		task->Run();
		task->Wait();
		Require(task->GetResult().empty(), task->GetResult());
	}

	void TestAuthoredEnvironmentCapture(const std::filesystem::path& workspace)
	{
		const auto path = workspace / "Content" / "GIEnvironment.hdr";
		const auto rendererPath = workspace / "Content" /
			(App::HasEditor() ? "EditorRenderer.renderer" : "DefaultRenderer.renderer");
		Require(!std::filesystem::exists(rendererPath), "the GI fixture must own its renderer override");
		auto* registry = App::GetSubmodule<AssetRegistry>();
		struct RestoreRenderer
		{
			std::filesystem::path m_path;
			std::string m_text;
			~RestoreRenderer() { std::ofstream(m_path) << m_text; }
		} restoreRenderer{ rendererPath, {} };
		Require(AssetRegistry::ReadAllTextFile(registry->GetAssetInfoPtr(rendererPath.filename().string())->GetAssetFilepath(),
			restoreRenderer.m_text), "the previous renderer configuration must be retained");
		const auto writeEnvironment = [&](bool reversed)
		{
			const auto previous = std::filesystem::exists(path) ? std::filesystem::last_write_time(path) :
				std::filesystem::file_time_type{};
			std::ofstream output(path, std::ios::binary);
			output << "#?RADIANCE\nFORMAT=32-bit_rle_rgbe\n\n-Y 4 +X 4\n";
			for (uint32_t y = 0u; y < 4u; ++y)
			{
				for (uint32_t x = 0u; x < 4u; ++x)
				{
					const uint32_t row = reversed ? 3u - y : y;
					const std::array<uint8_t, 4> rgbe = row == 0u ? std::array<uint8_t, 4>{ 128, 16, 8, 131 } :
						row == 3u ? std::array<uint8_t, 4>{ 16, 128, 32, 130 } :
						(x < 2u) != reversed ? std::array<uint8_t, 4>{ 64, 16, 192, 130 } :
						std::array<uint8_t, 4>{ 32, 192, 8, 130 };
					output.write(reinterpret_cast<const char*>(rgbe.data()), rgbe.size());
				}
			}
			output.close();
			Require(static_cast<bool>(output), "the GI HDR fixture must be written");
			if (std::filesystem::last_write_time(path) <= previous)
				std::filesystem::last_write_time(path, previous + std::chrono::seconds(1));
		};
		writeEnvironment(false);
		TextureAssetInfo defaults;
		auto metadata = defaults.Serialize();
		const FileId id = FileId::CreateNewFileId();
		metadata["fileId"] = id;
		metadata["filename"] = path.filename().string();
		metadata["format"] = RHI::ETextureFormat::R32G32B32A32_SFLOAT;
		metadata["bShouldGenerateMips"] = false;
		metadata["bShouldKeepCpuBuffers"] = true;
		{
			std::ofstream output(path.string() + ".asset");
			output << metadata;
			Require(static_cast<bool>(output), "the GI HDR metadata must be written");
		}
		{
			std::ofstream output(rendererPath);
			output << "frame:\n- name: Environment\n  string:\n  - EnvironmentMap: GIEnvironment.hdr\n";
			Require(static_cast<bool>(output), "the GI renderer fixture must be written");
		}
		Require(registry->GetOrLoadFile(path.string()) == id, "the GI HDR must register");
		const FileId rendererId = registry->GetOrLoadFile(rendererPath.string());
		Require(rendererId && registry->GetAssetInfoPtr(rendererPath.filename().string())->GetFileId() == rendererId,
			"the workspace renderer must be the effective renderer asset");
		GIWorld world;
		world.Step();
		GIProbesSceneCaptureRequest request;
		request.m_settings.m_bIncludeSky = true;
		request.m_settings.m_bIncludeDirectLighting = false;
		request.m_settings.m_bIncludeEmissive = false;
		request.m_settings.m_bounceCount = 1u;
		const auto capture = [&]()
		{
			GIProbesSceneSnapshot scene;
			GIProbesPreparedScene prepared;
			std::string diagnostic;
			Require(CaptureGIProbesScene(&world, request, scene, diagnostic), diagnostic);
			Require(PrepareGIProbesScene(scene, request.m_settings, nullptr, prepared, diagnostic), diagnostic);
			return prepared;
		};
		const auto requireRadiance = [&](const GIProbesPreparedScene& scene, glm::vec3 direction, glm::vec3 expected)
		{
			GIProbeBakeRaySample ray;
			std::string diagnostic;
			Require(scene.m_sampler->Sample(glm::vec3(0.0f), direction, 20.0f, 1u, ray, diagnostic), diagnostic);
			Require(!ray.m_bHit && glm::length(ray.m_radiance - expected) < 0.0001f,
				"GI must sample the selected HDR instead of sky/fallback; red " + std::to_string(ray.m_radiance.r) +
				", expected " + std::to_string(expected.r));
		};
		const glm::vec3 north(4.0f, 0.5f, 0.25f), south(0.25f, 2.0f, 0.5f);
		const auto requireProbeDirection = [](const GIProbesData& data, bool reversed, std::string_view label)
		{
			glm::vec3 up(0.0f), down(0.0f);
			if (data.m_probes.IsEmpty()) throw std::runtime_error(std::string(label) + " must contain probes");
			for (const auto& probe : data.m_probes)
			{
				up += EvaluateProbeIrradianceSH(probe.m_irradiance, { 0, 1, 0 });
				down += EvaluateProbeIrradianceSH(probe.m_irradiance, { 0, -1, 0 });
			}
			const glm::vec3 difference = (up - down) * (reversed ? -1.0f : 1.0f) / static_cast<float>(data.m_probes.Num());
			if (!(difference.r > 0.1f && difference.g < -0.1f))
			{
				throw std::runtime_error(std::string(label) + " must follow HDR direction and color; red difference " +
					std::to_string(difference.r) + ", green difference " + std::to_string(difference.g));
			}
		};
		const auto bake = [&](const GIProbesPreparedScene& prepared, bool reversed)
		{
			GIProbesBakeRequest bakeRequest;
			bakeRequest.m_stateName = "Authored environment";
			bakeRequest.m_volumeMin = { 0, 0, 30 };
			bakeRequest.m_volumeMax = { 1, 1, 31 };
			bakeRequest.m_settings = prepared.m_effectiveSettings;
			bakeRequest.m_settings.m_raysPerProbe = 512u;
			bakeRequest.m_settings.m_minProbeSpacing = 8.0f;
			bakeRequest.m_settings.m_maxSubdivisionLevel = 0u;
			const auto result = GIProbesBaker::Bake(bakeRequest, *prepared.m_sampler);
			Require(result.IsSuccess(), result.m_diagnostic);
			requireProbeDirection(*result.m_data, reversed, "baked probes");
		};
		const auto first = capture();
		requireRadiance(first, { 0.0f, 1.0f, 0.0f }, north);
		requireRadiance(first, { 0.0f, -1.0f, 0.0f }, south);
		RequireGpuEnvironment(id, first);
		bake(first, false);
		auto runtimeSettings = world.GI().GetWorldSettings();
		runtimeSettings.m_runtimeProbes.m_bIncludeSky = true;
		runtimeSettings.m_runtimeProbes.m_bIncludeEmissive = false;
		runtimeSettings.m_runtimeProbes.m_bIncludeDirectLighting = false;
		std::string diagnostic;
		Require(world.GI().ApplyWorldSettings(runtimeSettings, diagnostic), diagnostic);
		world.WaitReady();
		const auto firstRuntime = world.GI().GetActiveSnapshot();
		Require(!firstRuntime->m_states.IsEmpty(), "runtime probes must publish their HDR lighting state");
		requireProbeDirection(*firstRuntime->m_states[0].m_data, false, "runtime probes");
		const uint64_t firstRuntimeRevision = world.GI().GetRuntimeGIProbesStatus().m_publishedRevision;
		auto sky = world.Instantiate("Ignored sky")->AddComponent<SkyComponent>();
		sky->SetGiIndirectIntensity(12.0f);
		const auto withSky = capture();
		requireRadiance(withSky, { 0.0f, 1.0f, 0.0f }, north);
		Require(withSky.m_lightingHash == first.m_lightingHash,
			"an overridden SkyComponent must not change authored environment energy or revision");
		GIProbesSceneSnapshot retainedCapture;
		Require(CaptureGIProbesScene(&world, request, retainedCapture, diagnostic), diagnostic);
		Require(static_cast<bool>(retainedCapture.m_environmentPixels.m_pixels),
			"the owner capture must retain the loaded HDR pixels for Background preparation");
		writeEnvironment(true);
		Require(App::UpdateAsset(id.ToString().c_str()), "the changed GI HDR must update");
		const auto second = capture();
		Require(second.m_geometryHash == first.m_geometryHash && second.m_lightingHash != first.m_lightingHash,
			"an HDR source edit must invalidate lighting but retain geometry");
		requireRadiance(second, { 0.0f, 1.0f, 0.0f }, south);
		requireRadiance(second, { 0.0f, -1.0f, 0.0f }, north);
		requireRadiance(first, { 0.0f, 1.0f, 0.0f }, north);
		auto background = Tasks::CreateTaskWithResult<std::pair<GIProbesPreparedScene, std::string>>(
			"Prepare retained HDR"_h, [retainedCapture, settings = request.m_settings]()
			{
				std::pair<GIProbesPreparedScene, std::string> result;
				if (PrepareGIProbesScene(retainedCapture, settings, nullptr, result.first, result.second)) result.second.clear();
				return result;
			}, EThreadType::Worker);
		background->Run();
		background->Wait();
		Require(background->GetResult().second.empty(), background->GetResult().second);
		requireRadiance(background->GetResult().first, { 0, 1, 0 }, north);
		RequireGpuEnvironment(id, second);
		bake(second, true);
		world.Step(0.6f);
		world.WaitReady(firstRuntimeRevision);
		const auto secondRuntime = world.GI().GetActiveSnapshot();
		Require(secondRuntime->m_lightingHash != firstRuntime->m_lightingHash,
			"runtime HDR edits must publish a new lighting revision");
		requireProbeDirection(*secondRuntime->m_states[0].m_data, true, "updated runtime probes");
		requireProbeDirection(*firstRuntime->m_states[0].m_data, false, "retained runtime probes");
		world.GI().SetRuntimeGIProbesWorkAllowed(false);
#if defined(SAILOR_FILE_IO_TEST_HOOKS)
		uint32_t rendererReads = 0u;
		auto previousObserver = AssetRegistry::ExchangeTextReadObserverForTests([&](const std::filesystem::path& readPath)
			{ if (readPath == rendererPath) ++rendererReads; });
		bool stableRevision = true;
		for (uint32_t i = 0u; i < 32u; ++i)
		{
			GIProbesSceneRevision revision;
			stableRevision &= ObserveGIProbesSceneRevision(&world, request, revision, diagnostic) &&
				revision == second.m_observedRevision;
		}
		AssetRegistry::ExchangeTextReadObserverForTests(std::move(previousObserver));
		Require(stableRevision && rendererReads == 0u, "warm GI observations must not reread renderer YAML");
#endif
		const auto selectEnvironment = [&](const std::string& filename)
		{
			std::ofstream output(rendererPath);
			output << "frame:\n- name: Environment\n  string:\n  - EnvironmentMap: '" << filename << "'\n";
			output.close();
			Require(static_cast<bool>(output) && App::UpdateAsset(rendererId.ToString().c_str()),
				"the renderer environment selection must update");
		};
		const auto ldrPath = workspace / "Content" / "GIEnvironment.tga";
		std::array<uint8_t, 30> tga{};
		tga[2] = 2u;
		tga[12] = tga[14] = 2u;
		tga[16] = 24u;
		for (uint32_t i = 18u; i < tga.size(); i += 3u)
		{
			tga[i] = 32u;
			tga[i + 1u] = 64u;
			tga[i + 2u] = 128u;
		}
		{
			std::ofstream output(ldrPath, std::ios::binary);
			output.write(reinterpret_cast<const char*>(tga.data()), tga.size());
		}
		const FileId ldrId = FileId::CreateNewFileId();
		metadata["fileId"] = ldrId;
		metadata["filename"] = ldrPath.filename().string();
		metadata["format"] = RHI::ETextureFormat::R8G8B8A8_SRGB;
		{ std::ofstream(ldrPath.string() + ".asset") << metadata; }
		Require(registry->GetOrLoadFile(ldrPath.string()) == ldrId, "the LDR environment must register");
		selectEnvironment(ldrPath.filename().string());
		const glm::vec3 encoded(128.0f / 255.0f, 64.0f / 255.0f, 32.0f / 255.0f);
		const auto srgb = capture();
		requireRadiance(srgb, { 0, 1, 0 }, Utils::SRGBToLinear(encoded));
		RequireGpuEnvironment(ldrId, srgb);
		metadata["format"] = RHI::ETextureFormat::R8G8B8A8_UNORM;
		{ std::ofstream(ldrPath.string() + ".asset") << metadata; }
		Require(App::UpdateAsset(ldrId.ToString().c_str()), "the LDR encoding change must update");
		const auto linear = capture();
		Require(linear.m_lightingHash != srgb.m_lightingHash && linear.m_geometryHash == srgb.m_geometryHash,
			"texture encoding changes must invalidate lighting only");
		requireRadiance(linear, { 0, 1, 0 }, encoded);
		RequireGpuEnvironment(ldrId, linear);

		selectEnvironment("");
		GIProbesSceneSnapshot skyScene;
		Require(CaptureGIProbesScene(&world, request, skyScene, diagnostic), diagnostic);
		Require(skyScene.m_environment.m_type == EEnvironmentSource::Sky &&
			skyScene.m_environment.m_skyIndirectIntensity == 12.0f,
			"removing the authored map must select the world sky again");
		GIWorld withoutSky;
		withoutSky.Step();
		GIProbesSceneSnapshot constantScene;
		GIProbesPreparedScene constant;
		Require(CaptureGIProbesScene(&withoutSky, request, constantScene, diagnostic), diagnostic);
		Require(constantScene.m_environment.m_type == EEnvironmentSource::Constant,
			"a world without HDR or sky must select the constant source");
		Require(PrepareGIProbesScene(constantScene, request.m_settings, nullptr, constant, diagnostic), diagnostic);
		requireRadiance(constant, { 0, 1, 0 }, glm::vec3(0.03f));
		selectEnvironment("MissingEnvironment.hdr");
		Require(!CaptureGIProbesScene(&world, request, skyScene, diagnostic) && !diagnostic.empty(),
			"a missing authored map must report failure instead of silently substituting the sky");
		request.m_settings.m_bIncludeSky = false;
		Require(CaptureGIProbesScene(&world, request, skyScene, diagnostic), diagnostic);
		Require(PrepareGIProbesScene(skyScene, request.m_settings, nullptr, constant, diagnostic), diagnostic);
		requireRadiance(constant, { 0, 1, 0 }, glm::vec3(0.0f));
		std::cout << "Authored HDR GI capture: CPU/GPU directions, baked/runtime probes, retained Background pixels, encodings, cached configuration and source transitions passed\n";
	}

	void TestGpuLayoutUploads(const GIProbesDataPtr& data, uint64_t publishedBytes)
	{
		auto task = Tasks::CreateTaskWithResult<std::string>("GI layout upload validation"_h, [data, publishedBytes]()
		{
			try
			{
				using namespace RHI;
				auto& driver = Renderer::GetDriver();
				auto commands = Renderer::GetDriverCommands();
				auto graph = RHIFrameGraphPtr::Make();
				std::array<RHIRenderSubmissionContextPtr, 2> flights{
					RHIRenderSubmissionContextPtr::Make(), RHIRenderSubmissionContextPtr::Make() };
				uint64_t submission = 0;
				uint32_t cameraCount = 1;
				float stateWeight = 1.0f;
				auto renderMode = ESceneViewRenderMode::Lit;
				bool giEnabled = true, rejectNextUpload = false;
				auto lights = TSharedPtr<TVector<RHILightShaderData>>::Make();
				lights->Add(RHILightShaderData{});
				(*lights)[0].m_intensity = glm::vec3(3, 5, 7);
				auto bones = TSharedPtr<TVector<glm::mat4>>::Make();
				bones->Add(glm::mat4(2.0f));
				uint64_t lightingRevision = 1, animationRevision = 1;
				auto checkBuffers = [&](const RHISceneViewSnapshot& snapshot, RHISemaphorePtr wait = {})
				{
					const auto& gi = *snapshot.m_globalIllumination;
					RHIGlobalIlluminationGpuLayout layout;
					TVector<RHIGlobalIlluminationGpuCoefficients> coefficients;
					TVector<RHIGlobalIlluminationGpuState> states;
					std::string diagnostic;
					Require(BuildGlobalIlluminationGpuLayout(*gi.m_layout, layout, diagnostic), diagnostic);
					Require(BuildGlobalIlluminationGpuCoefficients(gi, coefficients, diagnostic), diagnostic);
					Require(BuildGlobalIlluminationGpuStates(gi, states, diagnostic), diagnostic);
					const auto debug = snapshot.m_renderMode == ESceneViewRenderMode::GlobalIlluminationOnly ?
						EGlobalIlluminationDebugVisualization::IndirectOnly : EGlobalIlluminationDebugVisualization::Lit;
					const auto header = BuildGlobalIlluminationGpuHeader(&gi, debug,
						snapshot.m_globalIlluminationMode, snapshot.m_bGlobalIlluminationEnabled);
					const glm::mat4 identity(1.0f);
					const size_t numBones = snapshot.m_cpuBoneMatrices ? snapshot.m_cpuBoneMatrices->Num() : 0;
					const size_t numLights = snapshot.m_cpuLightsData ? snapshot.m_cpuLightsData->Num() : 0;
					struct Buffer
					{
						StringHash name;
						const void* data;
						size_t size;
					};
					const std::array<Buffer, 11> expected{ {
						{ "globalIlluminationBvh"_h, layout.m_nodes.GetData(), layout.m_nodes.Num() * sizeof(RHIGlobalIlluminationGpuBvhNode) },
						{ "globalIlluminationBricks"_h, layout.m_bricks.GetData(), layout.m_bricks.Num() * sizeof(RHIGlobalIlluminationGpuBrick) },
						{ "globalIlluminationProbes"_h, layout.m_probes.GetData(), layout.m_probes.Num() * sizeof(RHIGlobalIlluminationGpuProbe) },
						{ "globalIlluminationCoefficients"_h, coefficients.GetData(), coefficients.Num() * sizeof(RHIGlobalIlluminationGpuCoefficients) },
						{ "globalIlluminationStates"_h, states.GetData(), states.Num() * sizeof(RHIGlobalIlluminationGpuState) },
						{ "globalIlluminationHeader"_h, &header, sizeof(header) },
						{ "light"_h, numLights ? snapshot.m_cpuLightsData->GetData() : nullptr, numLights * sizeof(RHILightShaderData) },
						{ "bones"_h, numBones ? snapshot.m_cpuBoneMatrices->GetData() : &identity, (std::max)(size_t{ 1 }, numBones) * sizeof(glm::mat4) },
						{ "lightsMatrices"_h, snapshot.m_shadowMatrices.GetData(), snapshot.m_shadowMatrices.Num() * sizeof(glm::mat4) },
						{ "shadowIndices"_h, snapshot.m_shadowIndices.GetData(), snapshot.m_shadowIndices.Num() * sizeof(uint32_t) },
						{ "shadowAtlasTiles"_h, snapshot.m_shadowAtlasTiles.GetData(), snapshot.m_shadowAtlasTiles.Num() * sizeof(uint32_t) }
					} };
					std::array<RHIBufferPtr, 11> readbacks;
					auto command = driver->CreateCommandList(false, ECommandListQueue::Graphics);
					commands->BeginCommandList(command, true);
					for (size_t i = 0; i < expected.size(); ++i)
					{
						const auto& buffer = expected[i];
						auto bindings = buffer.name == "bones"_h ? snapshot.m_boneMatrices : snapshot.m_rhiLightsData;
						auto binding = bindings->GetOrAddShaderBinding(buffer.name);
						Require(binding && binding->m_vulkan.m_valueBinding, "framegraph must publish each shared and per-view buffer");
						if (buffer.size == 0)
						{
							continue;
						}
						readbacks[i] = Tests::RecordBufferReadback(command, binding->m_vulkan.m_valueBinding, buffer.size);
					}
					commands->EndCommandList(command);
					auto fence = RHIFencePtr::Make();
					Require(driver->SubmitCommandList(command, fence, {}, wait), "shared resource readback submission must succeed");
					fence->Wait(5000000000ull);
					Require(fence->IsFinished(), "shared resource readback must complete");
					for (size_t i = 0; i < expected.size(); ++i)
					{
						Require(expected[i].size == 0 || std::memcmp(readbacks[i]->GetPointer(), expected[i].data, expected[i].size) == 0,
							std::string("GPU bytes must match the retained publication: ") + expected[i].name.ToString());
					}
					fence->ClearDependencies();
				};
				auto upload = [&](uint32_t flight, const GIProbesDataPtr& payload, uint64_t revision,
					uint64_t expectedBytes, bool expectSharedUpload = true)
				{
					flights[flight]->BeginSubmission(++submission, flight);
					auto view = RHISceneViewPtr::Make();
					auto gi = RHIGlobalIlluminationSnapshotPtr::Make();
					gi->m_generation = revision;
					gi->m_lightingHash = payload->m_lightingHash;
					gi->m_layout = payload;
					gi->m_qualityBudget = 1;
					RHIGlobalIlluminationState state;
					state.m_data = payload;
					state.m_effectiveWeight = stateWeight;
					gi->m_states.Add(state);
					view->m_snapshots.Resize(cameraCount);
					for (uint32_t camera = 0; camera < cameraCount; ++camera)
					{
						auto& snapshot = view->m_snapshots[camera];
						snapshot.m_submissionContext = flights[flight];
						snapshot.m_cameraIndex = camera;
						snapshot.m_camera = TUniquePtr<CameraData>::Make();
						snapshot.m_globalIlluminationMode = EGlobalIlluminationMode::Runtime;
						snapshot.m_bGlobalIlluminationEnabled = giEnabled;
						snapshot.m_globalIllumination = gi;
						snapshot.m_renderMode = renderMode;
						snapshot.m_cpuLightsData = lights;
						snapshot.m_lightingRevision = lightingRevision;
						snapshot.m_cpuBoneMatrices = bones;
						snapshot.m_animationRevision = animationRevision;
						snapshot.m_shadowMatrices.Add(glm::mat4(float(camera + 1)));
						snapshot.m_shadowIndices.Add(camera + 1);
						snapshot.m_shadowAtlasTiles.Add(camera * 4);
					}
					TVector<RHICommandListPtr> transfers, graphics;
					RHISemaphorePtr input, chain;
					if (rejectNextUpload)
					{
						auto before = driver->CreateCommandList(false, ECommandListQueue::Graphics);
						commands->BeginCommandList(before, true);
						commands->EndCommandList(before);
						input = driver->CreateWaitSemaphore();
						Require(driver->SubmitCommandList(before, RHIFencePtr::Make(), input), "incoming upload dependency must submit");
						const auto oldStats = graph->GetGlobalIlluminationRenderStats();
						Tests::RequireRejectedComputeSubmission([&]() { return graph->Process(view, transfers, graphics, input, chain); });
						Require(chain == input && transfers.IsEmpty() && graphics.IsEmpty(),
							"rejected shared upload must preserve the incoming semaphore and stop before per-view work");
						Require(graph->GetGlobalIlluminationRenderStats().m_activeRevision == oldStats.m_activeRevision,
							"rejected shared upload must not publish its GI statistics");
						flights[flight]->InvalidateSubmissionResources();
						flights[flight]->BeginSubmission(++submission, flight);
						rejectNextUpload = false;
					}
					const auto process = [&]
					{
						Require(graph->Process(view, transfers, graphics, input, chain), "the actual GI framegraph must process");
					};
#if defined(__APPLE__)
					const auto nativeWrites = Tests::CaptureVulkanBufferWrites(process);
					const std::array bindingNames{ "globalIlluminationBvh"_h, "globalIlluminationBricks"_h,
						"globalIlluminationProbes"_h, "globalIlluminationCoefficients"_h,
						"globalIlluminationStates"_h, "globalIlluminationHeader"_h };
					std::array<uint64_t, 6> writtenBytes{};
					for (size_t i = 0; i < bindingNames.size(); ++i)
					{
						const auto binding = view->m_snapshots[0].m_rhiLightsData->GetOrAddShaderBinding(bindingNames[i]);
						const auto memory = *binding->m_vulkan.m_valueBinding->Get();
						for (const auto& write : nativeWrites)
						{
							if (write.m_buffer != *memory.m_buffer) continue;
							const auto begin = std::max(write.m_offset, VkDeviceSize(memory.m_offset));
							const auto end = std::min(write.m_offset + write.m_size, VkDeviceSize(memory.m_offset + memory.m_size));
							if (end > begin) writtenBytes[i] += end - begin;
						}
					}
					uint64_t nativeBytes = 0;
					for (auto bytes : writtenBytes) nativeBytes += bytes;
					const auto layoutBytes = writtenBytes[0] + writtenBytes[1] + writtenBytes[2];
					std::cout << "GI native writes: submission=" << submission << " flight=" << flight << " cameras=" << cameraCount
						<< " total=" << nativeBytes << " layout=" << layoutBytes << " coefficients=" << writtenBytes[3]
						<< " states=" << writtenBytes[4] << " header=" << writtenBytes[5] << '\n';
					Require(nativeBytes == expectedBytes,
						"native GI transfer regions must match the expected payload bytes, independently of renderer statistics");
					if (expectedBytes <= 8 * sizeof(RHIGlobalIlluminationGpuCoefficients) +
						sizeof(RHIGlobalIlluminationGpuState) + sizeof(RHIGlobalIlluminationGpuHeader))
						Require(layoutBytes == 0, "lighting-only publications must not rewrite immutable GPU layout");
					if (expectedBytes == sizeof(RHIGlobalIlluminationGpuState))
						Require(writtenBytes[4] == expectedBytes, "weight changes must write only state metadata");
					if (expectedBytes == sizeof(RHIGlobalIlluminationGpuHeader))
						Require(writtenBytes[5] == expectedBytes, "mode changes must write only the GI header");
#else
					process();
#endif
					Require((chain != input) == expectSharedUpload,
						"only changed shared payloads may add a submission, independently of camera count");
					Require(transfers.Num() == cameraCount && graphics.Num() == cameraCount,
						"every camera must retain its own transfer and graphics commands");
					for (size_t i = 0; i < transfers.Num(); ++i)
					{
						for (const auto& command : { transfers[i], graphics[i] })
						{
							auto next = driver->CreateWaitSemaphore();
							Require(driver->SubmitCommandList(command, RHIFencePtr::Make(), next, chain),
								"framegraph commands must submit in dependency order");
							chain = next;
						}
					}
					auto& first = view->m_snapshots[0];
					for (auto& snapshot : view->m_snapshots)
					{
						Require(snapshot.m_boneMatrices == first.m_boneMatrices, "cameras must share one bones binding");
						for (const auto name : { "light"_h, "globalIlluminationHeader"_h, "globalIlluminationBvh"_h,
							"globalIlluminationBricks"_h, "globalIlluminationProbes"_h, "globalIlluminationCoefficients"_h, "globalIlluminationStates"_h })
						{
							Require(snapshot.m_rhiLightsData->GetOrAddShaderBinding(name)->m_vulkan.m_valueBinding ==
								first.m_rhiLightsData->GetOrAddShaderBinding(name)->m_vulkan.m_valueBinding,
								"camera bindings must reference the same shared GPU allocation");
						}
						if (snapshot.m_cameraIndex != 0)
						{
							Require(snapshot.m_frameBindings != first.m_frameBindings &&
								snapshot.m_rhiLightsData->GetOrAddShaderBinding("lightsMatrices"_h)->m_vulkan.m_valueBinding !=
								first.m_rhiLightsData->GetOrAddShaderBinding("lightsMatrices"_h)->m_vulkan.m_valueBinding,
								"camera frame and shadow allocations must remain independent");
						}
						checkBuffers(snapshot, chain);
						chain.Clear();
					}
					const auto stats = graph->GetGlobalIlluminationRenderStats();
					Require(stats.m_bActive == giEnabled && stats.m_flightSlot == flight, "GI activity and flight must match the submission");
					Require(stats.m_uploadedGpuBytes == expectedBytes && stats.m_copiedCpuBytes == expectedBytes,
						"GI uploaded " + std::to_string(stats.m_uploadedGpuBytes) + " bytes; expected " +
						std::to_string(expectedBytes) + " for changed payload ranges");
					return view;
				};
				Require(data->m_bricks.Num() == 1 && data->m_probes.Num() == 8, "GI upload fixture must have eight probes");
				const uint64_t layoutBytes = sizeof(RHIGlobalIlluminationGpuBvhNode) + sizeof(RHIGlobalIlluminationGpuBrick) +
					8 * sizeof(RHIGlobalIlluminationGpuProbe);
				const uint64_t lightingBytes = 8 * sizeof(RHIGlobalIlluminationGpuCoefficients) +
					sizeof(RHIGlobalIlluminationGpuState) + sizeof(RHIGlobalIlluminationGpuHeader);
				Require(publishedBytes == layoutBytes + lightingBytes,
					"runtime publication telemetry must match the actual complete GPU upload");
				auto olderFlight = upload(0, data, 1, layoutBytes + lightingBytes);
				auto refined = GIProbesDataPtr::Make(*data);
				++refined->m_lightingHash;
				for (auto& probe : refined->m_probes)
				{
					probe.m_irradiance[0] += glm::vec3(0.25f);
				}
				cameraCount = 3;
				auto otherFlight = upload(1, refined, 2, layoutBytes + lightingBytes);
				checkBuffers(olderFlight->m_snapshots[0]);
				upload(0, refined, 2, lightingBytes);
				auto stable = upload(0, refined, 2, 0, false);
				cameraCount = 1;
				auto fewerCameras = upload(0, refined, 2, 0, false);
				Require(stable->m_snapshots[0].m_rhiLightsData == fewerCameras->m_snapshots[0].m_rhiLightsData,
					"unchanged frames must reuse existing camera bindings");
				cameraCount = 4;
				upload(0, refined, 2, 0, false);
				auto changed = GIProbesDataPtr::Make(*refined);
				changed->m_probes[0].m_validity = changed->m_probes[0].m_validity < 0.5f ? 0.75f : 0.25f;
				Require(ComputeGIProbesTransportHash(*changed, changed->m_transportHash), "transport edit must hash");
				upload(0, changed, 3, layoutBytes + lightingBytes);
				checkBuffers(otherFlight->m_snapshots[0]);
				stateWeight = 0.5f;
				auto reallocated = GIProbesDataPtr::Make(*changed);
				Require(reallocated != changed, "weight-only publication must use a distinct CPU allocation");
				upload(0, reallocated, 3, sizeof(RHIGlobalIlluminationGpuState));
				upload(0, GIProbesDataPtr::Make(*changed), 3, 0, false);
				renderMode = ESceneViewRenderMode::GlobalIlluminationOnly;
				upload(0, changed, 3, sizeof(RHIGlobalIlluminationGpuHeader));
				giEnabled = false;
				upload(0, changed, 3, sizeof(RHIGlobalIlluminationGpuHeader));
				giEnabled = true;
				upload(0, changed, 3, sizeof(RHIGlobalIlluminationGpuHeader));

				(*lights)[0].m_intensity = glm::vec3(11, 13, 17);
				++lightingRevision;
				upload(0, changed, 3, 0);
				(*bones)[0] = glm::mat4(3.0f);
				++animationRevision;
				upload(0, changed, 3, 0);
				lights = TSharedPtr<TVector<RHILightShaderData>>::Make(*lights);
				(*lights)[0].m_intensity = glm::vec3(19, 23, 29);
				bones = TSharedPtr<TVector<glm::mat4>>::Make(*bones);
				(*bones)[0] = glm::mat4(4.0f);
				upload(0, changed, 3, 0);
				lights->Resize(17);
				bones->Resize(17);
				for (size_t i = 0; i < 17; ++i)
				{
					(*lights)[i].m_intensity = glm::vec3(float(i + 2));
					(*bones)[i] = glm::mat4(float(i + 3));
				}
				++lightingRevision;
				++animationRevision;
				upload(0, changed, 3, 0);
				upload(0, changed, 3, 0, false);
				lights->Clear();
				bones.Clear();
				++lightingRevision;
				++animationRevision;
				upload(0, changed, 3, 0);
				upload(0, changed, 3, 0, false);
				lights->Add(RHILightShaderData{});
				(*lights)[0].m_intensity = glm::vec3(31, 37, 41);
				bones = TSharedPtr<TVector<glm::mat4>>::Make();
				bones->Add(glm::mat4(5.0f));
				++lightingRevision;
				++animationRevision;
				rejectNextUpload = true;
				upload(0, changed, 4, layoutBytes + lightingBytes);
				upload(0, changed, 4, 0, false);

				const auto activeStats = graph->GetGlobalIlluminationRenderStats();
				auto otherGraph = RHIFrameGraphPtr::Make();
				auto emptyView = RHISceneViewPtr::Make();
				TVector<RHICommandListPtr> emptyTransfers, emptyGraphics;
				RHISemaphorePtr emptyChain;
				Require(otherGraph->Process(emptyView, emptyTransfers, emptyGraphics, {}, emptyChain),
					"a second graph must process an empty view");
				const auto retainedStats = graph->GetGlobalIlluminationRenderStats();
				Require(retainedStats.m_bActive && retainedStats.m_activeRevision == activeStats.m_activeRevision &&
					retainedStats.m_uploadedGpuBytes == activeStats.m_uploadedGpuBytes,
					"processing a second graph must not overwrite the first graph's GI statistics");
				Require(!otherGraph->GetGlobalIlluminationRenderStats().m_bActive,
					"an empty graph must not inherit another graph's active GI statistics");
				graph->Clear();
				Require(!graph->GetGlobalIlluminationRenderStats().m_bActive &&
					graph->GetGlobalIlluminationRenderStats().m_gpuAllocatedBytes == 0,
					"clearing a graph must reset its own GI statistics");
				std::cout << "FrameGraph shared uploads: 1-4 cameras, light/bone revisions, capacity growth, GI states/header, native rejection/retry and graph-owned statistics passed\n";
				std::cout << "GI GPU layout uploads passed: full=" << layoutBytes + lightingBytes
					<< ", SH-only=" << lightingBytes << ", unchanged=0; both flights retain exact buffer contents\n";
				return std::string{};
			}
			catch (const std::exception& error)
			{
				return std::string(error.what());
			}
		}, EThreadType::RHI);
		task->Run();
		task->Wait();
		Require(task->GetResult().empty(), task->GetResult());
	}

	void TestImporterRetry(const std::filesystem::path& workspace, const GIProbesData& data, bool collectFailed)
	{
		auto* importer = App::GetSubmodule<GIProbesImporter>();
		const char* name = collectFailed ? "RetryAfterGc.probes" : "Retry.probes";
		const auto info = App::GetSubmodule<AssetRegistry>()->GetAssetInfoPtr<GIProbesAssetInfoPtr>(name);
		Require(static_cast<bool>(info), "the broken probe fixture must be registered");
		const auto uid = info->GetFileId();
		GIProbesAssetPtr failed;
		Require(!importer->LoadGIProbes_Immediate(uid, failed) && failed && !failed->IsReady(),
			"the initial incomplete probe file must fail real I/O validation");
		const auto failedRevision = failed->GetRevision();
		if (collectFailed)
		{
			importer->CollectGarbage();
		}
		std::string diagnostic;
		Require(GIProbesBinary::SaveAtomic(workspace / "Content" / name, data, diagnostic), diagnostic);
		std::array<GIProbesAssetPtr, 8> assets;
		std::array<Tasks::TaskPtr<GIProbesAssetPtr>, 8> tasks;
		std::barrier gate(9);
		std::array<std::jthread, 8> callers;
		for (size_t i = 0; i < callers.size(); ++i)
		{
			callers[i] = std::jthread([&, i]()
			{
				gate.arrive_and_wait();
				tasks[i] = importer->LoadGIProbes(uid, assets[i]);
			});
		}
		gate.arrive_and_wait();
		for (auto& caller : callers)
		{
			caller.join();
		}
		for (size_t i = 0; i < tasks.size(); ++i)
		{
			Require(static_cast<bool>(tasks[i]), "retry must return a load task");
			tasks[i]->Wait();
			Require(assets[i] == failed && assets[i]->IsReady(),
				"repairing the same file without hot reload must make the retained asset ready");
			Require(tasks[i] == tasks[0], "concurrent requests must share the same import attempt");
		}
		Require(failed->GetRevision() == failedRevision + 1, "eight callers must import the repaired file only once");
		importer->RetainRuntimeGIProbes(uid);
		importer->RetainRuntimeGIProbes(uid);
		importer->ReleaseRuntimeGIProbes(uid);
		importer->CollectGarbage();
		GIProbesAssetPtr cached;
		Require(importer->LoadGIProbes_Immediate(uid, cached) && cached == failed && cached->GetRevision() == failedRevision + 1,
			"ready probes must remain deduplicated while one runtime binding retains them");
		importer->ReleaseRuntimeGIProbes(uid);
		Require(importer->LoadGIProbes_Immediate(uid, cached) && cached != failed && cached->GetRevision() == 1,
			"releasing the final binding must allow eviction and a fresh import");
	}
}

namespace Sailor::Tests
{
	int RunGIShutdownCommandTests(int argc, const char** argv, const std::filesystem::path& workspace)
	{
#if defined(SAILOR_GI_BAKE_TEST_HOOKS)
		std::string failures;
		for (uint32_t phase = 0u; phase < 3u; ++phase)
		{
			BakeShutdownObservation observation;
			shutdownObservation = &observation;
			try
			{
				Require(App::Initialize(argv, argc) == EAppInitializationResult::Ready && App::IsRendererInitialized(),
					"every GI shutdown phase must bootstrap a real native App");
				auto* editor = App::GetSubmodule<Editor>();
				Require(editor && editor->GetWorld(), "the hidden native App must own an Editor world");
				const char* outputs[] = { "CancelledShutdown.probes", "SavingShutdown.probes", "RestartedShutdown.probes" };
				const auto request = CreateShutdownBakeScene(workspace, outputs[phase]);
				observation.world = App::GetSubmodule<EngineLoop>()->GetWorld();
				GlobalIlluminationBakeControllerTestAccess::Observe(phase == 0u ? PauseBakeForShutdown : nullptr,
					phase == 1u ? PauseBakeForShutdown : nullptr, phase < 2u ? ObserveBakeJoinedBeforeDependencies : nullptr);
				std::string diagnostic;
				Require(editor->StartGIProbesBake(request, diagnostic), diagnostic);
				auto state = GlobalIlluminationBakeControllerTestAccess::State(*editor);
				observation.task = GlobalIlluminationBakeControllerTestAccess::Task(*editor);
				if (phase < 2u)
				{
					const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
					while (!observation.entered.load(std::memory_order_acquire) &&
						editor->GetGIProbesBakeStatus().IsRunning() && std::chrono::steady_clock::now() < deadline)
						std::this_thread::yield();
					Require(observation.entered.load(std::memory_order_acquire) && observation.background,
						"the real Background bake must reach its controlled preparation/save boundary: " +
						editor->GetGIProbesBakeStatus().m_diagnostic);
					auto marker = Tasks::CreateTask("Observe App shutdown Main drain"_h, [&]()
						{ observation.shutdownEntered.store(true, std::memory_order_release); }, EThreadType::Main);
					marker->Run();
					marker.Clear();
					std::jthread release([&, state]()
						{
							const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(5);
							while (!(phase == 0u ? state->m_cancel.load(std::memory_order_acquire) :
								observation.shutdownEntered.load(std::memory_order_acquire)) &&
								std::chrono::steady_clock::now() < until) std::this_thread::yield();
							observation.acceptedCancellation = state->m_cancel.load(std::memory_order_acquire);
							observation.Release();
						});
					const bool stopped = App::Shutdown();
					release.join();
					Require(stopped && !App::GetInstance(), "App shutdown must finish after the bake boundary is released");
					Require(observation.shutdownEntered && observation.acceptedCancellation == (phase == 0u),
						"App shutdown must cancel preparation before draining Background, but let atomic Saving finish");
					Require(observation.waitObserved && observation.dependenciesAlive && observation.taskReleased &&
						!observation.task.TryLock() && !observation.world.TryLock(),
						"App shutdown must join and release the bake task before destroying its world or importers");
				}
				else
				{
					GlobalIlluminationBakeControllerTestAccess::Controller(*editor).Wait();
					Require(editor->GetGIProbesBakeStatus().m_state == EEditorGIProbesBakeState::Succeeded,
						"a freshly bootstrapped App must complete a replacement bake: " + editor->GetGIProbesBakeStatus().m_diagnostic);
					Require(App::Shutdown() && !App::GetInstance(), "the replacement App must shut down normally");
				}
				const auto output = workspace / "Content" / outputs[phase];
				if (phase == 0u)
					Require(state->m_status.m_state == EEditorGIProbesBakeState::Cancelled && !std::filesystem::exists(output),
						"cancelled preparation must not publish a probes file");
				else
				{
					const auto loaded = GIProbesBinary::Load(output);
					Require(!state->m_cancel && state->m_status.m_state == EEditorGIProbesBakeState::Succeeded &&
						loaded.IsSuccess() && loaded.m_data->Validate(diagnostic) &&
						loaded.m_data->m_layoutHash == state->m_status.m_layoutHash &&
						loaded.m_data->m_probes.Num() == state->m_status.m_probeCount && state->m_status.m_probeCount > 0u,
						"shutdown/restart must retain the complete atomic bake payload: " + diagnostic);
				}
				std::cout << "App GI shutdown phase passed: " << phase << '\n';
			}
			catch (const std::exception& error)
			{
				observation.Release();
				if (App::GetInstance())
				{
					if (auto* editor = App::GetSubmodule<Editor>())
					{
						std::string diagnostic;
						editor->CancelGIProbesBake(diagnostic);
					}
					App::Shutdown();
				}
				failures += "GI shutdown phase " + std::to_string(phase) + ": " + error.what() + '\n';
			}
			GlobalIlluminationBakeControllerTestAccess::Observe(nullptr, nullptr, nullptr);
			shutdownObservation = nullptr;
		}
		if (!failures.empty()) { std::cerr << failures; return 1; }
		std::cout << "App GI shutdown: preparation cancellation, joined ownership, atomic save and fresh bootstrap passed\n";
		return 0;
#else
		std::cerr << "GI shutdown integration requires the test-enabled runtime\n";
		return 77;
#endif
	}

	void RunGIProbesCommandTests(const std::filesystem::path& workspace)
	{
		std::string failures;
		auto run = [&](const char* name, auto test)
		{
			try
			{
				test();
			}
			catch (const std::exception& error)
			{
				failures += std::string(name) + ": " + error.what() + '\n';
				std::cerr << name << ": " << error.what() << '\n';
			}
		};
		GIProbesDataPtr data;
		uint64_t publishedBytes = 0;
		run("Runtime GI task ownership", [&]() { RunRuntimeGIProbesTaskTests(); });
		run("Restart", [&]() { TestRestart(data, publishedBytes); });
		run("Material contributor revision", [&]() { TestContributorMaterialRevision(); });
		run("Changed-input recovery", [&]() { TestPreparationRecovery(PreparationRecovery::MaterialChange); });
		run("Explicit recovery", [&]() { TestPreparationRecovery(PreparationRecovery::Rebuild); });
		run("Restart after failed preparation", [&]() { TestPreparationRecovery(PreparationRecovery::Restart); });
		run("Stale preparation recovery", [&]() { TestStalePreparationRecovery(); });
		run("Lighting preparation", [&]() { TestLightingPreparation(); });
		run("Emission preparation", [&]() { TestEmissionPreparation(); });
		run("Emission during preparation", [&]() { TestEmissionDuringPreparation(); });
		run("Cloud-only changes", [&]() { TestCloudsDoNotInvalidateGI(); });
		run("Selected sky source", [&]() { TestSelectedSkyLightingSource(); });
		if (data)
		{
			run("GI layout uploads", [&]() { TestGpuLayoutUploads(data, publishedBytes); });
			run("Importer retry", [&]() { TestImporterRetry(workspace, *data, false); });
			run("Importer retry after GC", [&]() { TestImporterRetry(workspace, *data, true); });
		}
		run("Targeted asset capture", [&]() { TestTargetedAssetCapture(workspace); });
		run("Material preparation stress", [&]() { TestMaterialPreparationStress(workspace); });
		run("Background preparation cancellation", [&]() { TestBackgroundPreparationCancellation(); });
		run("Authored environment capture", [&]() { TestAuthoredEnvironmentCapture(workspace); });
		Require(failures.empty(), failures);
		std::cout << "GI restart, preparation recovery and importer retry tests passed\n";
	}
}
