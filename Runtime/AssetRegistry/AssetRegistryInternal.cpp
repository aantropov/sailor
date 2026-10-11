#include "AssetRegistry/AssetRegistryInternal.h"

#include "AssetRegistry/AssetScanSourceRevisionCache.h"
#include "Core/Utils.h"
#include "YamlExceptionBoundary.h"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <string>
#include <utility>

#include <yaml-cpp/yaml.h>

namespace Sailor::AssetRegistryInternal
{
	using namespace Workspace;
	std::atomic<uint64_t> g_nextAssetProcessingGeneration = 0;
	thread_local AssetScanSourceRevisionCache* g_activeSourceRevisionCache = nullptr;

	std::string AsFolderPath(const std::filesystem::path& path)
	{
		std::string result = PathToUtf8(path);
		if (!result.ends_with('/'))
		{
			result += '/';
		}
		return result;
	}

	std::string Lowercase(std::string value)
	{
		std::transform(value.begin(),
			value.end(),
			value.begin(),
			[](unsigned char character) { return static_cast<char>(std::tolower(character)); });
		return value;
	}

	std::string PathKey(const std::filesystem::path& path)
	{
		std::string value = PathToUtf8(path.lexically_normal());
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

	bool IsSafeVirtualPath(std::string_view path)
	{
		if (path.empty() || path.find('\0') != std::string::npos)
		{
			return false;
		}

		const auto value = PathFromUtf8(path);
		if (value.is_absolute() || value.has_root_name() || value.has_root_directory())
		{
			return false;
		}

		return std::none_of(
			value.begin(), value.end(), [](const std::filesystem::path& component) { return component == ".."; });
	}

	bool IsInside(const std::filesystem::path& root, const std::filesystem::path& candidate)
	{
		const std::string rootKey = PathKey(root);
		const std::string candidateKey = PathKey(candidate);
		return candidateKey == rootKey ||
			   (candidateKey.size() > rootKey.size() && candidateKey.compare(0, rootKey.size(), rootKey) == 0 &&
				   candidateKey[rootKey.size()] == '/');
	}

	std::string MountFileKey(const AssetMountDescriptor& mount, const std::string& virtualPath)
	{
		return PathKey(mount.m_root) + "\n" + VirtualPathKey(virtualPath);
	}

	std::string Extension(std::string_view path)
	{
		return Lowercase(Utils::GetFileExtension(path));
	}

	std::string HandlerExtension(const StagedAssetRecord& record)
	{
		if (record.m_bPrimary)
		{
			return Extension(record.m_assetVirtualPath);
		}

		return Extension(PathToUtf8(PathFromUtf8(record.m_metaVirtualPath).replace_extension()));
	}

	bool CandidateMatches(const AssetMountCandidate* winner, const AssetMountCandidate& candidate)
	{
		return winner != nullptr && PathKey(winner->m_physicalPath) == PathKey(candidate.m_physicalPath) &&
			   VirtualPathKey(winner->m_virtualPath) == VirtualPathKey(candidate.m_virtualPath) &&
			   winner->m_fileId == candidate.m_fileId;
	}

	bool SameEffectiveContent(const AssetRegistry::AssetReadLocation& left,
		const AssetRegistry::AssetReadLocation& right)
	{
		return PathKey(left.m_physicalPath) == PathKey(right.m_physicalPath) &&
			   VirtualPathKey(left.m_virtualPath) == VirtualPathKey(right.m_virtualPath) &&
			   left.m_mountKind == right.m_mountKind && left.m_revision == right.m_revision;
	}

	bool ReadMetadataIdentity(const std::filesystem::path& metaPath,
		std::string& outFileId,
		std::string& outFilename,
		std::string& outAssetInfoType,
		std::string& outError)
	{
		outFileId.clear();
		outFilename.clear();
		outAssetInfoType.clear();
		outError.clear();

		std::ifstream input(metaPath);
		if (!input.is_open())
		{
			outError = "Cannot open asset metadata: " + PathToUtf8(metaPath);
			return false;
		}

		bool bSuccess = false;
		std::string yamlDiagnostic;
		if (!External::GuardYamlExceptions(
				[&]()
				{
					const YAML::Node metadata = YAML::Load(input);
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
					FileId canonicalFileId;
					canonicalFileId.Deserialize(YAML::Node(outFileId));
					outFileId = canonicalFileId.ToString();
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
						!IsSafeVirtualPath(outFilename))
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

	FileId ParseFileId(std::string_view value)
	{
		FileId fileId;
		fileId.Deserialize(YAML::Node(value));
		return fileId;
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
