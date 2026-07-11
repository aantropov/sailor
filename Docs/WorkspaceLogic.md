# Workspace Logic Projects

Sailor workspaces keep user-authored game logic under `Source/` and generated build files under `Generated/`. The editor generates a CMake project that builds a shared `SailorGame` module against the engine's `Sailor::Runtime` CMake target.

## Workspace Layout

New workspaces use this layout:

```text
Content/
Source/
  Components/
    SampleComponent.cpp
    SampleComponent.h
  WorkspaceModule.cpp
  WorkspaceTypes.h
Generated/
  CMakeLists.txt
Cache/
  Build/
Binaries/
.gitignore
```

`Generated/CMakeLists.txt` is created with the workspace and is not overwritten when the workspace is opened or saved. It can be edited when the project needs custom CMake behavior, while game code remains under `Source/`. Build-system intermediates belong in `Cache/Build`; configuration-specific logic modules are written to `Binaries/<CONFIG>/`.

`WorkspaceTypes.h` is the explicit list of reflected game types exported by the module. Add each new reflected component to its `TWorkspaceTypeList`. `WorkspaceModule.cpp` defines the versioned module API and metadata entry points from that same list; both files are created once and remain user-editable.

Generated logic projects target 64-bit Windows and accept MSVC or clang-cl. Configuration stops with a diagnostic when the platform, architecture, or compiler front end does not match that contract.

The default manifest values are:

| Field | Default | Meaning |
| --- | --- | --- |
| `engineReferenceKind` | `source` | Use an engine source tree; `installed` selects a CMake package prefix. |
| `buildPath` | `Cache/Build` | CMake binary directory relative to the workspace. |
| `logicOutputPath` | `Binaries` | Root for configuration-specific module outputs. |
| `logicModuleName` | `SailorGame` | Generated shared-library target and module name. |

## Source Engine Reference

For `engineReferenceKind: source`, `enginePath` points to a Sailor source checkout. The generated project disables the engine executable and tests before adding the checkout:

```cmake
set(SAILOR_BUILD_EXECUTABLE OFF CACHE BOOL "" FORCE)
set(SAILOR_BUILD_TESTS OFF CACHE BOOL "" FORCE)
add_subdirectory("<enginePath>" "${CMAKE_BINARY_DIR}/SailorEngine" EXCLUDE_FROM_ALL)
target_link_libraries(SailorGame PRIVATE Sailor::Runtime)
```

Those two engine options default to `ON` for a standalone Sailor build and `OFF` when Sailor is included by another CMake project.

## Installed Engine Reference

Install Sailor to a package prefix after configuring and building it with real dependencies:

```powershell
cmake --install <engine-build> --config Release --prefix <engine-prefix>
```

An install configured with dependency stubs is rejected. The package contains the runtime library, public runtime headers, the public RenderDoc API header, `SailorConfig.cmake`, and the exported `Sailor::Runtime` target.

For `engineReferenceKind: installed`, set `enginePath` to `<engine-prefix>`. The generated project prepends that prefix to `CMAKE_PREFIX_PATH` and uses:

```cmake
find_package(Sailor CONFIG REQUIRED)
target_link_libraries(SailorGame PRIVATE Sailor::Runtime)
```

Sailor's public dependencies must remain discoverable by the consumer's CMake toolchain. With the repository vcpkg checkout, pass `External/vcpkg/scripts/buildsystems/vcpkg.cmake` when configuring.

## Configure And Build

From the workspace root:

```powershell
cmake -S Generated -B Cache/Build -DCMAKE_BUILD_TYPE=Release
cmake --build Cache/Build --config Release --target SailorGame
```

For a default Windows Release workspace, the resulting module and import library are `Binaries/Release/SailorGame.dll` and `Binaries/Release/SailorGame.lib`. Link and compile PDB outputs use the same configuration directory. Debug and other configurations use their matching subdirectory.

The engine reference, paths, and module name are creation-time inputs copied into `Generated/CMakeLists.txt`. Opening or saving a workspace does not regenerate that file. If those manifest values are edited later, update the corresponding CMake values before reconfiguring.

## Workspace Module ABI

Workspace modules export a V1 API-table entry point and retain the V1 metadata entry point:

```cpp
const Sailor::Workspace::WorkspaceModuleApiV1*
SailorGetWorkspaceModuleApiV1() noexcept;

uint32_t SailorGetWorkspaceTypeMetadataV1(
    char* destination,
    uint64_t destinationCapacity,
    uint64_t* outPayloadSize) noexcept;
```

`SailorGetWorkspaceModuleApiV1` returns a module-owned static POD table. It contains its structure size and API version, module name, exact build ABI tag, the metadata callback, and the explicit type-registration callback. The table and its strings remain valid until the module is unloaded. The ABI tag is independently compiled into the engine and game module and includes the ABI revision, architecture, compiler version, C runtime, iterator-debug mode, and build configuration; the host must require an exact match before using callbacks.

Registration passes a POD host table with an opaque context and collection callback. Each collected POD descriptor supplies bounded type and base names, an opaque `TypeInfo` pointer, size, alignment, and a `noexcept` placement factory. The generated callback only collects descriptors from `WorkspaceTypes`; it does not mutate engine registries. Workspace types must be default-constructible `Sailor::Component` subclasses, and `Component` must be their zero-offset object base; the generated factory rejects unsupported multiple-inheritance layouts before returning an object. All callbacks use the C calling convention, and no STL object, exception, allocation ownership, or caller-freed string crosses the module boundary.

For metadata, the caller first passes a null destination and zero capacity to query the required UTF-8 YAML byte count. The function returns `BufferTooSmall`, writes the exact non-null-terminated size, and never transfers allocation ownership across the DLL boundary. A second call with an adequate caller-owned buffer returns `Success`. Invalid pointers/capacities and serialization failures use explicit result codes from `WorkspaceModuleApi.h`; exceptions never cross the ABI.

The YAML document uses `metadataVersion: 1`, identifies `moduleName`, and preserves the existing consumer keys `timeStamp`, `engineTypes`, `cdos`, `enums`, and `assetTypes`. The generated sample exports `moveSpeed: float` with a default value of `5.0`.

Game modules compile with `SAILOR_WORKSPACE_MODULE`, which enables `SAILOR_WORKSPACE_REFLECTABLE`; every reflected game type must use that macro so it does not install a static registration helper. Engine types continue using `SAILOR_REFLECTABLE` unchanged, and `Reflection::ExportEngineTypes()` remains engine-only. The runtime lifecycle loads the DLL, validates the API and ABI before calling registration, preflights the complete collected set, and commits only accepted workspace types. It removes those registrations before unloading the module.

## Runtime Discovery And Lifecycle

At startup, the runtime resolves the active workspace before content scanning. `--workspace-manifest <path>` selects an explicit manifest inside that workspace. Without the option, discovery prefers `workspace.sailor`; if that file is absent, exactly one root-level `*.sailor` file is accepted. No manifest preserves legacy engine-only startup, while multiple candidates are rejected with an ambiguity diagnostic.

For manifest version 1, the runtime resolves the module as `<workspace>/<logicOutputPath>/<CONFIG>/<platform-module-name>`. The default values are `Binaries` and `SailorGame`. The manifest output path, module name, build configuration, and canonical module path are validated so discovery cannot escape the workspace. The active CMake configuration is part of both the path and ABI check, so a stale Debug/Release binary fails with rebuild guidance instead of being loaded as a compatible module.

The dynamic library is opened before asset importers scan `Content`. Static engine registration callbacks triggered by the platform loader are suppressed for that load operation; only the explicit V1 descriptor callback can add workspace types. The host validates the complete descriptor and metadata set before committing it under a module owner. Missing libraries, loader dependencies, entry points, incompatible API/ABI, metadata errors, and registration collisions return structured non-crashing diagnostics.

Standalone startup treats a configured module or requested-world activation failure as fatal and returns a nonzero exit code. Editor startup reports the same error but remains available with an empty world so the project can be repaired. During shutdown, worlds, importers, the asset registry, scheduler, and remaining submodules are destroyed before workspace registrations are removed and the library is closed. Runtime hot reload and live workspace switching are not supported by this contract.

## SDK Limitations

`Sailor::Runtime` is a consistent CMake target name, not a cross-version C++ ABI guarantee. Build the engine and game module with compatible compiler versions, toolsets, configurations, and C runtime settings.

The install tree does not bundle vcpkg packages, the Vulkan SDK, or runtime dependency DLLs. It is a CMake development package for a matching build environment, not a self-contained or redistributable SDK.
