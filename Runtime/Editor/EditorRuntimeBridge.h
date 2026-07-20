#pragma once

namespace Sailor::EditorRuntime
{
	void ResetForAppLifecycle();
	bool ApplyPendingEditorViewportOnEngineThread();
	bool HasAppliedEditorRenderArea();
	void PumpEditorRemoteViewportsOnEngineThread();
}
