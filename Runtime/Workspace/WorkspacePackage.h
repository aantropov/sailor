#pragma once

#include "Platform/FileSystem.h"
#include "Workspace/WorkspaceContext.h"

namespace Sailor::Workspace
{
	// Empty for an authoring workspace; packaged games use per-package user storage.
	SAILOR_SHARED_API bool ResolvePackageCache(const std::filesystem::path& root,
		std::filesystem::path& outCache, std::string& outDiagnostic);
	SAILOR_SHARED_API bool WriteInitialCacheArchive(const WorkspaceContext& context,
		const std::filesystem::path& destination, FileSystem::EArchiveCompression compression,
		std::string& outDiagnostic);

	// GamePackage.json is optional for a native workspace. A package's explicit
	// mount list is loaded before the asset registry or any importer is created.
	SAILOR_SHARED_API bool MountPackageContent(const WorkspaceContext& context,
		TVector<FileSystem::ArchiveMountPtr>& outMounts, std::string& outDiagnostic);
}
