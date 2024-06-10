#include "WorldPrefabImporter.h"
#include "AssetRegistry/FileId.h"
#include "AssetRegistry/AssetRegistry.h"
#include "WorldPrefabAssetInfo.h"
#include "Core/Utils.h"
#include <filesystem>
#include <fstream>
#include <algorithm>
#include <iostream>
#include "Memory/ObjectAllocator.hpp"
#include "Tasks/Scheduler.h"
#include "Engine/GameObject.h"
#include "Engine/World.h"
#include "ECS/TransformECS.h"

using namespace Sailor;

YAML::Node WorldPrefab::Serialize() const
{
	YAML::Node outData;
	::Serialize(outData, "name", m_name);

	TVector<YAML::Node> nodes;
	for (auto& go : m_gameObjects)
	{
		nodes.Add(go->Serialize());
	}

	outData["prefabs"] = nodes;

	return outData;
}

void WorldPrefab::Deserialize(const YAML::Node& inData)
{
	::Deserialize(inData, "name", m_name);

	check(inData["prefabs"].IsSequence());

	size_t numGameObjects = inData["prefabs"].size();
	m_gameObjects.Reserve(numGameObjects);

	for (uint32_t i = 0; i < numGameObjects; i++)
	{
		auto newPrefab = App::GetSubmodule<PrefabImporter>()->Create();
		newPrefab->Deserialize(inData["prefabs"][i]);
		m_gameObjects.Add(newPrefab);
	}
}

bool WorldPrefab::SaveToFile(const std::string& path) const
{
	AssetRegistry::WriteTextFile(path, Serialize());
	return true;
}

WorldPrefabPtr WorldPrefab::FromWorld(WorldPtr world)
{
	auto res = App::GetSubmodule<WorldPrefabImporter>()->Create();
	res->m_name = world->GetName();

	auto gameObjects = world->GetGameObjects();

	for (auto& go : gameObjects)
	{
		auto prefab = Prefab::FromGameObject(go);
		if (!go->GetFileId())
		{
			res->m_gameObjects.Add(prefab);
		}
		else
		{
			PrefabPtr basePrefab;
			App::GetSubmodule<PrefabImporter>()->LoadPrefab_Immediate(go->GetFileId(), basePrefab);

			PrefabPtr overridePrefab;
			if (prefab->GetOverridePrefab(basePrefab, overridePrefab))
			{
				res->m_gameObjects.Add(overridePrefab);
			}
			else
			{
				res->m_gameObjects.Add(prefab);
			}
		}
	}
	return res;
}

WorldPrefabImporter::WorldPrefabImporter(WorldPrefabAssetInfoHandler* infoHandler)
{
	SAILOR_PROFILE_FUNCTION();
	m_allocator = ObjectAllocatorPtr::Make(EAllocationPolicy::SharedMemory_MultiThreaded);
	infoHandler->Subscribe(this);
}

WorldPrefabImporter::~WorldPrefabImporter()
{
	for (auto& model : m_loadedWorldPrefabs)
	{
		model.m_second.DestroyObject(m_allocator);
	}
}

WorldPrefabPtr WorldPrefabImporter::Create()
{
	return WorldPrefabPtr::Make(m_allocator, FileId());
}

void WorldPrefabImporter::OnUpdateAssetInfo(AssetInfoPtr assetInfo, bool bWasExpired)
{
	SAILOR_PROFILE_FUNCTION();
	SAILOR_PROFILE_TEXT(assetInfo->GetAssetFilepath().c_str());

	if (WorldPrefabAssetInfoPtr modelAssetInfo = dynamic_cast<WorldPrefabAssetInfoPtr>(assetInfo))
	{
	}
}

void WorldPrefabImporter::OnImportAsset(AssetInfoPtr assetInfo) {}

bool WorldPrefabImporter::LoadWorld_Immediate(FileId uid, WorldPrefabPtr& outWorldPrefab)
{
	SAILOR_PROFILE_FUNCTION();

	auto task = LoadWorld(uid, outWorldPrefab);
	task->Wait();
	return task->GetResult().IsValid();
}

Tasks::TaskPtr<WorldPrefabPtr> WorldPrefabImporter::LoadWorld(FileId uid, WorldPrefabPtr& outWorldPrefab)
{
	SAILOR_PROFILE_FUNCTION();

	// Check promises first
	auto& promise = m_promises.At_Lock(uid, nullptr);
	auto& loadedWorldPrefab = m_loadedWorldPrefabs.At_Lock(uid, WorldPrefabPtr());

	// Check loaded assets
	if (loadedWorldPrefab)
	{
		outWorldPrefab = loadedWorldPrefab;
		auto res = promise ? promise : Tasks::TaskPtr<WorldPrefabPtr>::Make(outWorldPrefab);

		m_loadedWorldPrefabs.Unlock(uid);
		m_promises.Unlock(uid);

		return res;
	}

	// There is no promise, we need to load WorldPrefab
	if (WorldPrefabAssetInfoPtr assetInfo = App::GetSubmodule<AssetRegistry>()->GetAssetInfoPtr<WorldPrefabAssetInfoPtr>(uid))
	{
		SAILOR_PROFILE_TEXT(assetInfo->GetAssetFilepath().c_str());

		WorldPrefabPtr pWorldPrefab = WorldPrefabPtr::Make(m_allocator, uid);

		struct Data {};
		promise = Tasks::CreateTaskWithResult<TSharedPtr<Data>>("Load WorldPrefab",
			[pWorldPrefab, assetInfo, this]() mutable
			{
				TSharedPtr<Data> res = TSharedPtr<Data>::Make();

				std::string text;
				AssetRegistry::ReadTextFile(assetInfo->GetAssetFilepath(), text);

				YAML::Node node = YAML::Load(text);
				pWorldPrefab->Deserialize(node);
				pWorldPrefab->m_bIsReady = true;

				return res;

			})->Then<WorldPrefabPtr>([pWorldPrefab](TSharedPtr<Data> data) mutable
				{
					return pWorldPrefab;
				}, "Preload resources", Tasks::EThreadType::RHI)->ToTaskWithResult();

				outWorldPrefab = loadedWorldPrefab = pWorldPrefab;
				promise->Run();

				m_loadedWorldPrefabs.Unlock(uid);
				m_promises.Unlock(uid);

				return promise;
	}

	outWorldPrefab = nullptr;
	m_loadedWorldPrefabs.Unlock(uid);
	m_promises.Unlock(uid);

	return Tasks::TaskPtr<WorldPrefabPtr>();
}

bool WorldPrefabImporter::LoadAsset(FileId uid, TObjectPtr<Object>& out, bool bImmediate)
{
	WorldPrefabPtr outAsset;
	if (bImmediate)
	{
		bool bRes = LoadWorld_Immediate(uid, outAsset);
		out = outAsset;
		return bRes;
	}

	LoadWorld(uid, outAsset);
	out = outAsset;
	return true;
}

void WorldPrefabImporter::CollectGarbage()
{
	TVector<FileId> uidsToRemove;

	m_promises.LockAll();
	auto ids = m_promises.GetKeys();
	m_promises.UnlockAll();

	for (const auto& id : ids)
	{
		auto promise = m_promises.At_Lock(id);

		if (!promise.IsValid() || (promise.IsValid() && promise->IsFinished()))
		{
			FileId uid = id;
			uidsToRemove.Emplace(uid);
		}

		m_promises.Unlock(id);
	}

	for (auto& uid : uidsToRemove)
	{
		m_promises.Remove(uid);
	}
}