#include "ShaderCache.h"

#include "AssetRegistry/AssetRegistry.h"
#include "AssetRegistry/Shader/ShaderDependencyFingerprint.h"
#include "AssetRegistry/Shader/ShaderCompiler.h"
#include "AssetRegistry/Shader/ShaderCacheInternal.h"
#include "Core/Utils.h"
#include "Sailor.h"

#include <algorithm>
#include <filesystem>
#include <mutex>
#include <string>
#include <utility>

using namespace Sailor;
using namespace Sailor::ShaderCacheInternal;
ShaderCache::QuarantinedEntry* ShaderCache::FindQuarantinedEntryLocked(const FileId& uid, uint32_t permutation)
{
	const size_t index = m_quarantinedEntries.FindIf(
		[&](const QuarantinedEntry& entry) { return entry.m_fileId == uid && entry.m_permutation == permutation; });
	return index == static_cast<size_t>(-1) ? nullptr : &m_quarantinedEntries[index];
}

const ShaderCache::QuarantinedEntry* ShaderCache::FindQuarantinedEntryLocked(const FileId& uid,
	uint32_t permutation) const
{
	const size_t index = m_quarantinedEntries.FindIf(
		[&](const QuarantinedEntry& entry) { return entry.m_fileId == uid && entry.m_permutation == permutation; });
	return index == static_cast<size_t>(-1) ? nullptr : &m_quarantinedEntries[index];
}

bool ShaderCache::CaptureSourceState(const FileId& uid, ShaderSourceState& outState, std::string& outDiagnostic) const
{
	outState = {};
	outDiagnostic.clear();
	if (m_sourceStateProvider != nullptr)
	{
		if (!m_sourceStateProvider->Capture(uid, outState, outDiagnostic))
		{
			return false;
		}
		if (outState.m_fingerprint == 0)
		{
			outDiagnostic = "Shader source state provider returned an invalid fingerprint.";
			return false;
		}
		return true;
	}

	AssetRegistry* assetRegistry = App::GetSubmodule<AssetRegistry>();
	ShaderCompiler* shaderCompiler = App::GetSubmodule<ShaderCompiler>();
	if (assetRegistry == nullptr || shaderCompiler == nullptr)
	{
		outDiagnostic = "Shader dependency services are unavailable.";
		return false;
	}

	ShaderAssetInfoPtr assetInfo = assetRegistry->GetAssetInfoPtr<ShaderAssetInfoPtr>(uid);
	if (assetInfo == nullptr)
	{
		outDiagnostic = "Cannot find shader asset info for '" + uid.ToString() + "'.";
		return false;
	}

	TSharedPtr<ShaderAsset> shader = shaderCompiler->LoadShaderAsset(uid).Lock();
	if (!shader)
	{
		outDiagnostic = "Cannot parse shader source '" + assetInfo->GetAssetFilepath() + "'.";
		return false;
	}

	outState.m_timestamp = assetInfo->GetAssetLastModificationTime();
	TVector<ShaderDependencyFile> dependencies;
	dependencies.Reserve(shader->GetIncludes().Num() + 1);
	ShaderDependencyFile shaderSource;
	shaderSource.m_virtualPath = NormalizeDependencyVirtualPath(assetInfo->GetRelativeAssetFilepath());
	shaderSource.m_winnerIdentity = NormalizeDependencyPath(assetInfo->GetAssetFilepath());
	shaderSource.m_mountKind = static_cast<uint32_t>(assetInfo->GetMountKind());
	if (!Utils::TryGetFileRevision(assetInfo->GetAssetFilepath(), shaderSource.m_revision))
	{
		outDiagnostic = "Cannot capture shader source revision '" + assetInfo->GetAssetFilepath() + "'.";
		return false;
	}
	dependencies.Add(std::move(shaderSource));

	for (const std::string& include : shader->GetIncludes())
	{
		AssetRegistry::AssetReadLocation location;
		if (!assetRegistry->ResolveContentFile(include, location))
		{
			outDiagnostic = "Cannot resolve YAML shader include '" + include + "' for '" +
							assetInfo->GetAssetFilepath() + "'. Include paths must be Content-root virtual paths.";
			return false;
		}

		ShaderDependencyFile dependency;
		dependency.m_virtualPath = NormalizeDependencyVirtualPath(include);
		dependency.m_winnerIdentity = NormalizeDependencyPath(location.m_physicalPath);
		dependency.m_mountKind = static_cast<uint32_t>(location.m_mountKind);
		if (!Utils::TryGetFileRevision(location.m_physicalPath.generic_string(), dependency.m_revision))
		{
			outDiagnostic = "Cannot capture YAML shader include revision '" + include + "' from '" +
							location.m_physicalPath.generic_string() + "'.";
			return false;
		}
		dependencies.Add(std::move(dependency));

		std::time_t includeTimestamp = 0;
		if (assetRegistry->GetContentFileModificationTime(include, includeTimestamp))
		{
			outState.m_timestamp = std::max(outState.m_timestamp, includeTimestamp);
		}
	}

	outState.m_fingerprint = CalculateShaderDependencyFingerprint(dependencies);
	if (outState.m_fingerprint == 0)
	{
		outDiagnostic = "Shader dependency fingerprint contains an invalid source revision.";
		return false;
	}
	return true;
}

bool ShaderCache::CacheCompleteSpirvLocked(const FileId& uid,
	uint32_t permutation,
	const TVector<uint32_t>& vertexSpirv,
	const TVector<uint32_t>& fragmentSpirv,
	const TVector<uint32_t>& computeSpirv,
	const TVector<uint32_t>& debugVertexSpirv,
	const TVector<uint32_t>& debugFragmentSpirv,
	const TVector<uint32_t>& debugComputeSpirv,
	std::time_t sourceTimestamp,
	uint64_t sourceFingerprint,
	int32_t failArtifactIndex,
	std::string& outDiagnostic)
{
	ArtifactSet regularMetadata;
	ArtifactSet debugMetadata;
	if (!DescribeArtifactSet(vertexSpirv, fragmentSpirv, computeSpirv, regularMetadata, outDiagnostic) ||
		!DescribeArtifactSet(debugVertexSpirv, debugFragmentSpirv, debugComputeSpirv, debugMetadata, outDiagnostic))
	{
		return false;
	}
	if (!HasMatchingArtifactTopology(regularMetadata, debugMetadata))
	{
		outDiagnostic = "Regular and debug shader artifact sets must contain identical shader stages.";
		return false;
	}

	if (sourceFingerprint == 0)
	{
		outDiagnostic = "Cannot publish SPIR-V without a valid shader dependency fingerprint.";
		return false;
	}
	if (m_bPreserveStorageAfterLoadFailure)
	{
		QuarantinedEntry candidate;
		candidate.m_fileId = uid;
		candidate.m_permutation = permutation;
		candidate.m_timestamp = sourceTimestamp;
		candidate.m_sourceFingerprint = sourceFingerprint;
		candidate.m_vertex = vertexSpirv;
		candidate.m_fragment = fragmentSpirv;
		candidate.m_compute = computeSpirv;
		candidate.m_debugVertex = debugVertexSpirv;
		candidate.m_debugFragment = debugFragmentSpirv;
		candidate.m_debugCompute = debugComputeSpirv;
		if (QuarantinedEntry* existing = FindQuarantinedEntryLocked(uid, permutation))
		{
			*existing = std::move(candidate);
		}
		else
		{
			m_quarantinedEntries.Add(std::move(candidate));
		}
		outDiagnostic.clear();
		return true;
	}

	if (!EnsureOwnedDirectoriesLocked(outDiagnostic))
	{
		return false;
	}
	std::string generation;
	if (!GenerateUniqueGenerationLocked(uid, permutation, generation, outDiagnostic))
	{
		return false;
	}
	int32_t artifactIndex = 0;
	if (!WriteSpirvSetLocked(uid,
			permutation,
			generation,
			vertexSpirv,
			fragmentSpirv,
			computeSpirv,
			regularMetadata,
			false,
			artifactIndex,
			failArtifactIndex,
			outDiagnostic) ||
		!WriteSpirvSetLocked(uid,
			permutation,
			generation,
			debugVertexSpirv,
			debugFragmentSpirv,
			debugComputeSpirv,
			debugMetadata,
			true,
			artifactIndex,
			failArtifactIndex,
			outDiagnostic))
	{
		return false;
	}

	auto& entries = m_cache.m_entries[uid];
	auto existing = std::find_if(std::begin(entries),
		std::end(entries),
		[permutation](const ShaderCacheData::Entry& entry) { return entry.m_permutation == permutation; });
	ShaderCacheData::Entry candidate;
	candidate.m_fileId = uid;
	candidate.m_permutation = permutation;
	candidate.m_timestamp = sourceTimestamp;
	candidate.m_sourceFingerprint = sourceFingerprint;
	candidate.m_generation = generation;
	candidate.m_regular = regularMetadata;
	candidate.m_debug = debugMetadata;
	if (existing == std::end(entries))
	{
		entries.Add(std::move(candidate));
	}
	else
	{
		*existing = std::move(candidate);
	}

	const size_t quarantinedIndex = m_quarantinedEntries.FindIf(
		[&](const QuarantinedEntry& entry) { return entry.m_fileId == uid && entry.m_permutation == permutation; });
	if (quarantinedIndex != static_cast<size_t>(-1))
	{
		m_quarantinedEntries.RemoveAt(quarantinedIndex);
	}
	m_bIsDirty = true;
	outDiagnostic.clear();
	return true;
}

bool ShaderCache::CacheSpirv_ThreadSafe(const FileId& uid,
	uint32_t permutation,
	const TVector<uint32_t>& vertexSpirv,
	const TVector<uint32_t>& fragmentSpirv,
	const TVector<uint32_t>& computeSpirv,
	const TVector<uint32_t>& debugVertexSpirv,
	const TVector<uint32_t>& debugFragmentSpirv,
	const TVector<uint32_t>& debugComputeSpirv)
{
	SAILOR_PROFILE_FUNCTION();
	ShaderSourceState sourceState;
	std::string diagnostic;
	if (!CaptureSourceState(uid, sourceState, diagnostic))
	{
		std::lock_guard<std::mutex> lock(m_cacheMutex);
		m_lastSaveDiagnostic = std::move(diagnostic);
		SAILOR_LOG_ERROR("Complete SPIR-V cache publication failed: %s", m_lastSaveDiagnostic.c_str());
		return false;
	}

	return CacheSpirvForSourceFingerprint_ThreadSafe(uid,
		permutation,
		vertexSpirv,
		fragmentSpirv,
		computeSpirv,
		debugVertexSpirv,
		debugFragmentSpirv,
		debugComputeSpirv,
		sourceState.m_fingerprint);
}

bool ShaderCache::CacheSpirvForSourceFingerprint_ThreadSafe(const FileId& uid,
	uint32_t permutation,
	const TVector<uint32_t>& vertexSpirv,
	const TVector<uint32_t>& fragmentSpirv,
	const TVector<uint32_t>& computeSpirv,
	const TVector<uint32_t>& debugVertexSpirv,
	const TVector<uint32_t>& debugFragmentSpirv,
	const TVector<uint32_t>& debugComputeSpirv,
	uint64_t expectedSourceFingerprint)
{
	ShaderSourceState currentSourceState;
	std::string diagnostic;
	if (!CaptureSourceState(uid, currentSourceState, diagnostic))
	{
		std::lock_guard<std::mutex> lock(m_cacheMutex);
		m_lastSaveDiagnostic = std::move(diagnostic);
		SAILOR_LOG_ERROR("Complete SPIR-V cache publication failed: %s", m_lastSaveDiagnostic.c_str());
		return false;
	}
	if (expectedSourceFingerprint == 0 || currentSourceState.m_fingerprint != expectedSourceFingerprint)
	{
		std::lock_guard<std::mutex> lock(m_cacheMutex);
		m_lastSaveDiagnostic = "Shader source or a YAML include changed while its SPIR-V was compiling.";
		SAILOR_LOG_ERROR("Complete SPIR-V cache publication failed: %s", m_lastSaveDiagnostic.c_str());
		return false;
	}

	std::lock_guard<std::mutex> lock(m_cacheMutex);
	if (!CacheCompleteSpirvLocked(uid,
			permutation,
			vertexSpirv,
			fragmentSpirv,
			computeSpirv,
			debugVertexSpirv,
			debugFragmentSpirv,
			debugComputeSpirv,
			currentSourceState.m_timestamp,
			currentSourceState.m_fingerprint,
			-1,
			diagnostic))
	{
		m_lastSaveDiagnostic = std::move(diagnostic);
		SAILOR_LOG_ERROR("Complete SPIR-V cache publication failed: %s", m_lastSaveDiagnostic.c_str());
		return false;
	}
	m_lastSaveDiagnostic.clear();
	return true;
}

bool ShaderCache::GetSpirvCode(const FileId& uid,
	uint32_t permutation,
	TVector<uint32_t>& vertexSpirv,
	TVector<uint32_t>& fragmentSpirv,
	TVector<uint32_t>& computeSpirv,
	bool bIsDebug)
{
	SAILOR_PROFILE_FUNCTION();

	std::lock_guard<std::mutex> lock(m_cacheMutex);
	if (IsExpiredLocked(uid, permutation))
	{
		return false;
	}
	if (const QuarantinedEntry* quarantined = FindQuarantinedEntryLocked(uid, permutation))
	{
		const TVector<uint32_t>& candidateVertex = bIsDebug ? quarantined->m_debugVertex : quarantined->m_vertex;
		const TVector<uint32_t>& candidateFragment = bIsDebug ? quarantined->m_debugFragment : quarantined->m_fragment;
		const TVector<uint32_t>& candidateCompute = bIsDebug ? quarantined->m_debugCompute : quarantined->m_compute;
		vertexSpirv = candidateVertex;
		fragmentSpirv = candidateFragment;
		computeSpirv = candidateCompute;
		return true;
	}

	auto& entries = m_cache.m_entries[uid];
	auto entry = std::find_if(std::cbegin(entries),
		std::cend(entries),
		[permutation](const ShaderCacheData::Entry& candidate) { return candidate.m_permutation == permutation; });
	if (entry == std::cend(entries))
	{
		return false;
	}

	const ArtifactSet& artifacts = bIsDebug ? entry->m_debug : entry->m_regular;
	if (!IsValidArtifactSet(artifacts, false))
	{
		return false;
	}

	TVector<uint32_t> candidateVertex;
	TVector<uint32_t> candidateFragment;
	TVector<uint32_t> candidateCompute;
	std::string diagnostic;
	bool ignoredIoFailure = false;
	const std::filesystem::path ownedDirectory = bIsDebug ? GetCompiledDebugFolderLocked() : GetCompiledFolderLocked();
	if (!ReadOwnedSpirvArtifactLocked(ownedDirectory,
			GetArtifactPathLocked(*entry, VertexShaderTag, bIsDebug),
			artifacts.m_vertex,
			candidateVertex,
			diagnostic,
			ignoredIoFailure) ||
		!ReadOwnedSpirvArtifactLocked(ownedDirectory,
			GetArtifactPathLocked(*entry, FragmentShaderTag, bIsDebug),
			artifacts.m_fragment,
			candidateFragment,
			diagnostic,
			ignoredIoFailure) ||
		!ReadOwnedSpirvArtifactLocked(ownedDirectory,
			GetArtifactPathLocked(*entry, ComputeShaderTag, bIsDebug),
			artifacts.m_compute,
			candidateCompute,
			diagnostic,
			ignoredIoFailure))
	{
		if (ignoredIoFailure)
		{
			EnterStorageQuarantineLocked("Runtime shader artifact read failed for fileId '" + uid.ToString() +
										 "', permutation " + std::to_string(permutation) + ": " + diagnostic);
		}
		return false;
	}

	vertexSpirv = std::move(candidateVertex);
	fragmentSpirv = std::move(candidateFragment);
	computeSpirv = std::move(candidateCompute);
	return true;
}

bool ShaderCache::Contains(const FileId& uid) const
{
	std::lock_guard<std::mutex> lock(m_cacheMutex);
	return m_cache.m_entries.ContainsKey(uid) ||
		   m_quarantinedEntries.ContainsIf([&](const QuarantinedEntry& entry) { return entry.m_fileId == uid; });
}

bool ShaderCache::IsExpired(const FileId& uid, uint32_t permutation)
{
	std::lock_guard<std::mutex> lock(m_cacheMutex);
	return IsExpiredLocked(uid, permutation);
}

bool ShaderCache::IsExpiredLocked(const FileId& uid, uint32_t permutation)
{
	if (const QuarantinedEntry* quarantined = FindQuarantinedEntryLocked(uid, permutation))
	{
		ShaderSourceState sourceState;
		std::string diagnostic;
		return !CaptureSourceState(uid, sourceState, diagnostic) || quarantined->m_sourceFingerprint == 0 ||
			   quarantined->m_sourceFingerprint != sourceState.m_fingerprint;
	}
	if (!m_cache.m_entries.ContainsKey(uid))
	{
		return true;
	}

	const auto& entries = m_cache.m_entries[uid];
	const size_t index = entries.FindIf(
		[permutation](const ShaderCacheData::Entry& entry) { return entry.m_permutation == permutation; });
	if (index == static_cast<size_t>(-1))
	{
		return true;
	}

	const ShaderCacheData::Entry& entry = entries[index];
	if (!IsValidArtifactSet(entry.m_regular, false) || !IsValidArtifactSet(entry.m_debug, false) ||
		!HasMatchingArtifactTopology(entry.m_regular, entry.m_debug) || !IsValidGeneration(entry.m_generation))
	{
		return true;
	}

	std::string diagnostic;
	bool bArtifactIoFailure = false;
	if (!ValidateArtifactSetLocked(entry, entry.m_regular, false, diagnostic, bArtifactIoFailure) ||
		!ValidateArtifactSetLocked(entry, entry.m_debug, true, diagnostic, bArtifactIoFailure))
	{
		if (bArtifactIoFailure)
		{
			EnterStorageQuarantineLocked("Runtime shader artifact validation failed for fileId '" + uid.ToString() +
										 "', permutation " + std::to_string(permutation) + ": " + diagnostic);
		}
		return true;
	}

	ShaderSourceState sourceState;
	return !CaptureSourceState(uid, sourceState, diagnostic) || entry.m_sourceFingerprint == 0 ||
		   entry.m_sourceFingerprint != sourceState.m_fingerprint;
}
