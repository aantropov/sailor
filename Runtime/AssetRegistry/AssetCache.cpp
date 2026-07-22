#include "AssetCache.h"
#include "Containers/Containers.h"

#include "AssetRegistry/AssetRegistry.h"
#include "Containers/ConcurrentMap.h"
#include "Core/YamlSerializable.h"
#include "Sailor.h"
#include "Tasks/Tasks.h"
#include "YamlExceptionBoundary.h"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <sstream>
#include <unordered_set>

using namespace Sailor;

namespace
{
	constexpr const char* AssetCacheKind = "asset-cache";
	constexpr const char* AssetCacheProducer = "asset-cache-v1";
	constexpr uint32_t AssetCachePayloadVersion = 1;

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

	Workspace::WorkspaceCacheIdentity MakeExpectedIdentity()
	{
		return Workspace::MakeWorkspaceCacheIdentity(
			AssetCacheKind,
			AssetCacheProducer,
			AssetCachePayloadVersion,
			App::GetWorkspaceContext());
	}

	bool ReadRequiredField(
		const YAML::Node& map,
		const char* fieldName,
		YAML::Node& outField,
		std::string& outDiagnostic)
	{
		if (!map.IsMap())
		{
			outDiagnostic = "Asset cache payload field container must be a YAML map.";
			return false;
		}

		uint32_t matches = 0;
		for (const auto& field : map)
		{
			if (field.first.IsScalar() && field.first.Scalar() == fieldName)
			{
				outField = field.second;
				++matches;
			}
		}

		if (matches != 1)
		{
			outDiagnostic = matches == 0
				? "Asset cache payload is missing required field '" + std::string(fieldName) + "'."
				: "Asset cache payload contains duplicate field '" + std::string(fieldName) + "'.";
			return false;
		}

		return true;
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

	const char* CacheLoadStatusName(Workspace::EWorkspaceCacheLoadStatus status) noexcept
	{
		switch (status)
		{
		case Workspace::EWorkspaceCacheLoadStatus::Missing: return "Missing";
		case Workspace::EWorkspaceCacheLoadStatus::Loaded: return "Loaded";
		case Workspace::EWorkspaceCacheLoadStatus::StaleIdentity: return "StaleIdentity";
		case Workspace::EWorkspaceCacheLoadStatus::Corrupt: return "Corrupt";
		case Workspace::EWorkspaceCacheLoadStatus::UnsupportedVersion: return "UnsupportedVersion";
		case Workspace::EWorkspaceCacheLoadStatus::IoFailure: return "IoFailure";
		default: return "Unknown";
		}
	}
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
		if (!root.IsMap() || root.size() != 1)
		{
			outDiagnostic = "Asset cache payload root must contain exactly one 'assetCache' map.";
			return false;
		}

		YAML::Node assetCache;
		if (!ReadRequiredField(root, "assetCache", assetCache, outDiagnostic))
		{
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
	result["fileId"] = m_fileId;
	result["assetImportTime"] = m_assetImportTime;
	result["sourcePath"] = m_sourcePath;
	return result;
}

void AssetCache::AssetCacheData::Entry::Deserialize(const YAML::Node& inData)
{
	m_fileId = FileId();
	m_assetImportTime = 0;
	m_sourcePath.clear();
	std::string yamlDiagnostic;
	if (!Sailor::External::GuardYamlExceptions(
		[&]()
		{
			if (!inData.IsMap() || inData.size() != 3)
			{
				return;
			}

			const YAML::Node fileId = inData["fileId"];
			const YAML::Node assetImportTime = inData["assetImportTime"];
			const YAML::Node sourcePath = inData["sourcePath"];
			if (!fileId || !assetImportTime || !sourcePath || !fileId.IsScalar() ||
				!assetImportTime.IsScalar() || !sourcePath.IsScalar())
			{
				return;
			}

			m_fileId.Deserialize(fileId);
			m_assetImportTime = assetImportTime.as<std::time_t>();
			m_sourcePath = NormalizeSourcePath(sourcePath.as<std::string>());
			if (m_sourcePath.empty() || !m_fileId || m_assetImportTime <= 0)
			{
				m_fileId = FileId();
				m_assetImportTime = 0;
				m_sourcePath.clear();
			}
		},
		yamlDiagnostic))
	{
		m_fileId = FileId();
		m_assetImportTime = 0;
		m_sourcePath.clear();
	}
}

YAML::Node AssetCache::AssetCacheData::Serialize() const
{
	YAML::Node result(YAML::NodeType::Map);
	YAML::Node assets(YAML::NodeType::Map);
	for (const auto& entry : m_data)
	{
		assets[entry.m_first] = entry.m_second;
	}
	result["assets"] = assets;
	return result;
}

void AssetCache::AssetCacheData::Deserialize(const YAML::Node& inData)
{
	AssetCacheData candidate;
	std::string diagnostic;
	if (!TryDeserialize(inData, candidate, diagnostic))
	{
		m_data.Clear();
		return;
	}

	m_data = std::move(candidate.m_data);
}

bool AssetCache::AssetCacheData::TryDeserialize(
	const YAML::Node& inData,
	AssetCacheData& outData,
	std::string& outDiagnostic) noexcept
{
	auto deserialize = [&]() -> bool
	{
	if (!inData.IsMap() || inData.size() != 1)
	{
		outDiagnostic = "Asset cache data must contain exactly one 'assets' map.";
		return false;
	}

	YAML::Node assets;
	if (!ReadRequiredField(inData, "assets", assets, outDiagnostic))
	{
		return false;
	}
	if (!assets.IsMap())
	{
		outDiagnostic = "Asset cache field 'assets' must be a YAML map, including when empty.";
		return false;
	}

	AssetCacheData candidate;
	TSet<std::string> fileIds;
	for (const auto& serializedEntry : assets)
	{
		if (!serializedEntry.first.IsScalar())
		{
			outDiagnostic = "Asset cache contains a non-scalar file id.";
			return false;
		}

		const std::string serializedFileId = serializedEntry.first.Scalar();
		if (!fileIds.Insert(serializedFileId))
		{
			outDiagnostic = "Asset cache contains duplicate file id '" + serializedFileId + "'.";
			return false;
		}

		const FileId fileId = serializedEntry.first.as<FileId>();
		if (!fileId)
		{
			outDiagnostic = "Asset cache contains invalid file id '" + serializedFileId + "'.";
			return false;
		}

		const YAML::Node entryNode = serializedEntry.second;
		if (!entryNode.IsMap() || entryNode.size() != 3)
		{
			outDiagnostic = "Asset cache entry '" + serializedFileId +
				"' must contain exactly fileId, assetImportTime, and sourcePath.";
			return false;
		}

		YAML::Node entryFileId;
		YAML::Node assetImportTime;
		YAML::Node sourcePath;
		if (!ReadRequiredField(entryNode, "fileId", entryFileId, outDiagnostic) ||
			!ReadRequiredField(entryNode, "assetImportTime", assetImportTime, outDiagnostic) ||
			!ReadRequiredField(entryNode, "sourcePath", sourcePath, outDiagnostic))
		{
			return false;
		}
		if (!entryFileId.IsScalar() || !assetImportTime.IsScalar() || !sourcePath.IsScalar())
		{
			outDiagnostic = "Asset cache entry '" + serializedFileId + "' contains a non-scalar field.";
			return false;
		}

		AssetCacheData::Entry entry;
		entry.m_fileId = entryFileId.as<FileId>();
		entry.m_assetImportTime = assetImportTime.as<std::time_t>();
		const std::string serializedSourcePath = sourcePath.as<std::string>();
		if (serializedSourcePath.empty())
		{
			outDiagnostic = "Asset cache entry '" + serializedFileId + "' has an empty sourcePath.";
			return false;
		}
		entry.m_sourcePath = NormalizeSourcePath(serializedSourcePath);
		if (!entry.m_fileId || entry.m_fileId != fileId)
		{
			outDiagnostic = "Asset cache entry '" + serializedFileId + "' has a mismatched fileId field.";
			return false;
		}
		if (entry.m_sourcePath.empty())
		{
			outDiagnostic = "Asset cache entry '" + serializedFileId + "' has an empty sourcePath.";
			return false;
		}
		if (entry.m_assetImportTime <= 0)
		{
			outDiagnostic = "Asset cache entry '" + serializedFileId + "' contains an invalid assetImportTime.";
			return false;
		}
		candidate.m_data.Insert(fileId, std::move(entry));
	}

	outData.m_data = std::move(candidate.m_data);
	outDiagnostic.clear();
	return true;
	};

	bool bResult = false;
	std::string yamlDiagnostic;
	if (!Sailor::External::TryInvokeYaml(deserialize, bResult, yamlDiagnostic))
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
	SAILOR_PROFILE_FUNCTION();

	std::error_code createError;
	std::filesystem::create_directories(AssetRegistry::GetCacheFolder(), createError);
	LoadCache();
}

void AssetCache::Shutdown()
{
	SaveCache();
}

void AssetCache::SaveCache(bool bForcely)
{
	SAILOR_PROFILE_FUNCTION();

	std::lock_guard<std::mutex> lock(m_cacheMutex);
	std::error_code createCacheFolderError;
	std::filesystem::create_directories(
		AssetRegistry::GetCacheFolder(),
		createCacheFolderError);
	std::error_code cacheFileError;
	const bool bCacheFileExists = std::filesystem::is_regular_file(
		GetAssetCacheFilepath(),
		cacheFileError);
	if (!bCacheFileExists)
	{
		m_bIsDirty = true;
	}
	if (!ShouldWriteCacheFile(bForcely, m_bIsDirty, m_bPreserveStorageAfterLoadFailure))
	{
		return;
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
	}
	else
	{
		m_bIsDirty = true;
		m_lastSaveDiagnostic = std::move(diagnostic);
		SAILOR_LOG_ERROR("Asset cache save failed: %s", m_lastSaveDiagnostic.c_str());
	}
}

void AssetCache::LoadCache()
{
	SAILOR_PROFILE_FUNCTION();

	std::lock_guard<std::mutex> lock(m_cacheMutex);
	Workspace::WorkspaceCacheLoadResult loadResult;
	const auto identity = MakeExpectedIdentity();
	loadResult = Workspace::LoadWorkspaceCacheEnvelope(GetAssetCacheFilepath(), identity);
	if (loadResult.IsLoaded())
	{
		AssetCacheData candidate;
		std::string diagnostic;
		if (TryDeserializeAssetCachePayload(loadResult.m_payload, candidate, diagnostic))
		{
			m_cache.m_data = std::move(candidate.m_data);
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
		m_cache.m_data.Clear();
		m_bIsDirty = false;
		m_bPreserveStorageAfterLoadFailure = true;
		m_lastSaveDiagnostic.clear();
		m_lastLoadResult = std::move(loadResult);
		SAILOR_LOG_ERROR(
			"Asset cache load status=%s: %s The existing cache file was preserved.",
			CacheLoadStatusName(m_lastLoadResult.m_status),
			m_lastLoadResult.m_diagnostic.c_str());
		return;
	}

	ResetInvalidCacheLocked(std::move(loadResult));
}

bool AssetCache::WriteCacheLocked(std::string& outDiagnostic) noexcept
{
	std::string envelope;
	const auto identity = MakeExpectedIdentity();
	if (!Workspace::SerializeWorkspaceCacheEnvelope(
		identity,
		SerializeAssetCachePayload(m_cache),
		envelope,
		outDiagnostic))
	{
		return false;
	}

	return Workspace::AtomicReplaceWorkspaceCacheText(
		GetAssetCacheFilepath(),
		envelope,
		outDiagnostic);
}

void AssetCache::ResetInvalidCacheLocked(Workspace::WorkspaceCacheLoadResult loadResult)
{
	m_cache.m_data.Clear();
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
			CacheLoadStatusName(m_lastLoadResult.m_status),
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
			CacheLoadStatusName(m_lastLoadResult.m_status),
			m_lastLoadResult.m_diagnostic.c_str());
	}
}

void AssetCache::ClearAll()
{
	std::lock_guard<std::mutex> lock(m_cacheMutex);
	m_cache.m_data.Clear();
	m_bIsDirty = false;
	m_bPreserveStorageAfterLoadFailure = false;

	std::error_code removeError;
	std::filesystem::remove(GetAssetCacheFilepath(), removeError);
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
	return m_cache.m_data.ContainsKey(uid);
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

	return Update(
		info->GetFileId(),
		info->GetAssetImportTime(),
		sourcePath);
}

bool AssetCache::Update(
	const FileId& id,
	std::time_t assetImportTime,
	const std::string& sourcePath)
{
	std::string normalizedSourcePath = NormalizeSourcePath(sourcePath);
	if (!id || assetImportTime <= 0 || normalizedSourcePath.empty())
	{
		return false;
	}

	std::lock_guard<std::mutex> lock(m_cacheMutex);
	auto& entry = m_cache.m_data.At_Lock(id);
	struct EntryUnlockGuard final
	{
		TConcurrentMap<FileId, AssetCacheData::Entry>& m_data;
		const FileId& m_id;

		~EntryUnlockGuard() noexcept
		{
			m_data.Unlock(m_id);
		}
	} unlockGuard{ m_cache.m_data, id };

	const bool bChanged = entry.m_fileId != id ||
		entry.m_assetImportTime != assetImportTime ||
		entry.m_sourcePath != normalizedSourcePath;
	m_bIsDirty |= bChanged;
	entry.m_fileId = id;
	entry.m_assetImportTime = assetImportTime;
	entry.m_sourcePath = std::move(normalizedSourcePath);
	return bChanged;
}

bool AssetCache::RestoreAssetImportTime(AssetInfo* info) const
{
	if (info == nullptr || !info->GetFileId())
	{
		return false;
	}

	const std::string sourcePath = NormalizeSourcePath(info->GetAssetFilepath());
	std::lock_guard<std::mutex> lock(m_cacheMutex);
	if (!m_cache.m_data.ContainsKey(info->GetFileId()))
	{
		return false;
	}

	const AssetCacheData::Entry entry = m_cache.m_data[info->GetFileId()];
	if (entry.m_sourcePath != sourcePath)
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
	for (const auto& cachedAsset : m_cache.m_data)
	{
		if (!liveAssetIds.Contains(cachedAsset.m_first))
		{
			staleAssetIds.Add(cachedAsset.m_first);
		}
	}

	bool bChanged = false;
	for (const FileId& staleAssetId : staleAssetIds)
	{
		bChanged |= m_cache.m_data.Remove(staleAssetId);
	}
	m_bIsDirty |= bChanged;
	return bChanged;
}

void AssetCache::Remove(const FileId& uid)
{
	std::lock_guard<std::mutex> lock(m_cacheMutex);
	m_bIsDirty |= m_cache.m_data.Remove(uid);
}

bool AssetCache::IsExpired(const AssetInfo* info) const
{
	if (info == nullptr)
	{
		return true;
	}

	const FileId& fileId = info->GetFileId();
	const std::string sourcePath = NormalizeSourcePath(info->GetAssetFilepath());
	std::error_code sourceError;
	if (!std::filesystem::is_regular_file(sourcePath, sourceError) || sourceError)
	{
		return true;
	}

	std::lock_guard<std::mutex> lock(m_cacheMutex);
	if (!m_cache.m_data.ContainsKey(fileId))
	{
		return true;
	}

	const AssetCacheData::Entry entry = m_cache.m_data[fileId];
	return entry.m_sourcePath != sourcePath ||
		entry.m_assetImportTime < info->GetAssetLastModificationTime();
}
