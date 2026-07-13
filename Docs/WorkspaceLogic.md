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
.sailor/
  GeneratedProjectState.yaml
.gitignore
```

`Generated/CMakeLists.txt` and the sample files under `Source/` are created with the workspace and are not overwritten when the workspace is opened or saved. They become user-owned immediately after creation and can be edited when the project needs custom build or game behavior. Build-system intermediates belong in `Cache/Build`; configuration-specific logic modules are written to `Binaries/<CONFIG>/`.

`WorkspaceTypes.h` is the explicit list of reflected game types exported by the module. Add each new reflected component to its `TWorkspaceTypeList`. `WorkspaceModule.cpp` defines the versioned module API and metadata entry points from that same list; both files are created once and remain user-editable.

Generated logic projects target 64-bit platforms and accept MSVC, Clang/AppleClang, clang-cl, or GCC. Configuration stops with a diagnostic when the platform, architecture, or compiler front end does not match that contract.

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

## Generated State And Manifest Compatibility

`.sailor/GeneratedProjectState.yaml` is source-controlled generator metadata, not a cache and not an ownership claim over generated CMake or source files. New Workspace writes it last, after every user-owned project artifact has been created successfully. Open and Save only assess it: they never create, backfill, regenerate, or replace the sidecar, `Generated/CMakeLists.txt`, or any file under `Source/`.

The current generated-state contract is:

```yaml
generatorSchemaVersion: 1
creationInputs:
  manifestVersion: 1
  enginePath: ../Sailor
  engineReferenceKind: source
  contentPath: Content
  sourcePath: Source
  generatedProjectPath: Generated
  cachePath: Cache
  buildPath: Cache/Build
  logicOutputPath: Binaries
  logicModuleName: SailorGame
```

Only manifest values that affect the originally generated project are recorded. `name`, `workspaceId`, absolute workspace roots, timestamps, hashes, file content, and mtimes are deliberately excluded. Values are normalized with the manifest rules and compared in the fixed order shown above. A matching sidecar is `Current`; a missing sidecar is `Untracked`; an input mismatch is `Stale`; and an unreadable, unsafe, malformed, or unsupported sidecar is `Invalid`. All four outcomes are advisory for an otherwise valid workspace. Non-current outcomes remain visible in editor status with deterministic guidance, but do not enter activation `Repair` and do not authorize an implicit write. Reconciliation is manual: restore the recorded manifest inputs, or update the user-owned project and sidecar deliberately after verifying them; creating a clean workspace with the current editor is the safe fallback when no regeneration command exists.

The only supported workspace manifest format is currently `manifestVersion: 1`. A present manifest must contain exactly one positive scalar version field. Missing, zero, future, duplicate, and non-scalar versions are rejected before typed fields are deserialized, directories are recovered, recents or the active session are changed, or the manifest is written. The runtime's legacy mode applies only when no manifest file exists; a present versionless manifest is not legacy input.

Version 1 retains additive compatibility: unknown fields are ignored, and `engineReferenceKind`, `buildPath`, `logicOutputPath`, and `logicModuleName` receive their documented defaults only when their fields are absent. An explicitly null or empty value is invalid in both the editor and runtime. Opening a valid older v1 document may therefore normalize the in-memory model, but it does not rewrite the file.

A future migration must be editor-only and sequential (`vN -> vN+1` for every intermediate version). Each step must validate its source, create a unique backup without overwriting an earlier backup, transform entirely in memory, validate the target, write and durably flush a same-directory temporary file, and atomically replace the manifest. Any failure leaves the source manifest and generated project files intact. The runtime never migrates. Manifest migration never regenerates CMake/source files or updates generated state implicitly; changed creation inputs intentionally leave the sidecar stale until the user reconciles them.

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

For a default Release workspace, the resulting module is:

- Windows: `Binaries/Release/SailorGame.dll`
- macOS: `Binaries/Release/libSailorGame.dylib`
- Linux: `Binaries/Release/libSailorGame.so`

On Windows, the import library and PDB outputs follow the same configuration directory. Debug and other configurations use their matching subdirectory.

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

### Workspace Cache Envelopes

Workspace-generated caches use a strict version 1 YAML envelope. The native asset and shader caches and the managed editor-type cache use the same field contract:

| Field | Compatibility rule |
| --- | --- |
| `cacheVersion` | Common envelope format. The current value is `1`; a missing, older, or future value is unsupported. |
| `payloadVersion` | Producer-specific payload schema. It must exactly match the version expected by that consumer. |
| `cacheKind` | Identifies the consumer, such as `asset-cache`, `shader-cache`, or `editor-types`; it must match exactly. |
| `producerIdentity` | Identifies the payload producer and schema family, such as `asset-cache-v1` or `editor-types-v1`; it must match exactly. |
| `workspaceId` | Uses the manifest workspace ID, or the deterministic legacy identity described below; it must match exactly. |
| `engineVersion` | Uses the engine build's `SAILOR_ENGINE_VERSION`; it must match exactly. |
| `buildIdentity` | Combines the active build configuration with the workspace-module ABI tag, including architecture, compiler, C runtime, iterator mode, and configuration; it must match exactly. |
| `payload` | Contains the producer-owned serialized data and is published only after complete producer-specific validation. |

The current envelope is closed to unknown or duplicate fields. Version mismatches are not migrated implicitly, and identity mismatches are not accepted as a best-effort hit. For a workspace without a manifest ID, the runtime canonicalizes the immutable workspace root as UTF-8, normalizes separators, folds ASCII `A`-`Z` to lowercase on Windows, and prefixes it with `legacy-root:`. The managed launch contract uses the same byte-preserving rule; non-ASCII characters are not locale-case-folded. The editor marshals workspace and manifest launch paths explicitly as UTF-8, and the runtime decodes those command-line path bytes as UTF-8 before canonicalization, so both sides derive the same identity for non-ASCII roots. Two legacy workspaces at different canonical roots therefore cannot share cache identity accidentally.

Cache loads return one of these structured statuses:

| Status | Meaning |
| --- | --- |
| `Missing` | The owned cache file does not exist. |
| `Loaded` | The complete envelope, exact identity, payload version, and producer payload passed validation. |
| `StaleIdentity` | Cache kind, producer, workspace, engine, or build identity differs from the expected descriptor. |
| `Corrupt` | YAML, field structure, payload data, or referenced artifact integrity is malformed or incomplete. |
| `UnsupportedVersion` | The common envelope version or producer payload version is missing or not exactly supported. |
| `IoFailure` | The cache path could not be inspected or read reliably. Write and invalidation failures use separate diagnostics. |

No non-`Loaded` result publishes decoded state. Asset and shader payloads are decoded into temporary containers and swapped into their live caches only after complete validation. Shader payload version 2 assigns every entry a collision-resistant immutable generation and requires structurally complete regular and debug artifact sets with identical stage topology. Metadata records the byte length and checksum of every present artifact; truncated, misaligned, same-size checksum-mismatched, or missing required SPIR-V is corrupt rather than a partial cache hit. Editor type metadata is cleared before startup cache handling, the native runtime identity is compared with the exact editor launch identity after initialization, and only a fully validated combined type catalog may be published.

Invalidation is scoped by cache ownership:

- AssetCache owns only `Cache/AssetCache.yaml`. A missing, stale, corrupt, or unsupported load clears its in-memory map and atomically writes an empty current envelope without deleting neighboring files.
- ShaderCache owns `Cache/ShaderCache.yaml` plus `Cache/PrecompiledShaders/`, `Cache/CompiledShaders/`, and `Cache/CompiledShadersWithDebug/`. It verifies that those paths remain contained by the resolved Cache root before deleting or recreating them. Artifact garbage collection uses only the last successfully committed metadata snapshot as its whitelist, so an envelope failure cannot delete the generation still referenced on disk; other cache files and user-authored Content are never part of shader invalidation.
- EditorTypes owns `Cache/EditorTypes.yaml` and same-directory temporary files matching its own generated name pattern. Stale, corrupt, and unsupported caches are invalidated by removing only that target and its owned temporary files; it does not sweep the Cache directory.

An `IoFailure` is not evidence that stored bytes are invalid. All three consumers clear or withhold in-memory state after an unreadable cache, but preserve the existing target and owned artifacts so a transient sharing, antivirus, permission, or device error cannot destroy a potentially valid cache. After an AssetCache `IoFailure`, ordinary updates and shutdown preserve existing storage; only an explicit forced rebuild or `ClearAll` authorizes replacement. ShaderCache additionally enters a read-only storage quarantine when either envelope loading or later runtime artifact validation encounters an I/O failure: compilation may publish complete regular/debug bytecode in memory for the current session, but save, invalidation, artifact cleanup, and precompiled writes do not touch disk. Missing, corrupt, stale, unsupported, and repeated I/O reload results keep that quarantine and its session state intact. Only a fully successful reload discards the session-only state and restores persistent access; `ClearAll` is the explicit destructive escape hatch.

Cache YAML and shader artifacts are replaced through same-directory temporary files. The complete temporary file is written and flushed before an atomic replace exposes it; native POSIX writes also synchronize the containing directory, while Windows replacement requests write-through behavior. Shader compilation first writes all regular and required debug artifacts under a new immutable generation, then makes that complete generation eligible for the next metadata commit. Remove and expiry cleanup take the inverse order: they atomically commit candidate metadata before garbage-collecting artifacts no longer referenced by the committed snapshot. A successful shader save also sweeps the replaced immutable generation only after the new envelope commits. A failure before replacement leaves the previous target and generation intact and triggers best-effort temporary-file cleanup. If a durability synchronization reports failure after replacement, the target is still a complete old or new file, never a partially written one. Native cache callers retain dirty state after any reported save failure; shader compile-all performs a save-only retry when bytecode is current but its metadata is still dirty. Metadata is not committed as clean until replacement and post-commit cleanup succeed. An editor-type persistence failure likewise preserves the validated live catalog and reports the failed write instead of publishing unvalidated disk data.

Workspace switching therefore preserves A → B → A isolation. A cache copied from workspace A is `StaleIdentity` in B and is never published, so B starts from empty producer-owned state and writes a B envelope. Returning to A can load only data whose complete identity still matches A. Transactional editor activation additionally clears in-memory asset, world, and editor-type projections before the candidate starts, preventing an asynchronous result from the prior workspace from bypassing the on-disk identity check.

For manifest version 1, the runtime resolves the module as `<resolved-logic-output>/<CONFIG>/<platform-module-name>`. `WorkspaceModuleManager` consumes the captured context and never reparses the manifest. It canonicalizes the final module path again immediately before loading and rejects any symlink or junction change observed by that check. Concurrent mutation of the workspace or build tree during the platform's pathname-only library-loader call is not supported. The active CMake configuration is part of both the path and ABI check, so a stale Debug/Release binary fails with rebuild guidance instead of being loaded as a compatible module.

The dynamic library is opened before asset importers scan the resolved Workspace Content and Engine Content mounts. Asset, shader, precompiled-shader, and editor-type caches use the resolved Cache directory, including custom paths with spaces. The generated shader constants library is written below Workspace Content. Static engine registration callbacks triggered by the platform loader are suppressed for that load operation; only the explicit V1 descriptor callback can add workspace types. The host validates the complete descriptor and metadata set before committing it under a module owner. Missing libraries, loader dependencies, entry points, incompatible API/ABI, metadata errors, and registration collisions return structured non-crashing diagnostics.

Standalone startup treats a configured module or requested-world activation failure as fatal and returns a nonzero exit code. Editor startup reports the same error but remains available with an empty world so the project can be repaired. During shutdown, worlds, importers, the asset registry, scheduler, and remaining submodules are destroyed before workspace registrations are removed and the library is closed. Runtime hot reload and live workspace switching are not supported by this contract.

## Editor Type Metadata

`SerializeEngineTypes` remains the legacy engine-only C export. Editor consumers use the distinct `SerializeEditorTypes` export, which starts from a fresh engine snapshot and appends the validated metadata of the active workspace module. The combined document keeps each custom component's fully-qualified `typename`, reflected properties, and default object values.

The merge validates all four catalogs before publishing a result. Type and CDO identities use `typename`, enum identities use their map key, and asset-type identities use `typename`. Duplicate entries, engine/workspace collisions, malformed sections, or mismatched workspace type/default sets reject the complete merge without returning a partial catalog. A missing, failed, or unloaded workspace module produces a fresh engine-only editor snapshot so custom types cannot survive through stale runtime state.

## SDK Limitations

`Sailor::Runtime` is a consistent CMake target name, not a cross-version C++ ABI guarantee. Build the engine and game module with compatible compiler versions, toolsets, configurations, and C runtime settings.

The install tree does not bundle vcpkg packages, the Vulkan SDK, or runtime dependency DLLs. It is a CMake development package for a matching build environment, not a self-contained or redistributable SDK.
