#include "AssetRegistry/AssetInfo.h"
#include "AssetRegistry/AssetRegistry.h"
#include <filesystem>
#include <fstream>
#include "Core/Utils.h"
#include "Core/Reflection.h"
#include "Tasks/Scheduler.h"
#include "Tasks/Tasks.h"
#include "YamlExceptionBoundary.h"
#include <iostream>

using namespace Sailor;

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

void AssetInfo::SaveMetaFile()
{
	if (!m_bWritable)
	{
		SAILOR_LOG_ERROR("Cannot write read-only engine asset metadata: %s", GetMetaFilepath().c_str());
		return;
	}

	std::ofstream assetFile{ GetMetaFilepath() };

	YAML::Node node = Serialize();
	assetFile << node;
	assetFile.close();

	m_metaLoadTime = GetMetaLastModificationTime();
}

AssetInfo::AssetInfo()
{
	m_metaLoadTime = std::time(nullptr);
	m_assetImportTime = 0;
}

bool AssetInfo::IsAssetExpired() const
{
	if (!(std::filesystem::exists(m_folder)))
	{
		return true;
	}

	return m_assetImportTime < GetAssetLastModificationTime();
}

bool AssetInfo::IsMetaExpired() const
{
	if (!(std::filesystem::exists(m_folder)))
	{
		return true;
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
	std::filesystem::remove(assetInfoFilename);
	std::ofstream assetFile{ assetInfoFilename };

	YAML::Node newMeta;
	GetDefaultMeta(newMeta);

	auto fileId = FileId::CreateNewFileId();
	newMeta["fileId"] = fileId.Serialize();
	newMeta["filename"] = std::filesystem::path(assetFilepath).filename().string();

	assetFile << newMeta;
	assetFile.close();

	const std::string virtualMetaFilepath = virtualAssetFilepath.empty()
		? std::string{}
		: virtualAssetFilepath + "." + AssetRegistry::MetaFileExtension;
	auto assetInfoPtr = LoadAssetInfo(
		assetInfoFilename,
		virtualMetaFilepath,
		EAssetMountKind::Workspace,
		true,
		bNotifyListeners,
		bUpdateAssetCache);
	if (assetInfoPtr == nullptr)
	{
		std::error_code removeError;
		std::filesystem::remove(assetInfoFilename, removeError);
		return nullptr;
	}

	assetInfoPtr->m_bPendingImportNotification = !bNotifyListeners;
	if (bNotifyListeners)
	{
		NotifyImportAsset(assetInfoPtr);
	}

	return assetInfoPtr;
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

	// Temp to pass asset filename to Reload Asset Info
	const std::string filename = std::filesystem::path(assetInfoPath).filename().string();
	res->m_assetFilename = filename.substr(0, filename.length() - strlen(AssetRegistry::MetaFileExtension) - 1);
	if (!res->m_virtualMetaFilepath.empty())
	{
		res->m_virtualAssetFilepath = (
			std::filesystem::path(res->m_virtualMetaFilepath).parent_path() /
			res->m_assetFilename).generic_string();
	}

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

	const bool bWasMetaExpired = assetInfo->IsMetaExpired();

	std::string content;
	if (!AssetRegistry::ReadAllTextFile(assetInfo->GetMetaFilepath(), content))
	{
		SAILOR_LOG_ERROR(
			"Failed to read asset metadata: %s",
			assetInfo->GetMetaFilepath().c_str());
		return false;
	}

	std::string yamlDiagnostic;
	if (!External::GuardYamlExceptions(
			[assetInfo, &content]()
			{
				const YAML::Node meta = YAML::Load(content);
				assetInfo->Deserialize(meta);
			},
			yamlDiagnostic))
	{
		SAILOR_LOG_ERROR(
			"Invalid asset metadata '%s': %s",
			assetInfo->GetMetaFilepath().c_str(),
			yamlDiagnostic.c_str());
		return false;
	}

	if (!assetInfo->m_virtualMetaFilepath.empty())
	{
		assetInfo->m_virtualAssetFilepath = (
			std::filesystem::path(assetInfo->m_virtualMetaFilepath).parent_path() /
			assetInfo->m_assetFilename).generic_string();
	}
	assetInfo->m_metaLoadTime = std::time(nullptr);

	App::GetSubmodule<AssetRegistry>()->GetAssetCachedTime(
		assetInfo->GetFileId(),
		assetInfo->GetAssetFilepath(),
		assetInfo->m_assetImportTime);

	const bool bWasAssetExpired = assetInfo->IsAssetExpired();

	assetInfo->m_assetImportTime = assetInfo->GetAssetLastModificationTime();
	if (bUpdateAssetCache)
	{
		App::GetSubmodule<AssetRegistry>()->CacheAssetTime(
			assetInfo->GetFileId(),
			assetInfo->GetAssetFilepath(),
			assetInfo->m_assetImportTime);
	}

	assetInfo->m_bPendingUpdateNotification = !bNotifyListeners;
	assetInfo->m_bPendingWasExpired = bWasMetaExpired || bWasAssetExpired;
	if (bNotifyListeners)
	{
		NotifyUpdateAssetInfo(assetInfo);
	}

	return true;

	/*	if (bWasAssetExpired)
		{
			assetInfo->SaveMetaFile();
		}
		*/
}

void IAssetInfoHandler::NotifyUpdateAssetInfo(AssetInfoPtr assetInfo) const
{
	if (assetInfo == nullptr)
	{
		return;
	}

	const bool bWasExpired = assetInfo->m_bPendingWasExpired;
	assetInfo->m_bPendingUpdateNotification = false;
	assetInfo->m_bPendingWasExpired = false;
	for (IAssetInfoHandlerListener* listener : m_listeners)
	{
		listener->OnUpdateAssetInfo(assetInfo, bWasExpired);
	}
}

void IAssetInfoHandler::NotifyImportAsset(AssetInfoPtr assetInfo) const
{
	if (assetInfo == nullptr)
	{
		return;
	}

	assetInfo->m_bPendingImportNotification = false;
	for (IAssetInfoHandlerListener* listener : m_listeners)
	{
		listener->OnImportAsset(assetInfo);
	}
	assetInfo->SaveMetaFile();
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
