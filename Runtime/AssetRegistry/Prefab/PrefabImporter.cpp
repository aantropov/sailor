#include "PrefabImporter.h"
#include "AssetRegistry/FileId.h"
#include "AssetRegistry/AssetRegistry.h"
#include "PrefabAssetInfo.h"
#include "Core/Utils.h"
#include <filesystem>
#include <fstream>
#include <algorithm>
#include <iostream>
#include "Memory/ObjectAllocator.hpp"
#include "Tasks/Scheduler.h"
#include "Engine/GameObject.h"
#include "ECS/TransformECS.h"

using namespace Sailor;

YAML::Node Prefab::ReflectionData::Serialize() const
{
	YAML::Node outData;

	SERIALIZE_PROPERTY(outData, m_name);
	SERIALIZE_PROPERTY(outData, m_position);
	SERIALIZE_PROPERTY(outData, m_rotation);
	SERIALIZE_PROPERTY(outData, m_scale);
	SERIALIZE_PROPERTY(outData, m_parentIndex);
	SERIALIZE_PROPERTY(outData, m_instanceId);
	SERIALIZE_PROPERTY(outData, m_components);

	return outData;
}

void Prefab::ReflectionData::Deserialize(const YAML::Node& inData)
{
	DESERIALIZE_PROPERTY(inData, m_name);
	DESERIALIZE_PROPERTY(inData, m_position);
	DESERIALIZE_PROPERTY(inData, m_rotation);
	DESERIALIZE_PROPERTY(inData, m_scale);
	DESERIALIZE_PROPERTY(inData, m_parentIndex);
	DESERIALIZE_PROPERTY(inData, m_instanceId);
	DESERIALIZE_PROPERTY(inData, m_components);
}

YAML::Node Prefab::Serialize() const
{
	YAML::Node outData;

	SERIALIZE_PROPERTY(outData, m_components);
	SERIALIZE_PROPERTY(outData, m_gameObjects);

	return outData;
}

void Prefab::Deserialize(const YAML::Node& inData)
{
	DESERIALIZE_PROPERTY(inData, m_components);
	DESERIALIZE_PROPERTY(inData, m_gameObjects);
}

bool Prefab::SaveToFile(const std::string& path) const
{
	AssetRegistry::WriteTextFile(path, Serialize());
	return true;
}

void Prefab::SerializeGameObject(GameObjectPtr root, uint32_t parentIndex, TVector<ReflectionInfo>& components, TVector<Prefab::ReflectionData>& gameObjects)
{
	Prefab::ReflectionData rootData{};

	auto& transform = root->GetTransformComponent();
	rootData.m_position = transform.GetPosition();
	rootData.m_rotation = transform.GetRotation();
	rootData.m_scale = transform.GetScale();

	rootData.m_name = root->GetName();
	rootData.m_parentIndex = parentIndex;

	for (auto& component : root->GetComponents())
	{
		auto reflection = component->GetReflectionInfo();

		rootData.m_components.Add((uint32_t)components.Num());
		components.Add(reflection);
	}

	parentIndex = (uint32_t)gameObjects.Num();
	gameObjects.Add(rootData);

	for (auto& child : root->GetChildren())
	{
		SerializeGameObject(child, parentIndex, components, gameObjects);
	}
}

PrefabPtr Prefab::FromGameObject(GameObjectPtr root)
{
	PrefabPtr res = App::GetSubmodule<PrefabImporter>()->Create();

	SerializeGameObject(root, -1, res->m_components, res->m_gameObjects);

	return res;
}

PrefabImporter::PrefabImporter(PrefabAssetInfoHandler* infoHandler)
{
	SAILOR_PROFILE_FUNCTION();
	m_allocator = ObjectAllocatorPtr::Make(EAllocationPolicy::SharedMemory_MultiThreaded);
	infoHandler->Subscribe(this);
}

PrefabImporter::~PrefabImporter()
{
	for (auto& model : m_loadedPrefabs)
	{
		model.m_second.DestroyObject(m_allocator);
	}
}

PrefabPtr PrefabImporter::Create()
{
	return PrefabPtr::Make(m_allocator, FileId());
}

void PrefabImporter::OnUpdateAssetInfo(AssetInfoPtr assetInfo, bool bWasExpired)
{
	if (PrefabAssetInfoPtr modelAssetInfo = dynamic_cast<PrefabAssetInfoPtr>(assetInfo))
	{
	}
}

void PrefabImporter::OnImportAsset(AssetInfoPtr assetInfo) {}

bool PrefabImporter::LoadPrefab_Immediate(FileId uid, PrefabPtr& outPrefab)
{
	SAILOR_PROFILE_FUNCTION();

	auto task = LoadPrefab(uid, outPrefab);
	task->Wait();
	return task->GetResult().IsValid();
}

Tasks::TaskPtr<PrefabPtr> PrefabImporter::LoadPrefab(FileId uid, PrefabPtr& outPrefab)
{
	SAILOR_PROFILE_FUNCTION();

	// Check promises first
	auto& promise = m_promises.At_Lock(uid, nullptr);
	auto& loadedPrefab = m_loadedPrefabs.At_Lock(uid, PrefabPtr());

	// Check loaded assets
	if (loadedPrefab)
	{
		outPrefab = loadedPrefab;
		auto res = promise ? promise : Tasks::TaskPtr<PrefabPtr>::Make(outPrefab);

		m_loadedPrefabs.Unlock(uid);
		m_promises.Unlock(uid);

		return res;
	}

	// There is no promise, we need to load prefab
	if (PrefabAssetInfoPtr assetInfo = App::GetSubmodule<AssetRegistry>()->GetAssetInfoPtr<PrefabAssetInfoPtr>(uid))
	{
		PrefabPtr prefab = PrefabPtr::Make(m_allocator, uid);

		struct Data {};
		promise = Tasks::CreateTaskWithResult<TSharedPtr<Data>>("Load prefab",
			[prefab, assetInfo, this]() mutable
			{
				TSharedPtr<Data> res = TSharedPtr<Data>::Make();

				std::string text;
				AssetRegistry::ReadTextFile(assetInfo->GetAssetFilepath(), text);

				YAML::Node node = YAML::Load(text);
				prefab->Deserialize(node);

				return res;

			})->Then<PrefabPtr>([prefab](TSharedPtr<Data> data) mutable
				{
					return prefab;
				}, "Preload resources", Tasks::EThreadType::RHI)->ToTaskWithResult();

				outPrefab = loadedPrefab = prefab;
				promise->Run();

				m_loadedPrefabs.Unlock(uid);
				m_promises.Unlock(uid);

				return promise;
	}

	outPrefab = nullptr;
	m_loadedPrefabs.Unlock(uid);
	m_promises.Unlock(uid);

	return Tasks::TaskPtr<PrefabPtr>();
}

bool PrefabImporter::LoadAsset(FileId uid, TObjectPtr<Object>& out, bool bImmediate)
{
	PrefabPtr outAsset;
	bool bRes = LoadPrefab_Immediate(uid, outAsset);
	out = outAsset;
	return bRes;
}

void PrefabImporter::CollectGarbage()
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