#pragma once

#include "AssetRegistry/AssetRegistry.h"

#include <atomic>
#include <filesystem>
#include <string>

namespace Sailor
{
	class AssetScanSourceRevisionCache;
}

namespace Sailor::AssetRegistryInternal
{
	extern std::atomic<uint64_t> g_nextAssetProcessingGeneration;
	extern thread_local AssetScanSourceRevisionCache* g_activeSourceRevisionCache;

	struct StagedAssetRecord final
	{
		AssetMountCandidate m_candidate;
		std::filesystem::path m_assetPath;
		std::filesystem::path m_metaPath;
		std::string m_assetVirtualPath;
		std::string m_metaVirtualPath;
		std::string m_assetInfoType;
		bool m_bPrimary = false;
		bool m_bMetadataInvalid = false;
	};

	struct PendingAssetNotification final
	{
		IAssetInfoHandler* m_handler = nullptr;
		AssetInfoPtr m_assetInfo = nullptr;
		bool m_bImported = false;
		bool m_bNotifyUpdate = true;
	};

	struct ImportedMetadata final
	{
		IAssetInfoHandler* m_handler = nullptr;
		AssetInfoPtr m_assetInfo = nullptr;
	};

	std::string AsFolderPath(const std::filesystem::path& path);
	std::string Lowercase(std::string value);
	std::string PathKey(const std::filesystem::path& path);
	std::string VirtualPathKey(std::string path);
	bool IsSafeVirtualPath(const std::string& path);
	bool IsInside(const std::filesystem::path& root, const std::filesystem::path& candidate);
	std::string MountFileKey(const AssetMountDescriptor& mount, const std::string& virtualPath);
	std::string Extension(const std::string& path);
	std::string HandlerExtension(const StagedAssetRecord& record);
	bool CandidateMatches(const AssetMountCandidate* winner, const AssetMountCandidate& candidate);
	bool SameEffectiveContent(const AssetRegistry::AssetReadLocation& left,
		const AssetRegistry::AssetReadLocation& right);
	bool ReadMetadataIdentity(const std::filesystem::path& metaPath,
		std::string& outFileId,
		std::string& outFilename,
		std::string& outAssetInfoType,
		std::string& outError);
	FileId ParseFileId(const std::string& value);
	void DeleteAssetInfos(TMap<FileId, AssetInfoPtr>& assetInfos);
}
