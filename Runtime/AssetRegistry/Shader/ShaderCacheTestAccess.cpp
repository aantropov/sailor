#include "AssetRegistry/Shader/ShaderCache.h"

#include <yaml-cpp/yaml.h>

using namespace Sailor;

#if defined(SAILOR_SHADER_CACHE_TEST_HOOKS)
bool ShaderCacheTestAccess::Configure(ShaderCache& cache, const std::filesystem::path& cacheRoot)
{
	std::error_code error;
	std::filesystem::create_directories(cacheRoot, error);
	if (error)
	{
		return false;
	}

	const std::filesystem::path canonicalRoot = std::filesystem::weakly_canonical(cacheRoot, error);
	if (error)
	{
		return false;
	}

	std::lock_guard<std::mutex> lock(cache.m_cacheMutex);
	cache.m_cacheRoot = canonicalRoot;
	cache.m_identityOverride = Workspace::MakeWorkspaceCacheIdentity(ShaderCache::CacheKind,
		ShaderCache::GetCacheProducerIdentity(),
		ShaderCache::PayloadVersion,
		"shader-cache-tests",
		canonicalRoot.parent_path());
	std::string diagnostic;
	if (!cache.EnsureOwnedDirectoriesLocked(diagnostic))
	{
		return false;
	}

	return true;
}

std::string ShaderCacheTestAccess::GetGeneration(const ShaderCache& cache, const FileId& uid, uint32_t permutation)
{
	std::lock_guard<std::mutex> lock(cache.m_cacheMutex);
	if (!cache.m_cache.m_entries.ContainsKey(uid))
	{
		return {};
	}
	const auto& entries = cache.m_cache.m_entries[uid];
	const size_t index = entries.FindIf(
		[permutation](const ShaderCache::ShaderCacheData::Entry& entry) { return entry.m_permutation == permutation; });
	return index == static_cast<size_t>(-1) ? std::string() : entries[index].m_generation;
}

std::filesystem::path ShaderCacheTestAccess::GetArtifactPath(const ShaderCache& cache,
	const FileId& uid,
	uint32_t permutation,
	const char* stage,
	bool bIsDebug)
{
	std::lock_guard<std::mutex> lock(cache.m_cacheMutex);
	if (!cache.m_cache.m_entries.ContainsKey(uid))
	{
		return {};
	}
	const auto& entries = cache.m_cache.m_entries[uid];
	const size_t index = entries.FindIf(
		[permutation](const ShaderCache::ShaderCacheData::Entry& entry) { return entry.m_permutation == permutation; });
	return index == static_cast<size_t>(-1) ? std::filesystem::path()
											: cache.GetArtifactPathLocked(entries[index], stage, bIsDebug);
}

std::filesystem::path ShaderCacheTestAccess::GetCachePath(const ShaderCache& cache)
{
	std::lock_guard<std::mutex> lock(cache.m_cacheMutex);
	return cache.GetCacheFilepathLocked();
}

bool ShaderCacheTestAccess::PublishWithArtifactFailure(ShaderCache& cache,
	const FileId& uid,
	uint32_t permutation,
	const TVector<uint32_t>& vertex,
	const TVector<uint32_t>& fragment,
	const TVector<uint32_t>& compute,
	const TVector<uint32_t>& debugVertex,
	const TVector<uint32_t>& debugFragment,
	const TVector<uint32_t>& debugCompute,
	int32_t failArtifactIndex,
	std::string& outDiagnostic)
{
	ShaderSourceState sourceState;
	if (!cache.CaptureSourceState(uid, sourceState, outDiagnostic))
	{
		return false;
	}

	std::lock_guard<std::mutex> lock(cache.m_cacheMutex);
	return cache.CacheCompleteSpirvLocked(uid,
		permutation,
		vertex,
		fragment,
		compute,
		debugVertex,
		debugFragment,
		debugCompute,
		sourceState.m_timestamp,
		sourceState.m_fingerprint,
		failArtifactIndex,
		outDiagnostic);
}

bool ShaderCacheTestAccess::RemoveWithEnvelopeFailure(ShaderCache& cache, const FileId& uid, std::string& outDiagnostic)
{
	std::lock_guard<std::mutex> lock(cache.m_cacheMutex);
	return cache.RemoveLocked(uid, Workspace::EWorkspaceCacheAtomicWriteFailurePoint::BeforeReplace, outDiagnostic);
}

bool ShaderCacheTestAccess::ClearExpiredWithEnvelopeFailure(ShaderCache& cache, std::string& outDiagnostic)
{
	std::lock_guard<std::mutex> lock(cache.m_cacheMutex);
	return cache.ClearExpiredLocked(Workspace::EWorkspaceCacheAtomicWriteFailurePoint::BeforeReplace, outDiagnostic);
}

void ShaderCacheTestAccess::FailNextSaveBeforeReplace(ShaderCache& cache)
{
	std::lock_guard<std::mutex> lock(cache.m_cacheMutex);
	cache.m_nextSaveFailureForTests = Workspace::EWorkspaceCacheAtomicWriteFailurePoint::BeforeReplace;
}

void ShaderCacheTestAccess::SetArtifactReadIoFailure(ShaderCache& cache, bool bEnabled)
{
	std::lock_guard<std::mutex> lock(cache.m_cacheMutex);
	cache.m_bArtifactReadIoFailureForTests = bEnabled;
}

void ShaderCacheTestAccess::FailNextArtifactSweep(ShaderCache& cache)
{
	std::lock_guard<std::mutex> lock(cache.m_cacheMutex);
	cache.m_bArtifactSweepFailureForTests = true;
}

std::string ShaderCacheTestAccess::PayloadWithUnknownFields(const ShaderCache& cache)
{
	std::lock_guard<std::mutex> lock(cache.m_cacheMutex);
	YAML::Node payload = YAML::Load(ShaderCache::SerializeShaderCachePayload(cache.m_cache));
	payload["runtimeMetadata"] = "ignored";
	payload["shaderCache"]["runtimeMetadata"] = "ignored";
	return YAML::Dump(payload);
}

std::string ShaderCacheTestAccess::PayloadWithMissingDebug(const ShaderCache& cache)
{
	std::lock_guard<std::mutex> lock(cache.m_cacheMutex);
	ShaderCache::ShaderCacheData candidate = cache.m_cache;
	for (auto fileEntries : candidate.m_entries)
	{
		for (ShaderCache::ShaderCacheData::Entry& entry : *fileEntries.m_second)
		{
			entry.m_debug = {};
		}
	}
	return ShaderCache::SerializeShaderCachePayload(candidate);
}

std::string ShaderCacheTestAccess::PayloadWithMismatchedDebugTopology(const ShaderCache& cache)
{
	std::lock_guard<std::mutex> lock(cache.m_cacheMutex);
	ShaderCache::ShaderCacheData candidate = cache.m_cache;
	for (auto fileEntries : candidate.m_entries)
	{
		for (ShaderCache::ShaderCacheData::Entry& entry : *fileEntries.m_second)
		{
			entry.m_debug = {};
			entry.m_debug.m_compute = entry.m_regular.m_vertex;
		}
	}
	return ShaderCache::SerializeShaderCachePayload(candidate);
}

bool ShaderCacheTestAccess::ParsePayload(const std::string& payload, std::string& outDiagnostic)
{
	ShaderCache::ShaderCacheData candidate;
	return ShaderCache::TryDeserializeShaderCachePayload(payload, candidate, outDiagnostic);
}

bool ShaderCacheTestAccess::IsQuarantined(const ShaderCache& cache)
{
	std::lock_guard<std::mutex> lock(cache.m_cacheMutex);
	return cache.m_bPreserveStorageAfterLoadFailure;
}
#endif
