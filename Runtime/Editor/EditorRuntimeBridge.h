#pragma once

namespace Sailor::EditorRuntime
{
	void ResetForAppLifecycle();
	bool ApplyPendingEditorViewportOnEngineThread();
	void DrainEditorRemoteViewportInputOnEngineThread();
	bool HasAppliedEditorRenderArea();
	void PumpEditorRemoteViewportsOnEngineThread();
}
