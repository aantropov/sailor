#include "Workspace/WorkspaceModuleApi.h"

extern "C" SAILOR_WORKSPACE_MODULE_EXPORT uint32_t SAILOR_WORKSPACE_CALL
	SailorWorkspaceMissingEntryFixtureSentinel() noexcept
{
	return 1;
}
