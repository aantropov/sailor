# Building and running Sailor

[Project overview](README.md) · [CI and releases](CONTRIBUTING.md)

## Desktop packages

Download an archive from [GitHub Releases](https://github.com/aantropov/sailor/releases).

The release targets are **Windows x64**, **macOS Apple Silicon (arm64)** and
**Linux x64** (Ubuntu 24.04 or a compatible distribution with glibc 2.39+).
Each archive includes the editor, native runtime, content, matching C++ SDK,
dependency headers/libraries, MCP bridge, game-build CLI and a SHA-256 checksum.
Extract the entire archive, then open `SailorEditor.cmd` on Windows,
`SailorEditor.command` on macOS, or run `./SailorEditor.sh` on Linux.
Linux uses `.tar.gz` to preserve executable permissions and symlinks; Windows and
macOS use `.zip`. All three editors include their .NET runtime.
Windows needs a Vulkan-capable graphics driver. macOS packages are ad-hoc signed,
not Apple-notarized; allow the application through Privacy & Security if prompted.
Compiling C++ workspace logic also requires CMake and a C++20 toolchain, such as
Visual Studio 2022 or the Xcode command-line tools.
Windows SDK consumers also need the Vulkan SDK 1.3 or newer. On macOS, install
the Vulkan headers with `brew install vulkan-headers` before compiling C++ modules.
The Linux editor uses the GTK4 desktop head. On Ubuntu 24.04 install its native
runtime dependencies and a Vulkan driver:

```bash
sudo apt-get install libgtk-4-1 libvulkan1 libuuid1 libasound2t64 \
  libicu74 libssl3t64 libgssapi-krb5-2 zlib1g
```

To build C++ workspace modules on Linux, also install CMake, a C++20 toolchain and
`libvulkan-dev`. GPU drivers remain supplied by the operating system.

### macOS first launch

Version 0.9.0 is ad-hoc signed and has not been notarized by Apple. Safari marks
downloaded files with a quarantine attribute, so macOS may block the launcher or app.
After trying to open it, go to **System Settings → Privacy & Security → Open Anyway**.
See [Apple's instructions](https://support.apple.com/en-us/102445).

For a trusted archive downloaded from this repository's Releases, you can also
remove quarantine from that extracted package and run its launcher:

```sh
xattr -dr com.apple.quarantine "$HOME/Downloads/Sailor-0.9.0-macos-arm64"
"$HOME/Downloads/Sailor-0.9.0-macos-arm64/SailorEditor.command"
```

Adjust the path if you extracted the archive elsewhere. Distributing an app with
[Developer ID signing and notarization](https://developer.apple.com/documentation/security/notarizing-macos-software-before-distribution)
is a separate release-signing step.

## Build from source

Native builds require Python, CMake and a C++20 compiler. The editor uses .NET 10;
headless editor contract tests also require the .NET 9 runtime. C++ dependencies
are installed through the repository's vcpkg submodule.

```sh
git clone --recurse-submodules https://github.com/aantropov/sailor.git
cd sailor
```

After updating the repository or submodules, run `python3 update_deps.py` again.
The [release workflow](.github/workflows/release-desktop.yml) records the SDK,
workload and toolchain versions used for published packages.

### Windows x64

Use Visual Studio 2022 with C++ tools, the Vulkan SDK, .NET 10 and the MAUI Windows
workload. Run from a developer terminal:

```powershell
python update_deps.py
python build_engine.py --config Release --target SailorExec --arch x64
python run_editor.py --config Release
```

### macOS Apple Silicon

Install Xcode and a .NET SDK/MAUI workload compatible with that Xcode version.
Install the native dependencies and build:

```sh
brew install vulkan-headers molten-vk shaderc ninja
python3 update_deps.py
python3 build_engine.py --config Release --target SailorExec
python3 run_editor.py --config Release
```

## Linux (x86_64)

The Vulkan runtime uses GLFW. The editor shares the existing MAUI interface through the
[experimental GTK4 backend](https://github.com/dotnet/maui-labs/tree/main/platforms/Linux.Gtk4)
and displays resolved scene frames through the engine's CPU readback path. GTK widget
composition defaults to Cairo; the scene renders with Vulkan. Install .NET 10
for the editor; the editor contract tests also require the .NET 9 runtime.

On Ubuntu 24.04, install the native build dependencies:

```sh
sudo apt-get install -y build-essential clang-18 cmake ninja-build pkg-config autoconf automake libtool \
  libvulkan-dev vulkan-tools libshaderc-dev glslc glslang-tools libssl-dev libasound2-dev \
  libx11-dev libxrandr-dev libxi-dev libxcursor-dev libxinerama-dev \
  libwayland-dev libxkbcommon-dev libgtk-4-dev xvfb xauth
```

Initialize dependencies and build the runtime and editor:

```sh
git submodule update --init --recursive
CC=clang-18 CXX=clang++-18 python3 update_deps.py
python3 build_engine.py --config Release --target SailorExec
python3 run_editor.py --config Release
```

For native behavioral tests, configure with `-DSAILOR_BUILD_TESTS=ON`, build the test
targets, and run `ctest --test-dir build-linux --output-on-failure`. The Linux window
lifecycle test uses Xvfb when available.

The opt-in GPU regression `Binaries/VulkanSubmissionTests --gpu-depth-occlusion
--workspace /path/to/sailor --null-audio` exercises the production depth prepass,
Hi-Z pyramid and culling shaders with even/odd pyramids, direct/named resources
and contiguous/paged instance payloads. It verifies exact visible IDs and every
depth sample against drawing without culling. Run hardware GPU regressions in a desktop session with a Vulkan-capable driver.
Xvfb is used by the separate window lifecycle test.
`--gpu-default-depth-msaa2` checks the native color/depth targets and resolves,
including rejection by depth, resize and retained command lists with MSAA requested.

Package a workspace into a new directory outside that workspace:

```sh
python3 package_linux.py --workspace /path/to/workspace --output /path/to/Sailor-Linux
/path/to/Sailor-Linux/Sailor.sh --world Example.world
```

The package includes the executable, engine library, workspace content, project settings
and third-party license notices. Manifest workspaces also include the selected configuration
of their logic module and engine content. Build that module before packaging. A Vulkan-capable driver and the system libraries used
by the build are required on the destination machine.

## Desktop game builds

Open **Build → Build Game…** in the editor. Select the native target platform and
Debug or Release, choose folders or compressed bundles, select compression and optionally
include the initial cache. The same window saves independent Windows/Linux/macOS
options in the project's version-1 `BuildSettings.json` and supports cancellation.
Builds use saved project files on disk.

Engine users can run the same exporter with the .NET 10 SDK:

```sh
dotnet run --project Tools/SailorBuild -c Release -- \
  --engine /path/to/sailor --workspace /path/to/project \
  --configuration Release --assets Zip --compression Balanced \
  --output /path/to/new-game-build
```

Use a new output directory outside the project and engine. The exporter stages the
complete game before publishing that directory; cancellation or failure removes
the staged output. Existing builds are preserved. It builds the runtime and game
module in matching configurations, installs the matching SDK, and compiles the
existing generated game CMake project through that SDK. Module outputs are kept
separate from the authoring project's binaries. Prepared project and engine content is included as folders or `.bundle` archives,
together with project settings and license notices.
The exporter copies content into its staging directory and runs the engine's CPU
asset import there before sealing it. Raw glTF models get material, texture and
animation metadata without changing the authoring project. Invalid metadata,
failed imports or an incompatible game module stop the export before publication.

`--assets Folders` uses native copying with exact file modification times. ZIP
compression options are `Stored`, `Fastest`, `Balanced` and `Smallest`.
`--initial-cache` includes the portable cache subset described below; warm the
authoring project's cache with the selected runtime configuration first.
`--save-settings` retains the options for that platform.

The default engine build directory is `Cache/GameBuild/<platform>/<configuration>`.
`--engine-build-dir` selects an existing engine build directory for the requested
configuration. `--no-build-runtime` reuses that configured runtime; the game module
is still built through its SDK. `--engine` can also select an installed SDK that
contains the selected standalone runtime in `bin/`.

Launch the result using `Sailor.cmd`, `Sailor.sh` or `Sailor.command`. macOS exports
include relocated non-system dylibs (including MoltenVK) with local ad-hoc signatures;
distribution signing/notarization is a separate release step. Linux exports include
non-system library dependencies and require the destination's compatible system
libraries and Vulkan driver. Windows exports include runtime DLL dependencies and
available Debug PDBs. Release exports also include the compiler's redistributable
MSVC runtime. Windows Debug builds require the matching Visual C++ debug runtime
on the target development machine; use Release for distribution. Available targets depend on the installed compilers and SDKs. Cross-platform
builds require a configured toolchain for the selected target.

## Asset bundles

The engine executable can create ZIP assets without starting the renderer:

```sh
SailorEngine-Release --pack-assets /project/Content /package/Content.bundle balanced
SailorEngine-Release --describe-workspace /project
```

Use the executable from your build or installed runtime (with `.exe` on Windows).
Compression options are `stored` (raw ZIP without compression), `fastest`, `balanced`
and `smallest` (deflate). The archive destination must be a new file in an existing
directory outside the source tree. Symbolic links and special source files are rejected.

A game package explicitly mounts content archives using `GamePackage.json` next to
its workspace manifest:

```json
{
  "packageVersion": 1,
  "packageId": "d898c4f65e5246b88fa55d96bc0f1a87",
  "initialCache": null,
  "archives": [
    { "root": "Content", "file": "Content.bundle" },
    { "root": "Engine/Content", "file": "EngineContent.bundle" }
  ]
}
```

Mount roots identify the workspace or engine content directories. Archive paths
must remain inside the package. Omit an archive entry to keep that content as ordinary
folders. Workspaces without `GamePackage.json` continue to use native folders.

Archived content is read-only and is read directly through seekable streams; games do
not extract the content tree at startup. Each stream uses a bounded decoder buffer,
and full-file reads verify CRC. The version-1 internal ZIP index preserves exact file
timestamps for asset and shader cache validation. The runtime cache remains writable
outside the package, under `Sailor/Packages/<packageId>` in the user's cache directory
(`LOCALAPPDATA`, `~/Library/Caches`, or `XDG_CACHE_HOME` / `~/.cache`). Generate a new
32-character lowercase hexadecimal package ID for each export. Resolving a package
does not create directories inside it.

To include an optional initial cache, run the matching configuration's executable:

```sh
SailorEngine-Release --pack-cache /path/to/workspace /path/to/game/InitialCache.bundle balanced
```

Set `initialCache` in `GamePackage.json` to
`"InitialCache.bundle"`. On the first launch the runtime installs the seed into writable
user storage and binds its shader envelope to the running workspace. Subsequent
launches preserve that cache. The seed includes `ShaderCache.yaml`, `CompiledShaders`,
`CompiledShadersWithDebug` and `Lods`. Shader fingerprints use mount kind, virtual
paths and source revisions so moving the package does not invalidate them. Asset
indexes containing absolute source paths, editor state, native driver caches and
build files are rebuilt locally. Warm the cache using the runtime configuration
being exported; an incompatible or invalid shader envelope fails cache export.

