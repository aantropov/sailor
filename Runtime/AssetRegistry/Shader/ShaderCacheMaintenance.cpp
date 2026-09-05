#include "AssetRegistry/Shader/ShaderCache.h"

#include "AssetRegistry/Shader/ShaderCacheInternal.h"
#include "Sailor.h"
#include "Tasks/Scheduler.h"
#include "Tasks/Tasks.h"

#include <filesystem>
#include <utility>

using namespace Sailor;
using namespace Sailor::ShaderCacheInternal;

bool ShaderCache::SweepUnreferencedArtifactsLocked(const ShaderCacheData& committedSnapshot, std::string& outDiagnostic)
{
	outDiagnostic.clear();
#if defined(SAILOR_SHADER_CACHE_TEST_HOOKS)
	if (m_bArtifactSweepFailureForTests)
	{
		m_bArtifactSweepFailureForTests = false;
		outDiagnostic = "Injected shader artifact sweep failure for lifecycle validation.";
		return false;
	}
#endif
	if (!m_bHasCommittedSnapshot)
	{
		outDiagnostic = "Cannot sweep shader artifacts without a successfully committed metadata snapshot.";
		return false;
	}
	if (!EnsureOwnedDirectoriesLocked(outDiagnostic))
	{
		return false;
	}

	TSet<std::string> whitelist;
	for (const auto& fileEntries : committedSnapshot.m_entries)
	{
		for (const ShaderCacheData::Entry& entry : *fileEntries.m_second)
		{
			const std::pair<const ArtifactMetadata*, const char*> regular[] = {
				{&entry.m_regular.m_vertex, VertexShaderTag},
				{&entry.m_regular.m_fragment, FragmentShaderTag},
				{&entry.m_regular.m_compute, ComputeShaderTag}};
			const std::pair<const ArtifactMetadata*, const char*> debug[] = {{&entry.m_debug.m_vertex, VertexShaderTag},
				{&entry.m_debug.m_fragment, FragmentShaderTag},
				{&entry.m_debug.m_compute, ComputeShaderTag}};
			for (const auto& [metadata, kind] : regular)
			{
				if (metadata->IsPresent())
				{
					whitelist.Insert(GetArtifactPathLocked(entry, kind, false).lexically_normal().generic_string());
					if (m_bSavePrecompiledGlsl)
					{
						whitelist.Insert(GetShaderFilepath(GetPrecompiledFolderLocked(),
							entry.m_fileId,
							entry.m_permutation,
							kind,
							PrecompiledShaderFileExtension)
								.lexically_normal()
								.generic_string());
					}
				}
			}
			for (const auto& [metadata, kind] : debug)
			{
				if (metadata->IsPresent())
				{
					whitelist.Insert(GetArtifactPathLocked(entry, kind, true).lexically_normal().generic_string());
				}
			}
		}
	}

	bool bSuccess = true;
	const std::filesystem::path artifactFolders[] = {
		GetCompiledFolderLocked(), GetCompiledDebugFolderLocked(), GetPrecompiledFolderLocked()};
	for (const auto& folder : artifactFolders)
	{
		std::error_code error;
		for (std::filesystem::directory_iterator iterator(folder, error), end; !error && iterator != end;
			iterator.increment(error))
		{
			const auto& path = iterator->path();
			if (iterator->is_regular_file(error) && !whitelist.Contains(path.lexically_normal().generic_string()))
			{
				std::string diagnostic;
				if (!RemoveOwnedArtifact(m_cacheRoot, folder, path, diagnostic))
				{
					AppendDiagnostic(outDiagnostic, diagnostic);
					bSuccess = false;
				}
			}
		}
		if (error)
		{
			AppendDiagnostic(outDiagnostic,
				"Cannot sweep shader cache directory '" + folder.generic_string() + "': " + error.message());
			bSuccess = false;
		}
	}
	return bSuccess;
}

bool ShaderCache::RemoveLocked(const FileId& uid,
	Workspace::EWorkspaceCacheAtomicWriteFailurePoint failurePoint,
	std::string& outDiagnostic)
{
	for (size_t index = m_quarantinedEntries.Num(); index > 0; --index)
	{
		if (m_quarantinedEntries[index - 1].m_fileId == uid)
		{
			m_quarantinedEntries.RemoveAt(index - 1);
		}
	}
	if (m_bPreserveStorageAfterLoadFailure)
	{
		outDiagnostic.clear();
		return true;
	}
	if (!m_cache.m_entries.ContainsKey(uid))
	{
		outDiagnostic.clear();
		return true;
	}

	ShaderCacheData candidate = m_cache;
	candidate.m_entries.Remove(uid);
	if (!CommitCandidateLocked(std::move(candidate), outDiagnostic, failurePoint))
	{
		return false;
	}
	std::string sweepDiagnostic;
	if (!SweepUnreferencedArtifactsLocked(m_committedCache, sweepDiagnostic))
	{
		m_bIsDirty = true;
		AppendDiagnostic(outDiagnostic, sweepDiagnostic);
		return false;
	}
	outDiagnostic.clear();
	return true;
}

void ShaderCache::Remove(const FileId& uid)
{
	SAILOR_PROFILE_FUNCTION();

	std::lock_guard<std::mutex> lock(m_cacheMutex);
	std::string diagnostic;
	if (!RemoveLocked(uid, Workspace::EWorkspaceCacheAtomicWriteFailurePoint::None, diagnostic))
	{
		m_lastSaveDiagnostic = std::move(diagnostic);
		SAILOR_LOG_ERROR("Shader cache remove failed: %s", m_lastSaveDiagnostic.c_str());
	}
}

void ShaderCache::Invalidate(const FileId& uid)
{
	SAILOR_PROFILE_FUNCTION();

	std::lock_guard<std::mutex> lock(m_cacheMutex);
	bool bInvalidated = false;
	if (m_cache.m_entries.ContainsKey(uid))
	{
		for (ShaderCacheData::Entry& entry : m_cache.m_entries[uid])
		{
			// Keep the last durable generation as a fallback while forcing an exact
			// dependency comparison to reject it until a successful replacement exists.
			entry.m_timestamp = 0;
			entry.m_sourceFingerprint = 0;
			bInvalidated = true;
		}
	}
	for (QuarantinedEntry& entry : m_quarantinedEntries)
	{
		if (entry.m_fileId == uid)
		{
			entry.m_timestamp = 0;
			entry.m_sourceFingerprint = 0;
			bInvalidated = true;
		}
	}
	if (!bInvalidated)
	{
		return;
	}

	m_bIsDirty = true;
	if (!SaveCacheLocked(false))
	{
		SAILOR_LOG_ERROR("Shader cache invalidation could not be persisted for %s: %s",
			uid.ToString().c_str(),
			m_lastSaveDiagnostic.c_str());
	}
}

bool ShaderCache::ClearExpiredLocked(Workspace::EWorkspaceCacheAtomicWriteFailurePoint failurePoint,
	std::string& outDiagnostic)
{
	if (m_bPreserveStorageAfterLoadFailure)
	{
		outDiagnostic.clear();
		return true;
	}

	TVector<ShaderCacheData::Entry> expired;
	const ShaderCacheData inspectedCache = m_cache;
	for (const auto& fileEntries : inspectedCache.m_entries)
	{
		for (const ShaderCacheData::Entry& entry : *fileEntries.m_second)
		{
			if (IsExpiredLocked(entry.m_fileId, entry.m_permutation))
			{
				if (m_bPreserveStorageAfterLoadFailure)
				{
					outDiagnostic.clear();
					return true;
				}
				expired.Add(entry);
			}
		}
	}
	ShaderCacheData candidate = m_cache;
	for (const ShaderCacheData::Entry& entry : expired)
	{
		auto mapEntry = candidate.m_entries.Find(entry.m_fileId);
		if (mapEntry == candidate.m_entries.end())
		{
			continue;
		}
		auto& entries = candidate.m_entries[entry.m_fileId];
		entries.Remove(entry);
		if (entries.Num() == 0)
		{
			candidate.m_entries.Remove(entry.m_fileId);
		}
	}

	if (m_bIsDirty || expired.Num() != 0 || !m_bHasCommittedSnapshot)
	{
		if (!CommitCandidateLocked(std::move(candidate), outDiagnostic, failurePoint))
		{
			return false;
		}
	}
	if (!SweepUnreferencedArtifactsLocked(m_committedCache, outDiagnostic))
	{
		m_bIsDirty = true;
		return false;
	}
	outDiagnostic.clear();
	return true;
}

void ShaderCache::ClearExpired()
{
	SAILOR_PROFILE_FUNCTION();

	std::lock_guard<std::mutex> lock(m_cacheMutex);
	if (m_bPreserveStorageAfterLoadFailure)
	{
		SAILOR_LOG("Shader cache cleanup skipped during read-only I/O quarantine; disk storage is preserved.");
		return;
	}
	std::string diagnostic;
	if (!ClearExpiredLocked(Workspace::EWorkspaceCacheAtomicWriteFailurePoint::None, diagnostic))
	{
		m_lastSaveDiagnostic = std::move(diagnostic);
		SAILOR_LOG_ERROR("Shader cache cleanup failed: %s", m_lastSaveDiagnostic.c_str());
	}
}

void ShaderCache::ClearAll()
{
	std::lock_guard<std::mutex> lock(m_cacheMutex);
	m_cache.m_entries.Clear();
	m_committedCache.m_entries.Clear();
	m_quarantinedEntries.Clear();
	m_bPreserveStorageAfterLoadFailure = false;
	m_bHasCommittedSnapshot = false;
	m_bIsDirty = true;
	std::string clearDiagnostic;
	const bool bCleared = ClearOwnedCacheFilesLocked(clearDiagnostic);
	std::string writeDiagnostic;
	const bool bEnvelopeWritten = WriteCacheLocked(writeDiagnostic);
	if (bCleared && bEnvelopeWritten)
	{
		m_bIsDirty = false;
		m_committedCache = m_cache;
		m_bHasCommittedSnapshot = true;
		m_lastSaveDiagnostic.clear();
	}
	else
	{
		m_bIsDirty = true;
		m_lastSaveDiagnostic.clear();
		AppendDiagnostic(m_lastSaveDiagnostic, clearDiagnostic);
		AppendDiagnostic(m_lastSaveDiagnostic, writeDiagnostic);
		SAILOR_LOG_ERROR("Shader cache clear failed: %s", m_lastSaveDiagnostic.c_str());
	}
}

Workspace::WorkspaceCacheLoadResult ShaderCache::GetLastLoadResult() const
{
	std::lock_guard<std::mutex> lock(m_cacheMutex);
	return m_lastLoadResult;
}

std::string ShaderCache::GetLastSaveDiagnostic() const
{
	std::lock_guard<std::mutex> lock(m_cacheMutex);
	return m_lastSaveDiagnostic;
}

bool ShaderCache::IsDirty() const
{
	std::lock_guard<std::mutex> lock(m_cacheMutex);
	return m_bIsDirty;
}

bool ShaderCache::GetTimeStamp(const FileId& uid, time_t& outTimestamp) const
{
	ShaderSourceState sourceState;
	std::string diagnostic;
	if (!CaptureSourceState(uid, sourceState, diagnostic))
	{
		return false;
	}
	outTimestamp = sourceState.m_timestamp;
	return true;
}
