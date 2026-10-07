#include "Sailor.h"
#include "Editor/EditorRuntimeBridge.h"
#include "AssetRegistry/AssetRegistry.h"
#include "AssetRegistry/Prefab/PrefabImporter.h"
#include "Components/LightComponent.h"
#include "Editor/EditorViewportEvent.h"
#include "ECS/TransformECS.h"
#include "Engine/EngineLoop.h"
#include "Engine/GameObject.h"
#include "Engine/World.h"
#include "RHI/Renderer.h"
#include "Submodules/Editor.h"
#include "Support/TempDirectory.h"
#include "Workspace/WorkspacePathEncoding.h"

#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string_view>
#include <vector>

using namespace Sailor;

namespace
{
	void Require(bool condition, std::string_view message)
	{
		if (!condition) throw std::runtime_error(std::string(message));
	}

	void Write(const std::filesystem::path& path, const YAML::Node& document)
	{
		std::ofstream output(path);
		output << document;
		output.close();
		Require(static_cast<bool>(output), "the simulation fixture must be written");
	}

	YAML::Node SerializeCurrentWorld()
	{
		char* buffer = nullptr;
		const auto size = App::SerializeCurrentWorld(&buffer);
		const std::string document(buffer ? buffer : "", size);
		delete[] buffer;
		return YAML::Load(document);
	}

	void TestSimulationRestoration()
	{
		auto* engine = App::GetSubmodule<EngineLoop>();
		auto* editor = App::GetSubmodule<Editor>();
		auto world = engine->GetWorld();
		Require(world && world.GetRawPtr() == editor->GetWorld() && engine->GetWorlds().Num() == 1,
			"simulation must start in the real active editor world");
		auto root = world->Instantiate("Before Play");
		auto child = world->Instantiate("Child");
		child->SetParent(root);
		auto light = child->AddComponent<LightComponent>();
		light->SetRadius(12.0f);
		auto removed = world->Instantiate("Restore after removal");
		const auto rootId = root->GetInstanceId();
		const auto childId = child->GetInstanceId();
		const auto lightId = light->GetInstanceId();
		const auto removedId = removed->GetInstanceId();

		for (uint32_t cycle = 0; cycle < 3; ++cycle)
		{
			const glm::vec3 position(float(cycle + 1), 2.0f, 3.0f);
			root->GetTransformComponent().SetPosition(position);
			Require(EditorRuntime::SetEditorSelection({ rootId }) && App::SetEditorSimulationEnabled(true) &&
				App::IsEditorSimulationEnabled() && world->IsPhysicsSimulationEnabled(),
				"Play must retain the active world and enable its physics");
			Prefab::ReflectedGameObject edit;
			edit.m_name = "Changed during Play";
			edit.m_position = glm::vec4(50, 60, 70, 1);
			const auto yaml = YAML::Dump(edit.Serialize());
			Require(App::UpdateEditorObject(rootId.ToString().c_str(), yaml.c_str()),
				"simulation mutations must pass through the actual editor command");
			child->SetParent({});
			light->SetRadius(99.0f);
			world->DestroyImmediate(removed);
			auto temporary = world->Instantiate("Created during Play");
			const auto temporaryId = temporary->GetInstanceId();
			Require(EditorRuntime::SetEditorSelection({ lightId, childId, temporaryId, InstanceId::Invalid }) &&
				App::SetEditorSimulationEnabled(true), "repeated Play must not replace the original snapshot");
			Require(SerializeCurrentWorld().IsMap(), "serializing the live simulation must not consume its saved snapshot");
			Require(App::GetEditorManagedMutationRevision(2, rootId.ToString().c_str()) != 0,
				"the live mutation revision must advance before restoration");
			Require(App::SetEditorSimulationEnabled(false), "Stop must restore the saved scene");
			auto restored = engine->GetWorld();
			Require(restored && restored != world && restored.GetRawPtr() == editor->GetWorld() &&
				engine->GetWorlds().Num() == 1 && world->GetGameObjects().IsEmpty() && !root && !child && !light && !temporary,
				"Stop must promote one replacement world and invalidate all handles into the retired world");
			root = restored->GetObjectByInstanceId(rootId).DynamicCast<GameObject>();
			child = restored->GetObjectByInstanceId(childId).DynamicCast<GameObject>();
			removed = restored->GetObjectByInstanceId(removedId).DynamicCast<GameObject>();
			Require(root && child && removed && root->GetName() == "Before Play" && child->GetParent() == root &&
				glm::vec3(root->GetTransformComponent().GetPosition()) == position &&
				!restored->GetObjectByInstanceId(temporaryId), "Stop must restore values, hierarchy and deleted objects with their original IDs");
			light = child->GetComponent<LightComponent>();
			Require(light && light->GetInstanceId() == lightId && light->GetRadius() == 12.0f,
				"component state must be independent of changes in the simulated world");
			Require(restored->IsEditorSelected(childId) && !restored->IsEditorSelected(rootId) &&
				restored->IsEditorSelected(temporaryId) && restored->GetPrimaryEditorSelection() == child,
				"Stop must preserve stop-time selection IDs and ignore stale IDs when resolving the primary object");
			Require(!App::IsEditorSimulationEnabled() && !restored->IsPhysicsSimulationEnabled() &&
				App::GetEditorManagedMutationRevision(1, nullptr) == 0 &&
				App::GetEditorManagedMutationRevision(2, rootId.ToString().c_str()) == 0 &&
				EditorRuntime::PullEditorViewportEvents(8).IsEmpty() && App::SetEditorSimulationEnabled(false),
				"restoration must reset editor interaction/revisions and make repeated Stop a no-op");
			engine->ProcessPendingWorldExits();
			Require(engine->GetWorld() == restored && engine->GetWorlds().Num() == 1,
				"a second exit drain must not retire the restored world");
			world = std::move(restored);
		}
		std::cout << "Simulation: three real Play/mutate/Stop cycles preserve identity, hierarchy, components and stop-time selection\n";
	}

	void TestLinkedSnapshotFailures(const std::filesystem::path& workspace)
	{
		auto* engine = App::GetSubmodule<EngineLoop>();
		auto* editor = App::GetSubmodule<Editor>();
		auto* registry = App::GetSubmodule<AssetRegistry>();
		auto* importer = App::GetSubmodule<PrefabImporter>();
		YAML::Node source;
		source["components"] = YAML::Node(YAML::NodeType::Sequence);
		for (uint32_t i = 0; i < 2; ++i)
		{
			Prefab::ReflectedGameObject object;
			object.m_name = i ? "Source child" : "Source root";
			object.m_instanceId = InstanceId::GenerateNewInstanceId();
			object.m_parentIndex = i ? 0 : static_cast<uint32_t>(-1);
			source["gameObjects"].push_back(object.Serialize());
		}
		const auto sourcePath = workspace / "Content" / "Simulation.prefab";
		Write(sourcePath, source);
		const FileId sourceId = registry->GetOrLoadFile(Workspace::PathToUtf8(sourcePath));
		PrefabPtr prefab;
		Require(sourceId && importer->LoadPrefab_Immediate(sourceId, prefab), "the linked simulation fixture must load");
		auto world = engine->GetWorld();
		auto linked = world->Instantiate(prefab);
		Require(linked && world->IsPrefabLinked(linked->GetInstanceId()), "the fixture must have actual prefab metadata");
		const auto linkedId = linked->GetInstanceId();
		const auto childId = linked->GetChildren()[0]->GetInstanceId();
		linked->SetName("Saved instance override");
		Require(EditorRuntime::SetEditorSelection({ linkedId }) && App::SetEditorSimulationEnabled(true), "linked scene Play must succeed");
		linked->SetName("Runtime override");
		Require(SerializeCurrentWorld().IsMap(), "saving during Play must update live prefab baselines independently");
		std::filesystem::rename(sourcePath, workspace / "Content" / "Simulation.hidden");
		for (uint32_t retry = 0; retry < 2; ++retry)
		{
			Require(!App::SetEditorSimulationEnabled(false) && App::IsEditorSimulationEnabled() &&
				engine->GetWorld() == world && engine->GetWorlds().Num() == 1 && editor->GetWorld() == world.GetRawPtr() &&
				world->IsPhysicsSimulationEnabled() && linked->GetName() == "Runtime override" && world->IsEditorSelected(linkedId),
				"failed Stop must preserve the simulated world and snapshot for a later retry");
		}
		source["gameObjects"][1]["name"] = "Updated source child";
		Write(sourcePath, source);
		Require(App::SetEditorSimulationEnabled(false), "Stop must recover after the source asset is repaired");
		auto restored = engine->GetWorld();
		linked = restored->GetObjectByInstanceId(linkedId).DynamicCast<GameObject>();
		auto child = restored->GetObjectByInstanceId(childId).DynamicCast<GameObject>();
		Require(restored != world && world->GetGameObjects().IsEmpty() && linked && child &&
			linked->GetName() == "Saved instance override" && child->GetName() == "Updated source child" &&
			child->GetParent() == linked && restored->IsPrefabLinked(linkedId),
			"retry must preserve the frozen instance overrides while reconciling current prefab source data");
		std::cout << "Simulation: repeated failed Stop retains the world; repaired source restores frozen overrides and current prefab data\n";

		std::filesystem::rename(sourcePath, workspace / "Content" / "Simulation.repaired");
		const auto info = registry->GetAssetInfoPtr<PrefabAssetInfoPtr>(sourceId);
		importer->OnUpdateAssetInfo(info, true);
		Require(!App::SetEditorSimulationEnabled(true) && !App::IsEditorSimulationEnabled() &&
			!restored->IsPhysicsSimulationEnabled() && engine->GetWorld() == restored && engine->GetWorlds().Num() == 1,
			"a failed snapshot must not start simulation or discard the current editor scene");
		Write(sourcePath, source);
		Require(App::SetEditorSimulationEnabled(true) && App::SetEditorSimulationEnabled(false),
			"Play must retry after snapshot creation becomes possible again");
		std::cout << "Simulation: failed snapshot leaves preview intact and Play succeeds after repair\n";
	}

	void TestSessionReplacement()
	{
		auto* engine = App::GetSubmodule<EngineLoop>();
		auto* editor = App::GetSubmodule<Editor>();
		auto previous = engine->GetWorld();
		Require(App::SetEditorSimulationEnabled(true) && App::CreateEditorWorld() &&
			!App::IsEditorSimulationEnabled() && engine->GetWorlds().Num() == 1 &&
			engine->GetWorld() != previous && previous->GetGameObjects().IsEmpty(),
			"New Scene during simulation must finish the old session without retaining another world");
		auto current = engine->GetWorld();
		editor->SetWorld(nullptr);
		Require(engine->ExitWorld(current.GetRawPtr()), "the empty-session fixture must retire its active world");
		engine->ProcessPendingWorldExits();
		Require(!App::SetEditorSimulationEnabled(true) && App::SetEditorSimulationEnabled(false) &&
			!App::IsEditorSimulationEnabled() && engine->GetWorlds().IsEmpty(), "Play without a world must fail without creating session state");
		Require(App::CreateEditorWorld() && App::SetEditorSimulationEnabled(true) && App::SetEditorSimulationEnabled(false),
			"a fresh empty scene must still support a complete simulation cycle");
		std::cout << "Simulation: new-scene replacement, absent world and fresh-session restoration passed\n";
	}
}

namespace Sailor::Tests
{
	int RunEditorSimulationTests(int argc, const char** argv)
	{
		TempDirectory workspace("editor-simulation");
		int result = 1;
		try
		{
			std::string enginePath = Workspace::PathToUtf8(std::filesystem::current_path());
			for (int i = 1; i + 1 < argc; ++i)
				if (std::string_view(argv[i]) == "--workspace") enginePath = argv[i + 1];
			std::filesystem::create_directory(workspace.Path("Content"));
			YAML::Node manifest;
			manifest["manifestVersion"] = 1;
			manifest["workspaceId"] = "00000000-0000-0000-0000-000000000277";
			manifest["name"] = "Editor simulation test";
			manifest["enginePath"] = enginePath;
			manifest["engineReferenceKind"] = "source";
			manifest["contentPath"] = "Content";
			manifest["sourcePath"] = "Source";
			manifest["generatedProjectPath"] = "Generated";
			manifest["cachePath"] = "Cache";
			manifest["buildPath"] = "Cache/Build";
			manifest["logicOutputPath"] = "Binaries";
			manifest["logicModuleName"] = "EditorSimulationTest";
			Write(workspace.Path("workspace.sailor"), manifest);
			const auto root = Workspace::PathToUtf8(workspace.Get());
			std::vector<const char*> arguments(argv, argv + argc);
			arguments.insert(arguments.end(), { "--workspace", root.c_str(), "--editor", "--port", "0", "--world", "", "--new-world" });
			App::Initialize(arguments.data(), static_cast<int>(arguments.size()));
			Require(App::IsRendererInitialized(), "simulation tests require the real initialized native App");
			App::GetSubmodule<Tasks::Scheduler>()->WaitIdle({ EThreadType::Main, EThreadType::Worker, EThreadType::RHI, EThreadType::Render });
			TestSimulationRestoration();
			TestLinkedSnapshotFailures(workspace.Get());
			TestSessionReplacement();
			result = 0;
		}
		catch (const std::exception& error)
		{
			std::cerr << "Editor simulation test failed: " << error.what() << '\n';
		}
		if (!App::Shutdown()) result = 1;
		return result;
	}
}
