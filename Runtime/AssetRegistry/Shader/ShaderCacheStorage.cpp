#include "AssetRegistry/Shader/ShaderCache.h"

#include "AssetRegistry/AssetRegistry.h"
#include "AssetRegistry/Shader/ShaderCacheInternal.h"
#include "Sailor.h"

#include <filesystem>
#include <iomanip>
#include <random>
#include <sstream>

using namespace Sailor;
using namespace Sailor::ShaderCacheInternal;

void ShaderCache::Initialize()
{
	SAILOR_PROFILE_FUNCTION();

	std::error_code error;
	m_cacheRoot = std::filesystem::path(AssetRegistry::GetCacheFolder());
	std::filesystem::create_directories(m_cacheRoot, error);
	if (!error)
	{
		m_cacheRoot = std::filesystem::weakly_canonical(m_cacheRoot, error);
	}

	{
		std::lock_guard<std::mutex> lock(m_cacheMutex);
		std::string diagnostic;
		m_bStorageReady = !error && EnsureOwnedDirectoriesLocked(diagnostic);
		if (!m_bStorageReady)
		{
			m_lastSaveDiagnostic =
				error ? "Cannot initialize shader cache root: " + error.message() : std::move(diagnostic);
			SAILOR_LOG_ERROR("Shader cache storage initialization failed: %s", m_lastSaveDiagnostic.c_str());
		}
	}

	LoadCache();
}

void ShaderCache::Shutdown()
{
	ClearExpired();
	SaveCache();
}

void ShaderCache::SaveCache(bool bForcely)
{
	SAILOR_PROFILE_FUNCTION();

	std::lock_guard<std::mutex> lock(m_cacheMutex);
	SaveCacheLocked(bForcely);
}

bool ShaderCache::RecoverMissingStorage()
{
	SAILOR_PROFILE_FUNCTION();

	std::lock_guard<std::mutex> lock(m_cacheMutex);
	if (m_bPreserveStorageAfterLoadFailure)
	{
		return false;
	}

	bool bStorageMissing = false;
	auto inspect = [&](const std::filesystem::path& path, bool bExpectDirectory) -> bool
	{
		std::error_code error;
		const std::filesystem::file_status status = std::filesystem::symlink_status(path, error);
		if (error == std::errc::no_such_file_or_directory || error == std::errc::not_a_directory)
		{
			bStorageMissing = true;
			return true;
		}
		if (error)
		{
			m_lastSaveDiagnostic =
				"Cannot inspect shader cache storage '" + path.generic_string() + "': " + error.message();
			SAILOR_LOG_ERROR("Shader cache recovery failed: %s", m_lastSaveDiagnostic.c_str());
			return false;
		}

		const bool bExpectedType =
			bExpectDirectory ? std::filesystem::is_directory(status) : std::filesystem::is_regular_file(status);
		bStorageMissing |= !bExpectedType;
		return true;
	};

	if (!inspect(GetCacheFilepathLocked(), false) || !inspect(GetPrecompiledFolderLocked(), true) ||
		!inspect(GetCompiledFolderLocked(), true) || !inspect(GetCompiledDebugFolderLocked(), true) || !bStorageMissing)
	{
		return false;
	}

	Workspace::WorkspaceCacheLoadResult loadResult;
	loadResult.m_status = Workspace::EWorkspaceCacheLoadStatus::Missing;
	loadResult.m_diagnostic = "Shader cache storage disappeared during the running session.";
	ResetInvalidCacheLocked(std::move(loadResult));
	return m_bHasCommittedSnapshot && !m_bIsDirty && !m_bPreserveStorageAfterLoadFailure;
}

bool ShaderCache::SaveCacheLocked(bool bForcely, Workspace::EWorkspaceCacheAtomicWriteFailurePoint failurePoint)
{
	if (m_bPreserveStorageAfterLoadFailure)
	{
		return true;
	}
	if (!bForcely && !m_bIsDirty)
	{
		return true;
	}
#if defined(SAILOR_SHADER_CACHE_TEST_HOOKS)
	if (failurePoint == Workspace::EWorkspaceCacheAtomicWriteFailurePoint::None &&
		m_nextSaveFailureForTests != Workspace::EWorkspaceCacheAtomicWriteFailurePoint::None)
	{
		failurePoint = m_nextSaveFailureForTests;
		m_nextSaveFailureForTests = Workspace::EWorkspaceCacheAtomicWriteFailurePoint::None;
	}
#endif

	std::string diagnostic;
	if (WriteCacheDataLocked(m_cache, diagnostic, failurePoint))
	{
		m_bIsDirty = false;
		m_committedCache = m_cache;
		m_bHasCommittedSnapshot = true;

		std::string sweepDiagnostic;
		if (SweepUnreferencedArtifactsLocked(m_committedCache, sweepDiagnostic))
		{
			m_lastSaveDiagnostic.clear();
			return true;
		}

		m_bIsDirty = true;
		m_lastSaveDiagnostic = std::move(sweepDiagnostic);
		SAILOR_LOG_ERROR("Shader cache post-commit cleanup failed: %s", m_lastSaveDiagnostic.c_str());
		return false;
	}

	m_bIsDirty = true;
	m_lastSaveDiagnostic = std::move(diagnostic);
	SAILOR_LOG_ERROR("Shader cache save failed: %s", m_lastSaveDiagnostic.c_str());
	return false;
}

void ShaderCache::LoadCache()
{
	SAILOR_PROFILE_FUNCTION();

	std::lock_guard<std::mutex> lock(m_cacheMutex);
	Workspace::WorkspaceCacheLoadResult loadResult;
	const auto identity = GetExpectedIdentityLocked();
	loadResult = Workspace::LoadWorkspaceCacheEnvelope(GetCacheFilepathLocked(), identity);

	if (loadResult.IsLoaded())
	{
		ShaderCacheData candidate;
		std::string diagnostic;
		bool bValidArtifacts = false;
		bool bArtifactIoFailure = false;
		if (TryDeserializeShaderCachePayload(loadResult.m_payload, candidate, diagnostic))
		{
			bValidArtifacts = ValidateAllArtifactsLocked(candidate, diagnostic, bArtifactIoFailure);
		}
		if (bValidArtifacts)
		{
			m_cache = std::move(candidate);
			m_committedCache = m_cache;
			m_bIsDirty = false;
			m_bPreserveStorageAfterLoadFailure = false;
			m_bHasCommittedSnapshot = true;
			m_quarantinedEntries.Clear();
			m_lastSaveDiagnostic.clear();
			m_lastLoadResult = std::move(loadResult);
			return;
		}

		loadResult.m_status = bArtifactIoFailure ? Workspace::EWorkspaceCacheLoadStatus::IoFailure
												 : Workspace::EWorkspaceCacheLoadStatus::Corrupt;
		loadResult.m_diagnostic = std::move(diagnostic);
		loadResult.m_payload.clear();
	}
	if (m_bPreserveStorageAfterLoadFailure)
	{
		m_cache.m_entries.Clear();
		m_committedCache.m_entries.Clear();
		m_bIsDirty = false;
		m_bHasCommittedSnapshot = false;
		m_lastSaveDiagnostic.clear();
		m_lastLoadResult = std::move(loadResult);
		SAILOR_LOG_ERROR("Shader cache reload status=%s: %s Read-only I/O quarantine remains active until a fully "
						 "successful reload or ClearAll.",
			std::string(magic_enum::enum_name(m_lastLoadResult.m_status)).c_str(),
			m_lastLoadResult.m_diagnostic.c_str());
		return;
	}
	if (!ShouldResetCache(loadResult.m_status))
	{
		m_cache.m_entries.Clear();
		m_committedCache.m_entries.Clear();
		m_bIsDirty = false;
		m_bPreserveStorageAfterLoadFailure = true;
		m_bHasCommittedSnapshot = false;
		m_lastSaveDiagnostic.clear();
		m_lastLoadResult = std::move(loadResult);
		SAILOR_LOG_ERROR(
			"Shader cache load status=%s: %s Existing cache metadata and artifact directories were preserved.",
			std::string(magic_enum::enum_name(m_lastLoadResult.m_status)).c_str(),
			m_lastLoadResult.m_diagnostic.c_str());
		return;
	}

	ResetInvalidCacheLocked(std::move(loadResult));
}

bool ShaderCache::WriteCacheDataLocked(const ShaderCacheData& cache,
	std::string& outDiagnostic,
	Workspace::EWorkspaceCacheAtomicWriteFailurePoint failurePoint)
{
	std::string envelope;
	const auto identity = GetExpectedIdentityLocked();
	if (!Workspace::SerializeWorkspaceCacheEnvelope(
			identity, SerializeShaderCachePayload(cache), envelope, outDiagnostic))
	{
		return false;
	}

	return Workspace::AtomicReplaceWorkspaceCacheText(GetCacheFilepathLocked(), envelope, outDiagnostic, failurePoint);
}

bool ShaderCache::WriteCacheLocked(std::string& outDiagnostic)
{
	return WriteCacheDataLocked(m_cache, outDiagnostic);
}

bool ShaderCache::CommitCandidateLocked(ShaderCacheData candidate,
	std::string& outDiagnostic,
	Workspace::EWorkspaceCacheAtomicWriteFailurePoint failurePoint)
{
	if (m_bPreserveStorageAfterLoadFailure)
	{
		outDiagnostic = "Shader cache storage is quarantined after an I/O failure; persistent mutation is disabled.";
		return false;
	}
	if (!WriteCacheDataLocked(candidate, outDiagnostic, failurePoint))
	{
		return false;
	}
	m_cache = std::move(candidate);
	m_committedCache = m_cache;
	m_bHasCommittedSnapshot = true;
	m_bIsDirty = false;
	m_lastSaveDiagnostic.clear();
	return true;
}

void ShaderCache::ResetInvalidCacheLocked(Workspace::WorkspaceCacheLoadResult loadResult)
{
	m_cache.m_entries.Clear();
	m_committedCache.m_entries.Clear();
	m_quarantinedEntries.Clear();
	m_bIsDirty = true;
	m_bPreserveStorageAfterLoadFailure = false;
	m_bHasCommittedSnapshot = false;
	m_lastLoadResult = std::move(loadResult);

	std::string resetDiagnostic;
	const bool bFilesReset = ClearOwnedCacheFilesLocked(resetDiagnostic);
	std::string writeDiagnostic;
	const bool bEnvelopeWritten = WriteCacheLocked(writeDiagnostic);
	if (bFilesReset && bEnvelopeWritten)
	{
		m_bIsDirty = false;
		m_bPreserveStorageAfterLoadFailure = false;
		m_committedCache = m_cache;
		m_bHasCommittedSnapshot = true;
		m_lastSaveDiagnostic.clear();
		AppendDiagnostic(m_lastLoadResult.m_diagnostic,
			"The shader cache and owned artifact directories were reset to an empty current envelope.");
		SAILOR_LOG("Shader cache load status=%s: %s",
			std::string(magic_enum::enum_name(m_lastLoadResult.m_status)).c_str(),
			m_lastLoadResult.m_diagnostic.c_str());
		return;
	}

	m_bIsDirty = true;
	m_bPreserveStorageAfterLoadFailure = false;
	m_bHasCommittedSnapshot = false;
	m_lastSaveDiagnostic.clear();
	AppendDiagnostic(m_lastSaveDiagnostic, resetDiagnostic);
	AppendDiagnostic(m_lastSaveDiagnostic, writeDiagnostic);
	AppendDiagnostic(m_lastLoadResult.m_diagnostic, "The shader cache reset was incomplete: " + m_lastSaveDiagnostic);
	SAILOR_LOG_ERROR("Shader cache load status=%s: %s",
		std::string(magic_enum::enum_name(m_lastLoadResult.m_status)).c_str(),
		m_lastLoadResult.m_diagnostic.c_str());
}

void ShaderCache::EnterStorageQuarantineLocked(std::string diagnostic)
{
	m_cache.m_entries.Clear();
	m_committedCache.m_entries.Clear();
	m_bIsDirty = false;
	m_bPreserveStorageAfterLoadFailure = true;
	m_bHasCommittedSnapshot = false;
	m_lastSaveDiagnostic.clear();
	m_lastLoadResult.m_status = Workspace::EWorkspaceCacheLoadStatus::IoFailure;
	m_lastLoadResult.m_diagnostic = std::move(diagnostic);
	m_lastLoadResult.m_payload.clear();
	SAILOR_LOG_ERROR(
		"Shader cache entered read-only I/O quarantine: %s Existing metadata and artifacts were preserved.",
		m_lastLoadResult.m_diagnostic.c_str());
}

bool ShaderCache::EnsureOwnedDirectoriesLocked(std::string& outDiagnostic)
{
	outDiagnostic.clear();
	std::error_code error;
	std::filesystem::create_directories(m_cacheRoot, error);
	if (error)
	{
		outDiagnostic = "Cannot create shader cache root '" + m_cacheRoot.generic_string() + "': " + error.message();
		return false;
	}

	bool bSuccess = true;
	const std::filesystem::path directories[] = {
		GetPrecompiledFolderLocked(), GetCompiledFolderLocked(), GetCompiledDebugFolderLocked()};
	for (const auto& directory : directories)
	{
		error.clear();
		const auto status = std::filesystem::symlink_status(directory, error);
		if (!error && std::filesystem::is_symlink(status))
		{
			AppendDiagnostic(
				outDiagnostic, "Refusing symlinked shader cache directory '" + directory.generic_string() + "'.");
			bSuccess = false;
			continue;
		}

		error.clear();
		std::filesystem::create_directories(directory, error);
		if (error)
		{
			AppendDiagnostic(outDiagnostic,
				"Cannot create shader cache directory '" + directory.generic_string() + "': " + error.message());
			bSuccess = false;
			continue;
		}

		std::filesystem::path canonical;
		std::string diagnostic;
		if (!ResolveDirectCacheChild(m_cacheRoot, directory, canonical, diagnostic))
		{
			AppendDiagnostic(outDiagnostic, diagnostic);
			bSuccess = false;
		}
	}

	m_bStorageReady = bSuccess;
	return bSuccess;
}

bool ShaderCache::ClearOwnedCacheFilesLocked(std::string& outDiagnostic)
{
	outDiagnostic.clear();
	bool bSuccess = true;
	bSuccess &= RemovePath(m_cacheRoot, GetCacheFilepathLocked(), false, outDiagnostic);
	bSuccess &= RemovePath(m_cacheRoot, GetPrecompiledFolderLocked(), true, outDiagnostic);
	bSuccess &= RemovePath(m_cacheRoot, GetCompiledFolderLocked(), true, outDiagnostic);
	bSuccess &= RemovePath(m_cacheRoot, GetCompiledDebugFolderLocked(), true, outDiagnostic);

	std::string createDiagnostic;
	if (!EnsureOwnedDirectoriesLocked(createDiagnostic))
	{
		AppendDiagnostic(outDiagnostic, createDiagnostic);
		bSuccess = false;
	}
	return bSuccess;
}

bool ShaderCache::ValidateArtifactSetLocked(const ShaderCacheData::Entry& entry,
	const ArtifactSet& artifacts,
	bool bIsDebug,
	std::string& outDiagnostic,
	bool& outIoFailure) const
{
	TVector<uint32_t> ignored;
	const std::filesystem::path ownedDirectory = bIsDebug ? GetCompiledDebugFolderLocked() : GetCompiledFolderLocked();
	if (!ReadOwnedSpirvArtifactLocked(ownedDirectory,
			GetArtifactPathLocked(entry, VertexShaderTag, bIsDebug),
			artifacts.m_vertex,
			ignored,
			outDiagnostic,
			outIoFailure))
	{
		return false;
	}
	if (!ReadOwnedSpirvArtifactLocked(ownedDirectory,
			GetArtifactPathLocked(entry, FragmentShaderTag, bIsDebug),
			artifacts.m_fragment,
			ignored,
			outDiagnostic,
			outIoFailure))
	{
		return false;
	}
	return ReadOwnedSpirvArtifactLocked(ownedDirectory,
		GetArtifactPathLocked(entry, ComputeShaderTag, bIsDebug),
		artifacts.m_compute,
		ignored,
		outDiagnostic,
		outIoFailure);
}

bool ShaderCache::ReadOwnedSpirvArtifactLocked(const std::filesystem::path& ownedDirectory,
	const std::filesystem::path& artifact,
	const ArtifactMetadata& metadata,
	TVector<uint32_t>& outSpirv,
	std::string& outDiagnostic,
	bool& outIoFailure) const
{
#if defined(SAILOR_SHADER_CACHE_TEST_HOOKS)
	if (m_bArtifactReadIoFailureForTests)
	{
		outIoFailure = true;
		outDiagnostic = "Injected shader artifact read I/O failure for lifecycle validation.";
		return false;
	}
#endif

	std::filesystem::path canonicalArtifact;
	if (!ResolveOwnedArtifactPath(
			m_cacheRoot, ownedDirectory, artifact, canonicalArtifact, outDiagnostic, outIoFailure))
	{
		return false;
	}
	return ReadSpirvArtifactInternal(artifact, metadata, outSpirv, outDiagnostic, outIoFailure);
}

bool ShaderCache::ValidateAllArtifactsLocked(const ShaderCacheData& candidate,
	std::string& outDiagnostic,
	bool& outIoFailure) const
{
	outIoFailure = false;
	for (const auto& fileEntries : candidate.m_entries)
	{
		for (const ShaderCacheData::Entry& entry : *fileEntries.m_second)
		{
			if (!ValidateArtifactSetLocked(entry, entry.m_regular, false, outDiagnostic, outIoFailure) ||
				!ValidateArtifactSetLocked(entry, entry.m_debug, true, outDiagnostic, outIoFailure))
			{
				outDiagnostic = "Shader cache artifact validation failed for fileId '" + entry.m_fileId.ToString() +
								"', permutation " + std::to_string(entry.m_permutation) + ": " + outDiagnostic;
				return false;
			}
		}
	}
	outDiagnostic.clear();
	return true;
}

bool ShaderCache::WriteSpirvSetLocked(const FileId& uid,
	uint32_t permutation,
	const std::string& generation,
	const TVector<uint32_t>& vertexSpirv,
	const TVector<uint32_t>& fragmentSpirv,
	const TVector<uint32_t>& computeSpirv,
	const ArtifactSet& metadata,
	bool bIsDebug,
	int32_t& artifactIndex,
	int32_t failArtifactIndex,
	std::string& outDiagnostic)
{
	if (!EnsureOwnedDirectoriesLocked(outDiagnostic))
	{
		return false;
	}

	if (!IsValidArtifactSet(metadata, false))
	{
		outDiagnostic = "A shader artifact set must contain a vertex/fragment pair or compute artifact.";
		return false;
	}

	ShaderCacheData::Entry pathEntry;
	pathEntry.m_fileId = uid;
	pathEntry.m_permutation = permutation;
	pathEntry.m_generation = generation;
	auto write = [&](const TVector<uint32_t>& spirv, const ArtifactMetadata& artifact, const char* shaderKind) -> bool
	{
		if (!artifact.IsPresent())
		{
			return true;
		}
		const auto path = GetArtifactPathLocked(pathEntry, shaderKind, bIsDebug);
		const auto ownedDirectory = bIsDebug ? GetCompiledDebugFolderLocked() : GetCompiledFolderLocked();
		std::filesystem::path canonicalArtifact;
		bool ignoredIoFailure = false;
		if (!ResolveOwnedArtifactPath(
				m_cacheRoot, ownedDirectory, path, canonicalArtifact, outDiagnostic, ignoredIoFailure))
		{
			return false;
		}
		std::string diagnostic;
		const auto failurePoint = artifactIndex == failArtifactIndex
									  ? Workspace::EWorkspaceCacheAtomicWriteFailurePoint::BeforeReplace
									  : Workspace::EWorkspaceCacheAtomicWriteFailurePoint::None;
		++artifactIndex;
		if (!Workspace::AtomicReplaceWorkspaceCacheBinary(
				path, &spirv[0], artifact.m_byteLength, diagnostic, failurePoint))
		{
			outDiagnostic = "Cannot atomically write shader artifact '" + path.generic_string() + "': " + diagnostic;
			return false;
		}
		return true;
	};

	if (!write(vertexSpirv, metadata.m_vertex, VertexShaderTag) ||
		!write(fragmentSpirv, metadata.m_fragment, FragmentShaderTag) ||
		!write(computeSpirv, metadata.m_compute, ComputeShaderTag))
	{
		return false;
	}

	outDiagnostic.clear();
	return true;
}

bool ShaderCache::GenerateUniqueGenerationLocked(const FileId& uid,
	uint32_t permutation,
	std::string& outGeneration,
	std::string& outDiagnostic) const
{
	std::random_device random;
	for (uint32_t attempt = 0; attempt < 64; ++attempt)
	{
		std::ostringstream stream;
		stream << std::hex << std::nouppercase << std::setfill('0');
		for (uint32_t word = 0; word < 4; ++word)
		{
			stream << std::setw(8) << static_cast<uint32_t>(random());
		}
		const std::string generation = stream.str();
		if (!IsValidGeneration(generation))
		{
			continue;
		}

		ShaderCacheData::Entry entry;
		entry.m_fileId = uid;
		entry.m_permutation = permutation;
		entry.m_generation = generation;
		const std::pair<std::filesystem::path, bool> candidates[] = {
			{GetArtifactPathLocked(entry, VertexShaderTag, false), false},
			{GetArtifactPathLocked(entry, FragmentShaderTag, false), false},
			{GetArtifactPathLocked(entry, ComputeShaderTag, false), false},
			{GetArtifactPathLocked(entry, VertexShaderTag, true), true},
			{GetArtifactPathLocked(entry, FragmentShaderTag, true), true},
			{GetArtifactPathLocked(entry, ComputeShaderTag, true), true}};
		bool bCollision = false;
		for (const auto& [candidate, bIsDebug] : candidates)
		{
			std::filesystem::path canonicalArtifact;
			bool ignoredIoFailure = false;
			const std::filesystem::path ownedDirectory =
				bIsDebug ? GetCompiledDebugFolderLocked() : GetCompiledFolderLocked();
			if (!ResolveOwnedArtifactPath(
					m_cacheRoot, ownedDirectory, candidate, canonicalArtifact, outDiagnostic, ignoredIoFailure))
			{
				return false;
			}
			std::error_code error;
			if (std::filesystem::exists(candidate, error))
			{
				bCollision = true;
				break;
			}
			if (error)
			{
				outDiagnostic = "Cannot check immutable shader generation collision for '" +
								candidate.generic_string() + "': " + error.message();
				return false;
			}
		}
		if (!bCollision)
		{
			outGeneration = generation;
			outDiagnostic.clear();
			return true;
		}
	}
	outDiagnostic = "Cannot allocate a collision-free immutable shader artifact generation.";
	return false;
}

bool ShaderCache::CachePrecompiledGlsl(const FileId& uid,
	uint32_t permutation,
	const std::string& vertexGlsl,
	const std::string& fragmentGlsl,
	const std::string& computeGlsl)
{
	SAILOR_PROFILE_FUNCTION();

	if (!m_bSavePrecompiledGlsl)
	{
		return true;
	}

	std::lock_guard<std::mutex> lock(m_cacheMutex);
	if (m_bPreserveStorageAfterLoadFailure)
	{
		m_lastSaveDiagnostic = "Precompiled GLSL persistence is disabled during read-only shader cache quarantine.";
		return false;
	}
	std::string diagnostic;
	if (!EnsureOwnedDirectoriesLocked(diagnostic))
	{
		m_lastSaveDiagnostic = std::move(diagnostic);
		SAILOR_LOG_ERROR("Precompiled shader cache write failed: %s", m_lastSaveDiagnostic.c_str());
		return false;
	}

	auto write = [&](const std::string& glsl, const char* shaderKind) -> bool
	{
		if (glsl.empty())
		{
			return true;
		}
		const auto path = GetShaderFilepath(GetPrecompiledFolderLocked(),
			uid,
			static_cast<int32_t>(permutation),
			shaderKind,
			PrecompiledShaderFileExtension);
		std::filesystem::path canonicalArtifact;
		bool ignoredIoFailure = false;
		if (!ResolveOwnedArtifactPath(m_cacheRoot,
				GetPrecompiledFolderLocked(),
				path,
				canonicalArtifact,
				m_lastSaveDiagnostic,
				ignoredIoFailure))
		{
			SAILOR_LOG_ERROR("Precompiled shader cache write failed: %s", m_lastSaveDiagnostic.c_str());
			return false;
		}
		std::string writeDiagnostic;
		if (!Workspace::AtomicReplaceWorkspaceCacheText(path, glsl, writeDiagnostic))
		{
			m_lastSaveDiagnostic =
				"Cannot atomically write precompiled shader '" + path.generic_string() + "': " + writeDiagnostic;
			SAILOR_LOG_ERROR("Precompiled shader cache write failed: %s", m_lastSaveDiagnostic.c_str());
			return false;
		}
		return true;
	};

	return write(vertexGlsl, VertexShaderTag) && write(fragmentGlsl, FragmentShaderTag) &&
		   write(computeGlsl, ComputeShaderTag);
}
