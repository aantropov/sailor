#include "Workspace/WorkspaceModuleApi.h"

#include <string>

namespace
{
	constexpr char ModuleName[] = "IncompatibleFixture";

	std::string MakeStaleAbiTag()
	{
		std::string abiTag = Sailor::Workspace::GetWorkspaceModuleAbiTagV1();
		const std::string currentRevision = "sailor-workspace-abi-" +
			std::to_string(Sailor::Workspace::WorkspaceModuleAbiRevision);
		const std::string staleRevision = "sailor-workspace-abi-" +
			std::to_string(Sailor::Workspace::WorkspaceModuleAbiRevision - 1);
		const size_t revisionPosition = abiTag.find(currentRevision);
		if (revisionPosition != std::string::npos)
		{
			abiTag.replace(revisionPosition, currentRevision.size(), staleRevision);
		}
		return abiTag;
	}

	uint32_t SAILOR_WORKSPACE_CALL GetMetadata(char*, uint64_t, uint64_t* outPayloadSize) noexcept
	{
		if (outPayloadSize != nullptr)
		{
			*outPayloadSize = 0;
		}
		return static_cast<uint32_t>(Sailor::Workspace::EWorkspaceModuleResult::SerializationFailed);
	}

	uint32_t SAILOR_WORKSPACE_CALL RegisterTypes(
		const Sailor::Workspace::WorkspaceHostApiV1*) noexcept
	{
		return static_cast<uint32_t>(Sailor::Workspace::EWorkspaceModuleResult::RegistrationFailed);
	}
}

extern "C" SAILOR_WORKSPACE_MODULE_EXPORT const Sailor::Workspace::WorkspaceModuleApiV1* SAILOR_WORKSPACE_CALL
	SailorGetWorkspaceModuleApiV1() noexcept
{
	static const std::string staleAbiTag = MakeStaleAbiTag();
	static const Sailor::Workspace::WorkspaceModuleApiV1 api
	{
		static_cast<uint32_t>(sizeof(Sailor::Workspace::WorkspaceModuleApiV1)),
		Sailor::Workspace::WorkspaceModuleApiVersion,
		ModuleName,
		static_cast<uint64_t>(sizeof(ModuleName) - 1),
		staleAbiTag.data(),
		static_cast<uint64_t>(staleAbiTag.size()),
		&GetMetadata,
		&RegisterTypes
	};

	return &api;
}
