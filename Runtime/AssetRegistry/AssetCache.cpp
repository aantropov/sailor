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

	template<typename T>
	bool TryDecodeScalar(const YAML::Node& node, T& outValue)
	{
		return node.IsScalar() && YAML::convert<T>::decode(node, outValue);
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
	YAML::Node sourceRevision(YAML::NodeType::Map);
	sourceRevision["modificationTimeNanoseconds"] = m_sourceRevision.m_modificationTimeNanoseconds;
	sourceRevision["fileSize"] = m_sourceRevision.m_fileSize;
	sourceRevision["contentHash"] = m_sourceRevision.m_contentHash;
	result["sourceRevision"] = sourceRevision;
	result["metadataFilename"] = m_metadataFilename;
	YAML::Node metadataRevision(YAML::NodeType::Map);
	metadataRevision["modificationTimeNanoseconds"] = m_metadataRevision.m_modificationTimeNanoseconds;
	metadataRevision["fileSize"] = m_metadataRevision.m_fileSize;
	metadataRevision["contentHash"] = m_metadataRevision.m_contentHash;
	result["metadataRevision"] = metadataRevision;
	result["assetInfoType"] = m_assetInfoType;
	return result;
}

void AssetCache::AssetCacheData::Entry::Deserialize(const YAML::Node& inData)
{
	auto reset = [&]()
	{
		m_fileId = FileId();
		m_assetImportTime = 0;
		m_sourcePath.clear();
		m_sourceRevision = {};
		m_metadataFilename.clear();
		m_metadataRevision = {};
		m_assetInfoType.clear();
	};
	reset();
	bool bValid = false;
	std::string yamlDiagnostic;
	const bool bDeserialized = Sailor::External::GuardYamlExceptions(
		[&]()
		{
			if (!inData.IsMap() || inData.size() != 7)
			{
				return;
			}

			const YAML::Node fileId = inData["fileId"];
			const YAML::Node assetImportTime = inData["assetImportTime"];
			const YAML::Node sourcePath = inData["sourcePath"];
			const YAML::Node sourceRevision = inData["sourceRevision"];
			if (!fileId || !assetImportTime || !sourcePath || !sourceRevision ||
				!fileId.IsScalar() || !assetImportTime.IsScalar() || !sourcePath.IsScalar() ||
				!sourceRevision.IsMap() || sourceRevision.size() != 3)
			{
				return;
			}
			const YAML::Node modificationTimeNanoseconds = sourceRevision["modificationTimeNanoseconds"];
			const YAML::Node fileSize = sourceRevision["fileSize"];
			const YAML::Node contentHash = sourceRevision["contentHash"];
			if (!modificationTimeNanoseconds || !fileSize || !contentHash ||
				!modificationTimeNanoseconds.IsScalar() || !fileSize.IsScalar() || !contentHash.IsScalar())
			{
				return;
			}

			std::string decodedSourcePath;
			if (!TryDecodeScalar(fileId, m_fileId) ||
				!TryDecodeScalar(assetImportTime, m_assetImportTime) ||
				!TryDecodeScalar(sourcePath, decodedSourcePath) ||
				!TryDecodeScalar(modificationTimeNanoseconds, m_sourceRevision.m_modificationTimeNanoseconds) ||
				!TryDecodeScalar(fileSize, m_sourceRevision.m_fileSize) ||
				!TryDecodeScalar(contentHash, m_sourceRevision.m_contentHash))
			{
				return;
			}
			m_sourcePath = NormalizeSourcePath(decodedSourcePath);
			m_sourceRevision.m_bIsValid = true;
			const YAML::Node metadataFilename = inData["metadataFilename"];
			const YAML::Node metadataRevision = inData["metadataRevision"];
			const YAML::Node assetInfoType = inData["assetInfoType"];
			if (!metadataFilename.IsScalar() || !metadataRevision.IsMap() ||
				metadataRevision.size() != 3 || !assetInfoType.IsScalar())
			{
				return;
			}
			const YAML::Node metadataModificationTime = metadataRevision["modificationTimeNanoseconds"];
			const YAML::Node metadataFileSize = metadataRevision["fileSize"];
			const YAML::Node metadataContentHash = metadataRevision["contentHash"];
			if (!metadataModificationTime.IsScalar() || !metadataFileSize.IsScalar() ||
				!metadataContentHash.IsScalar())
			{
				return;
			}
			if (!TryDecodeScalar(metadataFilename, m_metadataFilename) ||
				!TryDecodeScalar(metadataModificationTime, m_metadataRevision.m_modificationTimeNanoseconds) ||
				!TryDecodeScalar(metadataFileSize, m_metadataRevision.m_fileSize) ||
				!TryDecodeScalar(metadataContentHash, m_metadataRevision.m_contentHash) ||
				!TryDecodeScalar(assetInfoType, m_assetInfoType))
			{
				return;
			}
			m_metadataRevision.m_bIsValid = true;
			if (m_sourcePath.empty() || m_metadataFilename.empty() ||
				std::filesystem::path(m_metadataFilename).filename() != m_metadataFilename ||
				m_assetInfoType.empty() ||
				!m_fileId || m_assetImportTime <= 0)
			{
				return;
			}
			bValid = true;
		},
		yamlDiagnostic);
	if (!bDeserialized || !bValid)
	{
		reset();
	}
}

YAML::Node AssetCache::AssetCacheData::Serialize() const
{
	YAML::Node result(YAML::NodeType::Map);
	YAML::Node assets(YAML::NodeType::Map);
	for (const auto& entry : m_data)
	{
		// FileIds are unique in m_data, so avoid YAML's linear key lookup for every entry.
		assets.force_insert(entry.m_first.ToString(), entry.m_second.Serialize());
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

		FileId fileId;
		if (!TryDecodeScalar(serializedEntry.first, fileId) || !fileId)
		{
			outDiagnostic = "Asset cache contains invalid file id '" + serializedFileId + "'.";
			return false;
		}

		const YAML::Node entryNode = serializedEntry.second;
		if (!entryNode.IsMap() || entryNode.size() != 7)
		{
			outDiagnostic = "Asset cache entry '" + serializedFileId +
				"' must contain exactly the asset watermark and lazy metadata index fields.";
			return false;
		}

		YAML::Node entryFileId;
		YAML::Node assetImportTime;
		YAML::Node sourcePath;
		YAML::Node sourceRevision;
		if (!ReadRequiredField(entryNode, "fileId", entryFileId, outDiagnostic) ||
			!ReadRequiredField(entryNode, "assetImportTime", assetImportTime, outDiagnostic) ||
			!ReadRequiredField(entryNode, "sourcePath", sourcePath, outDiagnostic) ||
			!ReadRequiredField(entryNode, "sourceRevision", sourceRevision, outDiagnostic))
		{
			return false;
		}
		if (!entryFileId.IsScalar() || !assetImportTime.IsScalar() || !sourcePath.IsScalar() ||
			!sourceRevision.IsMap() || sourceRevision.size() != 3)
		{
			outDiagnostic = "Asset cache entry '" + serializedFileId + "' contains an invalid field.";
			return false;
		}

		YAML::Node modificationTimeNanoseconds;
		YAML::Node fileSize;
		YAML::Node contentHash;
		if (!ReadRequiredField(
				sourceRevision,
				"modificationTimeNanoseconds",
				modificationTimeNanoseconds,
				outDiagnostic) ||
			!ReadRequiredField(sourceRevision, "fileSize", fileSize, outDiagnostic) ||
			!ReadRequiredField(sourceRevision, "contentHash", contentHash, outDiagnostic))
		{
			return false;
		}
		if (!modificationTimeNanoseconds.IsScalar() || !fileSize.IsScalar() || !contentHash.IsScalar())
		{
			outDiagnostic = "Asset cache entry '" + serializedFileId +
				"' contains a non-scalar sourceRevision field.";
			return false;
		}

		AssetCacheData::Entry entry;
		std::string serializedSourcePath;
		if (!TryDecodeScalar(entryFileId, entry.m_fileId) ||
			!TryDecodeScalar(assetImportTime, entry.m_assetImportTime) ||
			!TryDecodeScalar(modificationTimeNanoseconds, entry.m_sourceRevision.m_modificationTimeNanoseconds) ||
			!TryDecodeScalar(fileSize, entry.m_sourceRevision.m_fileSize) ||
			!TryDecodeScalar(contentHash, entry.m_sourceRevision.m_contentHash) ||
			!TryDecodeScalar(sourcePath, serializedSourcePath))
		{
			outDiagnostic = "Asset cache entry '" + serializedFileId +
				"' contains an invalid scalar value.";
			return false;
		}
		entry.m_sourceRevision.m_bIsValid = true;
		if (serializedSourcePath.empty())
		{
			outDiagnostic = "Asset cache entry '" + serializedFileId + "' has an empty sourcePath.";
			return false;
		}
		entry.m_sourcePath = NormalizeSourcePath(serializedSourcePath);
		YAML::Node metadataFilename;
		YAML::Node metadataRevision;
		YAML::Node assetInfoType;
		if (!ReadRequiredField(entryNode, "metadataFilename", metadataFilename, outDiagnostic) ||
			!ReadRequiredField(entryNode, "metadataRevision", metadataRevision, outDiagnostic) ||
			!ReadRequiredField(entryNode, "assetInfoType", assetInfoType, outDiagnostic) ||
			!metadataFilename.IsScalar() || !metadataRevision.IsMap() ||
			metadataRevision.size() != 3 || !assetInfoType.IsScalar())
		{
			return false;
		}
		YAML::Node metadataModificationTime;
		YAML::Node metadataFileSize;
		YAML::Node metadataContentHash;
		if (!ReadRequiredField(metadataRevision, "modificationTimeNanoseconds", metadataModificationTime, outDiagnostic) ||
			!ReadRequiredField(metadataRevision, "fileSize", metadataFileSize, outDiagnostic) ||
			!ReadRequiredField(metadataRevision, "contentHash", metadataContentHash, outDiagnostic) ||
			!metadataModificationTime.IsScalar() || !metadataFileSize.IsScalar() ||
			!metadataContentHash.IsScalar())
		{
			return false;
		}
		if (!TryDecodeScalar(metadataFilename, entry.m_metadataFilename) ||
			!TryDecodeScalar(metadataModificationTime, entry.m_metadataRevision.m_modificationTimeNanoseconds) ||
			!TryDecodeScalar(metadataFileSize, entry.m_metadataRevision.m_fileSize) ||
			!TryDecodeScalar(metadataContentHash, entry.m_metadataRevision.m_contentHash) ||
			!TryDecodeScalar(assetInfoType, entry.m_assetInfoType))
		{
			outDiagnostic = "Asset cache entry '" + serializedFileId +
				"' contains an invalid lazy metadata scalar value.";
			return false;
		}
		entry.m_metadataRevision.m_bIsValid = true;
		if (entry.m_metadataFilename.empty() ||
			std::filesystem::path(entry.m_metadataFilename).filename() != entry.m_metadataFilename ||
			entry.m_assetInfoType.empty())
		{
			outDiagnostic = "Asset cache entry '" + serializedFileId +
				"' contains an incomplete lazy metadata index.";
			return false;
		}
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
			AssetCacheKind,
			AssetCacheProducer,
			AssetCachePayloadVersion,
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
			std::string(magic_enum::enum_name(m_lastLoadResult.m_status)).c_str(),
			m_lastLoadResult.m_diagnostic.c_str());
		return;
	}

	ResetInvalidCacheLocked(std::move(loadResult));
}

bool AssetCache::WriteCacheLocked(std::string& outDiagnostic) noexcept
{
	std::string envelope;
	const auto identity = GetConfiguredIdentity();
	if (!Workspace::SerializeWorkspaceCacheEnvelope(
		identity,
		SerializeAssetCachePayload(m_cache),
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
	m_cache.m_data.Clear();
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
	if (!m_cache.m_data.ContainsKey(info->GetFileId()))
	{
		return false;
	}

	const AssetCacheData::Entry entry = m_cache.m_data[info->GetFileId()];
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
	FileRevision sourceRevision;
	if (!Utils::TryGetFileRevision(sourcePath, sourceRevision))
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
		entry.m_sourceRevision != sourceRevision;
}
