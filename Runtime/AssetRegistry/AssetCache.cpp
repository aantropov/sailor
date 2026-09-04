#include "AssetCache.h"
#include "Containers/Containers.h"

#include "AssetRegistry/AssetRegistry.h"
#include "Containers/ConcurrentMap.h"
#include "Core/Utils.h"
#include "Core/YamlSerializable.h"
#include "Sailor.h"
#include "Tasks/Tasks.h"
#include "YamlExceptionBoundary.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <filesystem>
#include <sstream>

using namespace Sailor;

namespace
{
	std::string NormalizeSourcePath(const std::string& sourcePath)
	{
		if (sourcePath.empty())
		{
			return {};
		}

		std::error_code error;
		std::string result = std::filesystem::weakly_canonical(sourcePath, error).generic_string();
		if (error)
		{
			result = std::filesystem::path(sourcePath).lexically_normal().generic_string();
		}
#if defined(_WIN32)
		std::transform(result.begin(), result.end(), result.begin(), [](unsigned char character)
			{
				return static_cast<char>(std::tolower(character));
			});
#endif
		return result;
	}

	void AppendDiagnostic(std::string& diagnostic, const std::string& suffix)
	{
		if (suffix.empty())
		{
			return;
		}

		if (!diagnostic.empty())
		{
			diagnostic += " ";
		}
		diagnostic += suffix;
	}

}

Workspace::WorkspaceCacheIdentity AssetCache::MakeExpectedIdentity()
{
	return Workspace::MakeWorkspaceCacheIdentity(
		CacheKind,
		CacheProducer,
		PayloadVersion,
		App::GetWorkspaceContext());
}

std::string AssetCache::SerializeAssetCachePayload(const AssetCacheData& cache)
{
	YAML::Node payload(YAML::NodeType::Map);
	payload["assetCache"] = cache.Serialize();

	std::ostringstream stream;
	stream << payload;
	return stream.str();
}

bool AssetCache::TryDeserializeAssetCachePayload(
	const std::string& payload,
	AssetCacheData& outData,
	std::string& outDiagnostic) noexcept
{
	auto deserialize = [&]() -> bool
	{
		const YAML::Node root = YAML::Load(payload);
		const YAML::Node assetCache = root["assetCache"];
		if (!assetCache)
		{
			outDiagnostic = "Asset cache payload is missing 'assetCache'.";
			return false;
		}
		return AssetCacheData::TryDeserialize(assetCache, outData, outDiagnostic);
	};

	bool bResult = false;
	std::string yamlDiagnostic;
	if (!Sailor::External::TryInvokeYaml(deserialize, bResult, yamlDiagnostic))
	{
		outDiagnostic = "Asset cache payload contains invalid YAML: " + yamlDiagnostic;
		return false;
	}
	return bResult;
}

std::string AssetCache::GetAssetCacheFilepath()
{
	return (std::filesystem::path(AssetRegistry::GetCacheFolder()) / "AssetCache.yaml").string();
}

YAML::Node AssetCache::AssetCacheData::Entry::Serialize() const
{
	YAML::Node result(YAML::NodeType::Map);
	SERIALIZE_PROPERTY(result, m_fileId);
	SERIALIZE_PROPERTY(result, m_assetImportTime);
	SERIALIZE_PROPERTY(result, m_sourcePath);
	SERIALIZE_PROPERTY(result, m_sourceRevision);
	SERIALIZE_PROPERTY(result, m_metadataFilename);
	SERIALIZE_PROPERTY(result, m_metadataRevision);
	SERIALIZE_PROPERTY(result, m_assetInfoType);
	return result;
}

void AssetCache::AssetCacheData::Entry::Deserialize(const YAML::Node& inData)
{
	std::fprintf(stderr, "ACDBG entry begin\n");
	std::fflush(stderr);
	*this = Entry{};
	std::fprintf(stderr, "ACDBG entry reset\n");
	std::fflush(stderr);
	std::string yamlDiagnostic;
	const bool bDecoded = Sailor::External::GuardYamlExceptions(
		[&]()
		{
			std::fprintf(stderr, "ACDBG entry fileId\n");
			std::fflush(stderr);
			DESERIALIZE_PROPERTY(inData, m_fileId);
			std::fprintf(stderr, "ACDBG entry importTime\n");
			std::fflush(stderr);
			DESERIALIZE_PROPERTY(inData, m_assetImportTime);
			std::fprintf(stderr, "ACDBG entry sourcePath\n");
			std::fflush(stderr);
			DESERIALIZE_PROPERTY(inData, m_sourcePath);
			DESERIALIZE_PROPERTY(inData, m_sourceRevision);
			DESERIALIZE_PROPERTY(inData, m_metadataFilename);
			DESERIALIZE_PROPERTY(inData, m_metadataRevision);
			DESERIALIZE_PROPERTY(inData, m_assetInfoType);
			m_sourcePath = NormalizeSourcePath(m_sourcePath);
		},
		yamlDiagnostic);
	std::fprintf(stderr, "ACDBG entry guarded %d\n", bDecoded ? 1 : 0);
	std::fflush(stderr);
	if (!bDecoded)
	{
		*this = Entry{};
		std::fprintf(stderr, "ACDBG entry failure reset\n");
		std::fflush(stderr);
	}
	std::fprintf(stderr, "ACDBG entry complete\n");
	std::fflush(stderr);
}

bool AssetCache::AssetCacheData::Entry::Validate(
	const FileId& key,
	std::string& outDiagnostic) const
{
	if (!m_fileId || m_fileId != key)
	{
		outDiagnostic = "Asset cache entry has a mismatched fileId.";
		return false;
	}
	if (m_assetImportTime <= 0 || m_sourcePath.empty() ||
		!m_sourceRevision.m_bIsValid)
	{
		outDiagnostic = "Asset cache entry has an invalid source watermark.";
		return false;
	}
	if (m_metadataFilename.empty() ||
		std::filesystem::path(m_metadataFilename).filename() !=
			m_metadataFilename ||
		!m_metadataRevision.m_bIsValid || m_assetInfoType.empty())
	{
		outDiagnostic = "Asset cache entry has an invalid metadata index.";
		return false;
	}
	return true;
}

YAML::Node AssetCache::AssetCacheData::Serialize() const
{
	YAML::Node result(YAML::NodeType::Map);
	YAML::Node assets(YAML::NodeType::Map);
	for (const auto& asset : m_assets)
	{
		assets.force_insert(
			asset.m_first.ToString(),
			asset.m_second.Serialize());
	}
	result["assets"] = assets;
	return result;
}

void AssetCache::AssetCacheData::Deserialize(const YAML::Node& inData)
{
	std::fprintf(stderr, "ACDBG data begin\n");
	std::fflush(stderr);
	AssetCacheData candidate;
	std::string diagnostic;
	std::fprintf(stderr, "ACDBG data before try\n");
	std::fflush(stderr);
	const bool bDecoded = TryDeserialize(inData, candidate, diagnostic);
	std::fprintf(stderr, "ACDBG data after try %d\n", bDecoded ? 1 : 0);
	std::fflush(stderr);
	if (bDecoded)
	{
		m_assets = std::move(candidate.m_assets);
	}
	else
	{
		std::fprintf(stderr, "ACDBG data before clear\n");
		std::fflush(stderr);
		m_assets.Clear();
		std::fprintf(stderr, "ACDBG data after clear\n");
		std::fflush(stderr);
	}
	std::fprintf(stderr, "ACDBG data complete\n");
	std::fflush(stderr);
}

bool AssetCache::AssetCacheData::DeserializeProperties(
	const YAML::Node& inData)
{
	return DESERIALIZE_PROPERTY(inData, m_assets);
}

bool AssetCache::AssetCacheData::Validate(
	std::string& outDiagnostic) const
{
	for (const auto& asset : m_assets)
	{
		if (!asset.m_first ||
			!asset.m_second.Validate(asset.m_first, outDiagnostic))
		{
			return false;
		}
	}
	return true;
}

bool AssetCache::AssetCacheData::TryDeserialize(
	const YAML::Node& inData,
	AssetCacheData& outData,
	std::string& outDiagnostic) noexcept
{
	auto deserialize = [&]() -> bool
	{
		std::fprintf(stderr, "ACDBG try candidate begin\n");
		std::fflush(stderr);
		AssetCacheData candidate;
		std::fprintf(stderr, "ACDBG try before properties\n");
		std::fflush(stderr);
		const bool bProperties = candidate.DeserializeProperties(inData);
		std::fprintf(stderr, "ACDBG try after properties %d\n", bProperties ? 1 : 0);
		std::fflush(stderr);
		const bool bValid = bProperties && candidate.Validate(outDiagnostic);
		std::fprintf(stderr, "ACDBG try after validate %d\n", bValid ? 1 : 0);
		std::fflush(stderr);
		if (!bValid)
		{
			if (outDiagnostic.empty())
			{
				outDiagnostic = "Asset cache payload is missing required data.";
			}
			return false;
		}

		outData.m_assets = std::move(candidate.m_assets);
		outDiagnostic.clear();
		return true;
	};

	bool bResult = false;
	std::string yamlDiagnostic;
	std::fprintf(stderr, "ACDBG try before guard\n");
	std::fflush(stderr);
	const bool bInvoked = Sailor::External::TryInvokeYaml(deserialize, bResult, yamlDiagnostic);
	std::fprintf(stderr, "ACDBG try after guard %d result %d\n", bInvoked ? 1 : 0, bResult ? 1 : 0);
	std::fflush(stderr);
	if (!bInvoked)
	{
		outDiagnostic = "Asset cache data contains invalid YAML values: " + yamlDiagnostic;
		return false;
	}
	return bResult;
}

bool AssetCache::ShouldResetCacheFile(Workspace::EWorkspaceCacheLoadStatus status) noexcept
{
	return status == Workspace::EWorkspaceCacheLoadStatus::Missing ||
		status == Workspace::EWorkspaceCacheLoadStatus::StaleIdentity ||
		status == Workspace::EWorkspaceCacheLoadStatus::Corrupt ||
		status == Workspace::EWorkspaceCacheLoadStatus::UnsupportedVersion;
}

bool AssetCache::ShouldWriteCacheFile(
	bool bForcely,
	bool bIsDirty,
	bool bPreserveStorageAfterLoadFailure) noexcept
{
	return bForcely || (!bPreserveStorageAfterLoadFailure && bIsDirty);
}

void AssetCache::Initialize()
{
	Initialize(App::GetWorkspaceContext());
}

void AssetCache::Initialize(
	const Workspace::WorkspaceContext& workspaceContext)
{
	SAILOR_PROFILE_FUNCTION();

	{
		std::lock_guard<std::mutex> lock(m_cacheMutex);
		m_cacheFolder = workspaceContext.GetCache();
		m_cacheIdentity = Workspace::MakeWorkspaceCacheIdentity(
			CacheKind,
			CacheProducer,
			PayloadVersion,
			workspaceContext);
		m_bHasStorageContext = true;
	}
	std::error_code createError;
	std::filesystem::create_directories(m_cacheFolder, createError);
	LoadCache();
}

void AssetCache::Shutdown()
{
	SaveCache();
}

bool AssetCache::SaveCache(bool bForcely)
{
	SAILOR_PROFILE_FUNCTION();

	std::lock_guard<std::mutex> lock(m_cacheMutex);
	std::error_code createCacheFolderError;
	std::filesystem::create_directories(
		m_bHasStorageContext
			? m_cacheFolder
			: std::filesystem::path(AssetRegistry::GetCacheFolder()),
		createCacheFolderError);
	std::error_code cacheFileError;
	const bool bCacheFileExists = std::filesystem::is_regular_file(
		GetConfiguredAssetCacheFilepath(),
		cacheFileError);
	if (!bCacheFileExists)
	{
		m_bIsDirty = true;
	}
	if (!ShouldWriteCacheFile(bForcely, m_bIsDirty, m_bPreserveStorageAfterLoadFailure))
	{
		return !m_bPreserveStorageAfterLoadFailure;
	}
	if (bForcely)
	{
		m_bPreserveStorageAfterLoadFailure = false;
	}

	std::string diagnostic;
	if (WriteCacheLocked(diagnostic))
	{
		m_bIsDirty = false;
		m_lastSaveDiagnostic.clear();
		return true;
	}

	m_bIsDirty = true;
	m_lastSaveDiagnostic = std::move(diagnostic);
	SAILOR_LOG_ERROR("Asset cache save failed: %s", m_lastSaveDiagnostic.c_str());
	return false;
}

void AssetCache::LoadCache()
{
	SAILOR_PROFILE_FUNCTION();

	std::lock_guard<std::mutex> lock(m_cacheMutex);
	Workspace::WorkspaceCacheLoadResult loadResult;
	const auto identity = GetConfiguredIdentity();
	loadResult = Workspace::LoadWorkspaceCacheEnvelope(
		GetConfiguredAssetCacheFilepath(),
		identity);
	if (loadResult.IsLoaded())
	{
		AssetCacheData candidate;
		std::string diagnostic;
		if (TryDeserializeAssetCachePayload(loadResult.m_payload, candidate, diagnostic))
		{
			m_cache.m_assets = std::move(candidate.m_assets);
			m_bIsDirty = false;
			m_bPreserveStorageAfterLoadFailure = false;
			m_lastSaveDiagnostic.clear();
			m_lastLoadResult = std::move(loadResult);
			return;
		}

		loadResult.m_status = Workspace::EWorkspaceCacheLoadStatus::Corrupt;
		loadResult.m_diagnostic = std::move(diagnostic);
		loadResult.m_payload.clear();
	}
	else if (!ShouldResetCacheFile(loadResult.m_status))
	{
		m_cache.m_assets.Clear();
		m_bIsDirty = false;
		m_bPreserveStorageAfterLoadFailure = true;
		m_lastSaveDiagnostic.clear();
		m_lastLoadResult = std::move(loadResult);
		SAILOR_LOG_ERROR(
			"Asset cache load status=%s: %s The existing cache file was preserved.",
			std::string(magic_enum::enum_name(m_lastLoadResult.m_status)).c_str(),
			m_lastLoadResult.m_diagnostic.c_str());
		return;
	}

	ResetInvalidCacheLocked(std::move(loadResult));
}

bool AssetCache::WriteCacheLocked(std::string& outDiagnostic) noexcept
{
	std::string payload;
	std::string serializationDiagnostic;
	if (!External::GuardYamlExceptions(
		[&]()
		{
			payload = SerializeAssetCachePayload(m_cache);
		},
		serializationDiagnostic))
	{
		outDiagnostic = "Cannot serialize asset cache: " + serializationDiagnostic;
		return false;
	}

	std::string envelope;
	const auto identity = GetConfiguredIdentity();
	if (!Workspace::SerializeWorkspaceCacheEnvelope(
		identity,
		payload,
		envelope,
		outDiagnostic))
	{
		return false;
	}

	return Workspace::AtomicReplaceWorkspaceCacheText(
		GetConfiguredAssetCacheFilepath(),
		envelope,
		outDiagnostic);
}

std::string AssetCache::GetConfiguredAssetCacheFilepath() const
{
	return m_bHasStorageContext
		? (m_cacheFolder / "AssetCache.yaml").string()
		: GetAssetCacheFilepath();
}

Workspace::WorkspaceCacheIdentity AssetCache::GetConfiguredIdentity() const
{
	return m_bHasStorageContext
		? m_cacheIdentity
		: MakeExpectedIdentity();
}

void AssetCache::ResetInvalidCacheLocked(Workspace::WorkspaceCacheLoadResult loadResult)
{
	m_cache.m_assets.Clear();
	m_bIsDirty = true;
	m_bPreserveStorageAfterLoadFailure = false;
	m_lastLoadResult = std::move(loadResult);

	std::string diagnostic;
	if (WriteCacheLocked(diagnostic))
	{
		m_bIsDirty = false;
		m_lastSaveDiagnostic.clear();
		AppendDiagnostic(m_lastLoadResult.m_diagnostic, "The cache was reset to an empty current envelope.");
		SAILOR_LOG(
			"Asset cache load status=%s: %s",
			std::string(magic_enum::enum_name(m_lastLoadResult.m_status)).c_str(),
			m_lastLoadResult.m_diagnostic.c_str());
	}
	else
	{
		m_lastSaveDiagnostic = diagnostic;
		AppendDiagnostic(
			m_lastLoadResult.m_diagnostic,
			"The cache could not be reset: " + diagnostic);
		SAILOR_LOG_ERROR(
			"Asset cache load status=%s: %s",
			std::string(magic_enum::enum_name(m_lastLoadResult.m_status)).c_str(),
			m_lastLoadResult.m_diagnostic.c_str());
	}
}

void AssetCache::ClearAll()
{
	std::lock_guard<std::mutex> lock(m_cacheMutex);
	m_cache.m_assets.Clear();
	m_bIsDirty = false;
	m_bPreserveStorageAfterLoadFailure = false;

	std::error_code removeError;
	std::filesystem::remove(GetConfiguredAssetCacheFilepath(), removeError);
	if (removeError)
	{
		m_bIsDirty = true;
		m_lastSaveDiagnostic = "Cannot remove AssetCache.yaml: " + removeError.message();
		SAILOR_LOG_ERROR("Asset cache clear failed: %s", m_lastSaveDiagnostic.c_str());
	}
	else
	{
		m_lastSaveDiagnostic.clear();
	}
}

Workspace::WorkspaceCacheLoadResult AssetCache::GetLastLoadResult() const
{
	std::lock_guard<std::mutex> lock(m_cacheMutex);
	return m_lastLoadResult;
}

std::string AssetCache::GetLastSaveDiagnostic() const
{
	std::lock_guard<std::mutex> lock(m_cacheMutex);
	return m_lastSaveDiagnostic;
}

bool AssetCache::IsDirty() const
{
	std::lock_guard<std::mutex> lock(m_cacheMutex);
	return m_bIsDirty;
}

bool AssetCache::Contains(const FileId& uid) const
{
	std::lock_guard<std::mutex> lock(m_cacheMutex);
	return m_cache.m_assets.ContainsKey(uid);
}

bool AssetCache::Update(const AssetInfo* info)
{
	if (info == nullptr)
	{
		return false;
	}

	const std::string sourcePath = info->GetAssetFilepath();
	std::error_code sourceError;
	if (!std::filesystem::is_regular_file(sourcePath, sourceError) || sourceError)
	{
		Remove(info->GetFileId());
		return false;
	}
	FileRevision currentRevision;
	if (!Utils::TryGetFileRevision(sourcePath, currentRevision))
	{
		Remove(info->GetFileId());
		return false;
	}
	if (!info->m_importedSourceRevision.m_bIsValid ||
		currentRevision != info->m_importedSourceRevision)
	{
		return false;
	}

	return Update(
		info->GetFileId(),
		info->GetAssetImportTime(),
		sourcePath,
		info->m_importedSourceRevision,
		std::filesystem::path(info->GetMetaFilepath()).filename().string(),
		info->m_metadataRevision,
		info->GetAssetInfoType());
}

bool AssetCache::Update(
	const FileId& id,
	std::time_t assetImportTime,
	const std::string& sourcePath,
	const FileRevision& sourceRevision,
	const std::string& metadataFilename,
	const FileRevision& metadataRevision,
	const std::string& assetInfoType)
{
	std::string normalizedSourcePath = NormalizeSourcePath(sourcePath);
	const bool bValidMetadataFilename = !metadataFilename.empty() &&
		std::filesystem::path(metadataFilename).filename() == metadataFilename;
	if (!id || assetImportTime <= 0 || normalizedSourcePath.empty() ||
		!sourceRevision.m_bIsValid || !bValidMetadataFilename ||
		!metadataRevision.m_bIsValid || assetInfoType.empty())
	{
		Remove(id);
		return false;
	}

	std::lock_guard<std::mutex> lock(m_cacheMutex);
	auto& entry = m_cache.m_assets.At_Lock(id);
	struct EntryUnlockGuard final
	{
		TConcurrentMap<FileId, AssetCacheData::Entry>& m_assets;
		const FileId& m_id;

		~EntryUnlockGuard() noexcept
		{
			m_assets.Unlock(m_id);
		}
	} unlockGuard{ m_cache.m_assets, id };

	const bool bChanged = entry.m_fileId != id ||
		entry.m_assetImportTime != assetImportTime ||
		entry.m_sourcePath != normalizedSourcePath ||
		entry.m_sourceRevision != sourceRevision ||
		entry.m_metadataFilename != metadataFilename ||
		entry.m_metadataRevision != metadataRevision ||
		entry.m_assetInfoType != assetInfoType;
	m_bIsDirty |= bChanged;
	entry.m_fileId = id;
	entry.m_assetImportTime = assetImportTime;
	entry.m_sourcePath = std::move(normalizedSourcePath);
	entry.m_sourceRevision = sourceRevision;
	entry.m_metadataFilename = metadataFilename;
	entry.m_metadataRevision = metadataRevision;
	entry.m_assetInfoType = assetInfoType;
	return bChanged;
}

bool AssetCache::RestoreAssetImportTime(
	AssetInfo* info,
	const FileRevision& sourceRevision) const
{
	if (info == nullptr || !info->GetFileId() || !sourceRevision.m_bIsValid)
	{
		return false;
	}

	const std::string sourcePath = NormalizeSourcePath(info->GetAssetFilepath());
	std::lock_guard<std::mutex> lock(m_cacheMutex);
	if (!m_cache.m_assets.ContainsKey(info->GetFileId()))
	{
		return false;
	}

	const AssetCacheData::Entry entry = m_cache.m_assets[info->GetFileId()];
	if (entry.m_sourcePath != sourcePath || entry.m_sourceRevision != sourceRevision)
	{
		return false;
	}

	info->m_assetImportTime = entry.m_assetImportTime;
	return true;
}

bool AssetCache::Prune(const TSet<FileId>& liveAssetIds)
{
	std::lock_guard<std::mutex> lock(m_cacheMutex);
	TVector<FileId> staleAssetIds;
	for (const auto& cachedAsset : m_cache.m_assets)
	{
		if (!liveAssetIds.Contains(cachedAsset.m_first))
		{
			staleAssetIds.Add(cachedAsset.m_first);
		}
	}

	bool bChanged = false;
	for (const FileId& staleAssetId : staleAssetIds)
	{
		bChanged |= m_cache.m_assets.Remove(staleAssetId);
	}
	m_bIsDirty |= bChanged;
	return bChanged;
}

void AssetCache::Remove(const FileId& uid)
{
	std::lock_guard<std::mutex> lock(m_cacheMutex);
	m_bIsDirty |= m_cache.m_assets.Remove(uid);
}

bool AssetCache::IsExpired(const AssetInfo* info) const
{
	if (info == nullptr)
	{
		return true;
	}

	const FileId& fileId = info->GetFileId();
	const std::string sourcePath = NormalizeSourcePath(info->GetAssetFilepath());
	FileRevision sourceRevision;
	if (!Utils::TryGetFileRevision(sourcePath, sourceRevision))
	{
		return true;
	}

	std::lock_guard<std::mutex> lock(m_cacheMutex);
	if (!m_cache.m_assets.ContainsKey(fileId))
	{
		return true;
	}

	const AssetCacheData::Entry entry = m_cache.m_assets[fileId];
	return entry.m_sourcePath != sourcePath ||
		entry.m_sourceRevision != sourceRevision;
}
