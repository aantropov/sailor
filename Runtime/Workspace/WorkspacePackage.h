#pragma once

#include "Platform/FileSystem.h"
#include "Workspace/WorkspaceContext.h"

namespace Sailor::Workspace
{
	// GamePackage.json is optional for a native workspace. A package's explicit
	// mount list is loaded before the asset registry or any importer is created.
	SAILOR_SHARED_API bool MountPackageContent(const WorkspaceContext& context,
		TVector<FileSystem::ArchiveMountPtr>& outMounts, std::string& outDiagnostic);
}
