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

using namespace Sailor;

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
		PrefabPtr model = PrefabPtr::Make(m_allocator, uid);

		struct Data {};
		promise = Tasks::CreateTaskWithResult<TSharedPtr<Data>>("Load prefab",
			[model, assetInfo, this]()
			{
				TSharedPtr<Data> res = TSharedPtr<Data>::Make();
				return res;
			})->Then<PrefabPtr>([model](TSharedPtr<Data> data) mutable
				{
					return model;
				}, "Update Prefab", Tasks::EThreadType::RHI)->ToTaskWithResult();

				outPrefab = loadedPrefab = model;
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