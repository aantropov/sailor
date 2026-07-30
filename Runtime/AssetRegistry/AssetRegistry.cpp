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
#include <atomic>
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
	std::atomic<uint64_t> g_nextAssetProcessingGeneration = 0;

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

	bool SameEffectiveContent(
		const AssetRegistry::AssetReadLocation& left,
		const AssetRegistry::AssetReadLocation& right)
	{
		return PathKey(left.m_physicalPath) == PathKey(right.m_physicalPath) &&
			VirtualPathKey(left.m_virtualPath) == VirtualPathKey(right.m_virtualPath) &&
			left.m_mountKind == right.m_mountKind &&
			left.m_revision == right.m_revision;
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

bool AssetRegistry::RestoreAssetImportTime(
	AssetInfoPtr info,
	const FileRevision& sourceRevision) const
{
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
	m_assetCache.Update(info);
}

AssetRegistry::AssetProcessingToken AssetRegistry::BeginAssetProcessing(AssetInfoPtr info)
{
	AssetProcessingToken token;
	if (info == nullptr || !info->GetFileId())
	{
		return token;
	}

	// Serialize capture, generation assignment, and publication. File revisions
	// are not time-ordered (a valid edit may be backdated), so generation alone
	// cannot safely decide which concurrently captured revision is newer.
	std::lock_guard<std::mutex> lock(m_assetProcessingMutex);
	token.m_fileId = info->GetFileId();
	token.m_sourcePath = info->GetAssetFilepath();
	token.m_assetImportTime = Utils::GetFileModificationTime(token.m_sourcePath);
	if (token.m_assetImportTime <= 0 ||
		!Utils::TryGetFileRevision(token.m_sourcePath, token.m_sourceRevision))
	{
		return {};
	}

	do
	{
		token.m_generation = g_nextAssetProcessingGeneration.fetch_add(
			1,
			std::memory_order_relaxed) + 1;
	}
	while (token.m_generation == 0);

	info->m_assetImportTime = token.m_assetImportTime;
	info->m_importedSourceRevision = token.m_sourceRevision;
	m_assetProcessingStates[token.m_fileId] = AssetProcessingState{ token, false };
	return token;
}

void AssetRegistry::CompleteAssetProcessing(
	const AssetProcessingToken& token,
	bool bSucceeded)
{
	if (!token)
	{
		return;
	}

	std::lock_guard<std::mutex> lock(m_assetProcessingMutex);
	auto processingState = m_assetProcessingStates.Find(token.m_fileId);
	if (processingState == m_assetProcessingStates.end() ||
		!processingState.Value().m_token.Matches(token))
	{
		return;
	}

	if (!bSucceeded)
	{
		processingState.Value().m_bRejected = true;
		SAILOR_LOG_ERROR(
			"Asset processing failed; preserving the previous cache revision for retry: %s",
			token.m_sourcePath.c_str());
		return;
	}

	const AssetProcessingToken acknowledgedToken = processingState.Value().m_token;
	m_assetCache.Update(
		acknowledgedToken.m_fileId,
		acknowledgedToken.m_assetImportTime,
		acknowledgedToken.m_sourcePath,
		acknowledgedToken.m_sourceRevision);
	m_assetCache.SaveCache();
	m_assetProcessingStates.Remove(acknowledgedToken.m_fileId);
}

void AssetRegistry::TrackScanProcessingTask(
	const Tasks::TaskPtr<bool>& processingTask)
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
		bSucceeded &= processingTask &&
			processingTask->IsFinished() &&
			processingTask->GetResult();
	}
	return bSucceeded;
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

bool AssetRegistry::ScanContentFolder()
{
	SAILOR_PROFILE_FUNCTION();
	{
		std::lock_guard<std::mutex> lock(m_assetProcessingMutex);
		m_scanProcessingTasks.Clear();
		m_bCollectScanProcessingTasks = false;
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
				SAILOR_LOG_ERROR(
					"Asset metadata reload was rejected; preserving the previous registry generation.");
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

		AssetReadLocation location
		{
			record.m_assetPath,
			record.m_assetVirtualPath,
			record.m_candidate.m_mount.m_kind,
			record.m_candidate.m_mount.m_bWritable
		};
		if (!Utils::TryGetFileRevision(
				location.m_physicalPath.generic_string(),
				location.m_revision))
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
			effectiveContentChanges[previousWinner.m_first] =
				previousWinner.m_second->m_virtualPath;
		}
	}
	for (const auto& stagedWinner : stagedContentWinners)
	{
		const auto previousWinner = m_contentFileWinners.Find(stagedWinner.m_first);
		if (previousWinner == m_contentFileWinners.end() ||
			!SameEffectiveContent(previousWinner.Value(), *stagedWinner.m_second))
		{
			effectiveContentChanges[stagedWinner.m_first] =
				stagedWinner.m_second->m_virtualPath;
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
			if ((metadataStatusError &&
				metadataStatusError != std::errc::no_such_file_or_directory &&
				metadataStatusError != std::errc::not_a_directory) ||
				std::filesystem::exists(metadataStatus))
			{
				rollbackStaging();
				SAILOR_LOG_ERROR(
					"Asset metadata appeared during staged import; preserving it and the previous registry generation: %s",
					record.m_metaPath.generic_string().c_str());
				return false;
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
				return false;
			}
			importedMetadata.Add({ handler, assetInfo });
			stagedAssetInfos[assetInfo->GetFileId()] = assetInfo;
			stagedFileIds[VirtualPathKey(record.m_assetVirtualPath)] = assetInfo->GetFileId();
			stagedPhysicalFileIds[PathKey(record.m_assetPath)] = assetInfo->GetFileId();
			pendingNotifications.Add({ handler, assetInfo, true, true });
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
		const bool bEffectiveContentChanged = bVirtualWinner &&
			effectiveContentChanges.ContainsKey(VirtualPathKey(record.m_assetVirtualPath));
		const bool bWasPreviouslyExpired = previousAssetInfo != m_loadedAssetInfo.end() &&
			(previousAssetInfo.Value()->IsMetaExpired() ||
				previousAssetInfo.Value()->IsAssetExpired() ||
				PathKey(previousAssetInfo.Value()->GetAssetFilepath()) != PathKey(record.m_assetPath) ||
				bEffectiveContentChanged);

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
			return false;
		}
		if (assetInfo->GetFileId().ToString() != record.m_candidate.m_fileId)
		{
			delete assetInfo;
			rollbackStaging();
			SAILOR_LOG_ERROR(
				"Asset metadata FileId changed during staged load: %s",
				record.m_metaPath.generic_string().c_str());
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
		const bool bNotifyUpdate = previousAssetInfo == m_loadedAssetInfo.end() ||
			assetInfo->m_bPendingWasExpired;
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
		pendingNotifications.Add({ handler, assetInfo, false, bNotifyUpdate });
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

	for (const auto& stagedAsset : stagedAssetInfos)
	{
		AssetInfoPtr assetInfo = *stagedAsset.m_second;
		if (assetInfo == nullptr || assetInfo->IsAssetExpired() || assetInfo->IsMetaExpired())
		{
			const std::string changedPath = assetInfo == nullptr
				? std::string("<null asset info>")
				: assetInfo->GetAssetFilepath();
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
			PathKey(effectiveWinner.Value().m_physicalPath) ==
				PathKey(pending.m_assetInfo->GetAssetFilepath()))
		{
			handledEffectiveContentChanges.Insert(virtualPathKey);
		}
	}

	if (Tasks::Scheduler* scheduler = App::GetSubmodule<Tasks::Scheduler>())
	{
		if (!scheduler->IsMainThread())
		{
			rollbackStaging();
			SAILOR_LOG_ERROR(
				"Asset registry generations may only be committed from the main thread; preserving the previous generation.");
			return false;
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
	{
		std::lock_guard<std::mutex> lock(m_assetProcessingMutex);
		m_bCollectScanProcessingTasks = true;
	}

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
				TrackScanProcessingTask(
					listener->OnEffectiveContentChanged(*effectiveChange.m_second));
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

void AssetRegistry::SubscribeContentChanges(IAssetRegistryContentListener* listener)
{
	if (listener != nullptr && !m_contentListeners.Contains(listener))
	{
		m_contentListeners.Add(listener);
	}
}

void AssetRegistry::UnsubscribeContentChanges(IAssetRegistryContentListener* listener)
{
	m_contentListeners.Remove(listener);
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

FileId AssetRegistry::RegisterGeneratedSecondaryAssetInfo(
	const std::filesystem::path& metadataPath)
{
	if (Tasks::Scheduler* scheduler = App::GetSubmodule<Tasks::Scheduler>();
		scheduler != nullptr && !scheduler->IsMainThread())
	{
		SAILOR_LOG_ERROR(
			"Generated secondary asset metadata may only be registered from the main thread: %s",
			metadataPath.generic_string().c_str());
		return FileId::Invalid;
	}

	std::error_code pathError;
	const std::filesystem::path canonicalMetadataPath =
		std::filesystem::weakly_canonical(metadataPath, pathError);
	if (pathError ||
		!std::filesystem::is_regular_file(canonicalMetadataPath, pathError) ||
		pathError)
	{
		SAILOR_LOG_ERROR(
			"Generated secondary asset metadata is not a regular file: %s",
			metadataPath.generic_string().c_str());
		return FileId::Invalid;
	}

	const AssetMountDescriptor* metadataMount = nullptr;
	for (const AssetMountDescriptor& mount : m_contentMounts)
	{
		if (mount.m_kind == EAssetMountKind::Workspace &&
			mount.m_bWritable &&
			IsInside(mount.m_root, canonicalMetadataPath))
		{
			metadataMount = &mount;
			break;
		}
	}
	if (metadataMount == nullptr)
	{
		SAILOR_LOG_ERROR(
			"Generated secondary asset metadata must be inside the writable workspace Content mount: %s",
			canonicalMetadataPath.generic_string().c_str());
		return FileId::Invalid;
	}

	std::string fileIdString;
	std::string filename;
	std::string assetInfoType;
	std::string metadataError;
	if (!ReadMetadataIdentity(
			canonicalMetadataPath,
			fileIdString,
			filename,
			assetInfoType,
			metadataError))
	{
		SAILOR_LOG_ERROR(
			"Cannot register generated secondary asset metadata '%s': %s.",
			canonicalMetadataPath.generic_string().c_str(),
			metadataError.c_str());
		return FileId::Invalid;
	}

	const std::filesystem::path canonicalSourcePath =
		std::filesystem::weakly_canonical(
			canonicalMetadataPath.parent_path() / filename,
			pathError);
	if (pathError ||
		!std::filesystem::is_regular_file(canonicalSourcePath, pathError) ||
		pathError ||
		!IsInside(metadataMount->m_root, canonicalSourcePath))
	{
		SAILOR_LOG_ERROR(
			"Generated secondary asset metadata '%s' references an invalid source '%s'.",
			canonicalMetadataPath.generic_string().c_str(),
			filename.c_str());
		return FileId::Invalid;
	}

	const FileId expectedFileId = ParseFileId(fileIdString);
	auto existingAssetInfo = m_loadedAssetInfo.Find(expectedFileId);
	if (existingAssetInfo != m_loadedAssetInfo.end())
	{
		if (existingAssetInfo.Value() != nullptr &&
			PathKey(existingAssetInfo.Value()->GetMetaFilepath()) ==
				PathKey(canonicalMetadataPath))
		{
			AssetInfoPtr existingInfo = existingAssetInfo.Value();
			if (!existingInfo->IsMetaExpired())
			{
				return expectedFileId;
			}

			IAssetInfoHandler* existingHandler = existingInfo->GetHandler();
			const bool bHadPendingUpdate =
				existingInfo->m_bPendingUpdateNotification;
			const bool bHadPendingWasExpired =
				existingInfo->m_bPendingWasExpired;
			const bool bHadPendingImport =
				existingInfo->m_bPendingImportNotification;
			if (existingHandler == nullptr ||
				!existingHandler->ReloadAssetInfo(
					existingInfo,
					false,
					false))
			{
				SAILOR_LOG_ERROR(
					"Cannot refresh generated secondary asset metadata: %s",
					canonicalMetadataPath.generic_string().c_str());
				return FileId::Invalid;
			}
			existingInfo->m_bPendingUpdateNotification =
				bHadPendingUpdate;
			existingInfo->m_bPendingWasExpired =
				bHadPendingWasExpired;
			existingInfo->m_bPendingImportNotification =
				bHadPendingImport;
			return expectedFileId;
		}

		SAILOR_LOG_ERROR(
			"Generated secondary asset metadata '%s' collides with an active FileId.",
			canonicalMetadataPath.generic_string().c_str());
		return FileId::Invalid;
	}
	for (const auto& loadedAsset : m_loadedAssetInfo)
	{
		if (loadedAsset.m_second != nullptr &&
			*loadedAsset.m_second != nullptr &&
			PathKey((*loadedAsset.m_second)->GetMetaFilepath()) ==
				PathKey(canonicalMetadataPath))
		{
			SAILOR_LOG_ERROR(
				"Generated secondary asset metadata path is already registered with another FileId: %s",
				canonicalMetadataPath.generic_string().c_str());
			return FileId::Invalid;
		}
	}

	const std::string virtualMetadataPath = canonicalMetadataPath
		.lexically_relative(metadataMount->m_root)
		.generic_string();
	if (!IsSafeVirtualPath(virtualMetadataPath))
	{
		SAILOR_LOG_ERROR(
			"Generated secondary asset metadata has an unsafe virtual path: %s",
			virtualMetadataPath.c_str());
		return FileId::Invalid;
	}

	const std::string handlerPath = std::filesystem::path(virtualMetadataPath)
		.replace_extension()
		.generic_string();
	IAssetInfoHandler* handler = GetAssetInfoHandler(
		Extension(handlerPath),
		assetInfoType,
		false);
	if (handler == nullptr)
	{
		SAILOR_LOG_ERROR(
			"Cannot find an asset info handler for generated metadata: %s",
			canonicalMetadataPath.generic_string().c_str());
		return FileId::Invalid;
	}

	AssetInfoPtr assetInfo = handler->LoadAssetInfo(
		canonicalMetadataPath.string(),
		virtualMetadataPath,
		metadataMount->m_kind,
		metadataMount->m_bWritable,
		false,
		false);
	if (assetInfo == nullptr ||
		assetInfo->GetFileId().ToString() != fileIdString ||
		PathKey(assetInfo->GetMetaFilepath()) != PathKey(canonicalMetadataPath) ||
		PathKey(assetInfo->GetAssetFilepath()) != PathKey(canonicalSourcePath))
	{
		delete assetInfo;
		SAILOR_LOG_ERROR(
			"Generated secondary asset metadata failed identity or path validation: %s",
			canonicalMetadataPath.generic_string().c_str());
		return FileId::Invalid;
	}

	assetInfo->m_bPendingUpdateNotification = false;
	assetInfo->m_bPendingWasExpired = false;
	assetInfo->m_bPendingImportNotification = false;
	m_loadedAssetInfo[expectedFileId] = assetInfo;
	return expectedFileId;
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
		AssetInfoPtr loadedInfo = GetAssetInfoPtr_Internal(physicalId.Value());
		if (loadedInfo != nullptr &&
			!loadedInfo->m_bPendingUpdateNotification &&
			!loadedInfo->m_bPendingImportNotification &&
			(loadedInfo->IsMetaExpired() || loadedInfo->IsAssetExpired() || IsAssetExpired(loadedInfo)))
		{
			SAILOR_LOG("Reload asset info: %s", loadedInfo->GetMetaFilepath().c_str());
			if (!loadedInfo->GetHandler()->ReloadAssetInfo(loadedInfo))
			{
				SAILOR_LOG_ERROR(
					"Asset reload failed; preserving the previous live asset: %s",
					loadedInfo->GetMetaFilepath().c_str());
			}
		}
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
		bImported = assetInfo != nullptr;
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
		if (bImported)
		{
			handler->DiscardImportedMetadataIfUnchanged(assetInfo);
		}
		delete assetInfo;
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
		Utils::TryGetFileRevision(
			location.m_physicalPath.generic_string(),
			location.m_revision);
		m_fileIds[virtualPathKey] = fileId;
		m_contentFileWinners[virtualPathKey] = location;
	}
	handler->NotifyUpdateAssetInfo(assetInfo);
	if (bImported)
	{
		handler->NotifyImportAsset(assetInfo);
	}
	CacheAsset(assetInfo);
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
