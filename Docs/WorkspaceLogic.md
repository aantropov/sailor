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
| `contentPath` | `Content` | User-authored assets owned by the workspace. |
| `sourcePath` | `Source` | User-authored game logic sources. |
| `generatedProjectPath` | `Generated` | Generated project files owned by the workspace. |
| `cachePath` | `Cache` | Disposable workspace runtime and editor caches. |
| `buildPath` | `Cache/Build` | CMake binary directory relative to the workspace. |
| `logicOutputPath` | `Binaries` | Root for configuration-specific module outputs. |
| `logicModuleName` | `SailorGame` | Generated shared-library target and module name. |

All workspace-owned paths in the table are safe relative paths. Rooted, drive-relative, traversal, and physical symlink or junction escapes are rejected. `enginePath` is intentionally different: it is an external engine source or install reference and may resolve outside the workspace.

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

An install configured with dependency stubs is rejected. The package contains the runtime library, public runtime headers, the public RenderDoc API header, `SailorConfig.cmake`, and the exported `Sailor::Runtime` target. It does not currently install the engine `Content/` tree. Runtime activation of an installed engine reference therefore remains unsupported until that prefix also provides `<engine-prefix>/Content`; workspace resolution reports the missing Engine Content directory instead of silently falling back to workspace assets.

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

Registration passes a POD host table with an opaque context and collection callback. Each collected POD descriptor supplies bounded type and base names, an opaque `TypeInfo` pointer, size, alignment, an immutable canonical YAML snapshot of its default values, reflection-integrity flags, and a `noexcept` placement factory. The generated callback only collects descriptors from `WorkspaceTypes`; it does not mutate engine registries. Workspace types must be default-constructible `Sailor::Component` subclasses, and `Component` must be their zero-offset object base; the generated factory rejects unsupported multiple-inheritance layouts before returning an object. All callbacks use the C calling convention, and no STL object, exception, allocation ownership, or caller-freed string crosses the module boundary.

For metadata, the caller first passes a null destination and zero capacity to query the required UTF-8 YAML byte count. The function returns `BufferTooSmall`, writes the exact non-null-terminated size, and never transfers allocation ownership across the DLL boundary. A second call with an adequate caller-owned buffer returns `Success`. Invalid pointers/capacities and serialization failures use explicit result codes from `WorkspaceModuleApi.h`; exceptions never cross the ABI.

The YAML document uses `metadataVersion: 1`, identifies `moduleName`, and preserves the existing consumer keys `timeStamp`, `engineTypes`, `cdos`, `enums`, and `assetTypes`. The generated sample exports `moveSpeed: float` with a default value of `5.0`.

Game modules compile with `SAILOR_WORKSPACE_MODULE`, which enables `SAILOR_WORKSPACE_REFLECTABLE`; every reflected game type must use that macro so it does not install a static registration helper. Engine types continue using `SAILOR_REFLECTABLE` unchanged, and `Reflection::ExportEngineTypes()` remains engine-only. The runtime lifecycle loads the DLL, validates the API and ABI before calling registration, preflights the complete collected set, and commits only accepted workspace types. Preflight requires exact unique type and CDO sets, matches the writable property schema, rejects ambiguous or shadowed reflected names, and compares every CDO to the descriptor's independently transported canonical snapshot. The snapshot and metadata share one cached default-object value, so nondeterministic constructors cannot create two different preflight views. Comparison is structural: scalar values and tags are exact, sequences preserve length and order, and maps require an exact duplicate-free key set independent of entry order. No YAML decoder is invoked during this comparison, so null object references and custom structured values remain safe. Getter-only properties participate in CDO validation, while `Transient` and `SkipCDO` properties do not.

Workspace placement factories run without the reflection registry mutex held, so constructors may perform reflection lookup. Each module owner has an invocation barrier: unregister first hides all of its types from new construction, waits for active factories to return, and only then permits the library to close. This barrier does not extend the lifetime of returned objects; worlds and every other workspace object must still be destroyed before module unload.

## Runtime Discovery And Lifecycle

At startup, the runtime resolves one immutable workspace context before module loading, content scanning, registry construction, or cache initialization. The context contains the canonical workspace and engine roots and manifest plus the resolved Workspace Content, Engine Content, Cache, Source, Generated, Build, logic-output, workspace identity, version, and module name. The editor resolves the same manifest-owned paths in its workspace session and passes the exact root, manifest, Content, and Cache paths into its launch contract.

`--workspace-manifest <path>` selects an explicit manifest inside the workspace. Without the option, discovery prefers `workspace.sailor`; if that file is absent, exactly one root-level `*.sailor` file is accepted. No manifest creates a legacy context using `<root>/Content` and `<root>/Cache`; multiple candidates are rejected with an ambiguity diagnostic.

Every workspace-owned manifest path is normalized lexically and then checked for physical containment after existing symlinks or Windows junctions are resolved. Validation completes before the context or editor session is published. A missing default `Content` directory is recreated, while a missing custom content path is rejected without creating a replacement. Cache directories are disposable and are recreated at their configured path. If later validation or recovery fails, directories created by that resolution attempt are rolled back.

### Editor Workspace Activation

Editor workspace changes run through one serialized FIFO activation coordinator. New, Open, and recent-workspace requests cannot overlap or overtake one another. Each request progresses through the observable phases `Idle`, `Preflighting`, `Stopping`, `Clearing`, `Committing`, `Starting`, and `Ready`; a failure after the irreversible boundary finishes in `Repair`.

`Preflighting` is reversible with respect to the active session. It resolves and validates the candidate workspace, performs any recoverable filesystem preparation, and captures an exact launch context without publishing the candidate as the current session or disturbing the active runtime. Cancellation or failure in this phase rolls back reversible directory recovery and leaves the previous editor session and runtime unchanged. Files produced by a successfully completed New Workspace preflight remain as the user's newly created workspace on disk, but that workspace is not activated.

Beginning `Stopping` is the irreversible boundary. The editor immediately closes the command/selection gates, advances their workspace epochs, and cancels pending command and selection work before awaiting native teardown. From that point it completes teardown and candidate publication even if the initiating caller is cancelled. It awaits the native run task, polling tasks, and native shutdown before unloading the active workspace module. The previous runtime is never restarted as an implicit rollback: its files or binaries may already have changed, so reusing its earlier launch context would not restore a known state.

After the old runtime stops, `Clearing` invalidates workspace-scoped state in a fixed order: command history, including active and delayed commands; selection and in-flight selection work; world caches and world callbacks; workspace-derived editor types; asset and project-content state; and finally hierarchy and inspector projections. Pending inspector edits are discarded instead of being committed during this reset. Workspace epochs tag asynchronous selection, world, content, history, and type results, so callbacks captured before the reset cannot repopulate the new session.

`Committing` publishes the prepared candidate session. `Starting` uses only the root, manifest, Workspace Content, Cache, and other values from the exact launch context captured during preflight; it does not rediscover the workspace from mutable current-directory or UI state. After the candidate type catalog is available, the editor mounts and projects candidate Content so custom asset metadata cannot be resolved through the previous catalog. The command and selection gates reopen only after all of those steps succeed. `Ready` therefore identifies a runtime and editor state derived from the same validated candidate.

If stopping, clearing, committing, or starting fails, activation still completes the safe teardown and, when preparation succeeded, publishes the candidate in `Repair`. Repair is an explicit editor-only state: the previous workspace session, runtime, and cached projections remain cleared, and the candidate is available for correcting its manifest, content, build output, or module. There is no rollback to the previous runtime. Recovery is performed by retrying activation after fixing the candidate or by opening another valid workspace; either action enters the same serialized preflight pipeline.

### Runtime Content Mounts

The asset registry exposes exactly two discoverable content roots in one virtual namespace:

- Workspace Content is writable and has higher priority.
- Engine Content is read-only and has lower priority.

Lookups use paths relative to either Content root. When both roots provide the same virtual path, Workspace Content wins. The same precedence applies when metadata in both roots declares the same `FileId`, so path-based and ID-based access select a consistent workspace override. Engine assets with unique paths and IDs remain available. Every virtual-path or `FileId` collision produces a deterministic diagnostic that identifies the selected and shadowed files.

The roots are canonicalized before discovery. If Workspace Content and Engine Content resolve to the same physical directory, the registry keeps one writable Workspace mount and reports the deduplication; this preserves legacy workspaces whose engine and workspace roots are the same. Distinct roots may not contain one another. A nested-root configuration is rejected before a new registry generation is published, preventing one file from entering the namespace through two relative paths.

Engine Content is never mutated by discovery. An Engine asset without metadata is diagnosed and skipped rather than imported or assigned newly generated metadata. Source rewrites and generated asset metadata are likewise restricted to Workspace Content; derived runtime data may still be written to the workspace Cache. Plain-text and binary content reads, including shader includes, particle data, and star-catalog data, use the same virtual-path resolver and workspace-over-engine precedence as registered assets.

Import callbacks may register newly generated, non-conflicting Workspace assets into the active generation. A newly created Workspace file that collides with an active `FileId` is rejected until the next explicit Content rescan can apply the complete mount-precedence policy transactionally. Runtime hot replacement remains outside this contract.

Cache is not a third discoverable content root. The editor's generated `../Cache/Temp.world` is supported through the direct cache-load exception, while normal asset discovery remains limited to Workspace Content and Engine Content.

For manifest version 1, the runtime resolves the module as `<resolved-logic-output>/<CONFIG>/<platform-module-name>`. `WorkspaceModuleManager` consumes the captured context and never reparses the manifest. It canonicalizes the final module path again immediately before loading and rejects any symlink or junction change observed by that check. Concurrent mutation of the workspace or build tree during the platform's pathname-only library-loader call is not supported. The active CMake configuration is part of both the path and ABI check, so a stale Debug/Release binary fails with rebuild guidance instead of being loaded as a compatible module.

The dynamic library is opened before asset importers scan the resolved Workspace Content and Engine Content mounts. Asset, shader, precompiled-shader, and editor-type caches use the resolved Cache directory, including custom paths with spaces. The generated shader constants library is written below Workspace Content. Static engine registration callbacks triggered by the platform loader are suppressed for that load operation; only the explicit V1 descriptor callback can add workspace types. The host validates the complete descriptor and metadata set before committing it under a module owner. Missing libraries, loader dependencies, entry points, incompatible API/ABI, metadata errors, and registration collisions return structured non-crashing diagnostics.

Standalone startup treats a configured module or requested-world activation failure as fatal and returns a nonzero exit code. Editor startup reports the same error but remains available with an empty world so the project can be repaired. During shutdown, worlds, importers, the asset registry, scheduler, and remaining submodules are destroyed before workspace registrations are removed and the library is closed. Runtime hot reload and live workspace switching are not supported by this contract.

## Editor Type Metadata

`SerializeEngineTypes` remains the legacy engine-only C export. Editor consumers use the distinct `SerializeEditorTypes` export, which starts from a fresh engine snapshot and appends the validated metadata of the active workspace module. The combined document keeps each custom component's fully-qualified `typename`, reflected properties, and default object values.

The merge validates all four catalogs before publishing a result. Type and CDO identities use `typename`, enum identities use their map key, and asset-type identities use `typename`. Duplicate entries, engine/workspace collisions, malformed sections, or mismatched workspace type/default sets reject the complete merge without returning a partial catalog. A missing, failed, or unloaded workspace module produces a fresh engine-only editor snapshot so custom types cannot survive through stale runtime state.

## SDK Limitations

`Sailor::Runtime` is a consistent CMake target name, not a cross-version C++ ABI guarantee. Build the engine and game module with compatible compiler versions, toolsets, configurations, and C runtime settings.

The install tree does not bundle vcpkg packages, the Vulkan SDK, or runtime dependency DLLs. It is a CMake development package for a matching build environment, not a self-contained or redistributable SDK.
