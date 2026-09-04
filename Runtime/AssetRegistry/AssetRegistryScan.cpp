#include "AssetRegistry/AssetRegistry.h"
#include "AssetRegistry/AssetRegistryInternal.h"
#include "AssetRegistry/AssetScanSourceRevisionCache.h"
#include "AssetRegistry/AssetInfo.h"
#include "Containers/Map.h"
#include "Tasks/Scheduler.h"

#include <filesystem>
#include <mutex>
#include <string>
#include <utility>

using namespace Sailor;
using namespace Sailor::AssetRegistryInternal;

bool AssetRegistry::ScanContentFolder()
{
	SAILOR_PROFILE_FUNCTION();
	bool bHasLazyIndex = false;
	if (g_bUseLazyAssetInfoLoading)
	{
		std::lock_guard<std::mutex> cacheLock(m_assetCache.m_cacheMutex);
		bHasLazyIndex = m_assetCache.m_cache.m_assets.Num() > 0;
	}
	if (bHasLazyIndex)
	{
		return ScanContentFolderLazy();
	}

	AssetScanSourceRevisionCache sourceRevisionCache;
	AssetScanSourceRevisionScope sourceRevisionScope(sourceRevisionCache);
	{
		std::lock_guard<std::mutex> lock(m_assetProcessingMutex);
		m_scanProcessingTasks.Clear();
		m_bCollectScanProcessingTasks = false;
		m_bScanProcessingActive = false;
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
		SAILOR_LOG_ERROR("Asset content mounts are invalid; preserving the previous registry generation.");
		return false;
	}

	TMap<std::string, const AssetMountDiscoveredFile*> filesByMountAndVirtualPath;
	TSet<std::string> metadataFileKeys;
	for (const AssetMountDiscoveredFile& file : discovery.m_files)
	{
		const std::string key = MountFileKey(file.m_mount, file.m_virtualPath);
		filesByMountAndVirtualPath.Insert(key, &file);
		if (Extension(file.m_virtualPath) == MetaFileExtension)
		{
			metadataFileKeys.Insert(key);
		}
	}

	TVector<StagedAssetRecord> records;
	TSet<std::string> consumedPrimaryMetadata;
	TSet<std::string> invalidMetadata;
	for (const AssetMountDiscoveredFile& metaFile : discovery.m_files)
	{
		if (Extension(metaFile.m_virtualPath) != MetaFileExtension)
		{
			continue;
		}

		std::string fileId;
		std::string filename;
		std::string assetInfoType;
		std::string metadataError;
		const std::string metaKey = MountFileKey(metaFile.m_mount, metaFile.m_virtualPath);
		if (!ReadMetadataIdentity(metaFile.m_physicalPath, fileId, filename, assetInfoType, metadataError))
		{
			invalidMetadata.Insert(metaKey);
			SAILOR_LOG_ERROR("Invalid asset metadata '%s': %s.",
				metaFile.m_physicalPath.generic_string().c_str(),
				metadataError.c_str());
			const std::string invalidMetaPath = PathKey(metaFile.m_physicalPath);
			bool bInvalidatesLiveAsset = false;
			for (const auto& loadedAsset : m_loadedAssetInfo)
			{
				if (loadedAsset.m_second != nullptr && *loadedAsset.m_second != nullptr &&
					PathKey((*loadedAsset.m_second)->GetMetaFilepath()) == invalidMetaPath)
				{
					bInvalidatesLiveAsset = true;
					break;
				}
			}
			if (bInvalidatesLiveAsset)
			{
				SAILOR_LOG_ERROR("Asset metadata reload was rejected; preserving the previous registry generation.");
				return false;
			}
			continue;
		}
		const std::string metadataPhysicalPath = PathKey(metaFile.m_physicalPath);
		for (const auto& loadedAsset : m_loadedAssetInfo)
		{
			if (loadedAsset.m_second != nullptr && *loadedAsset.m_second != nullptr &&
				PathKey((*loadedAsset.m_second)->GetMetaFilepath()) == metadataPhysicalPath &&
				(*loadedAsset.m_second)->GetFileId() != ParseFileId(fileId))
			{
				SAILOR_LOG_ERROR(
					"Asset metadata FileId changed during reload; preserving the previous registry generation: %s",
					metaFile.m_physicalPath.generic_string().c_str());
				return false;
			}
		}

		const std::string basenameVirtualPath =
			std::filesystem::path(metaFile.m_virtualPath).replace_extension().generic_string();
		const auto basenameAsset = filesByMountAndVirtualPath.Find(MountFileKey(metaFile.m_mount, basenameVirtualPath));
		const bool bFilenameMatchesBasename = std::filesystem::path(filename).generic_string() ==
											  std::filesystem::path(basenameVirtualPath).filename().generic_string();

		StagedAssetRecord record;
		record.m_candidate.m_mount = metaFile.m_mount;
		record.m_candidate.m_fileId = fileId;
		record.m_metaPath = metaFile.m_physicalPath;
		record.m_metaVirtualPath = metaFile.m_virtualPath;
		record.m_assetInfoType = assetInfoType;
		if (basenameAsset != filesByMountAndVirtualPath.end() && bFilenameMatchesBasename)
		{
			record.m_bPrimary = true;
			record.m_assetPath = basenameAsset.Value()->m_physicalPath;
			record.m_assetVirtualPath = basenameAsset.Value()->m_virtualPath;
			record.m_candidate.m_physicalPath = record.m_assetPath;
			record.m_candidate.m_virtualPath = record.m_assetVirtualPath;
			consumedPrimaryMetadata.Insert(metaKey);
		}
		else
		{
			const std::string declaredVirtualPath =
				(std::filesystem::path(metaFile.m_virtualPath).parent_path() / filename).generic_string();
			const auto declaredAsset =
				filesByMountAndVirtualPath.Find(MountFileKey(metaFile.m_mount, declaredVirtualPath));
			if (declaredAsset == filesByMountAndVirtualPath.end())
			{
				SAILOR_LOG_ERROR("Asset metadata '%s' references missing source '%s' and was skipped.",
					metaFile.m_physicalPath.generic_string().c_str(),
					declaredVirtualPath.c_str());
				continue;
			}

			record.m_assetPath = declaredAsset.Value()->m_physicalPath;
			record.m_assetVirtualPath = declaredAsset.Value()->m_virtualPath;
			record.m_candidate.m_physicalPath = record.m_metaPath;
			record.m_candidate.m_virtualPath = record.m_metaVirtualPath;
		}
		record.m_candidate.m_fileId = fileId;
		records.Add(std::move(record));
	}

	for (const AssetMountDiscoveredFile& assetFile : discovery.m_files)
	{
		if (Extension(assetFile.m_virtualPath) == MetaFileExtension)
		{
			continue;
		}

		const std::string metaVirtualPath = assetFile.m_virtualPath + "." + MetaFileExtension;
		const std::string metaKey = MountFileKey(assetFile.m_mount, metaVirtualPath);
		if (consumedPrimaryMetadata.Contains(metaKey))
		{
			continue;
		}

		StagedAssetRecord record;
		record.m_candidate.m_mount = assetFile.m_mount;
		record.m_candidate.m_physicalPath = assetFile.m_physicalPath;
		record.m_candidate.m_virtualPath = assetFile.m_virtualPath;
		record.m_assetPath = assetFile.m_physicalPath;
		record.m_assetVirtualPath = assetFile.m_virtualPath;
		record.m_metaPath = assetFile.m_physicalPath.string() + "." + MetaFileExtension;
		record.m_metaVirtualPath = metaVirtualPath;
		record.m_bPrimary = true;
		record.m_bMetadataInvalid = invalidMetadata.Contains(metaKey) ||
									(metadataFileKeys.Contains(metaKey) && !consumedPrimaryMetadata.Contains(metaKey));
		records.Add(std::move(record));
	}

	TVector<AssetMountCandidate> candidates;
	candidates.Reserve(records.Num());
	for (const StagedAssetRecord& record : records)
	{
		candidates.Add(record.m_candidate);
	}
	const AssetMountResolutionResult resolution = ResolveAssetMountCandidates(std::move(candidates));
	for (const AssetMountDiagnostic& diagnostic : resolution.GetDiagnostics())
	{
		SAILOR_LOG("%s", diagnostic.m_message.c_str());
	}

	TMap<std::string, AssetReadLocation> stagedContentWinners;
	for (const StagedAssetRecord& record : records)
	{
		if (!record.m_bPrimary ||
			!CandidateMatches(resolution.FindByVirtualPath(record.m_candidate.m_virtualPath), record.m_candidate))
		{
			continue;
		}

		AssetReadLocation location{record.m_assetPath,
			record.m_assetVirtualPath,
			record.m_candidate.m_mount.m_kind,
			record.m_candidate.m_mount.m_bWritable};
		if (!sourceRevisionCache.TryGet(location.m_physicalPath.generic_string(), location.m_revision))
		{
			SAILOR_LOG_ERROR(
				"Failed to capture effective Content revision; preserving the previous registry generation: %s",
				location.m_physicalPath.generic_string().c_str());
			return false;
		}
		stagedContentWinners[VirtualPathKey(record.m_assetVirtualPath)] = std::move(location);
	}

	TMap<std::string, std::string> effectiveContentChanges;
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

	TMap<FileId, AssetInfoPtr> stagedAssetInfos;
	TMap<std::string, FileId> stagedFileIds;
	TMap<std::string, FileId> stagedPhysicalFileIds;
	TVector<PendingAssetNotification> pendingNotifications;
	TVector<ImportedMetadata> importedMetadata;
	auto rollbackStaging = [&]()
	{
		for (size_t index = importedMetadata.Num(); index > 0; --index)
		{
			importedMetadata[index - 1].m_handler->DiscardImportedMetadataIfUnchanged(
				importedMetadata[index - 1].m_assetInfo);
		}
		DeleteAssetInfos(stagedAssetInfos);
	};
	for (const StagedAssetRecord& record : records)
	{
		const bool bVirtualWinner =
			CandidateMatches(resolution.FindByVirtualPath(record.m_candidate.m_virtualPath), record.m_candidate);
		if (record.m_candidate.m_fileId.empty())
		{
			if (!record.m_bPrimary || !bVirtualWinner || record.m_bMetadataInvalid)
			{
				continue;
			}
			if (!record.m_candidate.m_mount.m_bWritable)
			{
				SAILOR_LOG_ERROR("Read-only Engine asset '%s' has no metadata and was not imported.",
					record.m_assetVirtualPath.c_str());
				const std::string missingMetaAssetPath = PathKey(record.m_assetPath);
				for (const auto& loadedAsset : m_loadedAssetInfo)
				{
					if (loadedAsset.m_second != nullptr && *loadedAsset.m_second != nullptr &&
						PathKey((*loadedAsset.m_second)->GetAssetFilepath()) == missingMetaAssetPath)
					{
						rollbackStaging();
						SAILOR_LOG_ERROR(
							"Read-only asset metadata disappeared; preserving the previous registry generation.");
						return false;
					}
				}
				continue;
			}

			IAssetInfoHandler* handler = GetAssetInfoHandler(Extension(record.m_assetVirtualPath));
			check(handler);
			std::error_code metadataStatusError;
			const std::filesystem::file_status metadataStatus =
				std::filesystem::symlink_status(record.m_metaPath, metadataStatusError);
			if ((metadataStatusError && metadataStatusError != std::errc::no_such_file_or_directory &&
					metadataStatusError != std::errc::not_a_directory) ||
				std::filesystem::exists(metadataStatus))
			{
				rollbackStaging();
				SAILOR_LOG_ERROR("Asset metadata appeared during staged import; preserving it and the previous "
								 "registry generation: %s",
					record.m_metaPath.generic_string().c_str());
				return false;
			}
			AssetInfoPtr assetInfo =
				handler->ImportAsset(record.m_assetPath.string(), record.m_assetVirtualPath, false, false);
			if (assetInfo == nullptr)
			{
				rollbackStaging();
				SAILOR_LOG_ERROR("Failed to import asset metadata during staged load: %s",
					record.m_metaPath.generic_string().c_str());
				return false;
			}
			importedMetadata.Add({handler, assetInfo});
			stagedAssetInfos[assetInfo->GetFileId()] = assetInfo;
			stagedFileIds[VirtualPathKey(record.m_assetVirtualPath)] = assetInfo->GetFileId();
			stagedPhysicalFileIds[PathKey(record.m_assetPath)] = assetInfo->GetFileId();
			pendingNotifications.Add({handler, assetInfo, true, true});
			continue;
		}

		if (!CandidateMatches(resolution.FindByFileId(record.m_candidate.m_fileId), record.m_candidate))
		{
			continue;
		}

		const FileId expectedFileId = ParseFileId(record.m_candidate.m_fileId);
		auto previousAssetInfo = m_loadedAssetInfo.Find(expectedFileId);
		const bool bEffectiveContentChanged =
			bVirtualWinner && effectiveContentChanges.ContainsKey(VirtualPathKey(record.m_assetVirtualPath));
		const bool bWasPreviouslyExpired =
			previousAssetInfo != m_loadedAssetInfo.end() &&
			(previousAssetInfo.Value()->IsMetaExpired() || previousAssetInfo.Value()->IsAssetExpired() ||
				PathKey(previousAssetInfo.Value()->GetAssetFilepath()) != PathKey(record.m_assetPath) ||
				bEffectiveContentChanged);

		IAssetInfoHandler* handler =
			GetAssetInfoHandler(HandlerExtension(record), record.m_assetInfoType, record.m_bPrimary);
		check(handler);
		AssetInfoPtr assetInfo = handler->LoadAssetInfo(record.m_metaPath.string(),
			record.m_metaVirtualPath,
			record.m_candidate.m_mount.m_kind,
			record.m_candidate.m_mount.m_bWritable,
			false,
			false);
		if (assetInfo == nullptr)
		{
			rollbackStaging();
			SAILOR_LOG_ERROR(
				"Failed to load asset metadata during staged load: %s", record.m_metaPath.generic_string().c_str());
			return false;
		}
		if (assetInfo->GetFileId().ToString() != record.m_candidate.m_fileId)
		{
			delete assetInfo;
			rollbackStaging();
			SAILOR_LOG_ERROR(
				"Asset metadata FileId changed during staged load: %s", record.m_metaPath.generic_string().c_str());
			return false;
		}
		if (PathKey(assetInfo->GetMetaFilepath()) != PathKey(record.m_metaPath) ||
			PathKey(assetInfo->GetAssetFilepath()) != PathKey(record.m_assetPath))
		{
			delete assetInfo;
			rollbackStaging();
			SAILOR_LOG_ERROR(
				"Asset metadata paths changed during staged load; preserving the previous registry generation: %s",
				record.m_metaPath.generic_string().c_str());
			return false;
		}

		const FileId fileId = assetInfo->GetFileId();
		assetInfo->m_bPendingWasExpired |= bWasPreviouslyExpired;
		const bool bNotifyUpdate = previousAssetInfo == m_loadedAssetInfo.end() || assetInfo->m_bPendingWasExpired;
		assetInfo->m_bPendingUpdateNotification = bNotifyUpdate;
		stagedAssetInfos[fileId] = assetInfo;
		if (record.m_bPrimary)
		{
			stagedPhysicalFileIds[PathKey(record.m_assetPath)] = fileId;
		}
		if (record.m_bPrimary && bVirtualWinner)
		{
			stagedFileIds[VirtualPathKey(record.m_assetVirtualPath)] = fileId;
		}
		pendingNotifications.Add({handler, assetInfo, false, bNotifyUpdate});
	}

	for (const StagedAssetRecord& record : records)
	{
		if (!record.m_bPrimary || record.m_candidate.m_fileId.empty() ||
			!CandidateMatches(resolution.FindByVirtualPath(record.m_candidate.m_virtualPath), record.m_candidate))
		{
			continue;
		}

		const FileId fileId = ParseFileId(record.m_candidate.m_fileId);
		if (stagedAssetInfos.ContainsKey(fileId))
		{
			stagedFileIds[VirtualPathKey(record.m_assetVirtualPath)] = fileId;
			stagedPhysicalFileIds[PathKey(record.m_assetPath)] = fileId;
		}
	}

	for (const auto& stagedAsset : stagedAssetInfos)
	{
		AssetInfoPtr assetInfo = *stagedAsset.m_second;
		if (assetInfo == nullptr || assetInfo->IsAssetExpired() || assetInfo->IsMetaExpired())
		{
			const std::string changedPath =
				assetInfo == nullptr ? std::string("<null asset info>") : assetInfo->GetAssetFilepath();
			rollbackStaging();
			SAILOR_LOG_ERROR(
				"Asset source or metadata changed during staged load; preserving the previous registry generation: %s",
				changedPath.c_str());
			return false;
		}
	}

	TSet<std::string> handledEffectiveContentChanges;
	for (const PendingAssetNotification& pending : pendingNotifications)
	{
		if ((!pending.m_bNotifyUpdate && !pending.m_bImported) || pending.m_assetInfo == nullptr ||
			Extension(pending.m_assetInfo->GetAssetFilepath()) != "glsl")
		{
			continue;
		}

		const std::string virtualPath = pending.m_assetInfo->GetRelativeAssetFilepath();
		const std::string virtualPathKey = VirtualPathKey(virtualPath);
		const auto effectiveWinner = stagedContentWinners.Find(virtualPathKey);
		if (effectiveWinner != stagedContentWinners.end() &&
			PathKey(effectiveWinner.Value().m_physicalPath) == PathKey(pending.m_assetInfo->GetAssetFilepath()))
		{
			handledEffectiveContentChanges.Insert(virtualPathKey);
		}
	}

	if (Tasks::Scheduler* scheduler = m_scheduler)
	{
		if (!scheduler->IsMainThread())
		{
			rollbackStaging();
			SAILOR_LOG_ERROR("Asset registry generations may only be committed from the main thread; preserving the "
							 "previous generation.");
			return false;
		}
		scheduler->WaitIdle({EThreadType::Main, EThreadType::Worker, EThreadType::RHI, EThreadType::Render});
	}

	for (const auto& stagedAsset : stagedAssetInfos)
	{
		AssetInfoPtr assetInfo = *stagedAsset.m_second;
		if (assetInfo == nullptr || assetInfo->IsMetaExpired())
		{
			const std::string changedMetadataPath =
				assetInfo == nullptr ? std::string("<null asset info>") : assetInfo->GetMetaFilepath();
			rollbackStaging();
			SAILOR_LOG_ERROR(
				"Asset metadata changed during the pre-commit wait; preserving the previous registry generation: %s",
				changedMetadataPath.c_str());
			return false;
		}
	}

	std::string changedSourcePath;
	if (!sourceRevisionCache.ValidateAll(changedSourcePath))
	{
		rollbackStaging();
		SAILOR_LOG_ERROR("Asset source changed during staged load; preserving the previous registry generation: %s",
			changedSourcePath.c_str());
		return false;
	}

	TMap<FileId, AssetInfoPtr> previousAssetInfos = std::move(m_loadedAssetInfo);
	m_loadedAssetInfo = std::move(stagedAssetInfos);
	m_fileIds = std::move(stagedFileIds);
	m_physicalFileIds = std::move(stagedPhysicalFileIds);
	m_contentMounts = discovery.m_mounts;
	m_contentFileWinners = std::move(stagedContentWinners);
	DeleteAssetInfos(previousAssetInfos);
	{
		std::lock_guard<std::mutex> lock(m_assetProcessingMutex);
		m_bCollectScanProcessingTasks = true;
		m_bScanProcessingActive = true;
	}
	// Listener callbacks can change a source. Start a fresh commit snapshot so
	// cache acknowledgements still inspect each shared source once and only record
	// the post-callback revision when it matches the imported revision.
	sourceRevisionCache.Reset();

	for (const PendingAssetNotification& pending : pendingNotifications)
	{
		if (pending.m_bNotifyUpdate)
		{
			pending.m_handler->NotifyUpdateAssetInfo(pending.m_assetInfo);
		}
		if (pending.m_bImported)
		{
			pending.m_handler->NotifyImportAsset(pending.m_assetInfo);
		}
		if (pending.m_bNotifyUpdate || pending.m_bImported)
		{
			CacheAsset(pending.m_assetInfo);
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
	{
		std::lock_guard<std::mutex> lock(m_assetProcessingMutex);
		m_bCollectScanProcessingTasks = false;
	}
	TSet<FileId> liveAssetIds;
	for (const auto& loadedAsset : m_loadedAssetInfo)
	{
		liveAssetIds.Insert(loadedAsset.m_first);
	}
	{
		std::lock_guard<std::mutex> lock(m_assetProcessingMutex);
		TVector<FileId> staleProcessingStates;
		for (const auto& processingState : m_assetProcessingStates)
		{
			if (!liveAssetIds.Contains(processingState.m_first))
			{
				staleProcessingStates.Add(processingState.m_first);
			}
		}
		for (const FileId& staleFileId : staleProcessingStates)
		{
			m_assetProcessingStates.Remove(staleFileId);
		}
	}
	m_assetCache.Prune(liveAssetIds);
	m_assetCache.SaveCache();
	return true;
}
