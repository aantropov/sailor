#include "AssetRegistry/GlobalIllumination/ProbeVolumeImporter.h"

#include "AssetRegistry/AssetRegistry.h"
#include "AssetRegistry/GlobalIllumination/ProbeVolumeBinary.h"
#include "Core/LogMacros.h"

#include <limits>

using namespace Sailor;

ProbeVolumeAsset::ProbeVolumeAsset(FileId uid) : Object(std::move(uid))
{}

bool ProbeVolumeAsset::IsReady() const
{
	return m_bReady.load(std::memory_order_acquire);
}

uint64_t ProbeVolumeAsset::GetRevision() const noexcept
{
	return m_revision.load(std::memory_order_acquire);
}

ProbeVolumeAssetSnapshot ProbeVolumeAsset::GetSnapshot() const
{
	m_lock.Lock();
	ProbeVolumeAssetSnapshot snapshot;
	snapshot.m_data = m_data;
	snapshot.m_revision = m_revision.load(std::memory_order_relaxed);
	snapshot.m_diagnostic = m_diagnostic;
	m_lock.Unlock();
	return snapshot;
}

void ProbeVolumeAsset::ApplyLoadResult(
	ProbeVolumeDataPtr data,
	std::string diagnostic)
{
	m_lock.Lock();
	m_data = std::move(data);
	m_diagnostic = std::move(diagnostic);
	m_revision.fetch_add(1u, std::memory_order_release);
	m_bReady.store(static_cast<bool>(m_data), std::memory_order_release);
	m_lock.Unlock();
}

ProbeVolumeImporter::ProbeVolumeImporter(
	ProbeVolumeAssetInfoHandler* infoHandler) :
	m_infoHandler(infoHandler)
{
	m_allocator = Memory::ObjectAllocatorPtr::Make(
		Memory::EAllocationPolicy::SharedMemory_MultiThreaded);
	if (m_infoHandler)
	{
		m_infoHandler->Subscribe(this);
	}
}

ProbeVolumeImporter::~ProbeVolumeImporter()
{
	if (m_infoHandler)
	{
		m_infoHandler->Unsubscribe(this);
	}
	for (auto& asset : m_loadedAssets)
	{
		asset.m_second.DestroyObject(m_allocator);
	}
}

void ProbeVolumeImporter::OnImportAsset(AssetInfoPtr)
{}

void ProbeVolumeImporter::OnUpdateAssetInfo(
	AssetInfoPtr assetInfo,
	bool bWasExpired)
{
	if (!assetInfo || !bWasExpired)
	{
		return;
	}

	const FileId uid = assetInfo->GetFileId();
	if (auto it = m_loadedAssets.Find(uid); it != m_loadedAssets.end())
	{
		ProbeVolumeAssetPtr asset = (*it).m_second;
		if (asset && ImportProbeVolume(uid, asset))
		{
#ifdef SAILOR_EDITOR
			asset->TraceHotReload(nullptr);
#endif
		}
	}
}

bool ProbeVolumeImporter::LoadAsset(
	FileId uid,
	TObjectPtr<Object>& out,
	bool bImmediate)
{
	ProbeVolumeAssetPtr asset;
	if (bImmediate)
	{
		const bool bLoaded = LoadProbeVolume_Immediate(uid, asset);
		out = asset;
		return bLoaded;
	}

	Tasks::TaskPtr<ProbeVolumeAssetPtr> task = LoadProbeVolume(uid, asset);
	out = asset;
	return task.IsValid();
}

Tasks::TaskPtr<ProbeVolumeAssetPtr> ProbeVolumeImporter::LoadProbeVolume(
	FileId uid,
	ProbeVolumeAssetPtr& outAsset)
{
	auto& promise = m_promises.At_Lock(uid, nullptr);
	auto& loadedAsset = m_loadedAssets.At_Lock(uid, ProbeVolumeAssetPtr{});
	if (loadedAsset)
	{
		outAsset = loadedAsset;
		auto result = promise
			? promise
			: Tasks::TaskPtr<ProbeVolumeAssetPtr>::Make(outAsset);
		m_loadedAssets.Unlock(uid);
		m_promises.Unlock(uid);
		return result;
	}

	if (!promise)
	{
		ProbeVolumeAssetPtr asset = ProbeVolumeAssetPtr::Make(m_allocator, uid);
		promise = Tasks::CreateTaskWithResult<ProbeVolumeAssetPtr>(
			"Load Probe Volume",
			[this, uid, asset]() mutable
			{
				ProbeVolumeAssetPtr imported = asset;
				ImportProbeVolume(uid, imported);
				return imported;
			},
			EThreadType::Worker);
		outAsset = loadedAsset = asset;
		promise->Run();
	}
	else
	{
		outAsset = loadedAsset;
	}

	m_loadedAssets.Unlock(uid);
	m_promises.Unlock(uid);
	return promise;
}

bool ProbeVolumeImporter::LoadProbeVolume_Immediate(
	FileId uid,
	ProbeVolumeAssetPtr& outAsset)
{
	Tasks::TaskPtr<ProbeVolumeAssetPtr> task = LoadProbeVolume(uid, outAsset);
	if (!task)
	{
		return false;
	}
	task->Wait();
	return outAsset && outAsset->IsReady();
}

void ProbeVolumeImporter::RetainRuntimeProbeVolume(FileId uid)
{
	if (!uid)
	{
		return;
	}
	uint32_t& count = m_runtimeRetentions.At_Lock(uid, 0u);
	if (count != (std::numeric_limits<uint32_t>::max)())
	{
		++count;
	}
	m_runtimeRetentions.Unlock(uid);
}

void ProbeVolumeImporter::ReleaseRuntimeProbeVolume(FileId uid)
{
	if (!uid || !m_runtimeRetentions.ContainsKey(uid))
	{
		return;
	}
	uint32_t& count = m_runtimeRetentions.At_Lock(uid, 0u);
	if (count > 0u)
	{
		--count;
	}
	const bool bCanEvict = count == 0u;
	m_runtimeRetentions.Unlock(uid);
	if (bCanEvict)
	{
		TryEvictReleasedProbeVolume(uid);
	}
}

void ProbeVolumeImporter::TryEvictReleasedProbeVolume(FileId uid)
{
	if (!m_runtimeRetentions.ContainsKey(uid))
	{
		return;
	}
	uint32_t& count = m_runtimeRetentions.At_Lock(uid, 0u);
	const bool bReleased = count == 0u;
	m_runtimeRetentions.Unlock(uid);
	if (!bReleased)
	{
		return;
	}

	Tasks::TaskPtr<ProbeVolumeAssetPtr> promise;
	if (m_promises.ContainsKey(uid))
	{
		promise = m_promises.At_Lock(uid);
		m_promises.Unlock(uid);
		if (promise && !promise->IsFinished())
		{
			return;
		}
		m_promises.Remove(uid);
	}
	m_loadedAssets.Remove(uid);
	m_runtimeRetentions.Remove(uid);
}

bool ProbeVolumeImporter::ImportProbeVolume(
	FileId uid,
	ProbeVolumeAssetPtr& outAsset)
{
	ProbeVolumeAssetInfoPtr info = App::GetSubmodule<AssetRegistry>()
		->GetAssetInfoPtr<ProbeVolumeAssetInfoPtr>(uid);
	if (!info)
	{
		return false;
	}

	ProbeVolumeBinaryResult result = ProbeVolumeBinary::Load(
		info->GetAssetFilepath());
	if (!outAsset)
	{
		outAsset = ProbeVolumeAssetPtr::Make(m_allocator, uid);
	}
	outAsset->ApplyLoadResult(result.m_data, result.m_diagnostic);
	if (!result.IsSuccess())
	{
		SAILOR_LOG_ERROR(
			"Cannot load probe volume '%s': %s.",
			info->GetAssetFilepath().c_str(),
			result.m_diagnostic.c_str());
		return false;
	}
	return true;
}

void ProbeVolumeImporter::CollectGarbage()
{
	TVector<FileId> completed;
	m_promises.LockAll();
	const TVector<FileId> ids = m_promises.GetKeys();
	m_promises.UnlockAll();
	for (const FileId& id : ids)
	{
		Tasks::TaskPtr<ProbeVolumeAssetPtr> promise = m_promises.At_Lock(id);
		if (!promise || promise->IsFinished())
		{
			completed.Add(id);
		}
		m_promises.Unlock(id);
	}
	for (const FileId& id : completed)
	{
		m_promises.Remove(id);
	}

	m_runtimeRetentions.LockAll();
	const TVector<FileId> retainedIds = m_runtimeRetentions.GetKeys();
	m_runtimeRetentions.UnlockAll();
	for (const FileId& id : retainedIds)
	{
		TryEvictReleasedProbeVolume(id);
	}
}
