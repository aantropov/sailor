#include "AssetRegistry/GlobalIllumination/GIProbesImporter.h"

#include "AssetRegistry/AssetRegistry.h"
#include "GlobalIllumination/GIProbesBinary.h"
#include "Core/LogMacros.h"

#include <limits>

using namespace Sailor;

GIProbesAsset::GIProbesAsset(FileId uid) : Object(std::move(uid))
{}

bool GIProbesAsset::IsReady() const
{
	return m_bReady.load(std::memory_order_acquire);
}

uint64_t GIProbesAsset::GetRevision() const noexcept
{
	return m_revision.load(std::memory_order_acquire);
}

GIProbesAssetSnapshot GIProbesAsset::GetSnapshot() const
{
	m_lock.Lock();
	GIProbesAssetSnapshot snapshot;
	snapshot.m_data = m_data;
	snapshot.m_revision = m_revision.load(std::memory_order_relaxed);
	snapshot.m_diagnostic = m_diagnostic;
	m_lock.Unlock();
	return snapshot;
}

void GIProbesAsset::ApplyLoadResult(
	GIProbesDataPtr data,
	std::string diagnostic)
{
	m_lock.Lock();
	m_data = std::move(data);
	m_diagnostic = std::move(diagnostic);
	m_revision.fetch_add(1u, std::memory_order_release);
	m_bReady.store(static_cast<bool>(m_data), std::memory_order_release);
	m_lock.Unlock();
}

GIProbesImporter::GIProbesImporter(
	GIProbesAssetInfoHandler* infoHandler) :
	m_infoHandler(infoHandler)
{
	m_allocator = Memory::ObjectAllocatorPtr::Make(
		Memory::EAllocationPolicy::SharedMemory_MultiThreaded);
	if (m_infoHandler)
	{
		m_infoHandler->Subscribe(this);
	}
}

GIProbesImporter::~GIProbesImporter()
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

void GIProbesImporter::OnImportAsset(AssetInfoPtr)
{}

void GIProbesImporter::OnUpdateAssetInfo(
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
		GIProbesAssetPtr asset = (*it).m_second;
		if (asset && ImportGIProbes(uid, asset))
		{
#ifdef SAILOR_EDITOR
			asset->TraceHotReload(nullptr);
#endif
		}
	}
}

bool GIProbesImporter::LoadAsset(
	FileId uid,
	TObjectPtr<Object>& out,
	bool bImmediate)
{
	GIProbesAssetPtr asset;
	if (bImmediate)
	{
		const bool bLoaded = LoadGIProbes_Immediate(uid, asset);
		out = asset;
		return bLoaded;
	}

	Tasks::TaskPtr<GIProbesAssetPtr> task = LoadGIProbes(uid, asset);
	out = asset;
	return task.IsValid();
}

Tasks::TaskPtr<GIProbesAssetPtr> GIProbesImporter::LoadGIProbes(
	FileId uid,
	GIProbesAssetPtr& outAsset)
{
	auto& promise = m_promises.At_Lock(uid, nullptr);
	auto& loadedAsset = m_loadedAssets.At_Lock(uid, GIProbesAssetPtr{});
	if (loadedAsset)
	{
		outAsset = loadedAsset;
		auto result = promise
			? promise
			: Tasks::TaskPtr<GIProbesAssetPtr>::Make(outAsset);
		m_loadedAssets.Unlock(uid);
		m_promises.Unlock(uid);
		return result;
	}

	if (!promise)
	{
		GIProbesAssetPtr asset = GIProbesAssetPtr::Make(m_allocator, uid);
		promise = Tasks::CreateTaskWithResult<GIProbesAssetPtr>(
			"Load GI Probes",
			[this, uid, asset]() mutable
			{
				GIProbesAssetPtr imported = asset;
				ImportGIProbes(uid, imported);
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

bool GIProbesImporter::LoadGIProbes_Immediate(
	FileId uid,
	GIProbesAssetPtr& outAsset)
{
	Tasks::TaskPtr<GIProbesAssetPtr> task = LoadGIProbes(uid, outAsset);
	if (!task)
	{
		return false;
	}
	task->Wait();
	return outAsset && outAsset->IsReady();
}

void GIProbesImporter::RetainRuntimeGIProbes(FileId uid)
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

void GIProbesImporter::ReleaseRuntimeGIProbes(FileId uid)
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
		TryEvictReleasedGIProbes(uid);
	}
}

void GIProbesImporter::TryEvictReleasedGIProbes(FileId uid)
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

	Tasks::TaskPtr<GIProbesAssetPtr> promise;
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

bool GIProbesImporter::ImportGIProbes(
	FileId uid,
	GIProbesAssetPtr& outAsset)
{
	GIProbesAssetInfoPtr info = App::GetSubmodule<AssetRegistry>()
		->GetAssetInfoPtr<GIProbesAssetInfoPtr>(uid);
	if (!info)
	{
		return false;
	}

	GIProbesBinaryResult result = GIProbesBinary::Load(
		info->GetAssetFilepath());
	if (!outAsset)
	{
		outAsset = GIProbesAssetPtr::Make(m_allocator, uid);
	}
	outAsset->ApplyLoadResult(result.m_data, result.m_diagnostic);
	if (!result.IsSuccess())
	{
		SAILOR_LOG_ERROR(
			"Cannot load GI probes '%s': %s.",
			info->GetAssetFilepath().c_str(),
			result.m_diagnostic.c_str());
		return false;
	}
	return true;
}

void GIProbesImporter::CollectGarbage()
{
	TVector<FileId> completed;
	m_promises.LockAll();
	const TVector<FileId> ids = m_promises.GetKeys();
	m_promises.UnlockAll();
	for (const FileId& id : ids)
	{
		Tasks::TaskPtr<GIProbesAssetPtr> promise = m_promises.At_Lock(id);
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
		TryEvictReleasedGIProbes(id);
	}
}
