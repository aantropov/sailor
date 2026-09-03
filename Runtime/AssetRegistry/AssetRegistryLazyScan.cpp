#include "AssetRegistry/AssetRegistry.h"
#include "AssetRegistry/AssetRegistryInternal.h"
#include "AssetRegistry/AssetScanSourceRevisionCache.h"
#include "AssetRegistry/AssetInfo.h"
#include "Tasks/Scheduler.h"

#include <filesystem>
#include <mutex>
#include <string>
#include <utility>

using namespace Sailor;
using namespace Sailor::AssetRegistryInternal;

bool AssetRegistry::ScanContentFolderLazy()
{
	SAILOR_PROFILE_FUNCTION();
	{
		std::lock_guard<std::mutex> lock(m_assetProcessingMutex);
		m_scanProcessingTasks.Clear();
		m_bCollectScanProcessingTasks = true;
		m_bScanProcessingActive = true;
		m_bScanProcessingFailed = false;
	}

	const AssetMountDiscoveryResult discovery = DiscoverAssetMountFiles(m_contentMounts);
	for (const AssetMountDiagnostic& diagnostic : discovery.m_diagnostics)
	{
		if (diagnostic.m_code == EAssetMountDiagnosticCode::InvalidMountRoot ||
			diagnostic.m_code == EAssetMountDiagnosticCode::OverlappingMountRoot)
		{
			SAILOR_LOG_ERROR("%s", diagnostic.m_message.c_str());
		}
		else
		{
			SAILOR_LOG("%s", diagnostic.m_message.c_str());
		}
	}
	if (discovery.HasFatalErrors())
	{
		std::lock_guard<std::mutex> lock(m_assetProcessingMutex);
		m_bCollectScanProcessingTasks = false;
		m_bScanProcessingFailed = true;
		return false;
	}

	AssetScanSourceRevisionCache revisionCache;
	AssetScanSourceRevisionScope revisionScope(revisionCache);
	TVector<AssetMountCandidate> sourceCandidates;
	TSet<std::string> activeSourcePaths;
	for (const AssetMountDiscoveredFile& file : discovery.m_files)
	{
		if (Extension(file.m_virtualPath) != MetaFileExtension)
		{
			const std::string physicalPathKey = PathKey(file.m_physicalPath);
			activeSourcePaths.Insert(physicalPathKey);
			sourceCandidates.Add({file.m_mount, file.m_physicalPath, file.m_virtualPath, physicalPathKey});
		}
	}
	const AssetMountResolutionResult sourceResolution = ResolveAssetMountCandidates(std::move(sourceCandidates));
	for (const AssetMountDiagnostic& diagnostic : sourceResolution.GetDiagnostics())
	{
		SAILOR_LOG("%s", diagnostic.m_message.c_str());
	}

	TMap<std::string, AssetReadLocation> stagedContentWinners;
	TMap<std::string, std::string> effectiveVirtualPathsByPhysicalPath;
	for (const AssetMountCandidate& candidate : sourceResolution.GetCandidates())
	{
		const AssetMountCandidate* winner = sourceResolution.FindByVirtualPath(candidate.m_virtualPath);
		if (winner == nullptr || PathKey(winner->m_physicalPath) != PathKey(candidate.m_physicalPath))
		{
			continue;
		}

		AssetReadLocation location{
			candidate.m_physicalPath, candidate.m_virtualPath, candidate.m_mount.m_kind, candidate.m_mount.m_bWritable};
		if (!revisionCache.TryGet(location.m_physicalPath.generic_string(), location.m_revision))
		{
			continue;
		}
		const std::string virtualPathKey = VirtualPathKey(location.m_virtualPath);
		stagedContentWinners[virtualPathKey] = location;
		effectiveVirtualPathsByPhysicalPath[PathKey(location.m_physicalPath)] = virtualPathKey;
	}
	TMap<std::string, std::string> effectiveContentChanges;
	if (!m_contentFileWinners.IsEmpty())
	{
		for (const auto& previousWinner : m_contentFileWinners)
		{
			const auto stagedWinner = stagedContentWinners.Find(previousWinner.m_first);
			if (stagedWinner == stagedContentWinners.end() ||
				!SameEffectiveContent(*previousWinner.m_second, stagedWinner.Value()))
			{
				effectiveContentChanges[previousWinner.m_first] = previousWinner.m_second->m_virtualPath;
			}
		}
		for (const auto& stagedWinner : stagedContentWinners)
		{
			const auto previousWinner = m_contentFileWinners.Find(stagedWinner.m_first);
			if (previousWinner == m_contentFileWinners.end() ||
				!SameEffectiveContent(previousWinner.Value(), *stagedWinner.m_second))
			{
				effectiveContentChanges[stagedWinner.m_first] = stagedWinner.m_second->m_virtualPath;
			}
		}
	}

	TVector<std::pair<FileId, LazyAssetInfoRecord>> cachedRecords;
	{
		std::lock_guard<std::mutex> cacheLock(m_assetCache.m_cacheMutex);
		cachedRecords.Reserve(m_assetCache.m_cache.m_assets.Num());
		for (const auto& cached : m_assetCache.m_cache.m_assets)
		{
			const AssetCache::AssetCacheData::Entry& entry = cached.m_second;
			cachedRecords.Emplace(cached.m_first,
				LazyAssetInfoRecord{entry.m_sourcePath,
					entry.m_sourceRevision,
					entry.m_metadataFilename,
					entry.m_metadataRevision,
					entry.m_assetInfoType});
		}
	}

	TMap<FileId, LazyAssetInfoRecord> stagedLazyAssetInfos;
	TVector<FileId> expiredAssetIds;
	TSet<std::string> indexedPrimarySourcePaths;
	TSet<std::string> indexedMetadataPaths;
	TSet<FileId> liveAssetIds;
	for (const auto& cached : cachedRecords)
	{
		const FileId& fileId = cached.first;
		const LazyAssetInfoRecord& record = cached.second;
		const std::string sourcePathKey = PathKey(record.m_sourcePath);
		if (!activeSourcePaths.Contains(sourcePathKey))
		{
			continue;
		}

		FileRevision currentSourceRevision;
		if (!revisionCache.TryGet(record.m_sourcePath, currentSourceRevision))
		{
			continue;
		}
		const std::filesystem::path metadataPath =
			std::filesystem::path(record.m_sourcePath).parent_path() / record.m_metadataFilename;
		FileRevision currentMetadataRevision;
		if (!revisionCache.TryGet(metadataPath.string(), currentMetadataRevision))
		{
			continue;
		}

		indexedMetadataPaths.Insert(PathKey(metadataPath));
		liveAssetIds.Insert(fileId);
		if (!m_loadedAssetInfo.ContainsKey(fileId))
		{
			stagedLazyAssetInfos[fileId] = record;
		}
		if (PathKey(metadataPath) == PathKey(record.m_sourcePath + "." + MetaFileExtension))
		{
			indexedPrimarySourcePaths.Insert(sourcePathKey);
		}

		const bool bSourceExpired = currentSourceRevision != record.m_sourceRevision;
		const bool bMetadataExpired = currentMetadataRevision != record.m_metadataRevision;
		if (bSourceExpired || bMetadataExpired)
		{
			expiredAssetIds.Add(fileId);
		}
	}

	TVector<FileId> staleLoadedAssetIds;
	for (const auto& loaded : m_loadedAssetInfo)
	{
		AssetInfoPtr info = *loaded.m_second;
		FileRevision sourceRevision;
		FileRevision metadataRevision;
		if (info == nullptr || !activeSourcePaths.Contains(PathKey(info->GetAssetFilepath())) ||
			!revisionCache.TryGet(info->GetAssetFilepath(), sourceRevision) ||
			!revisionCache.TryGet(info->GetMetaFilepath(), metadataRevision))
		{
			staleLoadedAssetIds.Add(loaded.m_first);
			continue;
		}
		liveAssetIds.Insert(loaded.m_first);
		indexedMetadataPaths.Insert(PathKey(info->GetMetaFilepath()));
		if (PathKey(info->GetMetaFilepath()) == PathKey(info->GetAssetFilepath() + "." + MetaFileExtension))
		{
			indexedPrimarySourcePaths.Insert(PathKey(info->GetAssetFilepath()));
		}
	}
	for (const FileId& staleAssetId : staleLoadedAssetIds)
	{
		auto stale = m_loadedAssetInfo.Find(staleAssetId);
		if (stale != m_loadedAssetInfo.end())
		{
			delete stale.Value();
			m_loadedAssetInfo.Remove(staleAssetId);
		}
	}

	{
		std::lock_guard<std::recursive_mutex> lazyLock(m_lazyAssetInfoMutex);
		m_lazyAssetInfos = std::move(stagedLazyAssetInfos);
		m_contentMounts = discovery.m_mounts;
		m_contentFileWinners = std::move(stagedContentWinners);
		m_fileIds.Clear();
		m_physicalFileIds.Clear();

		for (const auto& lazy : m_lazyAssetInfos)
		{
			const LazyAssetInfoRecord& record = *lazy.m_second;
			const std::filesystem::path metadataPath =
				std::filesystem::path(record.m_sourcePath).parent_path() / record.m_metadataFilename;
			if (record.m_metadataFilename.empty() ||
				PathKey(metadataPath) != PathKey(record.m_sourcePath + "." + MetaFileExtension))
			{
				continue;
			}
			const auto virtualPath = effectiveVirtualPathsByPhysicalPath.Find(PathKey(record.m_sourcePath));
			if (virtualPath != effectiveVirtualPathsByPhysicalPath.end())
			{
				m_fileIds[virtualPath.Value()] = lazy.m_first;
				m_physicalFileIds[PathKey(record.m_sourcePath)] = lazy.m_first;
			}
		}
		for (const auto& loaded : m_loadedAssetInfo)
		{
			AssetInfoPtr info = *loaded.m_second;
			if (info == nullptr || !activeSourcePaths.Contains(PathKey(info->GetAssetFilepath())))
			{
				continue;
			}
			if (PathKey(info->GetMetaFilepath()) == PathKey(info->GetAssetFilepath() + "." + MetaFileExtension))
			{
				const auto virtualPath = effectiveVirtualPathsByPhysicalPath.Find(PathKey(info->GetAssetFilepath()));
				if (virtualPath != effectiveVirtualPathsByPhysicalPath.end())
				{
					m_physicalFileIds[PathKey(info->GetAssetFilepath())] = loaded.m_first;
					m_fileIds[virtualPath.Value()] = loaded.m_first;
				}
			}
		}
	}

	bool bSucceeded = true;
	TSet<std::string> handledEffectiveContentChanges;
	for (const FileId& expiredAssetId : expiredAssetIds)
	{
		const bool bWasLoaded = m_loadedAssetInfo.ContainsKey(expiredAssetId);
		AssetInfoPtr info = bWasLoaded ? m_loadedAssetInfo[expiredAssetId] : GetAssetInfoPtr_Internal(expiredAssetId);
		if (info == nullptr || (bWasLoaded && !UpdateAsset(expiredAssetId)))
		{
			bSucceeded = false;
			if (!bWasLoaded)
			{
				liveAssetIds.Remove(expiredAssetId);
				std::lock_guard<std::recursive_mutex> lazyLock(m_lazyAssetInfoMutex);
				m_lazyAssetInfos.Remove(expiredAssetId);
			}
			continue;
		}

		if (Extension(info->GetAssetFilepath()) == "glsl")
		{
			const auto virtualPath = effectiveVirtualPathsByPhysicalPath.Find(PathKey(info->GetAssetFilepath()));
			if (virtualPath != effectiveVirtualPathsByPhysicalPath.end())
			{
				handledEffectiveContentChanges.Insert(virtualPath.Value());
			}
		}
	}

	for (const auto& winner : m_contentFileWinners)
	{
		const AssetReadLocation& location = *winner.m_second;
		if (indexedPrimarySourcePaths.Contains(PathKey(location.m_physicalPath)))
		{
			continue;
		}
		const FileId& importedId = LoadFile(location.m_virtualPath);
		if (importedId)
		{
			liveAssetIds.Insert(importedId);
			if (Extension(location.m_physicalPath.string()) == "glsl")
			{
				handledEffectiveContentChanges.Insert(winner.m_first);
			}
		}
		else
		{
			bSucceeded = false;
		}
	}

	for (const AssetMountDiscoveredFile& metadataFile : discovery.m_files)
	{
		if (Extension(metadataFile.m_virtualPath) != MetaFileExtension ||
			indexedMetadataPaths.Contains(PathKey(metadataFile.m_physicalPath)))
		{
			continue;
		}

		std::string fileIdString;
		std::string filename;
		std::string assetInfoType;
		std::string metadataError;
		if (!ReadMetadataIdentity(metadataFile.m_physicalPath, fileIdString, filename, assetInfoType, metadataError))
		{
			SAILOR_LOG_ERROR("Invalid new asset metadata '%s': %s.",
				metadataFile.m_physicalPath.string().c_str(),
				metadataError.c_str());
			continue;
		}

		std::error_code sourceError;
		const std::filesystem::path sourcePath =
			std::filesystem::weakly_canonical(metadataFile.m_physicalPath.parent_path() / filename, sourceError);
		if (sourceError || !activeSourcePaths.Contains(PathKey(sourcePath)))
		{
			SAILOR_LOG_ERROR("New asset metadata references a missing source and was skipped: %s",
				metadataFile.m_physicalPath.string().c_str());
			continue;
		}

		const bool bPrimary =
			PathKey(metadataFile.m_physicalPath) == PathKey(sourcePath.string() + "." + MetaFileExtension);
		if (bPrimary)
		{
			// Primary metadata is loaded by the source import path above.
			continue;
		}

		const FileId fileId = ParseFileId(fileIdString);
		if (!fileId)
		{
			SAILOR_LOG_ERROR(
				"New secondary metadata contains an invalid FileId: %s", metadataFile.m_physicalPath.string().c_str());
			continue;
		}
		AssetInfoPtr existingInfo = GetAssetInfoPtr_Internal(fileId);
		if (existingInfo != nullptr)
		{
			if (PathKey(existingInfo->GetMetaFilepath()) != PathKey(metadataFile.m_physicalPath))
			{
				SAILOR_LOG_ERROR("New secondary metadata collides with an active FileId and was skipped: %s",
					metadataFile.m_physicalPath.string().c_str());
			}
			continue;
		}

		std::filesystem::path handlerPath(metadataFile.m_virtualPath);
		handlerPath.replace_extension();
		IAssetInfoHandler* handler = GetAssetInfoHandler(Extension(handlerPath.string()), assetInfoType, false);
		if (handler == nullptr)
		{
			SAILOR_LOG_ERROR(
				"Cannot find a handler for new secondary metadata: %s", metadataFile.m_physicalPath.string().c_str());
			continue;
		}

		AssetInfoPtr info = handler->LoadAssetInfo(metadataFile.m_physicalPath.string(),
			metadataFile.m_virtualPath,
			metadataFile.m_mount.m_kind,
			metadataFile.m_mount.m_bWritable,
			false,
			false);
		if (info == nullptr || info->GetFileId() != fileId || PathKey(info->GetAssetFilepath()) != PathKey(sourcePath))
		{
			delete info;
			SAILOR_LOG_ERROR(
				"New secondary metadata failed identity validation: %s", metadataFile.m_physicalPath.string().c_str());
			continue;
		}

		m_loadedAssetInfo[fileId] = info;
		handler->NotifyUpdateAssetInfo(info);
		CacheAsset(info);
		liveAssetIds.Insert(fileId);
		indexedMetadataPaths.Insert(PathKey(metadataFile.m_physicalPath));
	}

	for (const auto& loaded : m_loadedAssetInfo)
	{
		AssetInfoPtr info = *loaded.m_second;
		FileRevision metadataRevision;
		if (info != nullptr && activeSourcePaths.Contains(PathKey(info->GetAssetFilepath())) &&
			revisionCache.TryGet(info->GetMetaFilepath(), metadataRevision))
		{
			liveAssetIds.Insert(loaded.m_first);
		}
	}
	for (const auto& effectiveChange : effectiveContentChanges)
	{
		if (handledEffectiveContentChanges.Contains(effectiveChange.m_first) ||
			Extension(*effectiveChange.m_second) != "glsl")
		{
			continue;
		}
		for (IAssetRegistryContentListener* listener : m_contentListeners)
		{
			if (listener != nullptr)
			{
				TrackScanProcessingTask(listener->OnEffectiveContentChanged(*effectiveChange.m_second));
			}
		}
	}
	m_assetCache.Prune(liveAssetIds);
	m_assetCache.SaveCache();
	{
		std::lock_guard<std::mutex> lock(m_assetProcessingMutex);
		m_bCollectScanProcessingTasks = false;
	}
	return bSucceeded;
}
