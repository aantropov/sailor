#pragma once

namespace Sailor::EditorRuntime
{
	bool ApplyPendingEditorViewportOnEngineThread();
	bool HasAppliedEditorRenderArea();
	void PumpEditorRemoteViewportsOnEngineThread();
}
