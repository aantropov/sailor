#pragma once
#include "Core/Defines.h"

namespace Sailor::EditorRemote { struct MacRendererFrameSource; }

namespace Sailor::EditorRuntime
{
	// Main thread: copies only the last completed renderer readback.
	SAILOR_API bool TryAcquireEditorReadbackFrameSource(EditorRemote::MacRendererFrameSource& outSource);
	void ResetForAppLifecycle();
	bool ApplyPendingEditorViewportOnEngineThread();
	void DrainEditorRemoteViewportInputOnEngineThread();
	void UpdateRuntimeGIWorkAllowanceOnEngineThread();
	bool HasAppliedEditorRenderArea();
	void PumpEditorRemoteViewportsOnEngineThread();
}
