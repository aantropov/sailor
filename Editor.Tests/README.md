# Editor tests

Run the ordinary managed contracts from the repository root:

```sh
dotnet test Editor.Tests/Editor.Contracts.Tests.csproj -c Release
```

## Native asset reimport integration

AssetReimportIntegrationTests uses the production EngineProtocolClient,
LocalEngineProtocolTransport, WebSocket transport, native host exports and
engine asset registry/importers. Its process adapter only starts and stops the
native child on its own main thread. Requests and responses travel through the
real socket; no protocol response or importer result is mocked.

Build VulkanSubmissionTests in a CMake configuration with engine tests enabled.
Set SAILOR_NATIVE_TEST_HOST to that executable and SAILOR_ENGINE_ROOT to the
source checkout. On macOS, also select the corresponding runtime library:

```sh
cmake --build build-audit --config Release --target VulkanSubmissionTests
SAILOR_NATIVE_TEST_HOST="$PWD/Binaries/Release/VulkanSubmissionTests" \
SAILOR_ENGINE_ROOT="$PWD" \
DYLD_LIBRARY_PATH="$PWD/build-audit/Lib/Release" \
dotnet test Editor.Tests/Editor.Contracts.Tests.csproj -c Release \
  --filter FullyQualifiedName~AssetReimportIntegrationTests
```

On Windows, use the staged VulkanSubmissionTests.exe and its adjacent runtime
DLLs. A working Vulkan device is required. Without SAILOR_NATIVE_TEST_HOST, the
integration case is explicitly skipped; it is not reported as a passing native
test. Invalid configured paths or failed native startup fail the test.

The fixture creates a temporary workspace and a hidden native rendering surface.
It does not launch the MAUI editor, open a visible window or modify project
assets. It starts the real engine loop, checks generated properties and authored
values, and creates a real model instance without changing Content bytes or
timestamps. It distinguishes ordinary Update from forced Reimport and covers malformed
output, unchanged retry, missing-output repair and byte/time-stable warm reuse.
The child is stopped and reaped before its temporary workspace is removed.
Native output is retained in the test result, including runtime loading details
on macOS. Do not enable DYLD_PRINT_LIBRARIES for dotnet/MSBuild itself: native
generator diagnostics can be interpreted as build errors.

This test does not instantiate AssetsService, EngineService or the MAUI content
menu. Their composition and UI lifecycle need separate coverage; passing this
case is not a GUI workflow or Windows GPU claim.
