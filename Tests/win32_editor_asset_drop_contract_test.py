#!/usr/bin/env python3

import pathlib
import re
import sys


def extract_function(source: str, signature: str) -> str:
    signature_offset = source.find(signature)
    if signature_offset < 0:
        raise AssertionError(f"Missing function: {signature}")

    body_offset = source.find("{", signature_offset)
    if body_offset < 0:
        raise AssertionError(f"Missing function body: {signature}")

    depth = 0
    for offset in range(body_offset, len(source)):
        if source[offset] == "{":
            depth += 1
        elif source[offset] == "}":
            depth -= 1
            if depth == 0:
                return source[body_offset:offset + 1]

    raise AssertionError(f"Unterminated function body: {signature}")


def require(text: str, fragment: str, message: str) -> None:
    if fragment not in text:
        raise AssertionError(message)


def require_order(text: str, first: str, second: str, message: str) -> None:
    first_offset = text.find(first)
    second_offset = text.find(second)
    if first_offset < 0 or second_offset < 0 or first_offset >= second_offset:
        raise AssertionError(message)


def main() -> int:
    if len(sys.argv) != 2:
        raise SystemExit(
            "usage: win32_editor_asset_drop_contract_test.py <repository root>"
        )

    repository = pathlib.Path(sys.argv[1]).resolve()
    window_header = (
        repository / "Runtime" / "Platform" / "Win32" / "Window.h"
    ).read_text(encoding="utf-8")
    window_source = (
        repository / "Runtime" / "Platform" / "Win32" / "Window.cpp"
    ).read_text(encoding="utf-8")
    editor_source = (
        repository / "Runtime" / "Submodules" / "Editor.cpp"
    ).read_text(encoding="utf-8")
    controller_source = (
        repository / "Runtime" / "Editor" / "EditorViewportController.cpp"
    ).read_text(encoding="utf-8")
    managed_payload = (
        repository / "Editor" / "Scene" / "SceneViewportAssetDropPayload.cs"
    ).read_text(encoding="utf-8")
    managed_scene = (
        repository / "Editor" / "Views" / "SceneView.xaml.cs"
    ).read_text(encoding="utf-8")
    protocol_schema = (
        repository / "Protocol" / "editor_engine.proto"
    ).read_text(encoding="utf-8")
    protocol_bridge = (
        repository / "Lib" / "EditorEngineProtocol.cpp"
    ).read_text(encoding="utf-8")
    dll_surfaces = "\n".join(
        source.read_text(encoding="utf-8")
        for source in sorted((repository / "Lib").glob("*.cpp"))
    )

    prefix_pattern = re.compile(
        r'c_editorAssetDropPrefix\s*=\s*L"SailorEditor\.Asset:"'
    )
    if not prefix_pattern.search(window_source):
        raise AssertionError(
            "Win32 OLE drops must use the generic SailorEditor.Asset: prefix"
        )
    require(
        managed_payload,
        'public const string Prefix = "SailorEditor.Asset:";',
        "managed and native asset-drop payload prefixes must match",
    )
    require(
        window_source,
        "c_unbracedFileIdLength = 36",
        "native OLE drops must accept repository FileIds created on macOS/Linux",
    )
    require(
        window_source,
        "c_bracedFileIdLength = 38",
        "native OLE drops must accept repository FileIds created on Windows",
    )
    require(
        managed_payload,
        "public const int UnbracedFileIdLength = 36;",
        "managed payloads must accept repository FileIds created on macOS/Linux",
    )
    require(
        managed_payload,
        "public const int BracedFileIdLength = 38;",
        "managed payloads must accept repository FileIds created on Windows",
    )
    native_file_id_validation = extract_function(
        window_source,
        "bool IsSerializedFileId(",
    )
    require(
        native_file_id_validation,
        "fileId.size() == c_bracedFileIdLength",
        "native payload validation must recognize braced FileIds",
    )
    require(
        native_file_id_validation,
        "fileId.size() != c_unbracedFileIdLength",
        "native payload validation must recognize unbraced FileIds",
    )
    if "SailorEditor.Model:" in window_source or "SailorEditor.Prefab:" in window_source:
        raise AssertionError(
            "the Win32 OLE target must not use type-specific asset prefixes"
        )

    require(
        window_header,
        "void QueueEditorViewportAssetDrop(",
        "Window must own the native asset-drop queue entrypoint",
    )
    require(
        window_header,
        "bool PullEditorViewportAssetDrop(",
        "Window must own the native asset-drop dequeue entrypoint",
    )
    require(
        window_header,
        "m_editorViewportAssetDropMutex",
        "Window asset-drop handoff must be synchronized",
    )
    require(
        window_header,
        "m_pendingEditorViewportAssetDrop",
        "Window must retain the pending native asset drop",
    )
    if re.search(
        r"SAILOR_(?:SHARED_)?API\s+(?:void|bool)\s+"
        r"(?:Queue|Pull)EditorViewportAssetDrop",
        window_header,
    ):
        raise AssertionError(
            "Window asset-drop queue methods must remain internal, not DLL exports"
        )

    for forbidden in (
        "QueueEditorViewportAssetDrop",
        "PullEditorViewportAssetDrop",
        "QueueEditorViewportToolShortcut",
        "PullEditorViewportToolShortcut",
        "SailorEditor.Asset:",
    ):
        if forbidden in dll_surfaces:
            raise AssertionError(
                "Win32 asset-drop handoff must not add a direct DLL/extern surface"
            )

    queue = extract_function(
        window_source,
        "void Window::QueueEditorViewportAssetDrop(",
    )
    pull = extract_function(
        window_source,
        "bool Window::PullEditorViewportAssetDrop(",
    )
    require(
        queue,
        "m_editorViewportAssetDropMutex",
        "queue writes must hold the Window asset-drop mutex",
    )
    require(
        queue,
        "m_pendingEditorViewportAssetDrop = EditorViewportAssetDrop",
        "queue writes must publish one complete asset-drop record",
    )
    require(
        pull,
        "m_editorViewportAssetDropMutex",
        "queue reads must hold the Window asset-drop mutex",
    )
    require(
        pull,
        "m_pendingEditorViewportAssetDrop.reset();",
        "pulling an asset drop must consume the pending record",
    )

    tick_viewport_tools = extract_function(
        editor_source,
        "void Editor::TickViewportTools()",
    )
    require_order(
        tick_viewport_tools,
        "PullEditorViewportAssetDrop(",
        "QueueAssetDropEvent(",
        "the engine worker must pull Window drops before publishing viewport events",
    )

    require(
        window_header,
        "void QueueEditorViewportToolShortcut(uint32_t keyCode);",
        "Window must own the native viewport shortcut queue entrypoint",
    )
    require(
        window_header,
        "bool PullEditorViewportToolShortcut(uint32_t& outKeyCode);",
        "Window must own the native viewport shortcut dequeue entrypoint",
    )
    if re.search(
        r"SAILOR_(?:SHARED_)?API\s+(?:void|bool)\s+"
        r"(?:Queue|Pull)EditorViewportToolShortcut",
        window_header,
    ):
        raise AssertionError(
            "Window viewport shortcut queue methods must remain internal"
        )

    shortcut_queue = extract_function(
        window_source,
        "void Window::QueueEditorViewportToolShortcut(",
    )
    shortcut_pull = extract_function(
        window_source,
        "bool Window::PullEditorViewportToolShortcut(",
    )
    require(
        shortcut_queue,
        "m_editorViewportToolShortcutMutex",
        "viewport shortcut queue writes must be synchronized",
    )
    require(
        shortcut_pull,
        "m_editorViewportToolShortcutMutex",
        "viewport shortcut queue reads must be synchronized",
    )
    require(
        shortcut_pull,
        "m_pendingEditorViewportToolShortcuts.RemoveAt(0);",
        "pulling a viewport shortcut must consume it",
    )
    require(
        window_source,
        "(lParam & 0x40000000) == 0",
        "Win32 key repeat must not emit repeated tool shortcut events",
    )
    require_order(
        tick_viewport_tools,
        "PullEditorViewportToolShortcut(",
        "QueueToolShortcutEvent(",
        "the engine worker must publish native shortcuts through the viewport event queue",
    )
    require(
        controller_source,
        'event["kind"] = "toolShortcut";',
        "the controller must publish a dedicated native tool-shortcut event",
    )
    require(
        protocol_schema,
        "message ViewportToolShortcutEvent {",
        "the editor protocol must define a typed viewport tool-shortcut event",
    )
    require(
        protocol_schema,
        "ViewportToolShortcutEvent tool_shortcut = 13;",
        "the typed viewport tool shortcut must use the additive payload tag 13",
    )
    require(
        protocol_bridge,
        "outEvent.mutable_tool_shortcut()->set_key_code(keyCode);",
        "the native protocol bridge must type the shortcut payload",
    )
    require(
        managed_scene,
        "case EditorViewportToolShortcutEvent shortcutEvent:",
        "SceneView must consume the typed viewport shortcut event",
    )
    require(
        managed_scene,
        "await ApplyViewportToolStateSafelyAsync(shortcutState);",
        "SceneView must apply native shortcuts through the async protocol RPC",
    )
    safe_tool_state_apply = extract_function(
        managed_scene,
        "async Task ApplyViewportToolStateSafelyAsync(",
    )
    if safe_tool_state_apply.count("await RefreshViewportToolStateAsync();") != 2:
        raise AssertionError(
            "rejected and exceptional toolbar updates must both reconcile against engine state"
        )
    require_order(
        safe_tool_state_apply[:safe_tool_state_apply.find("catch (Exception ex)")],
        "if (!await ApplyViewportToolStateAsync(",
        "await RefreshViewportToolStateAsync();",
        "rejected toolbar updates must reconcile after the failed RPC",
    )
    require_order(
        safe_tool_state_apply[safe_tool_state_apply.find("catch (Exception ex)"):],
        "catch (Exception ex)",
        "await RefreshViewportToolStateAsync();",
        "exceptional toolbar updates must reconcile after logging the exception",
    )

    register = extract_function(
        window_source,
        "void RegisterEditorViewportDropTarget(",
    )
    revoke = extract_function(
        window_source,
        "void RevokeEditorViewportDropTarget(",
    )
    create = extract_function(window_source, "bool Window::Create(")
    destroy = extract_function(window_source, "void Window::Destroy()")

    require_order(
        register,
        "OleInitialize(nullptr)",
        "RegisterDragDrop(",
        "OLE must initialize before registering the viewport drop target",
    )
    require(
        register,
        "OleUninitialize();",
        "failed drop-target registration must balance OLE initialization",
    )
    require(
        create,
        "m_parentHwnd &&",
        "only embedded editor windows may register the OLE drop target",
    )
    require(
        create,
        'm_windowClassName.starts_with("SailorEditor")',
        "OLE registration must be limited to Sailor editor viewport windows",
    )
    require(
        create,
        "RegisterEditorViewportDropTarget(",
        "Window creation must register the editor viewport drop target",
    )
    require_order(
        revoke,
        "RevokeDragDrop(",
        "dropTarget->Release();",
        "drop-target revocation must precede releasing the COM object",
    )
    require_order(
        revoke,
        "dropTarget->Release();",
        "OleUninitialize();",
        "OLE teardown must release the registered target before uninitializing",
    )
    require_order(
        destroy,
        "RevokeEditorViewportDropTarget(",
        "DestroyWindow(",
        "Window destruction must revoke OLE registration before destroying the HWND",
    )

    print("Win32 editor asset-drop contract passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
