#include "AssetRegistry/AssetRegistry.h"
#include "Containers/Containers.h"

#include "AssetRegistry/Animation/AnimationAssetInfo.h"
#include "AssetRegistry/AssetInfo.h"
#include "AssetRegistry/FrameGraph/FrameGraphAssetInfo.h"
#include "AssetRegistry/Material/MaterialAssetInfo.h"
#include "AssetRegistry/Model/ModelAssetInfo.h"
#include "AssetRegistry/Prefab/PrefabAssetInfo.h"
#include "AssetRegistry/Shader/ShaderAssetInfo.h"
#include "AssetRegistry/Texture/TextureAssetInfo.h"
#include "AssetRegistry/World/WorldPrefabAssetInfo.h"
#include "Containers/Map.h"
#include "Core/Utils.h"
#include "Tasks/Scheduler.h"
#include "Tasks/Tasks.h"
#include "YamlExceptionBoundary.h"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <map>
#include <set>
#include <string>
#include <tuple>
#include <utility>
#include <vector>
#include <yaml-cpp/yaml.h>

using namespace Sailor;

namespace
{
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
	};

	std::string AsFolderPath(const std::filesystem::path& path)
	{
		std::string result = path.generic_string();
		if (!result.ends_with('/'))
		{
			result += '/';
		}
		return result;
	}

	std::string Lowercase(std::string value)
	{
		std::transform(value.begin(), value.end(), value.begin(), [](unsigned char character)
			{
				return static_cast<char>(std::tolower(character));
			});
		return value;
	}

	std::string PathKey(const std::filesystem::path& path)
	{
		std::string value = path.lexically_normal().generic_string();
#if defined(_WIN32)
		value = Lowercase(std::move(value));
#endif
		while (value.size() > 1 && value.back() == '/')
		{
			value.pop_back();
		}
		return value;
	}

	std::string VirtualPathKey(std::string path)
	{
		std::replace(path.begin(), path.end(), '\\', '/');
		while (path.rfind("./", 0) == 0)
		{
			path.erase(0, 2);
		}
		while (!path.empty() && path.front() == '/')
		{
			path.erase(path.begin());
		}
#if defined(_WIN32)
		path = Lowercase(std::move(path));
#endif
		return path;
	}

	bool IsSafeVirtualPath(const std::string& path)
	{
		if (path.empty() || path.find('\0') != std::string::npos)
		{
			return false;
		}

		const std::filesystem::path value(path);
		if (value.is_absolute() || value.has_root_name() || value.has_root_directory())
		{
			return false;
		}

		return std::none_of(value.begin(), value.end(), [](const std::filesystem::path& component)
			{
				return component == "..";
			});
	}

	bool IsInside(const std::filesystem::path& root, const std::filesystem::path& candidate)
	{
		const std::string rootKey = PathKey(root);
		const std::string candidateKey = PathKey(candidate);
		return candidateKey == rootKey ||
			(candidateKey.size() > rootKey.size() &&
				candidateKey.compare(0, rootKey.size(), rootKey) == 0 &&
				candidateKey[rootKey.size()] == '/');
	}

	std::string MountFileKey(const AssetMountDescriptor& mount, const std::string& virtualPath)
	{
		return PathKey(mount.m_root) + "\n" + VirtualPathKey(virtualPath);
	}

	std::string Extension(const std::string& path)
	{
		return Lowercase(Utils::GetFileExtension(path));
	}

	std::string HandlerExtension(const StagedAssetRecord& record)
	{
		if (record.m_bPrimary)
		{
			return Extension(record.m_assetVirtualPath);
		}

		return Extension(std::filesystem::path(
			record.m_metaVirtualPath).replace_extension().generic_string());
	}

	bool CandidateMatches(const AssetMountCandidate* winner, const AssetMountCandidate& candidate)
	{
		return winner != nullptr &&
			PathKey(winner->m_physicalPath) == PathKey(candidate.m_physicalPath) &&
			VirtualPathKey(winner->m_virtualPath) == VirtualPathKey(candidate.m_virtualPath) &&
			winner->m_fileId == candidate.m_fileId;
	}

	bool ReadMetadataIdentity(
		const std::filesystem::path& metaPath,
		std::string& outFileId,
		std::string& outFilename,
		std::string& outAssetInfoType,
		std::string& outError)
	{
		outFileId.clear();
		outFilename.clear();
		outAssetInfoType.clear();
		outError.clear();

		bool bSuccess = false;
		std::string yamlDiagnostic;
		if (!External::GuardYamlExceptions(
				[&]()
				{
					const YAML::Node metadata = YAML::LoadFile(metaPath.string());
					if (!metadata.IsMap())
					{
						outError = "metadata root must be a map";
						return;
					}

					const YAML::Node fileId = metadata["fileId"];
					const YAML::Node filename = metadata["filename"];
					if (!fileId.IsScalar() || !filename.IsScalar())
					{
						outError = "metadata requires scalar fileId and filename";
						return;
					}

					outFileId = fileId.as<std::string>();
					outFilename = filename.as<std::string>();
					YAML::Node assetInfoType(YAML::NodeType::Undefined);
					for (const auto& field : metadata)
					{
						if (field.first.IsScalar() && field.first.Scalar() == "assetInfoType")
						{
							assetInfoType = field.second;
							break;
						}
					}
					if (assetInfoType.IsDefined() && !assetInfoType.IsNull() && !assetInfoType.IsScalar())
					{
						outError = "metadata assetInfoType must be scalar when present";
						return;
					}
					if (assetInfoType.IsScalar())
					{
						outAssetInfoType = assetInfoType.as<std::string>();
					}
					if (outFileId.empty() || outFilename.empty() ||
						!IsSafeVirtualPath(std::filesystem::path(outFilename).generic_string()))
					{
						outError = "metadata contains an empty FileId or unsafe filename";
						return;
					}

					bSuccess = true;
				},
				yamlDiagnostic))
		{
			outFileId.clear();
			outFilename.clear();
			outAssetInfoType.clear();
			outError = std::move(yamlDiagnostic);
			return false;
		}
		return bSuccess;
	}

	FileId ParseFileId(const std::string& value)
	{
		FileId result;
		result.Deserialize(YAML::Node(value));
		return result;
	}

	void DeleteAssetInfos(TMap<FileId, AssetInfoPtr>& assetInfos)
	{
		for (const auto& assetInfo : assetInfos)
		{
			delete *assetInfo.m_second;
		}
		assetInfos.Clear();
	}

}

#if defined(SAILOR_ASSET_REGISTRY_TEST_HOOKS)
bool AssetRegistryTestAccess::TryReadMetadataIdentity(
	const std::filesystem::path& metaPath,
	std::string& outFileId,
	std::string& outFilename,
	std::string& outAssetInfoType,
	std::string& outError)
{
	return ReadMetadataIdentity(
		metaPath,
		outFileId,
		outFilename,
		outAssetInfoType,
		outError);
}
#endif

std::string AssetRegistry::GetContentFolder()
{
	return GetWorkspaceContentFolder();
}

std::string AssetRegistry::GetWorkspaceContentFolder()
{
	return AsFolderPath(App::GetWorkspaceContext().GetContent());
}

std::string AssetRegistry::GetEngineContentFolder()
{
	return AsFolderPath(App::GetWorkspaceContext().GetEngineContent());
}

std::string AssetRegistry::GetCacheFolder()
{
	return AsFolderPath(App::GetWorkspaceContext().GetCache());
}

AssetRegistry::AssetRegistry()
{
	const Workspace::WorkspaceContext& context = App::GetWorkspaceContext();
	m_contentMounts =
	{
		AssetMountDescriptor{ context.GetEngineContent(), EAssetMountKind::Engine, 0, false },
		AssetMountDescriptor{ context.GetContent(), EAssetMountKind::Workspace, 100, true }
	};
	m_assetCache.Initialize();
}

void AssetRegistry::CacheAssetTime(
	const FileId& id,
	const std::string& sourcePath,
	const time_t& assetTimestamp)
{
	m_assetCache.Update(id, sourcePath, assetTimestamp);
}

bool AssetRegistry::GetAssetCachedTime(
	const FileId& id,
	const std::string& sourcePath,
	time_t& outAssetTimestamp) const
{
	return m_assetCache.GetTimeStamp(id, sourcePath, outAssetTimestamp);
}

bool AssetRegistry::IsAssetExpired(const AssetInfoPtr info) const
{
	return m_assetCache.IsExpired(info);
}

bool AssetRegistry::ReadAllTextFile(const std::string& filename, std::string& text)
{
	SAILOR_PROFILE_FUNCTION();

	constexpr auto readSize = std::size_t{ 4096 };
	auto stream = std::ifstream{ filename.data() };
	if (!stream.is_open())
	{
		return false;
	}

	text.clear();
	auto buffer = std::string(readSize, '\0');
	while (stream.read(buffer.data(), readSize))
	{
		text.append(buffer, 0, stream.gcount());
	}
	text.append(buffer, 0, stream.gcount());
	if (stream.bad())
	{
		text.clear();
		return false;
	}
	return true;
}

bool AssetRegistry::ResolveContentFile(
	const std::string& virtualPath,
	AssetReadLocation& outLocation) const
{
	if (!IsSafeVirtualPath(virtualPath))
	{
		return false;
	}

	const auto winner = m_contentFileWinners.Find(VirtualPathKey(virtualPath));
	if (winner == m_contentFileWinners.end())
	{
		return false;
	}
	outLocation = winner.Value();
	return true;
}

bool AssetRegistry::ReadContentText(const std::string& virtualPath, std::string& outText) const
{
	AssetReadLocation location;
	return ResolveContentFile(virtualPath, location) &&
		ReadAllTextFile(location.m_physicalPath.string(), outText);
}

bool AssetRegistry::GetContentFileModificationTime(
	const std::string& virtualPath,
	std::time_t& outTimestamp) const
{
	AssetReadLocation location;
	if (!ResolveContentFile(virtualPath, location))
	{
		return false;
	}
	outTimestamp = Utils::GetFileModificationTime(location.m_physicalPath.string());
	return true;
}

bool AssetRegistry::ResolveWorkspaceContentPathForWrite(
	const std::string& virtualPath,
	std::filesystem::path& outPath) const
{
	if (!IsSafeVirtualPath(virtualPath))
	{
		return false;
	}

	std::error_code error;
	const std::filesystem::path root = App::GetWorkspaceContext().GetContent();
	outPath = std::filesystem::weakly_canonical(root / std::filesystem::path(virtualPath), error);
	return !error && IsInside(root, outPath);
}

IAssetInfoHandler* AssetRegistry::GetAssetInfoHandler(const std::string& extension) const
{
	IAssetInfoHandler* handler = App::GetSubmodule<DefaultAssetInfoHandler>();
	auto handlerIt = m_assetInfoHandlers.Find(Lowercase(extension));
	if (handlerIt != m_assetInfoHandlers.end())
	{
		handler = *(*handlerIt).m_second;
	}
	return handler;
}

IAssetInfoHandler* AssetRegistry::GetAssetInfoHandler(
	const std::string& extension,
	const std::string& assetInfoType,
	bool bPrimary) const
{
	if (bPrimary || assetInfoType.empty())
	{
		return GetAssetInfoHandler(extension);
	}
	if (assetInfoType == "Sailor::AnimationAssetInfo")
	{
		return App::GetSubmodule<AnimationAssetInfoHandler>();
	}
	if (assetInfoType == "Sailor::FrameGraphAssetInfo")
	{
		return App::GetSubmodule<FrameGraphAssetInfoHandler>();
	}
	if (assetInfoType == "Sailor::MaterialAssetInfo")
	{
		return App::GetSubmodule<MaterialAssetInfoHandler>();
	}
	if (assetInfoType == "Sailor::ModelAssetInfo")
	{
		return App::GetSubmodule<ModelAssetInfoHandler>();
	}
	if (assetInfoType == "Sailor::PrefabAssetInfo")
	{
		return App::GetSubmodule<PrefabAssetInfoHandler>();
	}
	if (assetInfoType == "Sailor::ShaderAssetInfo")
	{
		return App::GetSubmodule<ShaderAssetInfoHandler>();
	}
	if (assetInfoType == "Sailor::TextureAssetInfo")
	{
		return App::GetSubmodule<TextureAssetInfoHandler>();
	}
	if (assetInfoType == "Sailor::WorldPrefabAssetInfo")
	{
		return App::GetSubmodule<WorldPrefabAssetInfoHandler>();
	}
	return GetAssetInfoHandler(extension);
}

void AssetRegistry::ScanContentFolder()
{
	SAILOR_PROFILE_FUNCTION();

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
		return;
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
		if (!ReadMetadataIdentity(
				metaFile.m_physicalPath,
				fileId,
				filename,
				assetInfoType,
				metadataError))
		{
			invalidMetadata.Insert(metaKey);
			SAILOR_LOG_ERROR(
				"Invalid asset metadata '%s': %s.",
				metaFile.m_physicalPath.generic_string().c_str(),
				metadataError.c_str());
			continue;
		}

		const std::string basenameVirtualPath = std::filesystem::path(
			metaFile.m_virtualPath).replace_extension().generic_string();
		const auto basenameAsset = filesByMountAndVirtualPath.Find(
			MountFileKey(metaFile.m_mount, basenameVirtualPath));
		const bool bFilenameMatchesBasename =
			std::filesystem::path(filename).generic_string() ==
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
			const std::string declaredVirtualPath = (
				std::filesystem::path(metaFile.m_virtualPath).parent_path() /
				filename).generic_string();
			const auto declaredAsset = filesByMountAndVirtualPath.Find(
				MountFileKey(metaFile.m_mount, declaredVirtualPath));
			if (declaredAsset == filesByMountAndVirtualPath.end())
			{
				SAILOR_LOG_ERROR(
					"Asset metadata '%s' references missing source '%s' and was skipped.",
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
			!CandidateMatches(
				resolution.FindByVirtualPath(record.m_candidate.m_virtualPath),
				record.m_candidate))
		{
			continue;
		}

		stagedContentWinners[VirtualPathKey(record.m_assetVirtualPath)] = AssetReadLocation
		{
			record.m_assetPath,
			record.m_assetVirtualPath,
			record.m_candidate.m_mount.m_kind,
			record.m_candidate.m_mount.m_bWritable
		};
	}

	TMap<FileId, AssetInfoPtr> stagedAssetInfos;
	TMap<std::string, FileId> stagedFileIds;
	TMap<std::string, FileId> stagedPhysicalFileIds;
	TVector<PendingAssetNotification> pendingNotifications;
	TVector<std::filesystem::path> importedMetadata;
	auto rollbackStaging = [&]()
	{
		DeleteAssetInfos(stagedAssetInfos);
		for (size_t index = importedMetadata.Num(); index > 0; --index)
		{
			std::error_code removeError;
			std::filesystem::remove(importedMetadata[index - 1], removeError);
		}
	};
	for (const StagedAssetRecord& record : records)
	{
		const bool bVirtualWinner = CandidateMatches(
			resolution.FindByVirtualPath(record.m_candidate.m_virtualPath),
			record.m_candidate);
		if (record.m_candidate.m_fileId.empty())
		{
			if (!record.m_bPrimary || !bVirtualWinner || record.m_bMetadataInvalid)
			{
				continue;
			}
			if (!record.m_candidate.m_mount.m_bWritable)
			{
				SAILOR_LOG_ERROR(
					"Read-only Engine asset '%s' has no metadata and was not imported.",
					record.m_assetVirtualPath.c_str());
				continue;
			}

			IAssetInfoHandler* handler = GetAssetInfoHandler(Extension(record.m_assetVirtualPath));
			check(handler);
			const bool bMetaExisted = std::filesystem::exists(record.m_metaPath);
			if (!bMetaExisted)
			{
				importedMetadata.Add(record.m_metaPath);
			}
			AssetInfoPtr assetInfo = handler->ImportAsset(
				record.m_assetPath.string(),
				record.m_assetVirtualPath,
				false,
				false);
			if (assetInfo == nullptr)
			{
				rollbackStaging();
				SAILOR_LOG_ERROR(
					"Failed to import asset metadata during staged load: %s",
					record.m_metaPath.generic_string().c_str());
				return;
			}
			stagedAssetInfos[assetInfo->GetFileId()] = assetInfo;
			stagedFileIds[VirtualPathKey(record.m_assetVirtualPath)] = assetInfo->GetFileId();
			stagedPhysicalFileIds[PathKey(record.m_assetPath)] = assetInfo->GetFileId();
			pendingNotifications.Add({ handler, assetInfo, true });
			continue;
		}

		if (!CandidateMatches(
				resolution.FindByFileId(record.m_candidate.m_fileId),
				record.m_candidate))
		{
			continue;
		}

		const FileId expectedFileId = ParseFileId(record.m_candidate.m_fileId);
		auto previousAssetInfo = m_loadedAssetInfo.Find(expectedFileId);
		const bool bWasPreviouslyExpired = previousAssetInfo != m_loadedAssetInfo.end() &&
			(previousAssetInfo.Value()->IsMetaExpired() ||
				previousAssetInfo.Value()->IsAssetExpired() ||
				PathKey(previousAssetInfo.Value()->GetAssetFilepath()) != PathKey(record.m_assetPath));

		IAssetInfoHandler* handler = GetAssetInfoHandler(
			HandlerExtension(record),
			record.m_assetInfoType,
			record.m_bPrimary);
		check(handler);
		AssetInfoPtr assetInfo = handler->LoadAssetInfo(
			record.m_metaPath.string(),
			record.m_metaVirtualPath,
			record.m_candidate.m_mount.m_kind,
			record.m_candidate.m_mount.m_bWritable,
			false,
			false);
		if (assetInfo == nullptr)
		{
			rollbackStaging();
			SAILOR_LOG_ERROR(
				"Failed to load asset metadata during staged load: %s",
				record.m_metaPath.generic_string().c_str());
			return;
		}
		if (assetInfo->GetFileId().ToString() != record.m_candidate.m_fileId)
		{
			delete assetInfo;
			rollbackStaging();
			SAILOR_LOG_ERROR(
				"Asset metadata FileId changed during staged load: %s",
				record.m_metaPath.generic_string().c_str());
			return;
		}

		const FileId fileId = assetInfo->GetFileId();
		assetInfo->m_bPendingWasExpired |= bWasPreviouslyExpired;
		stagedAssetInfos[fileId] = assetInfo;
		if (record.m_bPrimary)
		{
			stagedPhysicalFileIds[PathKey(record.m_assetPath)] = fileId;
		}
		if (record.m_bPrimary && bVirtualWinner)
		{
			stagedFileIds[VirtualPathKey(record.m_assetVirtualPath)] = fileId;
		}
		pendingNotifications.Add({ handler, assetInfo, false });
	}

	for (const StagedAssetRecord& record : records)
	{
		if (!record.m_bPrimary || record.m_candidate.m_fileId.empty() ||
			!CandidateMatches(
				resolution.FindByVirtualPath(record.m_candidate.m_virtualPath),
				record.m_candidate))
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

	if (Tasks::Scheduler* scheduler = App::GetSubmodule<Tasks::Scheduler>())
	{
		if (!scheduler->IsMainThread())
		{
			DeleteAssetInfos(stagedAssetInfos);
			for (size_t i = importedMetadata.Num(); i > 0; --i)
			{
				std::error_code removeError;
				std::filesystem::remove(importedMetadata[i - 1], removeError);
			}
			SAILOR_LOG_ERROR(
				"Asset registry generations may only be committed from the main thread; preserving the previous generation.");
			return;
		}
		scheduler->WaitIdle({
			EThreadType::Main,
			EThreadType::Worker,
			EThreadType::RHI,
			EThreadType::Render
		});
	}

	TMap<FileId, AssetInfoPtr> previousAssetInfos = std::move(m_loadedAssetInfo);
	m_loadedAssetInfo = std::move(stagedAssetInfos);
	m_fileIds = std::move(stagedFileIds);
	m_physicalFileIds = std::move(stagedPhysicalFileIds);
	m_contentMounts = discovery.m_mounts;
	m_contentFileWinners = std::move(stagedContentWinners);
	DeleteAssetInfos(previousAssetInfos);

	for (const PendingAssetNotification& pending : pendingNotifications)
	{
		CacheAssetTime(
			pending.m_assetInfo->GetFileId(),
			pending.m_assetInfo->GetAssetFilepath(),
			pending.m_assetInfo->GetAssetImportTime());
		pending.m_handler->NotifyUpdateAssetInfo(pending.m_assetInfo);
		if (pending.m_bImported)
		{
			pending.m_handler->NotifyImportAsset(pending.m_assetInfo);
		}
	}
	m_assetCache.SaveCache();
}

bool AssetRegistry::RegisterAssetInfoHandler(
	const TVector<std::string>& supportedExtensions,
	IAssetInfoHandler* assetInfoHandler)
{
	SAILOR_PROFILE_FUNCTION();

	bool bAssigned = false;
	for (const std::string& extension : supportedExtensions)
	{
		const std::string key = Lowercase(extension);
		if (!m_assetInfoHandlers.ContainsKey(key))
		{
			m_assetInfoHandlers[key] = assetInfoHandler;
			bAssigned = true;
		}
	}
	return bAssigned;
}

std::string AssetRegistry::GetMetaFilePath(const std::string& assetFilepath)
{
	return assetFilepath + "." + MetaFileExtension;
}

bool AssetRegistry::ResolveDirectLoadPath(
	const std::string& requestedPath,
	AssetReadLocation& outLocation) const
{
	if (IsSafeVirtualPath(requestedPath))
	{
		std::error_code workspaceError;
		const std::filesystem::path workspaceCandidate = std::filesystem::weakly_canonical(
			App::GetWorkspaceContext().GetContent() / requestedPath,
			workspaceError);
		if (!workspaceError &&
			std::filesystem::is_regular_file(workspaceCandidate, workspaceError) &&
			!workspaceError &&
			IsInside(App::GetWorkspaceContext().GetContent(), workspaceCandidate))
		{
			const std::string virtualPath = std::filesystem::path(requestedPath).generic_string();
			const auto winner = m_contentFileWinners.Find(VirtualPathKey(virtualPath));
			if (winner == m_contentFileWinners.end() ||
				winner.Value().m_mountKind == EAssetMountKind::Engine ||
				PathKey(winner.Value().m_physicalPath) == PathKey(workspaceCandidate))
			{
				outLocation = AssetReadLocation{
					workspaceCandidate,
					virtualPath,
					EAssetMountKind::Workspace,
					true
				};
				return true;
			}
		}
		if (ResolveContentFile(requestedPath, outLocation))
		{
			return true;
		}
	}

	std::error_code error;
	const std::filesystem::path requested(requestedPath);
	const std::filesystem::path candidate = std::filesystem::weakly_canonical(
		requested.is_absolute()
			? requested
			: App::GetWorkspaceContext().GetContent() / requested,
		error);
	if (error || !std::filesystem::is_regular_file(candidate, error) || error)
	{
		return false;
	}

	for (const AssetMountDescriptor& mount : m_contentMounts)
	{
		if (IsInside(mount.m_root, candidate))
		{
			const std::string virtualPath = candidate.lexically_relative(mount.m_root).generic_string();
			const auto winner = m_contentFileWinners.Find(VirtualPathKey(virtualPath));
			if (winner != m_contentFileWinners.end() &&
				PathKey(winner.Value().m_physicalPath) == PathKey(candidate))
			{
				outLocation = winner.Value();
				return true;
			}
			if (mount.m_kind != EAssetMountKind::Workspace || !mount.m_bWritable ||
				(winner != m_contentFileWinners.end() &&
					winner.Value().m_mountKind == EAssetMountKind::Workspace))
			{
				return false;
			}

			outLocation.m_physicalPath = candidate;
			outLocation.m_virtualPath = virtualPath;
			outLocation.m_mountKind = EAssetMountKind::Workspace;
			outLocation.m_bWritable = true;
			return true;
		}
	}

	const std::filesystem::path cache = App::GetWorkspaceContext().GetCache();
	const std::filesystem::path tempWorld = std::filesystem::weakly_canonical(
		cache / "Temp.world",
		error);
	if (!error && PathKey(candidate) == PathKey(tempWorld))
	{
		outLocation.m_physicalPath = candidate;
		outLocation.m_virtualPath = candidate.lexically_relative(
			App::GetWorkspaceContext().GetContent()).generic_string();
		outLocation.m_mountKind = EAssetMountKind::Workspace;
		outLocation.m_bWritable = true;
		return true;
	}
	return false;
}

const FileId& AssetRegistry::GetOrLoadFile(const std::string& assetFilepath)
{
	return LoadFile(assetFilepath);
}

const FileId& AssetRegistry::LoadFile(const std::string& requestedPath)
{
	AssetReadLocation location;
	if (!ResolveDirectLoadPath(requestedPath, location))
	{
		SAILOR_LOG_ERROR("Asset file could not be resolved: %s", requestedPath.c_str());
		return FileId::Invalid;
	}

	auto physicalId = m_physicalFileIds.Find(PathKey(location.m_physicalPath));
	if (physicalId != m_physicalFileIds.end())
	{
		return physicalId.Value();
	}

	if (Extension(location.m_physicalPath.string()) == MetaFileExtension)
	{
		SAILOR_LOG_ERROR("Direct metadata loading is not supported: %s", requestedPath.c_str());
		return FileId::Invalid;
	}

	IAssetInfoHandler* handler = GetAssetInfoHandler(Extension(location.m_physicalPath.string()));
	check(handler);
	const std::filesystem::path metaPath = GetMetaFilePath(location.m_physicalPath.string());
	AssetInfoPtr assetInfo = nullptr;
	bool bImported = false;
	if (std::filesystem::is_regular_file(metaPath))
	{
		assetInfo = handler->LoadAssetInfo(
			metaPath.string(),
			location.m_virtualPath + "." + MetaFileExtension,
			location.m_mountKind,
			location.m_bWritable,
			false,
			false);
	}
	else if (location.m_bWritable)
	{
		assetInfo = handler->ImportAsset(
			location.m_physicalPath.string(),
			location.m_virtualPath,
			false,
			false);
		bImported = true;
	}
	else
	{
		SAILOR_LOG_ERROR(
			"Read-only Engine asset '%s' has no metadata and was not imported.",
			location.m_virtualPath.c_str());
		return FileId::Invalid;
	}
	if (assetInfo == nullptr)
	{
		if (bImported)
		{
			std::error_code removeError;
			std::filesystem::remove(metaPath, removeError);
		}
		SAILOR_LOG_ERROR(
			"Failed to load asset metadata: %s",
			metaPath.generic_string().c_str());
		return FileId::Invalid;
	}

	const FileId fileId = assetInfo->GetFileId();
	auto existingAssetInfo = m_loadedAssetInfo.Find(fileId);
	if (existingAssetInfo != m_loadedAssetInfo.end())
	{
		const bool bSamePhysicalAsset =
			PathKey(existingAssetInfo.Value()->GetAssetFilepath()) ==
			PathKey(location.m_physicalPath);
		delete assetInfo;
		if (bImported)
		{
			std::error_code removeError;
			std::filesystem::remove(metaPath, removeError);
		}
		if (bSamePhysicalAsset)
		{
			m_physicalFileIds[PathKey(location.m_physicalPath)] = fileId;
			return existingAssetInfo.Value()->GetFileId();
		}

		SAILOR_LOG_ERROR(
			"Live asset '%s' collides with an active FileId; rescan Content to apply mount precedence.",
			location.m_virtualPath.c_str());
		return FileId::Invalid;
	}

	m_loadedAssetInfo[fileId] = assetInfo;
	m_physicalFileIds[PathKey(location.m_physicalPath)] = fileId;
	if (IsSafeVirtualPath(location.m_virtualPath))
	{
		const std::string virtualPathKey = VirtualPathKey(location.m_virtualPath);
		m_fileIds[virtualPathKey] = fileId;
		m_contentFileWinners[virtualPathKey] = location;
	}
	CacheAssetTime(fileId, location.m_physicalPath.string(), assetInfo->GetAssetImportTime());
	handler->NotifyUpdateAssetInfo(assetInfo);
	if (bImported)
	{
		handler->NotifyImportAsset(assetInfo);
	}
	return m_loadedAssetInfo.Find(fileId).Value()->GetFileId();
}

TObjectPtr<Object> AssetRegistry::LoadAsset(
	IAssetInfoHandler* assetInfoHandler,
	const FileId& id,
	bool bImmediate)
{
	TObjectPtr<Object> result;
	assetInfoHandler->GetFactory()->LoadAsset(id, result, bImmediate);
	return result;
}

AssetInfoPtr AssetRegistry::GetAssetInfoPtr_Internal(FileId uid) const
{
	SAILOR_PROFILE_FUNCTION();

	auto it = m_loadedAssetInfo.Find(uid);
	return it != m_loadedAssetInfo.end() ? it.Value() : nullptr;
}

AssetInfoPtr AssetRegistry::GetAssetInfoPtr_Internal(const std::string& assetFilepath) const
{
	if (IsSafeVirtualPath(assetFilepath))
	{
		auto virtualId = m_fileIds.Find(VirtualPathKey(assetFilepath));
		if (virtualId != m_fileIds.end())
		{
			return GetAssetInfoPtr_Internal(virtualId.Value());
		}
	}

	AssetReadLocation location;
	if (ResolveDirectLoadPath(assetFilepath, location))
	{
		auto physicalId = m_physicalFileIds.Find(PathKey(location.m_physicalPath));
		if (physicalId != m_physicalFileIds.end())
		{
			return GetAssetInfoPtr_Internal(physicalId.Value());
		}
	}
	return nullptr;
}

AssetRegistry::~AssetRegistry()
{
	m_assetCache.Shutdown();
	DeleteAssetInfos(m_loadedAssetInfo);
}
