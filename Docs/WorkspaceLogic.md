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

Workspace modules export one V1 API-table entry point:

```cpp
const Sailor::Workspace::WorkspaceModuleApiV1*
SailorGetWorkspaceModuleApiV1() noexcept;
```

`SailorGetWorkspaceModuleApiV1` returns a module-owned static POD table. It contains its structure size and API version, module name, exact build ABI tag and the explicit type-registration callback. The table and its strings remain valid until the module is unloaded. The ABI tag is independently compiled into the engine and game module and includes the ABI revision, architecture, compiler version, C runtime, iterator-debug mode, build configuration, and engine interface identity; the host requires an exact match before dereferencing module TypeInfo or calling registration.

CMake generates `Workspace/WorkspaceInterface.h` from the project-owned runtime headers, YAML dependency boundary, public target settings, and C++ build flags. A build dependency checks header contents on every build and rewrites the generated header only when the identity changes; header edits, additions and removals do not require a manual reconfigure. The generated header is available through `Sailor::Runtime` and is installed with the SDK. Input filenames are relative to the source root; relocating identical sources and settings preserves their identity. Private `.cpp` changes alone do not invalidate it. Header changes conservatively require a logic rebuild even when they happen to preserve ABI. This is an interface identity, not an ABI analyzer: consumers must use the SDK's dependency versions and compile settings rather than overriding packing or other ABI options. API and metadata schema versions remain 1.

Engine and workspace property schemas use the same `TypeInfo::Serialize` implementation, including inherited getter-only properties. `Transient` getters are omitted; `SkipCDO` getters remain part of the schema but not the default-object snapshot. The same reflection traversal exports referenced enums and nested value types, including list elements. Value types have editor schemas and default snapshots, but do not become registered component factories.

Template-local TypeInfo and default snapshots belong to their library. Each DLL initializes and destroys its own copy; sharing an engine value record means deduplicating its schema, not sharing a static object that another DLL can destroy during unload.

Registration passes a POD host table with an opaque context and collection callback. Each collected POD descriptor supplies an opaque `TypeInfo` pointer, size, alignment and a `noexcept` placement factory. Type and base names come from `TypeInfo`, not separate descriptor strings. The generated callback only collects descriptors from `WorkspaceTypes`; it does not mutate engine registries. Workspace types must be default-constructible `Sailor::Component` subclasses, and `Component` must be their zero-offset object base; the generated factory rejects unsupported multiple-inheritance layouts before returning an object. Registration callbacks use the C calling convention. The ABI-checked TypeInfo remains module-owned; it supplies the schema and lazily captures one typed default object through common reflection. Neither the host nor the editor constructs a second default object to validate a copy.

The host builds the editor catalog from the collected TypeInfo objects and their captured defaults. There is no separate module metadata export or independently serialized descriptor-default payload.

The YAML document uses `metadataVersion: 1`, identifies `moduleName`, and preserves the existing consumer keys `timeStamp`, `engineTypes`, `cdos`, `enums`, and `assetTypes`. The generated sample exports `moveSpeed: float` with a default value of `5.0`.

Game modules compile with `SAILOR_WORKSPACE_MODULE`, which enables `SAILOR_WORKSPACE_REFLECTABLE`; every reflected game type must use that macro so it does not install a static registration helper. Engine types continue using `SAILOR_REFLECTABLE` unchanged, and `Reflection::ExportEngineTypes()` remains engine-only. The runtime lifecycle loads the DLL, validates the API and ABI before calling registration, preflights the complete collected set, and commits only accepted workspace types. Preflight checks unique component identities, size/alignment, the reflected Component hierarchy, ambiguous or shadowed names and enum defaults, including nested records and lists. Registration publishes the complete accepted set atomically. Typed reflection supplies property names, types and values; the loader does not parse and compare a second schema or decode defaults into another object.

Workspace placement factories run without the reflection registry mutex held, so constructors may perform reflection lookup. Each module owner has an invocation barrier: unregister first hides all of its types from new construction, waits for active factories to return, and only then permits the library to close. This barrier does not extend the lifetime of returned objects; worlds and every other workspace object must still be destroyed before module unload.

The loader retains the prepared workspace catalog until unload. Editor requests merge that prepared catalog with the caller's current engine metadata; they do not parse the module payload or validate its property schemas again. Returned nodes and registry defaults own independent YAML data, so editing a returned catalog cannot change later requests. A failed merge leaves its output and the retained workspace catalog unchanged. Shared engine-owned value records and enums appear once; their schemas and defaults must agree. Component identities cannot be shared between owners.

The editor type cache includes a hash of the loaded module's schema and defaults in its producer identity. The loader computes it once from the prepared metadata, excluding the export timestamp. Restarting with changed reflected types invalidates the previous catalog; restarting the same module preserves its identity. This does not change other asset-cache identities. The editor still requests and publishes the live catalog before publishing the world.

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

Engine Content is never mutated by discovery. An Engine asset without metadata is diagnosed and skipped rather than imported or assigned newly generated metadata. Source rewrites and generated asset metadata are likewise restricted to the active writable Content root; derived runtime data may still be written to the workspace Cache. The engine-owned `Shaders/Constants.glsl` is generated only in Engine mode, where the active writable Content root and Engine Content are the same physical directory. A distinct workspace consumes that existing library through the read-only Engine mount and never creates a local override. Plain-text and binary content reads, including shader includes, particle data, and star-catalog data, use the same virtual-path resolver and workspace-over-engine precedence as registered assets.

Shader `includes` are declared in `.shader` YAML as Content-root-relative virtual paths, for example `Shaders/Math.glsl`. Every include resolves through the same Workspace-over-Engine winner policy, so a workspace file can deliberately override an Engine library and removing that override reveals the Engine fallback. A missing include is a hard compilation error: no partial precompiled source, SPIR-V, or shader-cache entry is published. Content rescans invalidate every registered dependent shader when the effective winner is added, removed, replaced, or changed, even when that shader has not previously been loaded. Native GLSL preprocessor `#include` directives and recursive include discovery are not supported; shared libraries must be listed explicitly in the `.shader` YAML `includes` sequence.

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

No non-`Loaded` result publishes decoded state. Asset and shader payloads are decoded into temporary containers and swapped into their live caches only after complete validation. Shader payload version 3 assigns every entry a collision-resistant immutable generation and requires structurally complete regular and debug artifact sets with identical stage topology. It also stores an exact source-dependency fingerprint covering the `.shader` source and every effective YAML include: virtual path, winning mount and normalized physical identity, and a nanosecond-mtime, byte-size, and content-hash revision. Cache reuse requires fingerprint equality, so same-timestamp edits, backdated edits, and Workspace/Engine winner transitions all expire the entry. Metadata records the byte length and checksum of every present artifact; truncated, misaligned, same-size checksum-mismatched, or missing required SPIR-V is corrupt rather than a partial cache hit. Editor type metadata is cleared before startup cache handling, the native runtime identity is compared with the exact editor launch identity after initialization, and only a fully validated combined type catalog may be published.

Invalidation is scoped by cache ownership:

- AssetCache owns only `Cache/AssetCache.yaml`. A missing, stale, corrupt, or unsupported load clears its in-memory map and atomically writes an empty current envelope without deleting neighboring files.
- ShaderCache owns `Cache/ShaderCache.yaml` plus `Cache/PrecompiledShaders/`, `Cache/CompiledShaders/`, and `Cache/CompiledShadersWithDebug/`. It verifies that those paths remain contained by the resolved Cache root before deleting or recreating them. Artifact garbage collection uses only the last successfully committed metadata snapshot as its whitelist, so an envelope failure cannot delete the generation still referenced on disk; other cache files and user-authored Content are never part of shader invalidation.
- EditorTypes owns `Cache/EditorTypes.yaml` and same-directory temporary files matching its own generated name pattern. Stale, corrupt, and unsupported caches are invalidated by removing only that target and its owned temporary files; it does not sweep the Cache directory.

An `IoFailure` is not evidence that stored bytes are invalid. All three consumers clear or withhold in-memory state after an unreadable cache, but preserve the existing target and owned artifacts so a transient sharing, antivirus, permission, or device error cannot destroy a potentially valid cache. After an AssetCache `IoFailure`, ordinary updates and shutdown preserve existing storage; only an explicit forced rebuild or `ClearAll` authorizes replacement. ShaderCache additionally enters a read-only storage quarantine when either envelope loading or later runtime artifact validation encounters an I/O failure: compilation may publish complete regular/debug bytecode in memory for the current session, but save, invalidation, artifact cleanup, and precompiled writes do not touch disk. Missing, corrupt, stale, unsupported, and repeated I/O reload results keep that quarantine and its session state intact. Only a fully successful reload discards the session-only state and restores persistent access; `ClearAll` is the explicit destructive escape hatch.

Cache YAML and shader artifacts are replaced through same-directory temporary files. The complete temporary file is written and flushed before an atomic replace exposes it; native POSIX writes also synchronize the containing directory, while Windows replacement requests write-through behavior. Shader compilation first writes all regular and required debug artifacts under a new immutable generation, then makes that complete generation eligible for the next metadata commit. Remove and expiry cleanup take the inverse order: they atomically commit candidate metadata before garbage-collecting artifacts no longer referenced by the committed snapshot. A successful shader save also sweeps the replaced immutable generation only after the new envelope commits. A failure before replacement leaves the previous target and generation intact and triggers best-effort temporary-file cleanup. If a durability synchronization reports failure after replacement, the target is still a complete old or new file, never a partially written one. Native cache callers retain dirty state after any reported save failure; shader compile-all performs a save-only retry when bytecode is current but its metadata is still dirty. Metadata is not committed as clean until replacement and post-commit cleanup succeed. An editor-type persistence failure likewise preserves the validated live catalog and reports the failed write instead of publishing unvalidated disk data.

Workspace switching therefore preserves A → B → A isolation. A cache copied from workspace A is `StaleIdentity` in B and is never published, so B starts from empty producer-owned state and writes a B envelope. Returning to A can load only data whose complete identity still matches A. Transactional editor activation additionally clears in-memory asset, world, and editor-type projections before the candidate starts, preventing an asynchronous result from the prior workspace from bypassing the on-disk identity check.

For manifest version 1, the runtime resolves the module as `<resolved-logic-output>/<CONFIG>/<platform-module-name>`. `WorkspaceModuleManager` consumes the captured context and never reparses the manifest. It canonicalizes the final module path again immediately before loading and rejects any symlink or junction change observed by that check. Concurrent mutation of the workspace or build tree during the platform's pathname-only library-loader call is not supported. The active CMake configuration is part of both the path and ABI check, so a stale Debug/Release binary fails with rebuild guidance instead of being loaded as a compatible module.

The dynamic library is opened before asset importers scan the resolved Workspace Content and Engine Content mounts. Asset, shader, precompiled-shader, and editor-type caches use the resolved Cache directory, including custom paths with spaces. The generated shader constants library is updated only in Engine mode; workspace startup mounts the existing Engine copy read-only. Static engine registration callbacks triggered by the platform loader are suppressed for that load operation; only the explicit V1 descriptor callback can add workspace types. The host validates the complete descriptor and metadata set before committing it under a module owner. Missing libraries, loader dependencies, entry points, incompatible API/ABI, metadata errors, and registration collisions return structured non-crashing diagnostics.

Standalone startup treats a configured module or requested-world activation failure as fatal and returns a nonzero exit code. Editor startup reports the same error but remains available with an empty world so the project can be repaired. During shutdown, worlds, importers, the asset registry, scheduler, and remaining submodules are destroyed before workspace registrations are removed and the library is closed. Runtime hot reload and live workspace switching are not supported by this contract.

## World And ECS Updates

EngineLoop has one active world, returned by `GetWorld()`. The first attached
world is active; additional worlds are dormant scene candidates, not parallel
simulations. A successful editor replacement attaches its candidate before
queuing the old world for exit. Processing exits drains rendering, clears the
retired world and promotes the next attached world. Failed construction leaves
the active world unchanged. `GetWorlds()` lists owned worlds, including dormant
candidates; an empty loop returns no active world. Dependency resolution may
visit every owned world, but only the active world receives a CPU frame.

The FrameState passed to `ProcessCpuFrame` must name that active world. Its
resource-update slot 0 belongs to this world and slot 1 to ImGui. Frame copies
own independent input/timing storage while retaining command lists and the
prepared ImGui task through Sailor smart pointers. Moving a frame transfers its
storage without allocating. World retirement drains render work before clearing
the world borrowed by a frame; a frame does not extend the world's lifetime.

ECS `Tick(float)` and `PostTick()` are synchronous `void` methods. A system's
frame-visible CPU results are complete when it returns. Local jobs such as Jolt
stepping or mesh preparation may run in parallel, but their owner joins them
before publishing their results. Background asset/GI preparation can remain
pending for a later frame; queued audio/backend commands own their input values.
Neither is represented by a task returned from ECS Tick.

World updates use this order after gameplay or editor callbacks:

1. Resolve authored transforms.
2. Synchronize and step physics when simulation is enabled.
3. Resolve transforms written by physics.
4. Tick the remaining systems, then call PostTick.

`GetOrder()` orders the remaining publication systems, such as cameras, lights,
audio and mesh rendering. It does not move a system across the transform/physics
boundary. A completed transform update consumes its dirty queue; a subsequent
phase with no new edits does no transform work. `GetECS<T>()` is a lookup and
does not register a missing system.

Editor preview still resolves authored transforms and publishes render data,
without starting gameplay or accumulating disabled physics time. Rebuild C++
workspace modules against the current engine headers after an ECS interface
change; old module binaries are not compatible with a changed virtual interface.

### Directional Shadows

Realtime rendering has one cascaded-shadow owner per world and a separate CSM
set for each camera. The lowest registered light slot that is active, contributes
to realtime lighting and requests directional shadows owns that set. Other
directional lights still illuminate the scene, but their published shadow mode
is None. Their authored settings are unchanged. Disabling or removing the owner,
or changing it to Baked Only, transfers the cascades to the next eligible light.
This selection does not restrict the CPU path tracer's light/shadow support.

CSM matrices and textures always use the same selected light. Per-flight shadow
resources remain owned by LightingECS's plain shadow state, separate from light
storage and the configured memory budget. Retained scene publications keep
their original light data, matrices and bindings when a later flight changes
the selected owner.

## Native Viewport Hosts

The macOS UI transfers its CAMetalLayer through a synchronous local native
call, before the handler may remove and dispose the layer. Connect and
disconnect publish host changes immediately, even before a usable layout
exists. Native object addresses are not queued through the editor's asynchronous
protocol lane for this handoff.

MacNativeHostHandle owns one Objective-C reference; copied pending and
presentation handles retain their own reference. The viewport's serialized
update/presentation code applies the latest host. The UI handoff does not
create Metal resources or wait for the presentation lock. A later frame applies
detach even without another layout update. Replacing or destroying a pending
host releases it, while a binding already in use keeps its original layer alive.

The local handoff participates in the existing protocol lifecycle gate.
Shutdown closes admission and drains accepted operations before releasing App
state, including pending hosts that never reached their first bind. C++ and
managed clients must be rebuilt together for the local native export; the
serialized protocol version remains 1.

## Input Ownership

The CPU-frame owner is the only writer of gameplay input and the engine ImGui
context. Native window callbacks enqueue owned `Platform::InputEvent` values;
`GlobalInput::ProcessPendingEvents` swaps the pending batch before applying it.
The short queue lock does not cover ImGui, gameplay callbacks or rendering. Text
owns its UTF-8 bytes across this handoff. Frame input remains a value snapshot.

Standalone startup pumps native messages on the window's creating thread, then
consumes the queued input before building a frame. Embedded editor mode uses
the host's UI pump and discards events from the hidden native rendering window.
Remote viewport events are consumed on engine Main through the same input
handler. Session input admission remains separate from the GPU transport lock.

Remote gameplay input has one active session, not a separate keyboard/mouse
state for every viewport. A current packet can establish an idle input source;
after that, focus gain, capture gain or a mouse press transfers ownership.
Unfocused hover and another viewport's focus/capture loss do not take over or
reset the active source. Losing the active source releases its keys and buttons.

Queued input carries a weak reference to its originating binding as well as its
epoch/generation. Reusing a viewport ID cannot make an old packet current. Stale
packets are discarded without clearing accepted input. Resets share the input
queue and affect only their owning binding; resize queues its reset before
advancing the generation. Stamping and enqueueing are ordered together. The
frame owner also checks its last accepted session when the queue is empty, so
destroying or invalidating a session cannot leave held input behind. Mouse and
modifier reconciliation uses the actual gameplay state, not duplicate caches.

Pointer-button events include their client coordinates, and wheel values use
scroll steps for both gameplay and ImGui. Native focus loss releases keys,
buttons and press origins without resetting absolute cursor/wheel origins into
fake next-frame deltas. Platform cursor capture remains owned by the native
window thread. Direct `GlobalInput` setters and `ApplyEvent` are frame-owner
operations, not APIs for UI producers.

Cursor shape travels in the opposite direction as one atomic snapshot of the
last prepared ImGui frame. Windows applies it on `WM_SETCURSOR` without reading
the ImGui context on its UI thread. A hidden cursor differs from no override;
`NoMouseCursorChange` and ImGui shutdown return ownership to the native host.

This is an internal runtime event contract, not a new editor protocol version.
The remote protocol still has its own supported event kinds; normalized native
text delivery does not imply that remote text/IME producers are implemented.

## ImGui Context Lifetime

`ImGuiApi` owns the context created for its App session. Shutdown drains CPU/RHI
tasks and GPU work before releasing prepared frames, texture bindings, platform
and renderer backends, and finally the context on the CPU owner. A failed GPU
drain retains the session for retry; it must not destroy the context early.

Workspace code borrows `ImGuiApi::GetCurrentContext()` only for the active
session. Modules that statically link ImGui must bind that context and the
allocator functions returned by `ImGuiApi::GetAllocatorFunctions()` before UI
work. Neither a context pointer nor a prepared frame may be retained across App
shutdown. Rebind on the next session; a pointer from the previous run is invalid.
The engine clears its ImGuizmo context binding during teardown. Module-private
ImGui globals remain the module's responsibility, not engine-owned contexts.

### Draw Callbacks

`ImDrawList::AddCallback` runs later on an RHI worker while recording the prepared
frame, not during the component's UI update. A callback must not read or modify
the live ImGui context or other mutable CPU-frame state. The built-in
`ImDrawCallback_ResetRenderState` remains supported.

Pass a nonzero data size for a frame-owned copy of a plain payload. The snapshot
copies bytes, not C++ object ownership: do not put owning smart pointers in that
payload. Any pointed-to resources still need an external owner until the RHI
reader finishes. With size zero, the payload itself is borrowed too; the caller
must keep its storage alive and unchanged through that reader. Stack data from
the UI update is not a valid borrowed payload.

App shutdown drains those readers before destroying world components or
unloading the workspace module that implements a callback. This shutdown order
does not extend arbitrary object lifetimes during gameplay. Component removal,
resource replacement and module unloading must respect any outstanding readers.

## Editor Type Metadata

`SerializeEngineTypes` is available as an engine-only protobuf command for consumers that need the unmodified engine catalog. Editor consumers normally use the distinct `SerializeEditorTypes` protobuf command, which starts from a fresh engine snapshot and appends the validated metadata of the active workspace module. Both commands use the shared authenticated binary WebSocket transport; neither adds a dedicated C export. The combined document keeps each custom component's fully-qualified `typename`, reflected properties, and default object values.

The merge validates all four catalogs before publishing a result. Type and CDO identities use `typename`, enum identities use their map key, and asset-type identities use `typename`. Duplicate entries, engine/workspace collisions, malformed sections, or mismatched workspace type/default sets reject the complete merge without returning a partial catalog. A missing, failed, or unloaded workspace module produces a fresh engine-only editor snapshot so custom types cannot survive through stale runtime state.

## SDK Limitations

`Sailor::Runtime` is a consistent CMake target name, not a cross-version C++ ABI guarantee. Build the engine and game module with compatible compiler versions, toolsets, configurations, and C runtime settings.

The install tree does not bundle vcpkg packages, the Vulkan SDK, or runtime dependency DLLs. It is a CMake development package for a matching build environment, not a self-contained or redistributable SDK.
