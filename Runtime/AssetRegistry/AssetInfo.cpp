#include "AssetRegistry/AssetInfo.h"
#include "Platform/AtomicFile.h"
#include "AssetRegistry/AssetRegistry.h"
#include "AssetRegistry/AssetScanSourceRevisionCache.h"
#include <filesystem>
#include <fstream>
#include "Core/Utils.h"
#include "Core/Reflection.h"
#include "Tasks/Scheduler.h"
#include "Tasks/Tasks.h"
#include "Memory/UniquePtr.hpp"
#include "YamlExceptionBoundary.h"
#include <cerrno>
#include <cstdio>
#include <iostream>
#include <iterator>
#include <sstream>

using namespace Sailor;

namespace
{
	bool ReadFileExactly(const std::filesystem::path& filepath, std::string& outContents)
	{
		std::ifstream file(filepath, std::ios::binary);
		if (!file.is_open())
		{
			return false;
		}

		outContents.assign(
			std::istreambuf_iterator<char>(file),
			std::istreambuf_iterator<char>());
		return !file.bad();
	}

	bool RemoveFileIfContentsMatch(
		const std::filesystem::path& filepath,
		const std::string& expectedContents)
	{
		std::string currentContents;
		if (!ReadFileExactly(filepath, currentContents) || currentContents != expectedContents)
		{
			return false;
		}

		std::error_code removeError;
		return std::filesystem::remove(filepath, removeError) && !removeError;
	}

	bool WriteNewMetadataFile(
		const std::filesystem::path& filepath,
		const YAML::Node& metadata,
		std::string& outContents,
		std::string& outDiagnostic)
	{
		std::stringstream serialized;
		serialized << metadata;
		outContents = serialized.str();

		errno = 0;
		std::FILE* file = nullptr;
		int openError = 0;
#if defined(_WIN32)
		openError = _wfopen_s(&file, filepath.c_str(), L"wbx");
#else
		file = std::fopen(filepath.c_str(), "wbx");
		openError = errno;
#endif
		if (file == nullptr)
		{
			const std::error_code error(openError, std::generic_category());
			outDiagnostic = error
				? error.message()
				: "the metadata file already exists or could not be created exclusively";
			return false;
		}

		const size_t written = std::fwrite(outContents.data(), 1, outContents.size(), file);
		const bool bClosed = std::fclose(file) == 0;
		if (written == outContents.size() && bClosed)
		{
			return true;
		}

		const std::error_code error(errno, std::generic_category());
		outDiagnostic = error ? error.message() : "the complete metadata payload could not be written";
		RemoveFileIfContentsMatch(filepath, outContents.substr(0, written));
		return false;
	}

	bool TryGetSourceRevision(
		const std::string& filepath,
		FileRevision& outRevision)
	{
		AssetScanSourceRevisionCache* sourceRevisionCache =
			GetActiveAssetScanSourceRevisionCache();
		return sourceRevisionCache != nullptr ?
			sourceRevisionCache->TryGet(filepath, outRevision)
			: Utils::TryGetFileRevision(filepath, outRevision);
	}
}

std::string AssetInfo::GetAssetInfoType() const
{
	return GetTypeInfo().Name();
}

YAML::Node AssetInfo::Serialize() const
{
	return SerializeReflectedAssetInfo(*this);
}

void AssetInfo::Deserialize(const YAML::Node& inData)
{
	DeserializeReflectedAssetInfo(*this, inData);
}

void AssetInfo::CopyMetadata(const AssetInfo& source)
{
	CopyReflectedAssetInfo(*this, source);
}

bool AssetInfo::SaveMetaFile()
{
	const std::string filepath = GetMetaFilepath();
	if (!m_bWritable)
	{
		SAILOR_LOG_ERROR("Cannot write read-only engine asset metadata: %s", filepath.c_str());
		return false;
	}

	std::string contents, diagnostic;
	if (!External::GuardYamlExceptions(
		[this, &contents]() { contents = YAML::Dump(Serialize()); }, diagnostic) ||
		!Platform::IsAtomicWriteComplete(Platform::AtomicWriteFile(filepath, contents, diagnostic)))
	{
		SAILOR_LOG_ERROR("Cannot save asset metadata '%s': %s", filepath.c_str(), diagnostic.c_str());
		return false;
	}

	const std::time_t metadataLoadTime = GetMetaLastModificationTime();
	FileRevision metadataRevision;
	if (!Utils::TryGetFileRevision(filepath, metadataRevision))
	{
		SAILOR_LOG_ERROR("Cannot capture saved asset metadata revision: %s", filepath.c_str());
		return false;
	}
	m_metaLoadTime = metadataLoadTime;
	m_metadataRevision = metadataRevision;
	return true;
}

AssetInfo::AssetInfo()
{
	m_metaLoadTime = std::time(nullptr);
	m_assetImportTime = 0;
}

bool AssetInfo::IsAssetExpired() const
{
	FileRevision currentRevision;
	if (!TryGetSourceRevision(GetAssetFilepath(), currentRevision))
	{
		return true;
	}

	if (m_importedSourceRevision.m_bIsValid)
	{
		return m_importedSourceRevision != currentRevision;
	}

	return m_assetImportTime < GetAssetLastModificationTime();
}

bool AssetInfo::IsMetaExpired() const
{
	FileRevision currentRevision;
	if (!Utils::TryGetFileRevision(GetMetaFilepath(), currentRevision))
	{
		return true;
	}

	if (m_metadataRevision.m_bIsValid)
	{
		return m_metadataRevision != currentRevision;
	}

	return m_metaLoadTime < GetMetaLastModificationTime();
}

DefaultAssetInfoHandler::DefaultAssetInfoHandler(AssetRegistry* assetRegistry)
{
	assetRegistry->RegisterAssetInfoHandler(m_supportedExtensions, this);
}

std::time_t AssetInfo::GetAssetLastModificationTime() const
{
	return Sailor::Utils::GetFileModificationTime(GetAssetFilepath());
}

std::time_t AssetInfo::GetAssetImportTime() const
{
	return m_assetImportTime;
}

std::time_t AssetInfo::GetMetaLastModificationTime() const
{
	return Sailor::Utils::GetFileModificationTime(GetMetaFilepath());
}

std::string AssetInfo::GetMetaFilepath() const
{
	if (!m_metaFilepath.empty())
	{
		return m_metaFilepath;
	}
	return m_folder + m_assetFilename + "." + AssetRegistry::MetaFileExtension;
}

std::string AssetInfo::GetRelativeAssetFilepath() const
{
	if (!m_virtualAssetFilepath.empty())
	{
		return m_virtualAssetFilepath;
	}

	std::string res = GetAssetFilepath();
	Utils::Erase(res, AssetRegistry::GetContentFolder());
	return res;
}

std::string AssetInfo::GetRelativeMetaFilepath() const
{
	if (!m_virtualMetaFilepath.empty())
	{
		return m_virtualMetaFilepath;
	}

	std::string res = GetMetaFilepath();
	Utils::Erase(res, AssetRegistry::GetContentFolder());
	return res;
}

IAssetInfoHandler* AssetInfo::GetHandler()
{
	return App::GetSubmodule<DefaultAssetInfoHandler>();
}

AssetInfoPtr IAssetInfoHandler::ImportAsset(
	const std::string& assetFilepath,
	const std::string& virtualAssetFilepath,
	bool bNotifyListeners,
	bool bUpdateAssetCache) const
{
	const std::string assetInfoFilename = AssetRegistry::GetMetaFilePath(assetFilepath);

	YAML::Node newMeta;
	GetDefaultMeta(newMeta);
	if (!newMeta.IsMap())
	{
		SAILOR_LOG_ERROR("Cannot import asset because its handler returned invalid default metadata: %s", assetFilepath.c_str());
		return nullptr;
	}

	AssetInfoPtr defaultAssetInfo = CreateAssetInfo();
	if (defaultAssetInfo == nullptr)
	{
		SAILOR_LOG_ERROR("Cannot import asset because its handler could not create AssetInfo: %s", assetFilepath.c_str());
		return nullptr;
	}
	newMeta["assetInfoType"] = defaultAssetInfo->GetAssetInfoType();
	delete defaultAssetInfo;

	auto fileId = FileId::CreateNewFileId();
	newMeta["fileId"] = fileId.Serialize();
	newMeta["filename"] = std::filesystem::path(assetFilepath).filename().string();

	std::string importedMetadataContents;
	std::string writeDiagnostic;
	if (!WriteNewMetadataFile(
			assetInfoFilename,
			newMeta,
			importedMetadataContents,
			writeDiagnostic))
	{
		SAILOR_LOG_ERROR(
			"Cannot import asset without overwriting metadata '%s': %s",
			assetInfoFilename.c_str(),
			writeDiagnostic.c_str());
		return nullptr;
	}

	const std::string virtualMetaFilepath = virtualAssetFilepath.empty()
		? std::string{}
		: virtualAssetFilepath + "." + AssetRegistry::MetaFileExtension;
	auto assetInfoPtr = LoadAssetInfo(
		assetInfoFilename,
		virtualMetaFilepath,
		EAssetMountKind::Workspace,
		true,
		false,
		false);
	if (assetInfoPtr == nullptr)
	{
		RemoveFileIfContentsMatch(assetInfoFilename, importedMetadataContents);
		return nullptr;
	}

	assetInfoPtr->m_importedMetadataContents = std::move(importedMetadataContents);
	assetInfoPtr->m_bPendingUpdateNotification = true;
	assetInfoPtr->m_bPendingWasExpired = false;
	assetInfoPtr->m_bPendingImportNotification = true;
	if (bNotifyListeners)
	{
		NotifyUpdateAssetInfo(assetInfoPtr);
		NotifyImportAsset(assetInfoPtr);
	}
	if (bUpdateAssetCache)
	{
		App::GetSubmodule<AssetRegistry>()->CacheAsset(assetInfoPtr);
	}

	return assetInfoPtr;
}

bool IAssetInfoHandler::DiscardImportedMetadataIfUnchanged(AssetInfoPtr assetInfo) const
{
	if (assetInfo == nullptr || assetInfo->m_importedMetadataContents.empty())
	{
		return false;
	}

	const bool bRemoved = RemoveFileIfContentsMatch(
		assetInfo->GetMetaFilepath(),
		assetInfo->m_importedMetadataContents);
	assetInfo->m_importedMetadataContents.clear();
	return bRemoved;
}

AssetInfoPtr IAssetInfoHandler::LoadAssetInfo(
	const std::string& assetInfoPath,
	const std::string& virtualMetaFilepath,
	EAssetMountKind mountKind,
	bool bWritable,
	bool bNotifyListeners,
	bool bUpdateAssetCache) const
{
	AssetInfoPtr res = CreateAssetInfo();
	res->m_folder = std::filesystem::path(assetInfoPath).remove_filename().string();
	res->m_metaFilepath = assetInfoPath;
	res->m_virtualMetaFilepath = virtualMetaFilepath;
	res->m_mountKind = mountKind;
	res->m_bWritable = bWritable;

	if (!ReloadAssetInfo(res, bNotifyListeners, bUpdateAssetCache))
	{
		delete res;
		return nullptr;
	}

	return res;
}

bool IAssetInfoHandler::ReloadAssetInfo(
	AssetInfoPtr assetInfo,
	bool bNotifyListeners,
	bool bUpdateAssetCache) const
{
	if (assetInfo == nullptr)
	{
		SAILOR_LOG_ERROR("Cannot reload null asset metadata.");
		return false;
	}

	const bool bHadLoadedIdentity = static_cast<bool>(assetInfo->GetFileId());
	const bool bWasMetaExpired = bHadLoadedIdentity && (assetInfo->IsMetaExpired() || assetInfo->m_bPendingWasExpired);
	const bool bWasAssetExpired = bHadLoadedIdentity && assetInfo->IsAssetExpired();
	const std::string metadataPath = assetInfo->GetMetaFilepath();
	const std::time_t metadataLoadTime = assetInfo->GetMetaLastModificationTime();
	FileRevision metadataRevision;
	if (!Utils::TryGetFileRevision(metadataPath, metadataRevision))
	{
		SAILOR_LOG_ERROR("Failed to capture asset metadata revision: %s", metadataPath.c_str());
		return false;
	}
	std::string content;
	if (!AssetRegistry::ReadAllTextFile(metadataPath, content))
	{
		SAILOR_LOG_ERROR("Failed to read asset metadata: %s", metadataPath.c_str());
		return false;
	}

	// A complete file starts from typed defaults, not the previous live values.
	TUniquePtr<AssetInfo> metadata(CreateAssetInfo());
	metadata->m_assetFilename = std::filesystem::path(metadataPath).stem().string();
	YAML::Node document;
	std::string diagnostic;
	if (!External::TryLoadYaml(content, document, diagnostic) ||
		!External::GuardYamlExceptions([&]() { metadata->Deserialize(document); }, diagnostic))
	{
		SAILOR_LOG_ERROR("Invalid asset metadata '%s': %s", metadataPath.c_str(), diagnostic.c_str());
		return false;
	}
	FileRevision currentMetadataRevision;
	if (!Utils::TryGetFileRevision(metadataPath, currentMetadataRevision) || currentMetadataRevision != metadataRevision)
	{
		SAILOR_LOG_ERROR("Asset metadata changed while it was being loaded; preserving the previous live asset: %s",
			metadataPath.c_str());
		return false;
	}
	if (bHadLoadedIdentity && (metadata->GetFileId() != assetInfo->GetFileId() ||
		metadata->GetAssetFilename() != assetInfo->GetAssetFilename()))
	{
		SAILOR_LOG_ERROR("Asset metadata FileId and source cannot change during an in-place reload: %s", metadataPath.c_str());
		return false;
	}

	FileRevision sourceRevision;
	const std::string sourceFilepath = assetInfo->m_folder + metadata->GetAssetFilename();
	if (!TryGetSourceRevision(sourceFilepath, sourceRevision))
	{
		SAILOR_LOG_ERROR("Failed to capture asset source revision: %s", sourceFilepath.c_str());
		return false;
	}

	assetInfo->CopyMetadata(*metadata);
	if (!assetInfo->m_virtualMetaFilepath.empty())
	{
		assetInfo->m_virtualAssetFilepath = (
			std::filesystem::path(assetInfo->m_virtualMetaFilepath).parent_path() /
			assetInfo->m_assetFilename).generic_string();
	}
	AssetRegistry* assetRegistry = App::GetSubmodule<AssetRegistry>();
	const bool bWasCacheExpired = assetRegistry == nullptr ||
		!assetRegistry->RestoreAssetImportTime(assetInfo, sourceRevision);

	assetInfo->m_assetImportTime = assetInfo->GetAssetLastModificationTime();
	assetInfo->m_importedSourceRevision = sourceRevision;
	assetInfo->m_metaLoadTime = metadataLoadTime;
	assetInfo->m_metadataRevision = metadataRevision;
	assetInfo->m_bPendingUpdateNotification = true;
	assetInfo->m_bPendingWasExpired = bWasMetaExpired || bWasAssetExpired || bWasCacheExpired;
	if (bNotifyListeners)
	{
		NotifyUpdateAssetInfo(assetInfo);
	}
	if (bUpdateAssetCache && assetRegistry != nullptr)
	{
		assetRegistry->CacheAsset(assetInfo);
	}
	return true;
}

void IAssetInfoHandler::NotifyUpdateAssetInfo(AssetInfoPtr assetInfo) const
{
	if (assetInfo == nullptr)
	{
		return;
	}

	const bool bWasExpired = assetInfo->m_bPendingWasExpired;
	assetInfo->m_bPendingUpdateNotification = true;
	for (IAssetInfoHandlerListener* listener : m_listeners)
	{
		listener->OnUpdateAssetInfo(assetInfo, bWasExpired);
	}
	assetInfo->m_bPendingUpdateNotification = false;
	assetInfo->m_bPendingWasExpired = false;
}

void IAssetInfoHandler::NotifyImportAsset(AssetInfoPtr assetInfo) const
{
	if (assetInfo == nullptr)
	{
		return;
	}

	for (IAssetInfoHandlerListener* listener : m_listeners)
	{
		listener->OnImportAsset(assetInfo);
	}
	assetInfo->m_bPendingImportNotification = false;
	assetInfo->m_importedMetadataContents.clear();
}

void DefaultAssetInfoHandler::GetDefaultMeta(YAML::Node& outDefaultYaml) const
{
	AssetInfo defaultObject;
	outDefaultYaml = defaultObject.Serialize();
}

AssetInfoPtr DefaultAssetInfoHandler::CreateAssetInfo() const
{
	return new AssetInfo();
}
