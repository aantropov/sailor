#pragma once
#include "Containers/Containers.h"

#include <cstdint>
#include <filesystem>
#include <map>
#include <string>
#include <vector>

namespace Sailor
{
	enum class EAssetMountKind : uint8_t
	{
		Engine,
		Workspace
	};

	enum class EAssetMountDiagnosticCode : uint8_t
	{
		InvalidMountRoot,
		DuplicateMountRoot,
		OverlappingMountRoot,
		PhysicalEscapeSkipped,
		DirectoryCycleSkipped,
		PathInspectionFailed,
		InvalidCandidate,
		VirtualPathCollision,
		FileIdCollision
	};

	struct AssetMountDescriptor final
	{
		std::filesystem::path m_root;
		EAssetMountKind m_kind = EAssetMountKind::Engine;
		int32_t m_priority = 0;
		bool m_bWritable = false;
	};

	struct AssetMountDiscoveredFile final
	{
		AssetMountDescriptor m_mount;
		std::filesystem::path m_physicalPath;
		std::string m_virtualPath;
	};

	struct AssetMountCandidate final
	{
		AssetMountDescriptor m_mount;
		std::filesystem::path m_physicalPath;
		std::string m_virtualPath;
		std::string m_fileId;
	};

	struct AssetMountDiagnostic final
	{
		EAssetMountDiagnosticCode m_code = EAssetMountDiagnosticCode::PathInspectionFailed;
		std::string m_key;
		std::filesystem::path m_winnerPath;
		std::filesystem::path m_conflictingPath;
		EAssetMountKind m_winnerKind = EAssetMountKind::Engine;
		EAssetMountKind m_conflictingKind = EAssetMountKind::Engine;
		std::string m_winnerFileId;
		std::string m_conflictingFileId;
		std::string m_message;
	};

	struct AssetMountDiscoveryResult final
	{
		TVector<AssetMountDescriptor> m_mounts;
		TVector<AssetMountDiscoveredFile> m_files;
		TVector<AssetMountDiagnostic> m_diagnostics;

		bool HasFatalErrors() const noexcept;
	};

	class AssetMountResolutionResult;
	AssetMountResolutionResult ResolveAssetMountCandidates(
		TVector<AssetMountCandidate> candidates);

	class AssetMountResolutionResult final
	{
	public:
		const TVector<AssetMountCandidate>& GetCandidates() const noexcept { return m_candidates; }
		const TVector<AssetMountDiagnostic>& GetDiagnostics() const noexcept { return m_diagnostics; }

		const AssetMountCandidate* FindByVirtualPath(const std::string& virtualPath) const noexcept;
		const AssetMountCandidate* FindByFileId(const std::string& fileId) const noexcept;

	private:
		TVector<AssetMountCandidate> m_candidates;
		TMap<std::string, size_t> m_virtualPathWinners;
		TMap<std::string, size_t> m_fileIdWinners;
		TVector<AssetMountDiagnostic> m_diagnostics;

		friend AssetMountResolutionResult ResolveAssetMountCandidates(
			TVector<AssetMountCandidate> candidates);
	};

	AssetMountDiscoveryResult DiscoverAssetMountFiles(
		const TVector<AssetMountDescriptor>& mounts);
}
