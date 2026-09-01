#pragma once

namespace Sailor::EditorRuntime
{
	void ResetForAppLifecycle();
	bool ApplyPendingEditorViewportOnEngineThread();
	void DrainEditorRemoteViewportInputOnEngineThread();
	void UpdateRuntimeGIWorkAllowanceOnEngineThread();
	bool HasAppliedEditorRenderArea();
	void PumpEditorRemoteViewportsOnEngineThread();
}
