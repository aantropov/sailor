#include "Sailor.h"
#include "AssetRegistry/AssetRegistry.h"
#include "AssetRegistry/Prefab/PrefabImporter.h"
#include "AssetRegistry/World/WorldPrefabImporter.h"
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
}

namespace Sailor::Tests
{
	void RunPrefabImporterCommandTests(const std::filesystem::path& workspace)
	{
		TestCpuCompletion(workspace);
		TestFailedLoadRetry(workspace);
		std::cout << "Prefab/world CPU completion, concurrent callers, dependencies and failed-load retry passed\n";
	}
}
