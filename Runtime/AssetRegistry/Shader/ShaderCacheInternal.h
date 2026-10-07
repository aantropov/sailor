#pragma once

#include "AssetRegistry/FileId.h"
#include "Workspace/WorkspaceCacheContract.h"

#include <filesystem>
#include <string>
#include <string_view>

namespace Sailor::ShaderCacheInternal
{
	std::string NormalizeDependencyPath(const std::filesystem::path& path);
	std::string NormalizeDependencyVirtualPath(std::string_view path);
	std::filesystem::path GetCacheChildPath(const char* child);
	std::filesystem::path GetShaderFilepath(const std::filesystem::path& folder,
		const FileId& uid,
		int32_t permutation,
		std::string_view shaderKind,
		std::string_view extension,
		std::string_view generation = {});
	void AppendDiagnostic(std::string& diagnostic, std::string_view suffix);
	bool ResolveDirectCacheChild(const std::filesystem::path& cacheRoot,
		const std::filesystem::path& candidate,
		std::filesystem::path& outCanonical,
		std::string& outDiagnostic);
	bool RemovePath(const std::filesystem::path& cacheRoot,
		const std::filesystem::path& path,
		bool bRecursive,
		std::string& outDiagnostic);
	bool ResolveOwnedArtifactPath(const std::filesystem::path& cacheRoot,
		const std::filesystem::path& ownedDirectory,
		const std::filesystem::path& artifact,
		std::filesystem::path& outCanonicalArtifact,
		std::string& outDiagnostic,
		bool& outIoFailure);
	bool RemoveOwnedArtifact(const std::filesystem::path& cacheRoot,
		const std::filesystem::path& ownedDirectory,
		const std::filesystem::path& artifact,
		std::string& outDiagnostic);
	bool ShouldResetCache(Workspace::EWorkspaceCacheLoadStatus status) noexcept;
}
