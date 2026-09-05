#include "AssetRegistry/AssetRegistry.h"
#include "AssetRegistry/AssetRegistryInternal.h"
#include "AssetRegistry/AssetInfo.h"
#include "Core/Utils.h"

#include <filesystem>
#include <mutex>
#include <string>

using namespace Sailor;
using namespace Sailor::AssetRegistryInternal;

bool AssetRegistry::UpdateAsset(const FileId& fileId)
{
	SAILOR_PROFILE_FUNCTION();

	AssetInfoPtr targetAssetInfo = GetAssetInfoPtr_Internal(fileId);
	if (targetAssetInfo == nullptr)
	{
		SAILOR_LOG_ERROR("Cannot update an unregistered asset: %s", fileId.ToString().c_str());
		return false;
	}

	auto isCurrentProcessingPending = [this](AssetInfoPtr assetInfo)
	{
		if (assetInfo == nullptr)
		{
			return false;
		}

		FileRevision currentSourceRevision;
		if (!Utils::TryGetFileRevision(assetInfo->GetAssetFilepath(), currentSourceRevision))
		{
			return false;
		}

		std::lock_guard<std::mutex> lock(m_assetProcessingMutex);
		auto processingState = m_assetProcessingStates.Find(assetInfo->GetFileId());
		if (processingState == m_assetProcessingStates.end() || processingState.Value().m_bRejected)
		{
			return false;
		}

		const AssetProcessingToken& token = processingState.Value().m_token;
		return token && token.m_fileId == assetInfo->GetFileId() &&
			   PathKey(token.m_sourcePath) == PathKey(assetInfo->GetAssetFilepath()) &&
			   token.m_sourceRevision == currentSourceRevision &&
			   assetInfo->m_importedSourceRevision == currentSourceRevision;
	};

	struct AssetExpirationState final
	{
		bool m_bMetadataExpired = false;
		bool m_bSourceExpired = false;
		bool m_bCacheExpired = false;

		explicit operator bool() const noexcept
		{
			return m_bMetadataExpired || m_bSourceExpired || m_bCacheExpired;
		}
	};

	auto getExpirationState = [this](AssetInfoPtr assetInfo)
	{
		AssetExpirationState result;
		if (assetInfo != nullptr)
		{
			result.m_bMetadataExpired = assetInfo->IsMetaExpired();
			result.m_bSourceExpired = assetInfo->IsAssetExpired();
			result.m_bCacheExpired = IsAssetExpired(assetInfo);
		}
		return result;
	};

	const std::string sharedSourcePath = PathKey(targetAssetInfo->GetAssetFilepath());
	const FileRevision initialTargetSourceRevision = targetAssetInfo->m_importedSourceRevision;
	TVector<AssetInfoPtr> assetsToUpdate;
	assetsToUpdate.Add(targetAssetInfo);
	bool bSharedSourceFamilyAdded = false;
	auto addSharedSourceFamily = [&]()
	{
		if (bSharedSourceFamilyAdded)
		{
			return;
		}

		bSharedSourceFamilyAdded = true;
		for (const auto& loadedAsset : m_loadedAssetInfo)
		{
			AssetInfoPtr assetInfo = *loadedAsset.m_second;
			if (assetInfo != nullptr && assetInfo != targetAssetInfo &&
				PathKey(assetInfo->GetAssetFilepath()) == sharedSourcePath)
			{
				assetsToUpdate.Add(assetInfo);
			}
		}
	};

	bool bReloadedAny = false;
	bool bSucceeded = true;
	for (size_t index = 0; index < assetsToUpdate.Num(); ++index)
	{
		AssetInfoPtr assetInfo = assetsToUpdate[index];
		const AssetExpirationState expiration = getExpirationState(assetInfo);
		if (!expiration)
		{
			continue;
		}

		if (assetInfo == targetAssetInfo && expiration.m_bSourceExpired)
		{
			addSharedSourceFamily();
		}

		if (!expiration.m_bMetadataExpired && !expiration.m_bSourceExpired && expiration.m_bCacheExpired &&
			isCurrentProcessingPending(assetInfo))
		{
			continue;
		}

		IAssetInfoHandler* handler = assetInfo->GetHandler();
		if (handler == nullptr || !handler->ReloadAssetInfo(assetInfo, true, false))
		{
			SAILOR_LOG_ERROR("Asset update failed; preserving the previous live asset where possible: %s",
				assetInfo->GetMetaFilepath().c_str());
			bSucceeded = false;
			break;
		}

		CacheAsset(assetInfo);
		bReloadedAny = true;
		if (assetInfo == targetAssetInfo && assetInfo->m_importedSourceRevision != initialTargetSourceRevision)
		{
			addSharedSourceFamily();
		}
	}

	for (AssetInfoPtr assetInfo : assetsToUpdate)
	{
		if (!bSucceeded)
		{
			break;
		}

		const AssetExpirationState expiration = getExpirationState(assetInfo);
		if (expiration.m_bMetadataExpired || expiration.m_bSourceExpired ||
			(expiration.m_bCacheExpired && !isCurrentProcessingPending(assetInfo)))
		{
			SAILOR_LOG_ERROR("Asset changed while its targeted update was being committed: %s",
				assetInfo->GetAssetFilepath().c_str());
			bSucceeded = false;
		}
	}

	const bool bCacheSaved = !bReloadedAny || m_assetCache.SaveCache();
	return bSucceeded && bCacheSaved;
}

FileId AssetRegistry::RegisterGeneratedSecondaryAssetInfo(const std::filesystem::path& metadataPath)
{
	if (m_scheduler != nullptr && !m_scheduler->IsMainThread())
	{
		SAILOR_LOG_ERROR("Generated secondary asset metadata may only be registered from the main thread: %s",
			metadataPath.generic_string().c_str());
		return FileId::Invalid;
	}

	std::error_code pathError;
	const std::filesystem::path canonicalMetadataPath = std::filesystem::weakly_canonical(metadataPath, pathError);
	if (pathError || !std::filesystem::is_regular_file(canonicalMetadataPath, pathError) || pathError)
	{
		SAILOR_LOG_ERROR(
			"Generated secondary asset metadata is not a regular file: %s", metadataPath.generic_string().c_str());
		return FileId::Invalid;
	}

	const AssetMountDescriptor* metadataMount = nullptr;
	for (const AssetMountDescriptor& mount : m_contentMounts)
	{
		if (mount.m_kind == EAssetMountKind::Workspace && mount.m_bWritable &&
			IsInside(mount.m_root, canonicalMetadataPath))
		{
			metadataMount = &mount;
			break;
		}
	}
	if (metadataMount == nullptr)
	{
		SAILOR_LOG_ERROR("Generated secondary asset metadata must be inside the writable workspace Content mount: %s",
			canonicalMetadataPath.generic_string().c_str());
		return FileId::Invalid;
	}

	std::string fileIdString;
	std::string filename;
	std::string assetInfoType;
	std::string metadataError;
	if (!ReadMetadataIdentity(canonicalMetadataPath, fileIdString, filename, assetInfoType, metadataError))
	{
		SAILOR_LOG_ERROR("Cannot register generated secondary asset metadata '%s': %s.",
			canonicalMetadataPath.generic_string().c_str(),
			metadataError.c_str());
		return FileId::Invalid;
	}

	pathError.clear();
	const std::filesystem::path canonicalSourcePath =
		std::filesystem::weakly_canonical(canonicalMetadataPath.parent_path() / filename, pathError);
	if (pathError || !std::filesystem::is_regular_file(canonicalSourcePath, pathError) || pathError ||
		!IsInside(metadataMount->m_root, canonicalSourcePath))
	{
		SAILOR_LOG_ERROR("Generated secondary asset metadata '%s' references an invalid source '%s'.",
			canonicalMetadataPath.generic_string().c_str(),
			filename.c_str());
		return FileId::Invalid;
	}

	const FileId expectedFileId = ParseFileId(fileIdString);
	if (!expectedFileId)
	{
		SAILOR_LOG_ERROR("Generated secondary asset metadata contains an invalid FileId: %s",
			canonicalMetadataPath.generic_string().c_str());
		return FileId::Invalid;
	}

	auto existingAssetInfo = m_loadedAssetInfo.Find(expectedFileId);
	if (existingAssetInfo != m_loadedAssetInfo.end())
	{
		if (existingAssetInfo.Value() != nullptr &&
			PathKey(existingAssetInfo.Value()->GetMetaFilepath()) == PathKey(canonicalMetadataPath))
		{
			AssetInfoPtr existingInfo = existingAssetInfo.Value();
			// A caller may have atomically replaced the metadata within the same
			// filesystem timestamp tick. Always reload this explicit registration.
			IAssetInfoHandler* existingHandler = existingInfo->GetHandler();
			const bool bHadPendingUpdate = existingInfo->m_bPendingUpdateNotification;
			const bool bHadPendingWasExpired = existingInfo->m_bPendingWasExpired;
			const bool bHadPendingImport = existingInfo->m_bPendingImportNotification;
			if (existingHandler == nullptr || !existingHandler->ReloadAssetInfo(existingInfo, false, false))
			{
				SAILOR_LOG_ERROR("Cannot refresh generated secondary asset metadata: %s",
					canonicalMetadataPath.generic_string().c_str());
				return FileId::Invalid;
			}
			existingInfo->m_bPendingUpdateNotification = bHadPendingUpdate;
			existingInfo->m_bPendingWasExpired = bHadPendingWasExpired;
			existingInfo->m_bPendingImportNotification = bHadPendingImport;
			return expectedFileId;
		}

		SAILOR_LOG_ERROR("Generated secondary asset metadata '%s' collides with an active FileId.",
			canonicalMetadataPath.generic_string().c_str());
		return FileId::Invalid;
	}

	for (const auto& loadedAsset : m_loadedAssetInfo)
	{
		if (loadedAsset.m_second != nullptr && *loadedAsset.m_second != nullptr &&
			PathKey((*loadedAsset.m_second)->GetMetaFilepath()) == PathKey(canonicalMetadataPath))
		{
			SAILOR_LOG_ERROR("Generated secondary asset metadata path is already registered with another FileId: %s",
				canonicalMetadataPath.generic_string().c_str());
			return FileId::Invalid;
		}
	}

	const std::string virtualMetadataPath =
		canonicalMetadataPath.lexically_relative(metadataMount->m_root).generic_string();
	if (!IsSafeVirtualPath(virtualMetadataPath))
	{
		SAILOR_LOG_ERROR(
			"Generated secondary asset metadata has an unsafe virtual path: %s", virtualMetadataPath.c_str());
		return FileId::Invalid;
	}

	const std::string handlerPath = std::filesystem::path(virtualMetadataPath).replace_extension().generic_string();
	IAssetInfoHandler* handler = GetAssetInfoHandler(Extension(handlerPath), assetInfoType, false);
	if (handler == nullptr)
	{
		SAILOR_LOG_ERROR("Cannot find an asset info handler for generated metadata: %s",
			canonicalMetadataPath.generic_string().c_str());
		return FileId::Invalid;
	}

	AssetInfoPtr assetInfo = handler->LoadAssetInfo(canonicalMetadataPath.string(),
		virtualMetadataPath,
		metadataMount->m_kind,
		metadataMount->m_bWritable,
		false,
		false);
	if (assetInfo == nullptr || assetInfo->GetFileId() != expectedFileId ||
		PathKey(assetInfo->GetMetaFilepath()) != PathKey(canonicalMetadataPath) ||
		PathKey(assetInfo->GetAssetFilepath()) != PathKey(canonicalSourcePath))
	{
		delete assetInfo;
		SAILOR_LOG_ERROR("Generated secondary asset metadata failed identity or path validation: %s",
			canonicalMetadataPath.generic_string().c_str());
		return FileId::Invalid;
	}

	assetInfo->m_bPendingUpdateNotification = false;
	assetInfo->m_bPendingWasExpired = false;
	assetInfo->m_bPendingImportNotification = false;
	m_loadedAssetInfo[expectedFileId] = assetInfo;
	return expectedFileId;
}
