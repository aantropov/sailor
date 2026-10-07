#pragma once
#include "Core/Defines.h"
#include <cstdint>
#include <string>
#include <string_view>

namespace Sailor::EditorRemote { struct MacRendererFrameSource; }

namespace Sailor::EditorRuntime
{
	SAILOR_API void SetEditorRenderTargetSize(uint32_t width, uint32_t height);
	// True means applied. Busy/resize-deferred updates return false and retain
	// the latest request for the viewport pump; GetState reports actual state.
	SAILOR_API bool UpsertEditorRemoteViewport(uint64_t viewportId, uint32_t windowPosX, uint32_t windowPosY, uint32_t width, uint32_t height, bool bVisible, bool bFocused);
	SAILOR_API bool DestroyEditorRemoteViewport(uint64_t viewportId);
	SAILOR_API uint32_t GetEditorRemoteViewportState(uint64_t viewportId);
	SAILOR_API uint32_t GetEditorRemoteViewportDiagnostics(uint64_t viewportId, char** diagnostics);
	SAILOR_API bool CaptureEditorRemoteViewportFrameEvidence(uint64_t viewportId, std::string& outDiagnostic);
	SAILOR_API bool RetryEditorRemoteViewport(uint64_t viewportId);
	SAILOR_API bool SetEditorRemoteViewportMacHostHandle(uint64_t viewportId, uint32_t hostHandleKind, uint64_t hostHandleValue);
	SAILOR_API bool SetEditorRemoteViewportWindowsHost(uint64_t viewportId, void* swapChainPanelInspectable, float compositionScale);
	SAILOR_API bool SendEditorRemoteViewportInput(uint64_t viewportId, uint32_t kind, float pointerX, float pointerY, float wheelDeltaX, float wheelDeltaY, uint32_t keyCode, uint32_t button, uint32_t modifiers, bool bPressed, bool bFocused, bool bCaptured, std::string_view text = {});

	// Main thread: retains the last completed renderer readback without copying pixels.
	SAILOR_API bool TryAcquireEditorReadbackFrameSource(EditorRemote::MacRendererFrameSource& outSource);
	void ResetForAppLifecycle();
	SAILOR_API bool ApplyPendingEditorViewportOnEngineThread();
	SAILOR_API void DrainEditorRemoteViewportInputOnEngineThread();
	void UpdateRuntimeGIWorkAllowanceOnEngineThread();
	bool HasAppliedEditorRenderArea();
	void PumpEditorRemoteViewportsOnEngineThread();
}
