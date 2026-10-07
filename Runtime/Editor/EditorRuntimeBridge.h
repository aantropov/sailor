#pragma once
#include "Core/Defines.h"

namespace Sailor::EditorRemote { struct MacRendererFrameSource; }

namespace Sailor::EditorRuntime
{
	// Main thread: retains the last completed renderer readback without copying pixels.
	SAILOR_API bool TryAcquireEditorReadbackFrameSource(EditorRemote::MacRendererFrameSource& outSource);
	void ResetForAppLifecycle();
	SAILOR_API bool ApplyPendingEditorViewportOnEngineThread();
	SAILOR_API void DrainEditorRemoteViewportInputOnEngineThread();
	void UpdateRuntimeGIWorkAllowanceOnEngineThread();
	bool HasAppliedEditorRenderArea();
	void PumpEditorRemoteViewportsOnEngineThread();
}
