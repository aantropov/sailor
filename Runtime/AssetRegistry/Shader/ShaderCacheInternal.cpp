#include "AssetRegistry/Shader/ShaderCacheInternal.h"

#include "AssetRegistry/AssetRegistry.h"

#include "Workspace/WorkspacePathEncoding.h"

#include <algorithm>
#include <cctype>

using namespace Sailor::Workspace;

namespace
{
	bool PathsEqual(const std::filesystem::path& lhs, const std::filesystem::path& rhs)
	{
		std::string left = PathToUtf8(lhs);
		std::string right = PathToUtf8(rhs);
#if defined(_WIN32)
		std::transform(left.begin(),
			left.end(),
			left.begin(),
			[](unsigned char character) { return static_cast<char>(std::tolower(character)); });
		std::transform(right.begin(),
			right.end(),
			right.begin(),
			[](unsigned char character) { return static_cast<char>(std::tolower(character)); });
#endif
		return left == right;
	}

}

bool Sailor::ShaderCacheInternal::ResolveDirectCacheChild(const std::filesystem::path& cacheRoot,
	const std::filesystem::path& candidate,
	std::filesystem::path& outCanonical,
	std::string& outDiagnostic)
{
	std::error_code error;
	const std::filesystem::path canonicalRoot = std::filesystem::weakly_canonical(cacheRoot, error);
	if (error)
	{
		outDiagnostic =
			"Cannot canonicalize shader cache root '" + PathToUtf8(cacheRoot) + "': " + error.message();
		return false;
	}

	error.clear();
	outCanonical = std::filesystem::weakly_canonical(candidate, error);
	if (error)
	{
		outDiagnostic =
			"Cannot canonicalize shader cache path '" + PathToUtf8(candidate) + "': " + error.message();
		return false;
	}

	if (!PathsEqual(outCanonical.parent_path(), canonicalRoot))
	{
		outDiagnostic =
			"Refusing shader cache access outside canonical cache root: '" + PathToUtf8(candidate) + "'.";
		return false;
	}
	return true;
}

std::string Sailor::ShaderCacheInternal::NormalizeDependencyPath(const std::filesystem::path& path)
{
	std::error_code error;
	std::string result = PathToUtf8(std::filesystem::weakly_canonical(path, error));
	if (error)
	{
		result = PathToUtf8(path.lexically_normal());
	}
#if defined(_WIN32)
	std::transform(result.begin(),
		result.end(),
		result.begin(),
		[](unsigned char character) { return static_cast<char>(std::tolower(character)); });
#endif
	return result;
}

std::string Sailor::ShaderCacheInternal::NormalizeDependencyVirtualPath(std::string_view path)
{
	std::string result = PathToUtf8(PathFromUtf8(path).lexically_normal());
#if defined(_WIN32)
	std::transform(result.begin(),
		result.end(),
		result.begin(),
		[](unsigned char character) { return static_cast<char>(std::tolower(character)); });
#endif
	return result;
}

std::filesystem::path Sailor::ShaderCacheInternal::GetCacheChildPath(const char* child)
{
	return PathFromUtf8(AssetRegistry::GetCacheFolder()) / child;
}

std::filesystem::path Sailor::ShaderCacheInternal::GetShaderFilepath(const std::filesystem::path& folder,
	const FileId& uid,
	int32_t permutation,
	std::string_view shaderKind,
	std::string_view extension,
	std::string_view generation)
{
	std::string filename = uid.ToString();
	filename += shaderKind;
	filename += std::to_string(permutation);
	if (!generation.empty())
	{
		filename += '.';
		filename += generation;
	}
	filename += '.';
	filename += extension;
	return folder / PathFromUtf8(filename);
}

void Sailor::ShaderCacheInternal::AppendDiagnostic(std::string& diagnostic, std::string_view suffix)
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

bool Sailor::ShaderCacheInternal::RemovePath(const std::filesystem::path& cacheRoot,
	const std::filesystem::path& path,
	bool bRecursive,
	std::string& outDiagnostic)
{
	std::filesystem::path canonical;
	std::string diagnostic;
	if (!ResolveDirectCacheChild(cacheRoot, path, canonical, diagnostic))
	{
		AppendDiagnostic(outDiagnostic, diagnostic);
		return false;
	}

	std::error_code error;
	if (bRecursive)
	{
		std::filesystem::remove_all(path, error);
	}
	else
	{
		std::filesystem::remove(path, error);
	}
	if (error)
	{
		AppendDiagnostic(
			outDiagnostic, "Cannot remove shader cache path '" + PathToUtf8(path) + "': " + error.message());
		return false;
	}
	return true;
}

bool Sailor::ShaderCacheInternal::ResolveOwnedArtifactPath(const std::filesystem::path& cacheRoot,
	const std::filesystem::path& ownedDirectory,
	const std::filesystem::path& artifact,
	std::filesystem::path& outCanonicalArtifact,
	std::string& outDiagnostic,
	bool& outIoFailure)
{
	outIoFailure = false;
	std::error_code error;
	const auto status = std::filesystem::symlink_status(ownedDirectory, error);
	if (error)
	{
		outIoFailure = error != std::errc::no_such_file_or_directory;
		outDiagnostic =
			"Cannot inspect owned shader cache directory '" + PathToUtf8(ownedDirectory) + "': " + error.message();
		return false;
	}
	if (std::filesystem::is_symlink(status))
	{
		outDiagnostic = "Refusing shader artifact access through symlinked cache directory '" +
						PathToUtf8(ownedDirectory) + "'.";
		return false;
	}

	const std::filesystem::path canonicalRoot = std::filesystem::weakly_canonical(cacheRoot, error);
	if (error)
	{
		outIoFailure = true;
		outDiagnostic =
			"Cannot canonicalize shader cache root '" + PathToUtf8(cacheRoot) + "': " + error.message();
		return false;
	}

	error.clear();
	const std::filesystem::path canonicalDirectory = std::filesystem::weakly_canonical(ownedDirectory, error);
	if (error)
	{
		outIoFailure = true;
		outDiagnostic = "Cannot canonicalize owned shader cache directory '" + PathToUtf8(ownedDirectory) +
						"': " + error.message();
		return false;
	}
	if (!PathsEqual(canonicalDirectory.parent_path(), canonicalRoot))
	{
		outDiagnostic =
			"Refusing shader artifact access outside the canonical cache root: '" + PathToUtf8(artifact) + "'.";
		return false;
	}

	error.clear();
	outCanonicalArtifact = std::filesystem::weakly_canonical(artifact, error);
	if (error)
	{
		outIoFailure = error != std::errc::no_such_file_or_directory;
		outDiagnostic = "Cannot canonicalize shader artifact '" + PathToUtf8(artifact) + "': " + error.message();
		return false;
	}
	if (!PathsEqual(outCanonicalArtifact.parent_path(), canonicalDirectory))
	{
		outDiagnostic = "Refusing shader artifact access outside owned directory '" +
						PathToUtf8(canonicalDirectory) + "': '" + PathToUtf8(artifact) + "'.";
		return false;
	}
	return true;
}

bool Sailor::ShaderCacheInternal::RemoveOwnedArtifact(const std::filesystem::path& cacheRoot,
	const std::filesystem::path& ownedDirectory,
	const std::filesystem::path& artifact,
	std::string& outDiagnostic)
{
	std::filesystem::path canonicalArtifact;
	bool ignoredIoFailure = false;
	if (!ResolveOwnedArtifactPath(
			cacheRoot, ownedDirectory, artifact, canonicalArtifact, outDiagnostic, ignoredIoFailure))
	{
		return false;
	}

	std::error_code error;
	std::filesystem::remove(artifact, error);
	if (error)
	{
		AppendDiagnostic(outDiagnostic,
			"Cannot remove shader cache artifact '" + PathToUtf8(artifact) + "': " + error.message());
		return false;
	}
	return true;
}

bool Sailor::ShaderCacheInternal::ShouldResetCache(Workspace::EWorkspaceCacheLoadStatus status) noexcept
{
	return status == Workspace::EWorkspaceCacheLoadStatus::Missing ||
		   status == Workspace::EWorkspaceCacheLoadStatus::StaleIdentity ||
		   status == Workspace::EWorkspaceCacheLoadStatus::Corrupt ||
		   status == Workspace::EWorkspaceCacheLoadStatus::UnsupportedVersion;
}
