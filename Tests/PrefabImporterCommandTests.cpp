#include "Sailor.h"
#include "AssetRegistry/AssetRegistry.h"
#include "AssetRegistry/Prefab/PrefabImporter.h"
#include "AssetRegistry/World/WorldPrefabImporter.h"
#include "Components/LightComponent.h"
#include "Core/Utils.h"
#include "ECS/TransformECS.h"
#include "Engine/GameObject.h"
#include "Engine/World.h"
#include "Tasks/Tasks.h"

#include <array>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <latch>
#include <stdexcept>
#include <thread>

using namespace Sailor;

namespace
{
	void Require(bool condition, const char* message)
	{
		if (!condition) throw std::runtime_error(message);
	}

	template<typename Predicate>
	bool WaitFor(Predicate ready)
	{
		const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
		while (!ready() && std::chrono::steady_clock::now() < deadline)
			std::this_thread::sleep_for(std::chrono::milliseconds(1));
		return ready();
	}

	class PauseQueue
	{
	public:
		explicit PauseQueue(EThreadType type)
		{
			const auto count = App::GetSubmodule<Tasks::Scheduler>()->GetNumThreads(type);
			for (uint32_t i = 0; i < count; ++i)
				m_tasks.Add(Tasks::CreateTask("Hold prefab test queue", [this]()
					{
						++m_entered;
						m_release.wait();
					}, type));
		}
		~PauseQueue()
		{
			Release();
			for (const auto& task : m_tasks) task->Wait();
		}
		void Start()
		{
			for (const auto& task : m_tasks) task->Run();
			Require(!m_tasks.IsEmpty() && WaitFor([&]() { return m_entered == m_tasks.Num(); }),
				"every worker on the controlled queue must be held");
		}
		void Release()
		{
			if (!m_released)
			{
				m_released = true;
				m_release.count_down();
			}
		}
	private:
		TVector<Tasks::ITaskPtr> m_tasks;
		std::atomic<uint32_t> m_entered{};
		std::latch m_release{ 1 };
		bool m_released = false;
	};

	YAML::Node PrefabDocument()
	{
		YAML::Node result;
		result["components"] = YAML::Node(YAML::NodeType::Sequence);
		for (uint32_t i = 0; i < 2; ++i)
		{
			Prefab::ReflectedGameObject object;
			object.m_name = i ? "Child" : "Root";
			object.m_instanceId = InstanceId::GenerateNewInstanceId();
			object.m_parentIndex = i ? 0 : static_cast<uint32_t>(-1);
			result["gameObjects"].push_back(object.Serialize());
		}
		return result;
	}

	YAML::Node WorldDocument(const YAML::Node& prefab)
	{
		YAML::Node result;
		result["name"] = "CPU world";
		result["prefabs"].push_back(YAML::Clone(prefab));
		return result;
	}

	void Write(const std::filesystem::path& path, const YAML::Node& document)
	{
		std::ofstream output(path);
		output << document;
		output.close();
		Require(static_cast<bool>(output), "prefab test document must be written");
	}

	void TestCpuCompletion(const std::filesystem::path& workspace)
	{
		auto* scheduler = App::GetSubmodule<Tasks::Scheduler>();
		auto* registry = App::GetSubmodule<AssetRegistry>();
		auto* prefabs = App::GetSubmodule<PrefabImporter>();
		auto* worlds = App::GetSubmodule<WorldPrefabImporter>();
		const auto prefabPath = workspace / "Content" / "CpuLoad.prefab";
		const auto worldPath = workspace / "Content" / "CpuLoad.world";
		const auto document = PrefabDocument();
		Write(prefabPath, document);
		Write(worldPath, WorldDocument(document));
		const FileId prefabId = registry->GetOrLoadFile(prefabPath.string());
		const FileId worldId = registry->GetOrLoadFile(worldPath.string());
		scheduler->WaitIdle({ EThreadType::Worker, EThreadType::Render, EThreadType::RHI });
		PauseQueue rhi(EThreadType::RHI);
		rhi.Start();
		PauseQueue workers(EThreadType::Worker);
		workers.Start();

		PrefabPtr prefab;
		WorldPrefabPtr world;
		auto prefabTask = prefabs->LoadPrefab(prefabId, prefab);
		auto worldTask = worlds->LoadWorld(worldId, world);
		Require(prefabTask && worldTask && prefab && world && !prefab->IsReady() && !world->IsReady(),
			"async loading must expose pending assets while CPU work is held");
		std::array<PrefabPtr, 4> repeatedPrefabs;
		std::array<WorldPrefabPtr, 4> repeatedWorlds;
		std::array<Tasks::TaskPtr<PrefabPtr>, 4> prefabTasks;
		std::array<Tasks::TaskPtr<WorldPrefabPtr>, 4> worldTasks;
		std::array<std::jthread, 4> requesters;
		for (size_t i = 0; i < requesters.size(); ++i)
			requesters[i] = std::jthread([&, i]()
				{
					prefabTasks[i] = prefabs->LoadPrefab(prefabId, repeatedPrefabs[i]);
					worldTasks[i] = worlds->LoadWorld(worldId, repeatedWorlds[i]);
				});
		for (auto& thread : requesters) thread.join();
		for (size_t i = 0; i < requesters.size(); ++i)
			Require(prefabTasks[i] == prefabTask && worldTasks[i] == worldTask &&
				repeatedPrefabs[i] == prefab && repeatedWorlds[i] == world,
				"concurrent callers must share each in-flight asset and promise");

		auto prefabConsumer = prefabTask->Then<bool>([](PrefabPtr value)
			{
				return value && value->IsReady() && value->Serialize()["gameObjects"].size() == 2;
			}, "Consume loaded prefab", EThreadType::Worker);
		auto worldConsumer = worldTask->Then<bool>([](WorldPrefabPtr value)
			{
				return value && value->IsReady() && value->GetName() == "CPU world" &&
					value->GetGameObjects().Num() == 1 && value->GetGameObjects()[0]->IsReady();
			}, "Consume loaded world", EThreadType::Worker);
		workers.Release();
		const bool completed = WaitFor([&]() { return prefabConsumer->IsFinished() && worldConsumer->IsFinished(); });
		std::cout << "Prefab/world with RHI held: ready=" << prefab->IsReady() << '/' << world->IsReady()
			<< ", tasks=" << prefabTask->IsFinished() << '/' << worldTask->IsFinished() << '\n';
		Require(completed, "CPU prefab/world loading and consumers must finish while all RHI workers remain held");
		Require(prefabConsumer->GetResult() && worldConsumer->GetResult() &&
			prefabTask->GetResult() == prefab && worldTask->GetResult() == world,
			"dependent consumers must see complete parsed data and the original asset identity");
		prefabs->CollectGarbage();
		worlds->CollectGarbage();
		PrefabPtr cachedPrefab;
		WorldPrefabPtr cachedWorld;
		Require(prefabs->LoadPrefab_Immediate(prefabId, cachedPrefab) && cachedPrefab == prefab &&
			worlds->LoadWorld_Immediate(worldId, cachedWorld) && cachedWorld == world,
			"immediate cached loads after promise collection must retain ready asset identity");
	}

	void TestFailedLoadRetry(const std::filesystem::path& workspace)
	{
		auto* registry = App::GetSubmodule<AssetRegistry>();
		auto* prefabs = App::GetSubmodule<PrefabImporter>();
		auto* worlds = App::GetSubmodule<WorldPrefabImporter>();
		for (bool collect : { false, true })
			for (uint32_t failure = 0; failure < 3; ++failure)
			{
				const auto name = std::string("RetryCpu") + std::to_string(collect) + std::to_string(failure);
				const auto prefabPath = workspace / "Content" / (name + ".prefab");
				const auto worldPath = workspace / "Content" / (name + ".world");
				const auto valid = PrefabDocument();
				Write(prefabPath, valid);
				Write(worldPath, WorldDocument(valid));
				const FileId prefabId = registry->GetOrLoadFile(prefabPath.string());
				const FileId worldId = registry->GetOrLoadFile(worldPath.string());
				if (failure == 0)
				{
					std::ofstream(prefabPath) << "gameObjects: [";
					std::ofstream(worldPath) << "prefabs: [";
				}
				else if (failure == 1)
				{
					auto invalid = YAML::Clone(valid);
					invalid["gameObjects"][0]["parentIndex"] = 0;
					Write(prefabPath, invalid);
					Write(worldPath, WorldDocument(invalid));
				}
				else
				{
					Require(std::filesystem::remove(prefabPath) && std::filesystem::remove(worldPath),
						"only the temporary fixture source files must be removed");
				}
				PrefabPtr failedPrefab;
				WorldPrefabPtr failedWorld;
				Require(!prefabs->LoadPrefab_Immediate(prefabId, failedPrefab) && failedPrefab && !failedPrefab->IsReady() &&
					!worlds->LoadWorld_Immediate(worldId, failedWorld) && failedWorld && !failedWorld->IsReady(),
					"I/O, YAML and validation failures must all report not-ready assets");
				if (collect)
				{
					prefabs->CollectGarbage();
					worlds->CollectGarbage();
				}
				Write(prefabPath, valid);
				Write(worldPath, WorldDocument(valid));
				PrefabPtr repairedPrefab;
				WorldPrefabPtr repairedWorld;
				Require(prefabs->LoadPrefab_Immediate(prefabId, repairedPrefab) && repairedPrefab != failedPrefab &&
					worlds->LoadWorld_Immediate(worldId, repairedWorld) && repairedWorld != failedWorld,
					"repaired source files must retry with and without completed-promise collection");
				Require(!failedPrefab->IsReady() && !failedWorld->IsReady() &&
					repairedPrefab->Serialize()["gameObjects"].size() == 2 && repairedWorld->GetGameObjects().Num() == 1,
					"retry must publish complete replacement data without changing retained failed assets");
				TObjectPtr<Object> generic;
				Require(prefabs->LoadAsset(prefabId, generic) && generic && generic->IsReady() &&
					worlds->LoadAsset(worldId, generic) && generic && generic->IsReady(),
					"generic asset loading must retain the typed immediate readiness contract");
			}
	}

	class ObservePrefabLoading
	{
	public:
		ObservePrefabLoading(AssetRegistry::TextReadObserver reads, Prefab::ValidationObserver validations) :
			m_reads(AssetRegistry::ExchangeTextReadObserverForTests(std::move(reads))),
			m_validations(Prefab::ExchangeValidationObserverForTests(std::move(validations))) {}
		~ObservePrefabLoading()
		{
			Prefab::ExchangeValidationObserverForTests(std::move(m_validations));
			AssetRegistry::ExchangeTextReadObserverForTests(std::move(m_reads));
		}
	private:
		AssetRegistry::TextReadObserver m_reads;
		Prefab::ValidationObserver m_validations;
	};

	class PrefabWorld final : public World
	{
	public:
		PrefabWorld() : World("CPU world", uint8_t(EWorldBehaviourBit::EditorTick), CreateEcs()) {}
		~PrefabWorld() override { Clear(); }
	private:
		static TVector<ECS::TBaseSystemPtr> CreateEcs()
		{
			TVector<ECS::TBaseSystemPtr> systems;
			systems.Add(TUniquePtr<TransformECS>::Make());
			systems.Add(TUniquePtr<LightingECS>::Make());
			return systems;
		}
	};

	YAML::Node LightPrefabDocument(const char* childName)
	{
		auto result = PrefabDocument();
		result["gameObjects"][1]["name"] = childName;
		for (uint32_t i = 0; i < 2; ++i)
		{
			const auto owner = result["gameObjects"][i]["instanceId"].as<InstanceId>();
			YAML::Node component;
			component["typename"] = LightComponent::GetStaticTypeInfo().Name();
			component["overrideProperties"]["instanceId"] = InstanceId::GenerateNewComponentId(owner);
			component["overrideProperties"]["radius"] = float(10 + i);
			component["overrideProperties"]["intensity"] = glm::vec3(1, 2, 3);
			result["components"].push_back(component);
			result["gameObjects"][i]["components"].push_back(i);
		}
		return result;
	}

	YAML::Node LinkedRecord(const YAML::Node& source, FileId id, InstanceId parent, int overrideIndex)
	{
		auto result = YAML::Clone(source);
		TMap<InstanceId, InstanceId> ids;
		for (auto object : result["gameObjects"])
		{
			const auto sourceId = object["instanceId"].as<InstanceId>();
			const auto liveId = InstanceId::GenerateNewInstanceId();
			ids[sourceId] = liveId;
			object["instanceId"] = liveId;
		}
		for (auto component : result["components"])
		{
			const auto sourceId = component["overrideProperties"]["instanceId"].as<InstanceId>();
			component["overrideProperties"]["instanceId"] = InstanceId(sourceId.ComponentId(), ids[sourceId.GameObjectId()]);
		}
		TMap<InstanceId, YAML::Node> objectOverrides;
		TMap<InstanceId, ReflectedData> componentOverrides;
		if (overrideIndex >= 0)
		{
			const auto rootId = source["gameObjects"][0]["instanceId"].as<InstanceId>();
			const auto lightId = source["components"][0]["overrideProperties"]["instanceId"].as<InstanceId>();
			YAML::Node properties;
			properties["name"] = "Instance" + std::to_string(overrideIndex);
			objectOverrides[rootId] = properties;
			YAML::Node component;
			component["typename"] = LightComponent::GetStaticTypeInfo().Name();
			component["overrideProperties"]["radius"] = float(30 + overrideIndex);
			ReflectedData value;
			value.Deserialize(component);
			componentOverrides[lightId] = value;
			result["gameObjects"][0]["name"] = properties["name"].as<std::string>();
			result["components"][0]["overrideProperties"]["radius"] = float(30 + overrideIndex);
		}
		result["fileId"] = id;
		result["parentInstanceId"] = parent;
		result["instanceIds"] = ids;
		result["gameObjectOverrides"] = objectOverrides;
		result["componentOverrides"] = componentOverrides;
		return result;
	}

	void TestLinkedSourceSnapshots(const std::filesystem::path& workspace)
	{
		auto* registry = App::GetSubmodule<AssetRegistry>();
		auto* worlds = App::GetSubmodule<WorldPrefabImporter>();
		const auto firstPath = workspace / "Content" / "SnapshotA.prefab";
		const auto secondPath = workspace / "Content" / "SnapshotB.prefab";
		const auto source = LightPrefabDocument("Original source");
		const auto secondSource = LightPrefabDocument("Second source");
		auto changed = YAML::Clone(source);
		changed["gameObjects"][1]["name"] = "Changed source";
		changed["components"][1]["overrideProperties"]["intensity"] = glm::vec3(4, 5, 6);
		Write(firstPath, source);
		Write(secondPath, secondSource);
		const FileId firstId = registry->GetOrLoadFile(firstPath.string());
		const FileId secondId = registry->GetOrLoadFile(secondPath.string());
		const auto parent = PrefabDocument();
		const auto parentId = parent["gameObjects"][0]["instanceId"].as<InstanceId>();
		YAML::Node document = WorldDocument(parent);
		document["prefabs"].push_back(LinkedRecord(source, firstId, parentId, 0));
		document["prefabs"].push_back(LinkedRecord(secondSource, secondId, parentId, 1));
		document["prefabs"].push_back(LinkedRecord(source, firstId, parentId, 2));
		document["prefabs"].push_back(LinkedRecord(source, firstId, parentId, -1));
		const auto originalDocument = YAML::Clone(document);
		uint32_t firstReads = 0, secondReads = 0;
		uint32_t firstValidations = 0, secondValidations = 0;
		bool editAfterRead = true;
		ObservePrefabLoading observe([&](const std::filesystem::path& path)
			{
				if (std::filesystem::equivalent(path, firstPath))
				{
					++firstReads;
					if (editAfterRead)
					{
						editAfterRead = false;
						Write(firstPath, changed);
					}
				}
				if (std::filesystem::equivalent(path, secondPath)) ++secondReads;
			}, [&](const Prefab& prefab)
			{
				if (prefab.IsLinkedInstanceRecord()) return;
				if (prefab.GetFileId() == firstId) ++firstValidations;
				if (prefab.GetFileId() == secondId) ++secondValidations;
			});
		auto load = [&](const YAML::Node& input)
			{
				firstReads = secondReads = 0;
				firstValidations = secondValidations = 0;
				auto world = worlds->Create();
				world->Deserialize(input);
				if (!world->IsReady()) throw std::runtime_error(world->GetLoadDiagnostic());
				return world;
			};
		auto verifySnapshot = [&](WorldPrefabPtr world, bool updated)
			{
				Require(firstReads == 1 && secondReads == 1, "a world load must read each distinct source prefab exactly once");
				Require(firstValidations == 1 && secondValidations == 1,
					"a world load must validate each distinct source prefab exactly once");
				const auto result = world->Serialize();
				Require(result["prefabs"].size() == 5, "all independent linked instances and their parent must survive");
				for (uint32_t i = 1; i < 5; ++i)
				{
					const auto record = result["prefabs"][i];
					const auto original = originalDocument["prefabs"][i];
					Require(record["fileId"].as<FileId>() == original["fileId"].as<FileId>() &&
						record["parentInstanceId"].as<InstanceId>() == parentId &&
						Utils::AreYamlNodesEqual(record["instanceIds"], original["instanceIds"]),
						"source, parent and per-instance identity mappings must stay independent");
					const auto expectedRoot = i == 4 ? "Root" : "Instance" + std::to_string(i - 1);
					const float expectedRadius = i == 4 ? 10.0f : float(29 + i);
					const char* expectedChild = i == 2 ? "Second source" : updated ? "Changed source" : "Original source";
					const auto expectedIntensity = updated && i != 2 ? glm::vec3(4, 5, 6) : glm::vec3(1, 2, 3);
					Require(record["gameObjects"][0]["name"].as<std::string>() == expectedRoot &&
						record["components"][0]["overrideProperties"]["radius"].as<float>() == expectedRadius,
						"one instance's object/component overrides must not modify the shared source or another instance");
					Require(record["gameObjects"][1]["name"].as<std::string>() == expectedChild &&
						record["components"][1]["overrideProperties"]["intensity"].as<glm::vec3>() == expectedIntensity,
						"all instances of a source must use one consistent name and component snapshot");
				}
				Require(Utils::AreYamlNodesEqual(document, originalDocument), "loading must not mutate its authored input YAML");
			};
		auto first = load(document);
		const auto firstImage = YAML::Clone(first->Serialize());
		std::cout << "Linked source reads: A=" << firstReads << ", B=" << secondReads
			<< "; validations=" << firstValidations << '/' << secondValidations
			<< "; children=" << firstImage["prefabs"][1]["gameObjects"][1]["name"].as<std::string>()
			<< '/' << firstImage["prefabs"][3]["gameObjects"][1]["name"].as<std::string>() << '\n';
		verifySnapshot(first, false);
		auto next = load(document);
		verifySnapshot(next, true);
		Require(Utils::AreYamlNodesEqual(firstImage, first->Serialize()), "a later load must not mutate retained earlier snapshots");
		auto save = [&](WorldPrefabPtr prepared)
			{
				PrefabWorld live;
				for (auto prefab : prepared->GetGameObjects())
				{
					auto root = live.Instantiate(prefab, true);
					Require(static_cast<bool>(root), "the prepared record must instantiate with its persistent identities");
					if (!prefab->IsLinkedInstanceRecord()) continue;
					Require(root->GetParent() && root->GetParent()->GetInstanceId() == parentId &&
						root->GetChildren().Num() == 1, "linked roots must retain their parent and child hierarchy");
					const auto data = prefab->Serialize();
					for (uint32_t i = 0; i < 2; ++i)
					{
						const auto sourceId = data["gameObjects"][i]["instanceId"].as<InstanceId>();
						auto object = i ? root->GetChildren()[0] : root;
						auto light = object->GetComponent<LightComponent>();
						const auto properties = data["components"][i]["overrideProperties"];
						Require(object->GetInstanceId() == prefab->GetLinkedInstanceIds()[sourceId] && light &&
							light->GetInstanceId().GameObjectId() == object->GetInstanceId() &&
							light->GetInstanceId().ComponentId() == properties["instanceId"].as<InstanceId>().ComponentId() &&
							light->GetRadius() == properties["radius"].as<float>() &&
							light->GetIntensity() == properties["intensity"].as<glm::vec3>(),
							"instantiated lights must keep their own overrides and remapped owner/component identities");
					}
				}
				auto saved = WorldPrefab::FromWorld(&live);
				if (!saved->IsReady()) throw std::runtime_error(saved->GetLoadDiagnostic());
				return saved->Serialize();
			};
		const auto serialized = save(next);
		auto roundtrip = load(serialized);
		verifySnapshot(roundtrip, true);
		Require(Utils::AreYamlNodesEqual(serialized, save(roundtrip)), "save-load-save must not introduce new overrides");
		auto withoutOverrides = YAML::Clone(serialized);
		for (uint32_t i = 1; i < 5; ++i)
		{
			withoutOverrides["prefabs"][i].remove("gameObjectOverrides");
			withoutOverrides["prefabs"][i].remove("componentOverrides");
		}
		auto derived = load(withoutOverrides);
		verifySnapshot(derived, true);
		Require(Utils::AreYamlNodesEqual(serialized, save(derived)),
			"deriving absent override fields must reuse the checked source and preserve the existing save contract");
		std::cout << "Linked prefab snapshots: one real read and validation per source, coherent edits and independent overrides passed\n";
	}

	void TestLinkedValidationBoundary()
	{
		auto* prefabs = App::GetSubmodule<PrefabImporter>();
		const auto id = FileId::CreateNewFileId();
		const auto valid = LightPrefabDocument("Validation child");
		const auto record = LinkedRecord(valid, id, InstanceId::Invalid, -1);
		const auto ids = record["instanceIds"].as<TMap<InstanceId, InstanceId>>();
		auto source = prefabs->Create(id);
		auto target = prefabs->Create(id);
		std::string diagnostic;
		auto configure = [&]() { return target->ConfigureLinkedInstance(source, ids, InstanceId::Invalid, {}, {}, diagnostic); };
		source->Deserialize(valid);
		Require(configure() && target->IsReady(), "standalone linked configuration must validate a valid source");
		for (uint32_t failure = 0; failure < 4; ++failure)
		{
			auto invalid = YAML::Clone(valid);
			if (failure == 0) invalid["gameObjects"][0]["parentIndex"] = 0;
			if (failure == 1) invalid["components"][0]["typename"] = "UnknownPrefabComponent";
			if (failure == 2) invalid["components"][0]["overrideProperties"].remove("instanceId");
			if (failure == 3) invalid["gameObjects"][1]["instanceId"] = valid["gameObjects"][0]["instanceId"].as<InstanceId>();
			source->Deserialize(invalid);
			Require(!configure() && !target->IsReady() && !diagnostic.empty() &&
				target->Serialize()["gameObjects"].size() == 0,
				"standalone configuration must reject invalid sources and clear the previous ready target");
			source->Deserialize(valid);
			Require(configure() && target->IsReady(), "repairing a source must rebuild its validated index");
		}
		Require(!target->ConfigureLinkedInstance(target, ids, InstanceId::Invalid, {}, {}, diagnostic) && !target->IsReady(),
			"self-configuration must fail without retaining the old ready state");
		Require(configure() && target->IsReady(), "a distinct valid source must still configure after self-rejection");
		std::cout << "Standalone prefab validation rejects invalid hierarchy/type/identity and recovers after repair\n";
	}

	void TestLinkedSourceFailureRetry(const std::filesystem::path& workspace)
	{
		auto* registry = App::GetSubmodule<AssetRegistry>();
		auto* worlds = App::GetSubmodule<WorldPrefabImporter>();
		for (bool collect : { false, true })
			for (uint32_t failure = 0; failure < 4; ++failure)
			{
				const auto name = std::string("RetryLinked") + std::to_string(collect) + std::to_string(failure);
				const auto sourcePath = workspace / "Content" / (name + ".prefab");
				const auto worldPath = workspace / "Content" / (name + ".world");
				const auto source = LightPrefabDocument("Source child");
				Write(sourcePath, source);
				const auto sourceId = registry->GetOrLoadFile(sourcePath.string());
				auto document = WorldDocument(LinkedRecord(source, sourceId, InstanceId::Invalid, 0));
				document["prefabs"].push_back(LinkedRecord(source, sourceId, InstanceId::Invalid, -1));
				Write(worldPath, document);
				const auto worldId = registry->GetOrLoadFile(worldPath.string());
				if (failure == 0)
					std::ofstream(sourcePath) << "gameObjects: [";
				else if (failure == 1)
				{
					auto invalid = YAML::Clone(source);
					invalid["gameObjects"][0]["parentIndex"] = 0;
					Write(sourcePath, invalid);
				}
				else if (failure == 2)
					Require(std::filesystem::remove(sourcePath), "only the temporary linked source must be removed");
				else
				{
					auto unknown = YAML::Clone(document);
					unknown["prefabs"][0]["fileId"] = FileId::CreateNewFileId();
					Write(worldPath, unknown);
				}
				WorldPrefabPtr failed;
				Require(!worlds->LoadWorld_Immediate(worldId, failed) && failed && !failed->IsReady(),
					"an invalid or unavailable linked source must fail the whole world load");
				if (collect) worlds->CollectGarbage();
				Write(sourcePath, source);
				Write(worldPath, document);
				WorldPrefabPtr repaired;
				Require(worlds->LoadWorld_Immediate(worldId, repaired) && repaired != failed && !failed->IsReady(),
					"a repaired linked source must retry without reusing failed data, with or without promise collection");
				const auto result = repaired->Serialize();
				Require(result["prefabs"].size() == 2 &&
					result["prefabs"][0]["components"][0]["overrideProperties"]["radius"].as<float>() == 30 &&
					result["prefabs"][1]["components"][0]["overrideProperties"]["radius"].as<float>() == 10,
					"retry must rebuild every linked instance with independent overrides");
			}
		std::cout << "Linked source malformed/invalid/missing/unknown failures and repaired retries passed\n";
	}
}

namespace Sailor::Tests
{
	void RunPrefabImporterCommandTests(const std::filesystem::path& workspace)
	{
		TestCpuCompletion(workspace);
		TestFailedLoadRetry(workspace);
		TestLinkedSourceFailureRetry(workspace);
		auto snapshots = Tasks::CreateTaskWithResult<std::string>("Check linked prefab source snapshots", [&]()
			{
				try { TestLinkedSourceSnapshots(workspace); TestLinkedValidationBoundary(); return std::string{}; }
				catch (const std::exception& error) { return std::string(error.what()); }
			}, EThreadType::Worker);
		snapshots->Run();
		snapshots->Wait();
		if (!snapshots->GetResult().empty()) throw std::runtime_error(snapshots->GetResult());
		std::cout << "Prefab/world CPU completion, concurrent callers, dependencies and failed-load retry passed\n";
	}
}
