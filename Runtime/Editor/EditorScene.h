#pragma once

#include "Core/Defines.h"

#include <cstdint>

namespace Sailor::EditorRuntime
{
	SAILOR_API uint32_t SerializeCurrentWorld(char** yamlNode);
	SAILOR_API bool LoadEditorWorld(const char* strFileId);
	SAILOR_API bool CreateEditorWorld();
	SAILOR_API bool SetEditorSimulationEnabled(bool bEnabled);
	SAILOR_API bool IsEditorSimulationEnabled();
}
