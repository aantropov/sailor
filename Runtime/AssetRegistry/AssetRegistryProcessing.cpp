#include "AssetRegistry/AssetRegistry.h"
#include "AssetRegistry/AssetRegistryInternal.h"
#include "AssetRegistry/AssetScanSourceRevisionCache.h"
#include "AssetRegistry/AssetInfo.h"
#include "Core/Utils.h"

#include <filesystem>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

using namespace Sailor;
using namespace Sailor::AssetRegistryInternal;

bool AssetScanSourceRevisionCache::TryGet(const std::string& physicalSourcePath, FileRevision& outRevision)
{
	const std::string key = PathKey(physicalSourcePath);
	std::lock_guard<std::mutex> lock(m_mutex);
	Entry* cached = nullptr;
	if (m_entries.Find(key, cached) && cached != nullptr)
	{
		outRevision = cached->m_revision;
		return cached->m_bSucceeded;
	}

	Entry entry;
	entry.m_physicalPath = physicalSourcePath;
	entry.m_bSucceeded = Utils::TryGetFileRevision(physicalSourcePath, entry.m_revision);
	outRevision = entry.m_revision;
	m_entries[key] = entry;
	return entry.m_bSucceeded;
}

bool AssetScanSourceRevisionCache::ValidateAll(std::string& outChangedPhysicalPath) const
{
	outChangedPhysicalPath.clear();
	TVector<Entry> entries;
	{
		std::lock_guard<std::mutex> lock(m_mutex);
		entries.Reserve(m_entries.Num());
		for (const auto& cached : m_entries)
		{
			entries.Emplace(*cached.m_second);
		}
	}

	for (const Entry& entry : entries)
	{
		FileRevision currentRevision;
		const bool bSucceeded = Utils::TryGetFileRevision(entry.m_physicalPath, currentRevision);
		if (bSucceeded != entry.m_bSucceeded || (bSucceeded && currentRevision != entry.m_revision))
		{
			outChangedPhysicalPath = entry.m_physicalPath;
			return false;
		}
	}

	return true;
}

void AssetScanSourceRevisionCache::Reset()
{
	std::lock_guard<std::mutex> lock(m_mutex);
	m_entries.Clear();
}

AssetScanSourceRevisionScope::AssetScanSourceRevisionScope(AssetScanSourceRevisionCache& cache) noexcept
	: m_previous(g_activeSourceRevisionCache)
{
	g_activeSourceRevisionCache = &cache;
}

AssetScanSourceRevisionScope::~AssetScanSourceRevisionScope() noexcept
{
	g_activeSourceRevisionCache = m_previous;
}

AssetScanSourceRevisionCache* Sailor::GetActiveAssetScanSourceRevisionCache() noexcept
{
	return g_activeSourceRevisionCache;
}

bool AssetRegistry::RestoreAssetImportTime(AssetInfoPtr info, const FileRevision& sourceRevision)
{
	if (info == nullptr)
	{
		return false;
	}

	std::lock_guard<std::mutex> lock(m_assetProcessingMutex);
	if (m_assetProcessingStates.ContainsKey(info->GetFileId()))
	{
		return false;
	}
	return m_assetCache.RestoreAssetImportTime(info, sourceRevision);
}

void AssetRegistry::CacheAsset(const AssetInfoPtr info)
{
	if (info == nullptr)
	{
		return;
	}

	std::lock_guard<std::mutex> lock(m_assetProcessingMutex);
	if (m_assetProcessingStates.ContainsKey(info->GetFileId()))
	{
		return;
	}
	// Keep the gate locked through Update so BeginAssetProcessing cannot publish
	// a pending revision between the state check and the cache acknowledgement.
	AssetScanSourceRevisionCache* sourceRevisionCache = GetActiveAssetScanSourceRevisionCache();
	if (sourceRevisionCache == nullptr)
	{
		m_assetCache.Update(info);
		return;
	}

	FileRevision sourceRevision;
	if (!sourceRevisionCache->TryGet(info->GetAssetFilepath(), sourceRevision))
	{
		m_assetCache.Remove(info->GetFileId());
		return;
	}
	if (!info->m_importedSourceRevision.m_bIsValid || info->m_importedSourceRevision != sourceRevision)
	{
		m_assetCache.Remove(info->GetFileId());
		return;
	}
	m_assetCache.Update(info->GetFileId(),
		info->GetAssetImportTime(),
		info->GetAssetFilepath(),
		sourceRevision,
		std::filesystem::path(info->GetMetaFilepath()).filename().string(),
		info->m_metadataRevision,
		info->GetAssetInfoType());
}

AssetRegistry::AssetProcessingToken AssetRegistry::BeginAssetProcessing(AssetInfoPtr info)
{
	AssetProcessingToken token;
	std::lock_guard<std::mutex> lock(m_assetProcessingMutex);
	if (info == nullptr || !info->GetFileId())
	{
		m_bScanProcessingFailed |= m_bScanProcessingActive;
		return token;
	}

	// Serialize capture, generation assignment, and publication. File revisions
	// are not time-ordered (a valid edit may be backdated), so generation alone
	// cannot safely decide which concurrently captured revision is newer.
	token.m_fileId = info->GetFileId();
	token.m_sourcePath = info->GetAssetFilepath();
	const std::string metadataFilename = std::filesystem::path(info->GetMetaFilepath()).filename().string();
	FileRevision metadataRevision;
	const bool bHasMetadataRevision = Utils::TryGetFileRevision(info->GetMetaFilepath(), metadataRevision);
	const std::string assetInfoType = info->GetAssetInfoType();
	m_assetCache.Remove(token.m_fileId);
	const bool bRetryWatermarkPersisted = m_assetCache.SaveCache();
	token.m_assetImportTime = Utils::GetFileModificationTime(token.m_sourcePath);
	if (token.m_assetImportTime <= 0 || !bHasMetadataRevision || metadataFilename.empty() || assetInfoType.empty() ||
		!Utils::TryGetFileRevision(token.m_sourcePath, token.m_sourceRevision))
	{
		m_assetProcessingStates[token.m_fileId] = AssetProcessingState{token, metadataFilename, assetInfoType, true};
		m_bScanProcessingFailed |= m_bScanProcessingActive;
		SAILOR_LOG_ERROR("Cannot capture asset processing revision after invalidating its cache watermark: %s",
			token.m_sourcePath.c_str());
		return {};
	}
	if (!bRetryWatermarkPersisted)
	{
		m_assetProcessingStates[token.m_fileId] = AssetProcessingState{token, metadataFilename, assetInfoType, true};
		m_bScanProcessingFailed |= m_bScanProcessingActive;
		SAILOR_LOG_ERROR(
			"Cannot start retry-safe asset processing because its invalidated cache watermark was not persisted: %s",
			token.m_sourcePath.c_str());
		return {};
	}

	do
	{
		token.m_generation = g_nextAssetProcessingGeneration.fetch_add(1, std::memory_order_relaxed) + 1;
	} while (token.m_generation == 0);

	info->m_assetImportTime = token.m_assetImportTime;
	info->m_importedSourceRevision = token.m_sourceRevision;
	m_assetProcessingStates[token.m_fileId] = AssetProcessingState{token, metadataFilename, assetInfoType, false};
	return token;
}

void AssetRegistry::CompleteAssetProcessing(const AssetProcessingToken& token, bool bSucceeded)
{
	std::lock_guard<std::mutex> lock(m_assetProcessingMutex);
	if (!token)
	{
		return;
	}

	auto processingState = m_assetProcessingStates.Find(token.m_fileId);
	if (processingState == m_assetProcessingStates.end() || !processingState.Value().m_token.Matches(token))
	{
		return;
	}

	if (!bSucceeded)
	{
		processingState.Value().m_bRejected = true;
		m_bScanProcessingFailed |= m_bScanProcessingActive;
		SAILOR_LOG_ERROR("Asset processing failed; invalidated the cached source watermark for retry: %s",
			token.m_sourcePath.c_str());
		return;
	}

	FileRevision currentSourceRevision;
	if (!Utils::TryGetFileRevision(token.m_sourcePath, currentSourceRevision) ||
		currentSourceRevision != token.m_sourceRevision)
	{
		processingState.Value().m_bRejected = true;
		m_bScanProcessingFailed |= m_bScanProcessingActive;
		SAILOR_LOG_ERROR(
			"Asset source changed while it was being processed; invalidated its cached source watermark for retry: %s",
			token.m_sourcePath.c_str());
		return;
	}

	const AssetProcessingToken acknowledgedToken = processingState.Value().m_token;
	const std::filesystem::path metadataPath = std::filesystem::path(acknowledgedToken.m_sourcePath).parent_path() /
											   processingState.Value().m_metadataFilename;
	FileRevision currentMetadataRevision;
	if (!Utils::TryGetFileRevision(metadataPath.string(), currentMetadataRevision))
	{
		processingState.Value().m_bRejected = true;
		m_bScanProcessingFailed |= m_bScanProcessingActive;
		SAILOR_LOG_ERROR(
			"Asset metadata disappeared while its source was being processed: %s", metadataPath.string().c_str());
		return;
	}
	m_assetCache.Update(acknowledgedToken.m_fileId,
		acknowledgedToken.m_assetImportTime,
		acknowledgedToken.m_sourcePath,
		acknowledgedToken.m_sourceRevision,
		processingState.Value().m_metadataFilename,
		currentMetadataRevision,
		processingState.Value().m_assetInfoType);
	if (!m_assetCache.SaveCache())
	{
		m_assetCache.Remove(acknowledgedToken.m_fileId);
		processingState.Value().m_bRejected = true;
		m_bScanProcessingFailed |= m_bScanProcessingActive;
		SAILOR_LOG_ERROR("Cannot persist the completed asset processing watermark; preserving retry state: %s",
			token.m_sourcePath.c_str());
		return;
	}
	m_assetProcessingStates.Remove(acknowledgedToken.m_fileId);
}

void AssetRegistry::TrackScanProcessingTask(const Tasks::TaskPtr<bool>& processingTask)
{
	std::lock_guard<std::mutex> lock(m_assetProcessingMutex);
	if (m_bCollectScanProcessingTasks)
	{
		m_scanProcessingTasks.Add(processingTask);
	}
}

bool AssetRegistry::CompleteScanProcessing()
{
	TVector<Tasks::TaskPtr<bool>> processingTasks;
	{
		std::lock_guard<std::mutex> lock(m_assetProcessingMutex);
		processingTasks = std::move(m_scanProcessingTasks);
		m_scanProcessingTasks.Clear();
		m_bCollectScanProcessingTasks = false;
	}

	bool bSucceeded = true;
	for (const Tasks::TaskPtr<bool>& processingTask : processingTasks)
	{
		bSucceeded &= processingTask && processingTask->IsFinished() && processingTask->GetResult();
	}
	{
		std::lock_guard<std::mutex> lock(m_assetProcessingMutex);
		bSucceeded &= !m_bScanProcessingFailed;
		m_bScanProcessingActive = false;
		m_bScanProcessingFailed = false;
	}
	return bSucceeded;
}
