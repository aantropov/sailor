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
Generated/
  CMakeLists.txt
Cache/
  Build/
Binaries/
.gitignore
```

`Generated/CMakeLists.txt` is created with the workspace and is not overwritten when the workspace is opened or saved. It can be edited when the project needs custom CMake behavior, while game code remains under `Source/`. Build-system intermediates belong in `Cache/Build`; configuration-specific logic modules are written to `Binaries/<CONFIG>/`.

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

For a default Windows Release workspace, the resulting module and import library are `Binaries/Release/SailorGame.dll` and `Binaries/Release/SailorGame.lib`. Link and compile PDB outputs use the same configuration directory. Debug and other configurations use their matching subdirectory. Reconfigure after changing the engine reference, paths, or module name in the workspace manifest.

## SDK Limitations

`Sailor::Runtime` is a consistent CMake target name, not a cross-version C++ ABI guarantee. Build the engine and game module with compatible compiler versions, toolsets, configurations, and C runtime settings.

The install tree does not bundle vcpkg packages, the Vulkan SDK, or runtime dependency DLLs. It is a CMake development package for a matching build environment, not a self-contained or redistributable SDK.
