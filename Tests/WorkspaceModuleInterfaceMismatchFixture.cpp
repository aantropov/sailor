#include "Workspace/WorkspaceModuleApi.h"

namespace
{
	constexpr char ModuleName[] = "InterfaceMismatchFixture";
	uint32_t g_numCallbacks = 0;

	uint32_t SAILOR_WORKSPACE_CALL RegisterTypes(const Sailor::Workspace::WorkspaceHostApiV1*) noexcept
	{
		++g_numCallbacks;
		return static_cast<uint32_t>(Sailor::Workspace::EWorkspaceModuleResult::RegistrationFailed);
	}
}

extern "C" SAILOR_WORKSPACE_MODULE_EXPORT uint32_t SAILOR_WORKSPACE_CALL
	SailorWorkspaceInterfaceFixtureCallbackCount() noexcept
{
	return g_numCallbacks;
}

extern "C" SAILOR_WORKSPACE_MODULE_EXPORT const Sailor::Workspace::WorkspaceModuleApiV1* SAILOR_WORKSPACE_CALL
	SailorGetWorkspaceModuleApiV1() noexcept
{
	static const Sailor::Workspace::WorkspaceModuleApiV1 api
	{
		static_cast<uint32_t>(sizeof(Sailor::Workspace::WorkspaceModuleApiV1)),
		Sailor::Workspace::WorkspaceModuleApiVersion,
		ModuleName,
		sizeof(ModuleName) - 1,
		Sailor::Workspace::GetWorkspaceModuleAbiTagV1(),
		Sailor::Workspace::GetWorkspaceModuleAbiTagV1Length(),
		&RegisterTypes
	};
	return &api;
}
