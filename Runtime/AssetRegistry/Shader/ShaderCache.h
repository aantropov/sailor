#pragma once
#include "Containers/Containers.h"

#include "AssetRegistry/FileId.h"
#include "Containers/Map.h"
#include "Containers/Vector.h"
#include "Core/YamlSerializable.h"
#include "Sailor.h"
#include "Workspace/WorkspaceCacheContract.h"

#include <cstdint>
#include <ctime>
#include <filesystem>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace Sailor
{
	class ShaderCache
	{
	public:
		SAILOR_API ShaderCache();
		SAILOR_API ~ShaderCache();

		struct ArtifactMetadata final : IYamlSerializable
		{
			SAILOR_API ArtifactMetadata();
			SAILOR_API ~ArtifactMetadata();

			uint64_t m_byteLength = 0;
			uint64_t m_checksum = 0;

			bool IsPresent() const noexcept { return m_byteLength != 0; }

			SAILOR_API virtual YAML::Node Serialize() const override;
			SAILOR_API virtual void Deserialize(const YAML::Node& inData) override;
		};

		SAILOR_API static std::string GetShaderCacheFilepath();
		SAILOR_API static std::string GetPrecompiledShadersFolder();
		SAILOR_API static std::string GetCompiledShadersFolder();
		SAILOR_API static std::string GetCompiledShadersWithDebugFolder();

		static constexpr const char* FragmentShaderTag = "FRAGMENT";
		static constexpr const char* VertexShaderTag = "VERTEX";
		static constexpr const char* ComputeShaderTag = "COMPUTE";

		static constexpr const char* CompiledShaderFileExtension = "spirv";
		static constexpr const char* PrecompiledShaderFileExtension = "glsl";

		SAILOR_API void Initialize();
		SAILOR_API void Shutdown();

		SAILOR_API bool CachePrecompiledGlsl(
			const FileId& uid,
			uint32_t permutation,
			const std::string& vertexGlsl,
			const std::string& fragmentGlsl,
			const std::string& computeGlsl);
		SAILOR_API bool CacheSpirv_ThreadSafe(
			const FileId& uid,
			uint32_t permutation,
			const TVector<uint32_t>& vertexSpirv,
			const TVector<uint32_t>& fragmentSpirv,
			const TVector<uint32_t>& computeSpirv,
			const TVector<uint32_t>& debugVertexSpirv,
			const TVector<uint32_t>& debugFragmentSpirv,
			const TVector<uint32_t>& debugComputeSpirv);

		SAILOR_API bool GetSpirvCode(
			const FileId& uid,
			uint32_t permutation,
			TVector<uint32_t>& vertexSpirv,
			TVector<uint32_t>& fragmentSpirv,
			TVector<uint32_t>& computeSpirv,
			bool bIsDebug = false);

		SAILOR_API void Remove(const FileId& uid);

		SAILOR_API bool Contains(const FileId& uid) const;
		SAILOR_API bool IsExpired(const FileId& uid, uint32_t permutation);

		SAILOR_API void LoadCache();
		SAILOR_API void SaveCache(bool bForcely = false);

		SAILOR_API void ClearAll();
		SAILOR_API void ClearExpired();

		SAILOR_API Workspace::WorkspaceCacheLoadResult GetLastLoadResult() const;
		SAILOR_API std::string GetLastSaveDiagnostic() const;
		SAILOR_API bool IsDirty() const;

		SAILOR_API static uint64_t CalculateArtifactChecksum(const void* data, uint64_t size) noexcept;
		SAILOR_API static ArtifactMetadata DescribeArtifact(const void* data, uint64_t size) noexcept;
		SAILOR_API static bool ReadSpirvArtifact(
			const std::filesystem::path& path,
			const ArtifactMetadata& metadata,
			TVector<uint32_t>& outSpirv,
			std::string& outDiagnostic) noexcept;
		SAILOR_API static bool ValidateOwnedArtifactPath(
			const std::filesystem::path& cacheRoot,
			const std::filesystem::path& ownedDirectory,
			const std::filesystem::path& artifact,
			std::string& outDiagnostic) noexcept;

		SAILOR_API static std::filesystem::path GetPrecompiledShaderFilepath(
			const FileId& uid,
			int32_t permutation,
			const std::string& shaderKind);
		SAILOR_API static std::filesystem::path GetCachedShaderFilepath(
			const FileId& uid,
			int32_t permutation,
			const std::string& shaderKind);
		SAILOR_API static std::filesystem::path GetCachedShaderWithDebugFilepath(
			const FileId& uid,
			int32_t permutation,
			const std::string& shaderKind);

	protected:
		struct ArtifactSet final : IYamlSerializable
		{
			ArtifactMetadata m_vertex;
			ArtifactMetadata m_fragment;
			ArtifactMetadata m_compute;

			bool IsEmpty() const noexcept
			{
				return !m_vertex.IsPresent() && !m_fragment.IsPresent() && !m_compute.IsPresent();
			}

			SAILOR_API virtual YAML::Node Serialize() const override;
			SAILOR_API virtual void Deserialize(const YAML::Node& inData) override;
		};

		class ShaderCacheData final : IYamlSerializable
		{
		public:
			class Entry final : IYamlSerializable
			{
			public:
				FileId m_fileId{};
				std::time_t m_timestamp{};
				uint32_t m_permutation = 0;
				std::string m_generation;
				ArtifactSet m_regular;
				ArtifactSet m_debug;

				SAILOR_API bool operator==(const Entry& rhs) const
				{
					return m_fileId == rhs.m_fileId && m_permutation == rhs.m_permutation;
				}

				SAILOR_API virtual YAML::Node Serialize() const override;
				SAILOR_API virtual void Deserialize(const YAML::Node& inData) override;
			};

			TMap<FileId, TVector<Entry>> m_data;

			SAILOR_API virtual YAML::Node Serialize() const override;
			SAILOR_API virtual void Deserialize(const YAML::Node& inData) override;
		};

		SAILOR_API bool GetTimeStamp(const FileId& uid, time_t& outTimestamp) const;

	private:
		struct QuarantinedEntry final
		{
			FileId m_fileId{};
			std::time_t m_timestamp{};
			uint32_t m_permutation = 0;
			TVector<uint32_t> m_vertex;
			TVector<uint32_t> m_fragment;
			TVector<uint32_t> m_compute;
			TVector<uint32_t> m_debugVertex;
			TVector<uint32_t> m_debugFragment;
			TVector<uint32_t> m_debugCompute;
		};

		static std::string SerializeShaderCachePayload(const ShaderCacheData& cache);
		static bool TryDeserializeShaderCachePayload(
			const std::string& payload,
			ShaderCacheData& outData,
			std::string& outDiagnostic) noexcept;
		static bool ReadArtifactBytes(
			const std::filesystem::path& path,
			const ArtifactMetadata& metadata,
			TVector<uint8_t>& outBytes,
			std::string& outDiagnostic,
			bool& outIoFailure) noexcept;
		static bool ReadSpirvArtifactInternal(
			const std::filesystem::path& path,
			const ArtifactMetadata& metadata,
			TVector<uint32_t>& outSpirv,
			std::string& outDiagnostic,
			bool& outIoFailure) noexcept;
		static bool IsValidArtifactSet(const ArtifactSet& artifacts, bool bAllowEmpty) noexcept;
		static bool HasMatchingArtifactTopology(
			const ArtifactSet& regular,
			const ArtifactSet& debug) noexcept;
		static bool IsValidGeneration(const std::string& generation) noexcept;
		static bool DescribeArtifactSet(
			const TVector<uint32_t>& vertexSpirv,
			const TVector<uint32_t>& fragmentSpirv,
			const TVector<uint32_t>& computeSpirv,
			ArtifactSet& outMetadata,
			std::string& outDiagnostic) noexcept;

		Workspace::WorkspaceCacheIdentity GetExpectedIdentityLocked() const;
		std::filesystem::path GetCacheFilepathLocked() const;
		std::filesystem::path GetPrecompiledFolderLocked() const;
		std::filesystem::path GetCompiledFolderLocked() const;
		std::filesystem::path GetCompiledDebugFolderLocked() const;
		std::filesystem::path GetArtifactPathLocked(
			const ShaderCacheData::Entry& entry,
			const std::string& shaderKind,
			bool bIsDebug) const;

		bool WriteCacheDataLocked(
			const ShaderCacheData& cache,
			std::string& outDiagnostic,
			Workspace::EWorkspaceCacheAtomicWriteFailurePoint failurePoint =
				Workspace::EWorkspaceCacheAtomicWriteFailurePoint::None);
		bool WriteCacheLocked(std::string& outDiagnostic);
		bool SaveCacheLocked(
			bool bForcely,
			Workspace::EWorkspaceCacheAtomicWriteFailurePoint failurePoint =
				Workspace::EWorkspaceCacheAtomicWriteFailurePoint::None);
		bool CommitCandidateLocked(
			ShaderCacheData candidate,
			std::string& outDiagnostic,
			Workspace::EWorkspaceCacheAtomicWriteFailurePoint failurePoint =
				Workspace::EWorkspaceCacheAtomicWriteFailurePoint::None);
		void ResetInvalidCacheLocked(Workspace::WorkspaceCacheLoadResult loadResult);
		void EnterStorageQuarantineLocked(std::string diagnostic);
		bool ClearOwnedCacheFilesLocked(std::string& outDiagnostic);
		bool EnsureOwnedDirectoriesLocked(std::string& outDiagnostic);
		bool ValidateAllArtifactsLocked(
			const ShaderCacheData& candidate,
			std::string& outDiagnostic,
			bool& outIoFailure) const;
		bool ValidateArtifactSetLocked(
			const ShaderCacheData::Entry& entry,
			const ArtifactSet& artifacts,
			bool bIsDebug,
			std::string& outDiagnostic,
			bool& outIoFailure) const;
		bool ReadOwnedSpirvArtifactLocked(
			const std::filesystem::path& ownedDirectory,
			const std::filesystem::path& artifact,
			const ArtifactMetadata& metadata,
			TVector<uint32_t>& outSpirv,
			std::string& outDiagnostic,
			bool& outIoFailure) const;
		bool WriteSpirvSetLocked(
			const FileId& uid,
			uint32_t permutation,
			const std::string& generation,
			const TVector<uint32_t>& vertexSpirv,
			const TVector<uint32_t>& fragmentSpirv,
			const TVector<uint32_t>& computeSpirv,
			const ArtifactSet& metadata,
			bool bIsDebug,
			int32_t& artifactIndex,
			int32_t failArtifactIndex,
			std::string& outDiagnostic);
		bool CacheCompleteSpirvLocked(
			const FileId& uid,
			uint32_t permutation,
			const TVector<uint32_t>& vertexSpirv,
			const TVector<uint32_t>& fragmentSpirv,
			const TVector<uint32_t>& computeSpirv,
			const TVector<uint32_t>& debugVertexSpirv,
			const TVector<uint32_t>& debugFragmentSpirv,
			const TVector<uint32_t>& debugComputeSpirv,
			int32_t failArtifactIndex,
			std::string& outDiagnostic);
		bool GenerateUniqueGenerationLocked(
			const FileId& uid,
			uint32_t permutation,
			std::string& outGeneration,
			std::string& outDiagnostic) const;
		bool SweepUnreferencedArtifactsLocked(
			const ShaderCacheData& committedSnapshot,
			std::string& outDiagnostic);
		QuarantinedEntry* FindQuarantinedEntryLocked(const FileId& uid, uint32_t permutation);
		const QuarantinedEntry* FindQuarantinedEntryLocked(const FileId& uid, uint32_t permutation) const;
		bool RemoveLocked(
			const FileId& uid,
			Workspace::EWorkspaceCacheAtomicWriteFailurePoint failurePoint,
			std::string& outDiagnostic);
		bool ClearExpiredLocked(
			Workspace::EWorkspaceCacheAtomicWriteFailurePoint failurePoint,
			std::string& outDiagnostic);
		bool IsExpiredLocked(const FileId& uid, uint32_t permutation);

		mutable std::mutex m_cacheMutex;
		ShaderCacheData m_cache;
		ShaderCacheData m_committedCache;
		TVector<QuarantinedEntry> m_quarantinedEntries;
		std::filesystem::path m_cacheRoot;
		bool m_bIsDirty = false;
		bool m_bStorageReady = false;
		bool m_bPreserveStorageAfterLoadFailure = false;
		bool m_bHasCommittedSnapshot = false;
		const bool m_bSavePrecompiledGlsl = false;
		Workspace::WorkspaceCacheLoadResult m_lastLoadResult{};
		std::string m_lastSaveDiagnostic;
		[[maybe_unused]] std::optional<Workspace::WorkspaceCacheIdentity> m_identityOverride;
		[[maybe_unused]] std::optional<std::time_t> m_timestampOverrideForTests;
		[[maybe_unused]] Workspace::EWorkspaceCacheAtomicWriteFailurePoint m_nextSaveFailureForTests =
			Workspace::EWorkspaceCacheAtomicWriteFailurePoint::None;
		[[maybe_unused]] bool m_bArtifactReadIoFailureForTests = false;
		[[maybe_unused]] bool m_bArtifactSweepFailureForTests = false;

#if defined(SAILOR_SHADER_CACHE_TEST_HOOKS)
		friend class ShaderCacheTestAccess;
#endif
	};

#if defined(SAILOR_SHADER_CACHE_TEST_HOOKS)
	class ShaderCacheTestAccess final
	{
	public:
		SAILOR_API static bool Configure(
			ShaderCache& cache,
			const std::filesystem::path& cacheRoot,
			std::time_t timestamp = 100);
		SAILOR_API static std::string GetGeneration(
			const ShaderCache& cache,
			const FileId& uid,
			uint32_t permutation);
		SAILOR_API static std::filesystem::path GetArtifactPath(
			const ShaderCache& cache,
			const FileId& uid,
			uint32_t permutation,
			const char* stage,
			bool bIsDebug);
		SAILOR_API static std::filesystem::path GetCachePath(const ShaderCache& cache);
		SAILOR_API static bool PublishWithArtifactFailure(
			ShaderCache& cache,
			const FileId& uid,
			uint32_t permutation,
			const TVector<uint32_t>& vertex,
			const TVector<uint32_t>& fragment,
			const TVector<uint32_t>& compute,
			const TVector<uint32_t>& debugVertex,
			const TVector<uint32_t>& debugFragment,
			const TVector<uint32_t>& debugCompute,
			int32_t failArtifactIndex,
			std::string& outDiagnostic);
		SAILOR_API static bool RemoveWithEnvelopeFailure(
			ShaderCache& cache,
			const FileId& uid,
			std::string& outDiagnostic);
		SAILOR_API static bool ClearExpiredWithEnvelopeFailure(
			ShaderCache& cache,
			std::string& outDiagnostic);
		SAILOR_API static void FailNextSaveBeforeReplace(ShaderCache& cache);
		SAILOR_API static void SetArtifactReadIoFailure(ShaderCache& cache, bool bEnabled);
		SAILOR_API static void FailNextArtifactSweep(ShaderCache& cache);
		SAILOR_API static std::string PayloadWithMissingDebug(const ShaderCache& cache);
		SAILOR_API static std::string PayloadWithMismatchedDebugTopology(const ShaderCache& cache);
		SAILOR_API static bool ParsePayload(
			const std::string& payload,
			std::string& outDiagnostic);
		SAILOR_API static bool IsQuarantined(const ShaderCache& cache);
	};
#endif
}
