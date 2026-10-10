#include "AssetRegistry/AssetRegistry.h"
#include "Core/FileRevision.h"
#include "AssetRegistry/AssetRegistryInternal.h"
#include "AssetRegistry/AssetFactory.h"
#include "AssetRegistry/AssetInfo.h"
#include "Core/Utils.h"
#include "Tasks/Scheduler.h"

#include <filesystem>
#include <mutex>
#include <string>
#include <utility>

using namespace Sailor;
using namespace Sailor::AssetRegistryInternal;
using namespace Sailor::Workspace;

bool AssetRegistry::ResolveDirectLoadPath(std::string_view requestedPath, AssetReadLocation& outLocation) const
{
	if (IsSafeVirtualPath(requestedPath))
	{
		std::error_code workspaceError;
		const std::filesystem::path workspaceCandidate =
			std::filesystem::weakly_canonical(m_workspaceContext.GetContent() / PathFromUtf8(requestedPath), workspaceError);
		if (!workspaceError && std::filesystem::is_regular_file(workspaceCandidate, workspaceError) &&
			!workspaceError && IsInside(m_workspaceContext.GetContent(), workspaceCandidate))
		{
			const std::string virtualPath = PathToUtf8(PathFromUtf8(requestedPath));
			const auto winner = m_contentFileWinners.Find(VirtualPathKey(virtualPath));
			if (winner == m_contentFileWinners.end() || winner.Value().m_mountKind == EAssetMountKind::Engine ||
				PathKey(winner.Value().m_physicalPath) == PathKey(workspaceCandidate))
			{
				outLocation = AssetReadLocation{workspaceCandidate, virtualPath, EAssetMountKind::Workspace, true};
				return true;
			}
		}
		if (ResolveContentFile(requestedPath, outLocation))
		{
			return true;
		}
	}

	std::error_code error;
	const auto requested = PathFromUtf8(requestedPath);
	const std::filesystem::path candidate = std::filesystem::weakly_canonical(
		requested.is_absolute() ? requested : m_workspaceContext.GetContent() / requested, error);
	if (error || !std::filesystem::is_regular_file(candidate, error) || error)
	{
		return false;
	}

	for (const AssetMountDescriptor& mount : m_contentMounts)
	{
		if (IsInside(mount.m_root, candidate))
		{
			const std::string virtualPath = PathToUtf8(candidate.lexically_relative(mount.m_root));
			const auto winner = m_contentFileWinners.Find(VirtualPathKey(virtualPath));
			if (winner != m_contentFileWinners.end() && PathKey(winner.Value().m_physicalPath) == PathKey(candidate))
			{
				outLocation = winner.Value();
				return true;
			}
			if (mount.m_kind != EAssetMountKind::Workspace || !mount.m_bWritable ||
				(winner != m_contentFileWinners.end() && winner.Value().m_mountKind == EAssetMountKind::Workspace))
			{
				return false;
			}

			outLocation.m_physicalPath = candidate;
			outLocation.m_virtualPath = virtualPath;
			outLocation.m_mountKind = EAssetMountKind::Workspace;
			outLocation.m_bWritable = true;
			return true;
		}
	}

	const std::filesystem::path cache = m_workspaceContext.GetCache();
	const std::filesystem::path tempWorld = std::filesystem::weakly_canonical(cache / "Temp.world", error);
	if (!error && PathKey(candidate) == PathKey(tempWorld))
	{
		outLocation.m_physicalPath = candidate;
		outLocation.m_virtualPath = PathToUtf8(candidate.lexically_relative(m_workspaceContext.GetContent()));
		outLocation.m_mountKind = EAssetMountKind::Workspace;
		outLocation.m_bWritable = true;
		return true;
	}
	return false;
}

const FileId& AssetRegistry::GetOrLoadFile(std::string_view assetFilepath)
{
	return LoadFile(assetFilepath);
}

void AssetRegistry::RequestAssetUpdate(const FileId& fileId)
{
	if (m_scheduler->IsMainThread())
	{
		App::UpdateAsset(fileId.ToString().c_str());
		return;
	}

	{
		std::lock_guard<std::mutex> lock(m_assetProcessingMutex);
		if (m_pendingAssetUpdates.Contains(fileId)) return;
		m_pendingAssetUpdates.Insert(fileId);
	}

	// Main may be waiting for this caller. Enqueue without entering App's
	// synchronous dispatch, and leave the live asset unchanged until Main runs.
	auto update = Tasks::CreateTask("Reload requested asset"_h, [this, fileId]()
		{
			{
				std::lock_guard<std::mutex> lock(m_assetProcessingMutex);
				m_pendingAssetUpdates.Remove(fileId);
			}
			App::UpdateAsset(fileId.ToString().c_str());
		}, EThreadType::Main);
	update->Run();
}

const FileId& AssetRegistry::LoadFile(std::string_view requestedPath)
{
	AssetReadLocation location;
	if (!ResolveDirectLoadPath(requestedPath, location))
	{
		SAILOR_LOG_ERROR("Asset file could not be resolved: %.*s",
			static_cast<int>(requestedPath.size()), requestedPath.empty() ? "" : requestedPath.data());
		return FileId::Invalid;
	}

	auto physicalId = m_physicalFileIds.Find(PathKey(location.m_physicalPath));
	if (physicalId != m_physicalFileIds.end())
	{
		AssetInfoPtr loadedInfo = GetAssetInfoPtr_Internal(physicalId.Value());
		if (loadedInfo != nullptr && !loadedInfo->m_bPendingUpdateNotification &&
			!loadedInfo->m_bPendingImportNotification &&
			(loadedInfo->IsMetaExpired() || loadedInfo->IsAssetExpired() ||
				loadedInfo->m_bPendingWasExpired || IsAssetExpired(loadedInfo)))
		{
			if (m_scheduler && App::GetSubmodule<AssetRegistry>() == this)
			{
				RequestAssetUpdate(loadedInfo->GetFileId());
				return loadedInfo->GetFileId();
			}

			SAILOR_LOG("Reload asset info: %s", loadedInfo->GetMetaFilepath().c_str());
			if (!GetAssetInfoHandler(*loadedInfo)->ReloadAssetInfo(loadedInfo, true, false))
			{
				SAILOR_LOG_ERROR("Asset reload failed; preserving the previous live asset: %s",
					loadedInfo->GetMetaFilepath().c_str());
			}
			else
			{
				CacheAsset(loadedInfo);
			}
		}
		return loadedInfo ? loadedInfo->GetFileId() : physicalId.Value();
	}

	if (Extension(PathToUtf8(location.m_physicalPath)) == MetaFileExtension)
	{
		SAILOR_LOG_ERROR("Direct metadata loading is not supported: %.*s",
			static_cast<int>(requestedPath.size()), requestedPath.data());
		return FileId::Invalid;
	}

	IAssetInfoHandler* handler = GetAssetInfoHandler(Extension(PathToUtf8(location.m_physicalPath)));
	check(handler);
	auto metaPath = location.m_physicalPath;
	metaPath += "." + std::string(MetaFileExtension);
	AssetInfoPtr assetInfo = nullptr;
	bool bImported = false;
	if (std::filesystem::is_regular_file(metaPath))
	{
		assetInfo = handler->LoadAssetInfo(PathToUtf8(metaPath),
			location.m_virtualPath + "." + MetaFileExtension,
			location.m_mountKind,
			location.m_bWritable,
			false,
			false);
	}
	else if (location.m_bWritable)
	{
		assetInfo = handler->ImportAsset(PathToUtf8(location.m_physicalPath), location.m_virtualPath, false, false);
		bImported = assetInfo != nullptr;
	}
	else
	{
		SAILOR_LOG_ERROR(
			"Read-only Engine asset '%s' has no metadata and was not imported.", location.m_virtualPath.c_str());
		return FileId::Invalid;
	}
	if (assetInfo == nullptr)
	{
		SAILOR_LOG_ERROR("Failed to load asset metadata: %s", PathToUtf8(metaPath).c_str());
		return FileId::Invalid;
	}

	const FileId fileId = assetInfo->GetFileId();
	auto existingAssetInfo = m_loadedAssetInfo.Find(fileId);
	if (existingAssetInfo != m_loadedAssetInfo.end())
	{
		const bool bSamePhysicalAsset =
			PathKey(PathFromUtf8(existingAssetInfo.Value()->GetAssetFilepath())) == PathKey(location.m_physicalPath);
		if (bImported)
		{
			handler->DiscardImportedMetadataIfUnchanged(assetInfo);
		}
		delete assetInfo;
		if (bSamePhysicalAsset)
		{
			m_physicalFileIds[PathKey(location.m_physicalPath)] = fileId;
			return existingAssetInfo.Value()->GetFileId();
		}

		SAILOR_LOG_ERROR("Live asset '%s' collides with an active FileId; rescan Content to apply mount precedence.",
			location.m_virtualPath.c_str());
		return FileId::Invalid;
	}

	m_loadedAssetInfo[fileId] = assetInfo;
	{
		std::lock_guard<std::recursive_mutex> lazyLock(m_lazyAssetInfoMutex);
		m_lazyAssetInfos.Remove(fileId);
	}
	m_physicalFileIds[PathKey(location.m_physicalPath)] = fileId;
	if (IsSafeVirtualPath(location.m_virtualPath))
	{
		const std::string virtualPathKey = VirtualPathKey(location.m_virtualPath);
		Utils::TryGetFileRevision(PathToUtf8(location.m_physicalPath), location.m_revision);
		m_fileIds[virtualPathKey] = fileId;
		m_contentFileWinners[virtualPathKey] = location;
	}
	handler->NotifyRegisterAsset(assetInfo);
	if (bImported)
	{
		handler->NotifyImportAsset(assetInfo);
	}
	CacheAsset(assetInfo);
	return m_loadedAssetInfo.Find(fileId).Value()->GetFileId();
}

TObjectPtr<Object> AssetRegistry::LoadAsset(IAssetInfoHandler* assetInfoHandler, const FileId& id, bool bImmediate)
{
	TObjectPtr<Object> result;
	assetInfoHandler->GetFactory()->LoadAsset(id, result, bImmediate);
	return result;
}

AssetInfoPtr AssetRegistry::GetAssetInfoPtr_Internal(FileId uid) const
{
	SAILOR_PROFILE_FUNCTION();

	auto it = m_loadedAssetInfo.Find(uid);
	if (it != m_loadedAssetInfo.end())
	{
		return it.Value();
	}
	return g_bUseLazyAssetInfoLoading ? MaterializeLazyAssetInfo(uid) : nullptr;
}

AssetInfoPtr AssetRegistry::MaterializeLazyAssetInfo(FileId uid) const
{
	std::lock_guard<std::recursive_mutex> lazyLock(m_lazyAssetInfoMutex);
	auto loaded = m_loadedAssetInfo.Find(uid);
	if (loaded != m_loadedAssetInfo.end())
	{
		return loaded.Value();
	}

	auto lazy = m_lazyAssetInfos.Find(uid);
	if (lazy == m_lazyAssetInfos.end())
	{
		return nullptr;
	}
	const LazyAssetInfoRecord record = lazy.Value();
	if (record.m_metadataFilename.empty() || record.m_assetInfoType.empty())
	{
		return nullptr;
	}

	const auto sourcePath = PathFromUtf8(record.m_sourcePath);
	const std::filesystem::path metadataPath = sourcePath.parent_path() / PathFromUtf8(record.m_metadataFilename);
	FileRevision currentMetadataRevision;
	if (!Utils::TryGetFileRevision(PathToUtf8(metadataPath), currentMetadataRevision))
	{
		return nullptr;
	}
	const bool bMetadataChanged = currentMetadataRevision != record.m_metadataRevision;
	std::string resolvedAssetInfoType = record.m_assetInfoType;
	if (bMetadataChanged)
	{
		std::string fileId;
		std::string filename;
		std::string assetInfoType;
		std::string metadataError;
		if (!ReadMetadataIdentity(metadataPath, fileId, filename, assetInfoType, metadataError) ||
			ParseFileId(fileId) != uid || PathKey(metadataPath.parent_path() / PathFromUtf8(filename)) != PathKey(sourcePath))
		{
			SAILOR_LOG_ERROR(
				"Changed lazy asset metadata no longer matches its cached identity: %s", PathToUtf8(metadataPath).c_str());
			return nullptr;
		}
		resolvedAssetInfoType = std::move(assetInfoType);
	}
	const AssetMountDescriptor* metadataMount = nullptr;
	for (const AssetMountDescriptor& mount : m_contentMounts)
	{
		if (IsInside(mount.m_root, metadataPath))
		{
			metadataMount = &mount;
			break;
		}
	}
	if (metadataMount == nullptr)
	{
		SAILOR_LOG_ERROR("Lazy asset metadata is outside active Content mounts: %s", PathToUtf8(metadataPath).c_str());
		return nullptr;
	}

	const bool bPrimary = PathKey(metadataPath) == PathKey(PathFromUtf8(record.m_sourcePath + "." + MetaFileExtension));
	std::filesystem::path metadataAssetPath = metadataPath;
	const std::string handlerExtension =
		bPrimary ? Extension(record.m_sourcePath) : Extension(PathToUtf8(metadataAssetPath.replace_extension()));
	AssetRegistry* registry = const_cast<AssetRegistry*>(this);
	IAssetInfoHandler* handler = registry->GetAssetInfoHandler(handlerExtension, resolvedAssetInfoType, bPrimary);
	if (handler == nullptr)
	{
		return nullptr;
	}

	// Cached paths are case-folded on Windows; the mount root must use the same spelling.
	const auto mountRootKey = PathFromUtf8(PathKey(metadataMount->m_root));
	const std::string virtualMetadataPath = PathToUtf8(PathFromUtf8(PathKey(metadataPath)).lexically_relative(mountRootKey));
	AssetInfoPtr info = handler->LoadAssetInfo(
		PathToUtf8(metadataPath), virtualMetadataPath, metadataMount->m_kind, metadataMount->m_bWritable, false, false);
	if (info == nullptr || info->GetFileId() != uid || PathKey(PathFromUtf8(info->GetAssetFilepath())) != PathKey(sourcePath))
	{
		delete info;
		SAILOR_LOG_ERROR("Lazy asset metadata no longer matches its cache index: %s", PathToUtf8(metadataPath).c_str());
		return nullptr;
	}

	registry->m_loadedAssetInfo[uid] = info;
	registry->m_lazyAssetInfos.Remove(uid);
	info->m_bPendingWasExpired |= bMetadataChanged;
	info->m_bPendingUpdateNotification = false;
	if (bPrimary)
	{
		const std::string virtualSourcePath = PathToUtf8(PathFromUtf8(PathKey(sourcePath)).lexically_relative(mountRootKey));
		const std::string virtualSourcePathKey = VirtualPathKey(virtualSourcePath);
		const auto effectiveWinner = registry->m_contentFileWinners.Find(virtualSourcePathKey);
		if (effectiveWinner != registry->m_contentFileWinners.end() &&
			PathKey(effectiveWinner.Value().m_physicalPath) == PathKey(sourcePath))
		{
			registry->m_physicalFileIds[PathKey(PathFromUtf8(record.m_sourcePath))] = uid;
			registry->m_fileIds[virtualSourcePathKey] = uid;
		}
	}
	// Metadata lookup is read-only. Import/update dispatches the pending notification
	// before acknowledging the source; listeners may write generated Content.
	return info;
}

void AssetRegistry::GetLazyAssetInfoIds(std::string_view assetInfoType, TVector<FileId>& outIds) const
{
	outIds.Clear();
	std::lock_guard<std::recursive_mutex> lazyLock(m_lazyAssetInfoMutex);
	for (const auto& lazy : m_lazyAssetInfos)
	{
		if (lazy.m_second->m_assetInfoType == assetInfoType)
		{
			outIds.Add(lazy.m_first);
		}
	}
}

void AssetRegistry::GetAssetInfoIdsByTypeAndSource(std::string_view assetInfoType,
	std::string_view sourcePath,
	TVector<FileId>& outIds) const
{
	outIds.Clear();
	TSet<FileId> resultIds;
	const std::string sourcePathKey = PathKey(PathFromUtf8(sourcePath));
	for (const auto& loaded : m_loadedAssetInfo)
	{
		AssetInfoPtr info = *loaded.m_second;
		if (info != nullptr && info->GetAssetInfoType() == assetInfoType &&
			PathKey(PathFromUtf8(info->GetAssetFilepath())) == sourcePathKey)
		{
			resultIds.Insert(loaded.m_first);
		}
	}
	{
		std::lock_guard<std::recursive_mutex> lazyLock(m_lazyAssetInfoMutex);
		for (const auto& lazy : m_lazyAssetInfos)
		{
			if (lazy.m_second->m_assetInfoType == assetInfoType &&
				PathKey(PathFromUtf8(lazy.m_second->m_sourcePath)) == sourcePathKey)
			{
				resultIds.Insert(lazy.m_first);
			}
		}
	}
	outIds.Reserve(resultIds.Num());
	for (const FileId& fileId : resultIds)
	{
		outIds.Add(fileId);
	}
}

AssetInfoPtr AssetRegistry::GetAssetInfoPtr_Internal(std::string_view assetFilepath) const
{
	if (IsSafeVirtualPath(assetFilepath))
	{
		auto virtualId = m_fileIds.Find(VirtualPathKey(std::string(assetFilepath)));
		if (virtualId != m_fileIds.end())
		{
			return GetAssetInfoPtr_Internal(virtualId.Value());
		}
	}

	AssetReadLocation location;
	if (ResolveDirectLoadPath(assetFilepath, location))
	{
		auto physicalId = m_physicalFileIds.Find(PathKey(location.m_physicalPath));
		if (physicalId != m_physicalFileIds.end())
		{
			return GetAssetInfoPtr_Internal(physicalId.Value());
		}
		if (g_bUseLazyAssetInfoLoading)
		{
			AssetRegistry* registry = const_cast<AssetRegistry*>(this);
			const FileId& loadedId = registry->LoadFile(assetFilepath);
			if (loadedId)
			{
				return GetAssetInfoPtr_Internal(loadedId);
			}
		}
	}
	return nullptr;
}
