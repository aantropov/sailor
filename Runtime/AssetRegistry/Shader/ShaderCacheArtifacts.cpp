#include "AssetRegistry/Shader/ShaderCache.h"

#include "AssetRegistry/Shader/ShaderCacheInternal.h"
#include "Containers/Hash.h"

#include <algorithm>
#include <cstring>
#include <fstream>
#include <limits>

using namespace Sailor;
using namespace Sailor::ShaderCacheInternal;

std::string ShaderCache::GetShaderCacheFilepath()
{
	return GetCacheChildPath("ShaderCache.yaml").string();
}

std::string ShaderCache::GetPrecompiledShadersFolder()
{
	return GetCacheChildPath("PrecompiledShaders").string();
}

std::string ShaderCache::GetCompiledShadersFolder()
{
	return GetCacheChildPath("CompiledShaders").string();
}

std::string ShaderCache::GetCompiledShadersWithDebugFolder()
{
	return GetCacheChildPath("CompiledShadersWithDebug").string();
}

std::filesystem::path ShaderCache::GetPrecompiledShaderFilepath(const FileId& uid,
	int32_t permutation,
	const std::string& shaderKind)
{
	return GetShaderFilepath(
		GetCacheChildPath("PrecompiledShaders"), uid, permutation, shaderKind, PrecompiledShaderFileExtension);
}

std::filesystem::path ShaderCache::GetCachedShaderFilepath(const FileId& uid,
	int32_t permutation,
	const std::string& shaderKind)
{
	return GetShaderFilepath(
		GetCacheChildPath("CompiledShaders"), uid, permutation, shaderKind, CompiledShaderFileExtension);
}

std::filesystem::path ShaderCache::GetCachedShaderWithDebugFilepath(const FileId& uid,
	int32_t permutation,
	const std::string& shaderKind)
{
	return GetShaderFilepath(
		GetCacheChildPath("CompiledShadersWithDebug"), uid, permutation, shaderKind, CompiledShaderFileExtension);
}

uint64_t ShaderCache::CalculateArtifactChecksum(const void* data, uint64_t size) noexcept
{
	if (size != 0 && data == nullptr)
	{
		return 0;
	}

	return HashBytes(data, static_cast<size_t>(size));
}

ShaderCache::ArtifactMetadata ShaderCache::DescribeArtifact(const void* data, uint64_t size) noexcept
{
	ArtifactMetadata result;
	if (size == 0 || data == nullptr)
	{
		return result;
	}

	result.m_byteLength = size;
	result.m_checksum = CalculateArtifactChecksum(data, size);
	return result;
}

bool ShaderCache::ReadArtifactBytes(const std::filesystem::path& path,
	const ArtifactMetadata& metadata,
	TVector<uint8_t>& outBytes,
	std::string& outDiagnostic,
	bool& outIoFailure) noexcept
{
	outIoFailure = false;
	if (!metadata.Validate("Artifact metadata", outDiagnostic))
	{
		outDiagnostic += " Path: '" + path.generic_string() + "'.";
		return false;
	}
	if (!metadata.IsPresent())
	{
		outBytes.Clear();
		outDiagnostic.clear();
		return true;
	}
	if (metadata.m_byteLength > std::numeric_limits<size_t>::max() ||
		metadata.m_byteLength > static_cast<uint64_t>(std::numeric_limits<std::streamsize>::max()))
	{
		outDiagnostic = "Artifact is too large to read safely: '" + path.generic_string() + "'.";
		return false;
	}

	std::error_code sizeError;
	const uint64_t actualSize = std::filesystem::file_size(path, sizeError);
	if (sizeError)
	{
		outIoFailure = sizeError != std::errc::no_such_file_or_directory && sizeError != std::errc::not_a_directory;
		outDiagnostic = "Cannot inspect shader artifact '" + path.generic_string() + "': " + sizeError.message();
		return false;
	}
	if (actualSize != metadata.m_byteLength)
	{
		outDiagnostic = "Shader artifact '" + path.generic_string() + "' has byte length " +
						std::to_string(actualSize) + ", expected " + std::to_string(metadata.m_byteLength) + ".";
		return false;
	}

	std::ifstream stream(path, std::ios::binary);
	if (!stream.is_open())
	{
		std::error_code existsError;
		const bool bExists = std::filesystem::exists(path, existsError);
		outIoFailure = bExists || static_cast<bool>(existsError);
		outDiagnostic = "Cannot open shader artifact '" + path.generic_string() + "'.";
		return false;
	}

	TVector<uint8_t> candidate(static_cast<size_t>(metadata.m_byteLength));
	stream.read(reinterpret_cast<char*>(candidate.GetData()), static_cast<std::streamsize>(candidate.Num()));
	if (stream.gcount() != static_cast<std::streamsize>(candidate.Num()) || stream.bad())
	{
		outIoFailure = stream.bad();
		outDiagnostic = "Shader artifact read was incomplete for '" + path.generic_string() + "'.";
		return false;
	}
	if (CalculateArtifactChecksum(candidate.GetData(), candidate.Num()) != metadata.m_checksum)
	{
		outDiagnostic = "Shader artifact checksum mismatch for '" + path.generic_string() + "'.";
		return false;
	}

	outBytes = std::move(candidate);
	outDiagnostic.clear();
	return true;
}

bool ShaderCache::ReadSpirvArtifact(const std::filesystem::path& path,
	const ArtifactMetadata& metadata,
	TVector<uint32_t>& outSpirv,
	std::string& outDiagnostic) noexcept
{
	bool ignoredIoFailure = false;
	return ReadSpirvArtifactInternal(path, metadata, outSpirv, outDiagnostic, ignoredIoFailure);
}

bool ShaderCache::ValidateOwnedArtifactPath(const std::filesystem::path& cacheRoot,
	const std::filesystem::path& ownedDirectory,
	const std::filesystem::path& artifact,
	std::string& outDiagnostic) noexcept
{
	std::filesystem::path canonicalArtifact;
	bool ignoredIoFailure = false;
	return ResolveOwnedArtifactPath(
		cacheRoot, ownedDirectory, artifact, canonicalArtifact, outDiagnostic, ignoredIoFailure);
}

bool ShaderCache::ReadSpirvArtifactInternal(const std::filesystem::path& path,
	const ArtifactMetadata& metadata,
	TVector<uint32_t>& outSpirv,
	std::string& outDiagnostic,
	bool& outIoFailure) noexcept
{
	outIoFailure = false;
	if (metadata.IsPresent() && metadata.m_byteLength % sizeof(uint32_t) != 0)
	{
		outDiagnostic =
			"SPIR-V artifact byte length is not aligned to uint32 words for '" + path.generic_string() + "'.";
		return false;
	}

	TVector<uint8_t> bytes;
	if (!ReadArtifactBytes(path, metadata, bytes, outDiagnostic, outIoFailure))
	{
		return false;
	}

	TVector<uint32_t> candidate;
	candidate.Resize(bytes.Num() / sizeof(uint32_t));
	if (!bytes.IsEmpty())
	{
		std::memcpy(candidate.GetData(), bytes.GetData(), bytes.Num());
	}
	outSpirv = std::move(candidate);
	return true;
}

bool ShaderCache::IsValidArtifactSet(const ArtifactSet& artifacts, bool bAllowEmpty) noexcept
{
	const bool bHasVertex = artifacts.m_vertex.IsPresent();
	const bool bHasFragment = artifacts.m_fragment.IsPresent();
	const bool bHasCompute = artifacts.m_compute.IsPresent();
	if (!bHasVertex && !bHasFragment && !bHasCompute)
	{
		return bAllowEmpty;
	}
	return bHasVertex == bHasFragment && ((bHasVertex && bHasFragment) || bHasCompute);
}

bool ShaderCache::HasMatchingArtifactTopology(const ArtifactSet& regular, const ArtifactSet& debug) noexcept
{
	return regular.m_vertex.IsPresent() == debug.m_vertex.IsPresent() &&
		   regular.m_fragment.IsPresent() == debug.m_fragment.IsPresent() &&
		   regular.m_compute.IsPresent() == debug.m_compute.IsPresent();
}

bool ShaderCache::IsValidGeneration(const std::string& generation) noexcept
{
	if (generation.size() != 32)
	{
		return false;
	}
	return std::all_of(generation.begin(),
		generation.end(),
		[](unsigned char character)
		{ return (character >= '0' && character <= '9') || (character >= 'a' && character <= 'f'); });
}

bool ShaderCache::DescribeArtifactSet(const TVector<uint32_t>& vertexSpirv,
	const TVector<uint32_t>& fragmentSpirv,
	const TVector<uint32_t>& computeSpirv,
	ArtifactSet& outMetadata,
	std::string& outDiagnostic) noexcept
{
	ArtifactSet metadata;
	metadata.m_vertex = DescribeArtifact(vertexSpirv.Num() == 0 ? nullptr : &vertexSpirv[0],
		static_cast<uint64_t>(vertexSpirv.Num()) * sizeof(uint32_t));
	metadata.m_fragment = DescribeArtifact(fragmentSpirv.Num() == 0 ? nullptr : &fragmentSpirv[0],
		static_cast<uint64_t>(fragmentSpirv.Num()) * sizeof(uint32_t));
	metadata.m_compute = DescribeArtifact(computeSpirv.Num() == 0 ? nullptr : &computeSpirv[0],
		static_cast<uint64_t>(computeSpirv.Num()) * sizeof(uint32_t));
	if (!IsValidArtifactSet(metadata, false))
	{
		outDiagnostic = "A shader artifact set must contain a vertex/fragment pair or compute artifact.";
		return false;
	}
	outMetadata = metadata;
	outDiagnostic.clear();
	return true;
}

Workspace::WorkspaceCacheIdentity ShaderCache::GetExpectedIdentityLocked() const
{
#if defined(SAILOR_SHADER_CACHE_TEST_HOOKS)
	if (m_identityOverride.has_value())
	{
		return *m_identityOverride;
	}
#endif
	return MakeExpectedIdentity();
}

std::filesystem::path ShaderCache::GetCacheFilepathLocked() const
{
	return m_cacheRoot / "ShaderCache.yaml";
}

std::filesystem::path ShaderCache::GetPrecompiledFolderLocked() const
{
	return m_cacheRoot / "PrecompiledShaders";
}

std::filesystem::path ShaderCache::GetCompiledFolderLocked() const
{
	return m_cacheRoot / "CompiledShaders";
}

std::filesystem::path ShaderCache::GetCompiledDebugFolderLocked() const
{
	return m_cacheRoot / "CompiledShadersWithDebug";
}

std::filesystem::path ShaderCache::GetArtifactPathLocked(const ShaderCacheData::Entry& entry,
	const std::string& shaderKind,
	bool bIsDebug) const
{
	return GetShaderFilepath(bIsDebug ? GetCompiledDebugFolderLocked() : GetCompiledFolderLocked(),
		entry.m_fileId,
		static_cast<int32_t>(entry.m_permutation),
		shaderKind,
		CompiledShaderFileExtension,
		entry.m_generation);
}
