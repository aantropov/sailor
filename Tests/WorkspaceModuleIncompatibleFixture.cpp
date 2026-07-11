#include "Workspace/WorkspaceModuleApi.h"

namespace
{
	constexpr char ModuleName[] = "IncompatibleFixture";
	constexpr char IncompatibleAbi[] = "incompatible-workspace-abi";

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
	static const Sailor::Workspace::WorkspaceModuleApiV1 api
	{
		static_cast<uint32_t>(sizeof(Sailor::Workspace::WorkspaceModuleApiV1)),
		Sailor::Workspace::WorkspaceModuleApiVersion,
		ModuleName,
		static_cast<uint64_t>(sizeof(ModuleName) - 1),
		IncompatibleAbi,
		static_cast<uint64_t>(sizeof(IncompatibleAbi) - 1),
		&GetMetadata,
		&RegisterTypes
	};

	return &api;
}
