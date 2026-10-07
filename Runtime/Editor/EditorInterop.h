#pragma once

#include "Core/Defines.h"

#include <cstdint>

namespace Sailor::EditorRuntime
{
	SAILOR_API uint32_t PullEditorMessages(char** messages, uint32_t num);
	SAILOR_API uint32_t SerializeEngineTypes(char** yamlNode);
	SAILOR_API uint32_t SerializeEditorTypes(char** yamlNode);
	SAILOR_API uint32_t SerializeWorkspaceCacheIdentity(char** yamlNode);
	SAILOR_API bool PreviewEditorAudioAsset(const char* strFileId);
	SAILOR_API bool RequestModelFingerprint(const char* strFileId);
	SAILOR_API uint32_t GetModelFingerprintStatus(const char* strFileId);
}
