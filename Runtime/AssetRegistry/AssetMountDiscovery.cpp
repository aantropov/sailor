#include "AssetRegistry/AssetMountDiscovery.h"

#include <algorithm>
#include <cctype>
#include <functional>
#include <set>
#include <sstream>
#include <system_error>
#include <tuple>
#include <utility>

using namespace Sailor;

namespace
{
	std::string PathDisplay(const std::filesystem::path& path)
	{
		return path.generic_string();
	}

	std::string PathKey(const std::filesystem::path& path)
	{
		std::string key = PathDisplay(path.lexically_normal());
#if defined(_WIN32)
		std::transform(key.begin(), key.end(), key.begin(), [](unsigned char character)
			{
				return static_cast<char>(std::tolower(character));
			});
#endif
		while (key.size() > 1 && key.back() == '/')
		{
			key.pop_back();
		}
		return key;
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
		while (path.size() > 1 && path.back() == '/')
		{
			path.pop_back();
		}
#if defined(_WIN32)
		std::transform(path.begin(), path.end(), path.begin(), [](unsigned char character)
			{
				return static_cast<char>(std::tolower(character));
			});
#endif
		return path;
	}

	bool IsSafeVirtualPath(const std::string& path)
	{
		if (path.empty())
		{
			return false;
		}
		if (path.size() >= 2 && path[1] == ':' &&
			((path[0] >= 'A' && path[0] <= 'Z') || (path[0] >= 'a' && path[0] <= 'z')))
		{
			return false;
		}

		const std::filesystem::path virtualPath(path);
		if (virtualPath.is_absolute() || virtualPath.has_root_name() || virtualPath.has_root_directory())
		{
			return false;
		}

		return std::none_of(virtualPath.begin(), virtualPath.end(), [](const std::filesystem::path& component)
			{
				return component == "..";
			});
	}

	bool IsInside(const std::filesystem::path& root, const std::filesystem::path& candidate)
	{
		const std::string rootKey = PathKey(root);
		const std::string candidateKey = PathKey(candidate);
		if (candidateKey == rootKey)
		{
			return true;
		}
		const std::string prefix = !rootKey.empty() && rootKey.back() == '/'
			? rootKey
			: rootKey + '/';
		return candidateKey.rfind(prefix, 0) == 0;
	}

	int32_t KindRank(EAssetMountKind kind)
	{
		return kind == EAssetMountKind::Workspace ? 1 : 0;
	}

	const char* KindName(EAssetMountKind kind)
	{
		return kind == EAssetMountKind::Workspace ? "Workspace" : "Engine";
	}

	template<typename TCandidate>
	bool IsPreferred(const TCandidate& left, const TCandidate& right)
	{
		const int32_t leftKind = KindRank(left.m_mount.m_kind);
		const int32_t rightKind = KindRank(right.m_mount.m_kind);
		if (leftKind != rightKind)
		{
			return leftKind > rightKind;
		}
		if (left.m_mount.m_priority != right.m_mount.m_priority)
		{
			return left.m_mount.m_priority > right.m_mount.m_priority;
		}
		if (left.m_mount.m_bWritable != right.m_mount.m_bWritable)
		{
			return left.m_mount.m_bWritable;
		}
		if (PathKey(left.m_physicalPath) != PathKey(right.m_physicalPath))
		{
			return PathKey(left.m_physicalPath) < PathKey(right.m_physicalPath);
		}
		if (left.m_virtualPath != right.m_virtualPath)
		{
			return left.m_virtualPath < right.m_virtualPath;
		}
		return left.m_fileId < right.m_fileId;
	}

	bool IsPreferredMount(const AssetMountDescriptor& left, const AssetMountDescriptor& right)
	{
		AssetMountCandidate leftCandidate{ left, left.m_root, {}, {} };
		AssetMountCandidate rightCandidate{ right, right.m_root, {}, {} };
		return IsPreferred(leftCandidate, rightCandidate);
	}

	void SortDiagnostics(std::vector<AssetMountDiagnostic>& diagnostics)
	{
		std::sort(diagnostics.begin(), diagnostics.end(), [](const AssetMountDiagnostic& left, const AssetMountDiagnostic& right)
			{
				return std::tie(
					left.m_code,
					left.m_key,
					left.m_message) < std::tie(
						right.m_code,
						right.m_key,
						right.m_message);
			});
	}

	bool IsFatalDiagnostic(EAssetMountDiagnosticCode code)
	{
		return code == EAssetMountDiagnosticCode::InvalidMountRoot ||
			code == EAssetMountDiagnosticCode::OverlappingMountRoot;
	}

	AssetMountDiagnostic MakeInspectionDiagnostic(
		EAssetMountDiagnosticCode code,
		const AssetMountDescriptor& mount,
		const std::filesystem::path& path,
		const std::string& reason)
	{
		AssetMountDiagnostic diagnostic;
		diagnostic.m_code = code;
		diagnostic.m_key = PathDisplay(path);
		diagnostic.m_winnerPath = mount.m_root;
		diagnostic.m_conflictingPath = path;
		diagnostic.m_winnerKind = mount.m_kind;
		diagnostic.m_conflictingKind = mount.m_kind;
		diagnostic.m_message = reason;
		return diagnostic;
	}

	std::vector<AssetMountDescriptor> NormalizeMounts(
		const std::vector<AssetMountDescriptor>& mounts,
		std::vector<AssetMountDiagnostic>& diagnostics)
	{
		std::vector<AssetMountDescriptor> normalized;
		for (const AssetMountDescriptor& input : mounts)
		{
			std::error_code error;
			const std::filesystem::path root = std::filesystem::canonical(input.m_root, error);
			if (error || !std::filesystem::is_directory(root, error) || error)
			{
				AssetMountDiagnostic diagnostic;
				diagnostic.m_code = EAssetMountDiagnosticCode::InvalidMountRoot;
				diagnostic.m_key = PathDisplay(input.m_root);
				diagnostic.m_conflictingPath = input.m_root;
				diagnostic.m_conflictingKind = input.m_kind;
				diagnostic.m_message = "Asset mount root is not a readable directory: '" +
					PathDisplay(input.m_root) + "'.";
				diagnostics.emplace_back(std::move(diagnostic));
				continue;
			}

			AssetMountDescriptor descriptor = input;
			descriptor.m_root = root;
			normalized.emplace_back(std::move(descriptor));
		}

		std::sort(normalized.begin(), normalized.end(), [](const AssetMountDescriptor& left, const AssetMountDescriptor& right)
			{
				const std::string leftRoot = PathKey(left.m_root);
				const std::string rightRoot = PathKey(right.m_root);
				if (leftRoot != rightRoot)
				{
					return leftRoot < rightRoot;
				}
				return IsPreferredMount(left, right);
			});

		std::vector<AssetMountDescriptor> deduplicated;
		for (size_t begin = 0; begin < normalized.size();)
		{
			size_t end = begin + 1;
			while (end < normalized.size() && PathKey(normalized[end].m_root) == PathKey(normalized[begin].m_root))
			{
				++end;
			}

			const AssetMountDescriptor& winner = normalized[begin];
			deduplicated.emplace_back(winner);
			for (size_t duplicate = begin + 1; duplicate < end; ++duplicate)
			{
				const AssetMountDescriptor& conflicting = normalized[duplicate];
				AssetMountDiagnostic diagnostic;
				diagnostic.m_code = EAssetMountDiagnosticCode::DuplicateMountRoot;
				diagnostic.m_key = PathKey(winner.m_root);
				diagnostic.m_winnerPath = winner.m_root;
				diagnostic.m_conflictingPath = conflicting.m_root;
				diagnostic.m_winnerKind = winner.m_kind;
				diagnostic.m_conflictingKind = conflicting.m_kind;
				diagnostic.m_message = "Duplicate asset mount root '" + PathDisplay(winner.m_root) +
					"' keeps " + KindName(winner.m_kind) + " and discards " +
					KindName(conflicting.m_kind) + ".";
				diagnostics.emplace_back(std::move(diagnostic));
			}
			begin = end;
		}

		for (size_t leftIndex = 0; leftIndex < deduplicated.size(); ++leftIndex)
		{
			for (size_t rightIndex = leftIndex + 1; rightIndex < deduplicated.size(); ++rightIndex)
			{
				const AssetMountDescriptor& left = deduplicated[leftIndex];
				const AssetMountDescriptor& right = deduplicated[rightIndex];
				if (!IsInside(left.m_root, right.m_root) && !IsInside(right.m_root, left.m_root))
				{
					continue;
				}

				AssetMountDiagnostic diagnostic;
				diagnostic.m_code = EAssetMountDiagnosticCode::OverlappingMountRoot;
				diagnostic.m_key = PathKey(left.m_root) + "|" + PathKey(right.m_root);
				diagnostic.m_winnerPath = left.m_root;
				diagnostic.m_conflictingPath = right.m_root;
				diagnostic.m_winnerKind = left.m_kind;
				diagnostic.m_conflictingKind = right.m_kind;
				diagnostic.m_message = "Asset mount roots overlap and cannot be activated: " +
					std::string(KindName(left.m_kind)) + " '" + PathDisplay(left.m_root) +
					"' and " + KindName(right.m_kind) + " '" + PathDisplay(right.m_root) + "'.";
				diagnostics.emplace_back(std::move(diagnostic));
			}
		}

		return deduplicated;
	}

	void DiscoverDirectory(
		const AssetMountDescriptor& mount,
		const std::filesystem::path& logicalDirectory,
		std::set<std::string>& visitedDirectories,
		std::vector<AssetMountDiscoveredFile>& files,
		std::vector<AssetMountDiagnostic>& diagnostics)
	{
		std::error_code error;
		const std::filesystem::path physicalDirectory = std::filesystem::canonical(logicalDirectory, error);
		if (error)
		{
			diagnostics.emplace_back(MakeInspectionDiagnostic(
				EAssetMountDiagnosticCode::PathInspectionFailed,
				mount,
				logicalDirectory,
				"Asset mount directory could not be resolved: '" + PathDisplay(logicalDirectory) + "'."));
			return;
		}
		if (!IsInside(mount.m_root, physicalDirectory))
		{
			diagnostics.emplace_back(MakeInspectionDiagnostic(
				EAssetMountDiagnosticCode::PhysicalEscapeSkipped,
				mount,
				logicalDirectory,
				"Asset mount path escapes its root and was skipped: '" + PathDisplay(logicalDirectory) + "'."));
			return;
		}

		const std::string directoryKey = PathKey(physicalDirectory);
		if (!visitedDirectories.emplace(directoryKey).second)
		{
			diagnostics.emplace_back(MakeInspectionDiagnostic(
				EAssetMountDiagnosticCode::DirectoryCycleSkipped,
				mount,
				logicalDirectory,
				"Asset mount directory resolves to an already visited location and was skipped: '" +
					PathDisplay(logicalDirectory) + "'."));
			return;
		}

		std::vector<std::filesystem::directory_entry> entries;
		for (std::filesystem::directory_iterator it(
				logicalDirectory,
				std::filesystem::directory_options::skip_permission_denied,
				error), end;
			!error && it != end;
			it.increment(error))
		{
			entries.emplace_back(*it);
		}
		if (error)
		{
			diagnostics.emplace_back(MakeInspectionDiagnostic(
				EAssetMountDiagnosticCode::PathInspectionFailed,
				mount,
				logicalDirectory,
				"Asset mount directory could not be enumerated: '" + PathDisplay(logicalDirectory) + "'."));
			return;
		}

		std::sort(entries.begin(), entries.end(), [](const std::filesystem::directory_entry& left, const std::filesystem::directory_entry& right)
			{
				const std::string leftKey = PathKey(left.path());
				const std::string rightKey = PathKey(right.path());
				return leftKey != rightKey
					? leftKey < rightKey
					: PathDisplay(left.path()) < PathDisplay(right.path());
			});

		for (const std::filesystem::directory_entry& entry : entries)
		{
			error.clear();
			const bool bIsDirectory = entry.is_directory(error);
			if (error)
			{
				diagnostics.emplace_back(MakeInspectionDiagnostic(
					EAssetMountDiagnosticCode::PathInspectionFailed,
					mount,
					entry.path(),
					"Asset mount entry could not be inspected: '" + PathDisplay(entry.path()) + "'."));
				continue;
			}
			if (bIsDirectory)
			{
				DiscoverDirectory(mount, entry.path(), visitedDirectories, files, diagnostics);
				continue;
			}

			error.clear();
			if (!entry.is_regular_file(error) || error)
			{
				continue;
			}

			const std::filesystem::path physicalPath = std::filesystem::canonical(entry.path(), error);
			if (error)
			{
				diagnostics.emplace_back(MakeInspectionDiagnostic(
					EAssetMountDiagnosticCode::PathInspectionFailed,
					mount,
					entry.path(),
					"Asset mount file could not be resolved: '" + PathDisplay(entry.path()) + "'."));
				continue;
			}
			if (!IsInside(mount.m_root, physicalPath))
			{
				diagnostics.emplace_back(MakeInspectionDiagnostic(
					EAssetMountDiagnosticCode::PhysicalEscapeSkipped,
					mount,
					entry.path(),
					"Asset mount file escapes its root and was skipped: '" + PathDisplay(entry.path()) + "'."));
				continue;
			}

			const std::filesystem::path relativePath = entry.path().lexically_relative(mount.m_root);
			const std::string virtualPath = relativePath.generic_string();
			if (!IsSafeVirtualPath(virtualPath))
			{
				diagnostics.emplace_back(MakeInspectionDiagnostic(
					EAssetMountDiagnosticCode::PathInspectionFailed,
					mount,
					entry.path(),
					"Asset mount file has an invalid virtual path and was skipped: '" +
						PathDisplay(entry.path()) + "'."));
				continue;
			}

			files.emplace_back(AssetMountDiscoveredFile{ mount, physicalPath, virtualPath });
		}
	}

	AssetMountDiagnostic MakeCollisionDiagnostic(
		EAssetMountDiagnosticCode code,
		const std::string& key,
		const AssetMountCandidate& winner,
		const AssetMountCandidate& conflicting)
	{
		AssetMountDiagnostic diagnostic;
		diagnostic.m_code = code;
		diagnostic.m_key = key;
		diagnostic.m_winnerPath = winner.m_physicalPath;
		diagnostic.m_conflictingPath = conflicting.m_physicalPath;
		diagnostic.m_winnerKind = winner.m_mount.m_kind;
		diagnostic.m_conflictingKind = conflicting.m_mount.m_kind;
		diagnostic.m_winnerFileId = winner.m_fileId;
		diagnostic.m_conflictingFileId = conflicting.m_fileId;

		const char* collisionName = code == EAssetMountDiagnosticCode::VirtualPathCollision
			? "virtual path"
			: "FileId";
		diagnostic.m_message = "Asset " + std::string(collisionName) + " collision '" + key +
			"' keeps " + KindName(winner.m_mount.m_kind) + " '" +
			PathDisplay(winner.m_physicalPath) + "' (FileId '" + winner.m_fileId +
			"') over " + KindName(conflicting.m_mount.m_kind) + " '" +
			PathDisplay(conflicting.m_physicalPath) + "' (FileId '" + conflicting.m_fileId + "').";
		return diagnostic;
	}

	template<typename TKeySelector>
	void BuildWinnerIndex(
		const std::vector<AssetMountCandidate>& candidates,
		std::map<std::string, size_t>& winners,
		std::vector<AssetMountDiagnostic>& diagnostics,
		EAssetMountDiagnosticCode collisionCode,
		TKeySelector selectKey)
	{
		std::map<std::string, std::vector<size_t>> groups;
		for (size_t index = 0; index < candidates.size(); ++index)
		{
			const std::string key = selectKey(candidates[index]);
			if (!key.empty())
			{
				groups[key].emplace_back(index);
			}
		}

		for (const auto& [key, indexes] : groups)
		{
			size_t winnerIndex = indexes.front();
			for (size_t index : indexes)
			{
				if (IsPreferred(candidates[index], candidates[winnerIndex]))
				{
					winnerIndex = index;
				}
			}
			winners.emplace(key, winnerIndex);

			for (size_t index : indexes)
			{
				if (index == winnerIndex ||
					PathKey(candidates[index].m_physicalPath) == PathKey(candidates[winnerIndex].m_physicalPath))
				{
					continue;
				}
				diagnostics.emplace_back(MakeCollisionDiagnostic(
					collisionCode,
					key,
					candidates[winnerIndex],
					candidates[index]));
			}
		}
	}
}

AssetMountDiscoveryResult Sailor::DiscoverAssetMountFiles(
	const std::vector<AssetMountDescriptor>& mounts)
{
	AssetMountDiscoveryResult result;
	result.m_mounts = NormalizeMounts(mounts, result.m_diagnostics);
	if (result.HasFatalErrors())
	{
		SortDiagnostics(result.m_diagnostics);
		return result;
	}
	for (const AssetMountDescriptor& mount : result.m_mounts)
	{
		std::set<std::string> visitedDirectories;
		DiscoverDirectory(
			mount,
			mount.m_root,
			visitedDirectories,
			result.m_files,
			result.m_diagnostics);
	}

	std::sort(result.m_files.begin(), result.m_files.end(), [](const AssetMountDiscoveredFile& left, const AssetMountDiscoveredFile& right)
		{
			return std::tie(left.m_virtualPath, left.m_mount.m_kind, left.m_mount.m_priority, left.m_physicalPath) <
				std::tie(right.m_virtualPath, right.m_mount.m_kind, right.m_mount.m_priority, right.m_physicalPath);
		});
	SortDiagnostics(result.m_diagnostics);
	return result;
}

bool AssetMountDiscoveryResult::HasFatalErrors() const noexcept
{
	return std::any_of(m_diagnostics.begin(), m_diagnostics.end(), [](const AssetMountDiagnostic& diagnostic)
		{
			return IsFatalDiagnostic(diagnostic.m_code);
		});
}

AssetMountResolutionResult Sailor::ResolveAssetMountCandidates(
	std::vector<AssetMountCandidate> candidates)
{
	AssetMountResolutionResult result;
	for (AssetMountCandidate& candidate : candidates)
	{
		std::replace(candidate.m_virtualPath.begin(), candidate.m_virtualPath.end(), '\\', '/');
		while (candidate.m_virtualPath.rfind("./", 0) == 0)
		{
			candidate.m_virtualPath.erase(0, 2);
		}
		if (!IsSafeVirtualPath(candidate.m_virtualPath))
		{
			AssetMountDiagnostic diagnostic;
			diagnostic.m_code = EAssetMountDiagnosticCode::InvalidCandidate;
			diagnostic.m_key = candidate.m_virtualPath;
			diagnostic.m_conflictingPath = candidate.m_physicalPath;
			diagnostic.m_conflictingKind = candidate.m_mount.m_kind;
			diagnostic.m_conflictingFileId = candidate.m_fileId;
			diagnostic.m_message = "Asset candidate has an invalid virtual path and was skipped: '" +
				candidate.m_virtualPath + "'.";
			result.m_diagnostics.emplace_back(std::move(diagnostic));
			continue;
		}
		result.m_candidates.emplace_back(std::move(candidate));
	}

	std::sort(result.m_candidates.begin(), result.m_candidates.end(), [](const AssetMountCandidate& left, const AssetMountCandidate& right)
		{
			return std::tie(left.m_virtualPath, left.m_fileId, left.m_mount.m_kind, left.m_mount.m_priority, left.m_physicalPath) <
				std::tie(right.m_virtualPath, right.m_fileId, right.m_mount.m_kind, right.m_mount.m_priority, right.m_physicalPath);
		});

	BuildWinnerIndex(
		result.m_candidates,
		result.m_virtualPathWinners,
		result.m_diagnostics,
		EAssetMountDiagnosticCode::VirtualPathCollision,
		[](const AssetMountCandidate& candidate)
		{
			return VirtualPathKey(candidate.m_virtualPath);
		});
	BuildWinnerIndex(
		result.m_candidates,
		result.m_fileIdWinners,
		result.m_diagnostics,
		EAssetMountDiagnosticCode::FileIdCollision,
		[](const AssetMountCandidate& candidate)
		{
			return candidate.m_fileId;
		});
	SortDiagnostics(result.m_diagnostics);
	return result;
}

const AssetMountCandidate* AssetMountResolutionResult::FindByVirtualPath(
	const std::string& virtualPath) const noexcept
{
	const auto it = m_virtualPathWinners.find(VirtualPathKey(virtualPath));
	return it == m_virtualPathWinners.end() ? nullptr : &m_candidates[it->second];
}

const AssetMountCandidate* AssetMountResolutionResult::FindByFileId(
	const std::string& fileId) const noexcept
{
	const auto it = m_fileIdWinners.find(fileId);
	return it == m_fileIdWinners.end() ? nullptr : &m_candidates[it->second];
}
