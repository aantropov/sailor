# Native editor restart integration

This macOS headless target compiles the production `EngineService` Mac path and
uses the production protocol client, socket transport, module loader and cache.
Only process bootstrap and MAUI UI dependencies are adapted. UI notifications
remain queued so the test can deliver old-generation notifications after a
restart. This does not validate MAUI thread affinity or the visible editor.

Build the engine and test fixtures, then run from the repository root:

```sh
cmake --build build-audit --config Release --parallel 4
SAILOR_ENGINE_ROOT="$PWD" \
SAILOR_NATIVE_TEST_HOST="$PWD/Binaries/Release/VulkanSubmissionTests" \
SAILOR_NATIVE_WORKSPACE_FIXTURE="$PWD/build-audit/Tests/Release/libWorkspaceFixture.dylib" \
SAILOR_NATIVE_WORKSPACE_RELOAD_FIXTURE="$PWD/build-audit/Tests/Release/libReloadedWorkspaceFixture.dylib" \
DYLD_LIBRARY_PATH="$PWD/build-audit/Lib/Release" \
dotnet test Editor.Tests/EngineIntegration/Editor.Engine.Integration.Tests.csproj -c Release
```

The test skips unless `SAILOR_NATIVE_TEST_HOST` is set. All other paths are
required when running it. The five fresh processes cover the original module,
changed types/defaults, recovery of a world using the new component, editor-only
startup without the module, and recovery after the module is restored. All
workspace files and caches are generated in a unique temporary directory.
The shared host resets its ready/stop markers and checks native exit codes.

For ThreadSanitizer, build `build-audit-tsan` and use its `Binaries/Release`,
`Tests/Release` and `Lib/Release` directories with `TSAN_OPTIONS=halt_on_error=1`.
Do not run this project's build concurrently with `Editor.Contracts.Tests`:
both use the same generated protocol project. The normal contract suite also
uses the shared process bootstrap for its native asset-reimport integration.
