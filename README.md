# Sailor Engine

**Sailor Engine** is an independent real-time engine for games, visual effects, interactive art, and experimental demos. It combines a bindless `Vulkan 1.3` renderer with a multithreaded `C++20` runtime, custom memory allocators, and a data-driven frame graph.

The editor is built in **C# with .NET MAUI**, with component-based scene authoring, landscape tools, and project build workflows. Its MCP integration lets AI assistants work directly with scenes, assets, and components through the editor.

Sailor is a long-running passion project rooted in a lifetime of work with computer graphics. The engine's core was originally designed and written by hand, with a focus on extensible rendering and creative experimentation.

**In active development.** Current desktop targets are Windows and macOS. Linux, Android, and iOS ports are under evaluation.

## Showcase

[**First Look: watch the showcase on YouTube**](https://youtu.be/WgifakC2LU0)

Please visit the Wiki to explore the engine's technical design in detail: [**Technical Overview and Details**](https://github.com/aantropov/sailor/wiki).

## Get Started

The **Sailor** Wiki provides a centralized resource for all aspects of the engine [Build Instructions and Documentation](https://github.com/aantropov/sailor/wiki)
Visit the Wiki to explore everything you need to get started, understand the engine's internals, and contribute to its development.

**Set sail on your game development journey with Sailor Engine!**

## Screenshots

Four native 4K captures. Click an image to view it at full resolution.

Terrain, vegetation, water, and atmospheric lighting.

[![ForestRiver: a wooded river valley with vegetation shadows and atmospheric lighting](.github/media/forest-river.jpg)](.github/media/forest-river.jpg)

Physically based materials, reflections, and runtime global illumination.

[![DemoSailor Light Pavilion: material spheres, reflections, and indirect lighting](.github/media/light-pavilion.jpg)](.github/media/light-pavilion.jpg)

Raymarched forms and interactive visuals for art installations.

[![SwarmAlien: a human-shaped swarm rendered against a black background](.github/media/swarm-alien.jpg)](.github/media/swarm-alien.jpg)

### Editor

The C# / .NET MAUI editor, with the Content browser expanded and the Atmosphere object selected in the Inspector.

[![Sailor Editor: the Light Pavilion scene, expanded Content folders, and SkyComponent settings](.github/media/editor-pavilion.jpg)](.github/media/editor-pavilion.jpg)

<details>
<summary>Screenshot notes and asset credits</summary>

Captured with the Ultra preset and resolution factor 1.0. MSAA 8x applies to the relevant raster passes; SwarmAlien uses its own raymarching pass. The editor screenshot includes the complete application window. SwarmAlien uses synthetic audience input in this capture, not a live webcam recording.

ForestRiver includes [Poly Haven](https://polyhaven.com) assets (CC0). The pavilion includes [RobotExpressive](https://github.com/mrdoob/three.js/tree/dev/examples/models/gltf/RobotExpressive) by Tomas Laulhe / Quaternius, with modifications by Don McCurdy (CC0).

</details>

## Features

- **Rendering:** Bindless Vulkan renderer, data-driven frame graph, Forward+ lighting, physically based materials, GPU frustum and occlusion culling, and mesh LODs.
- **Lighting and atmosphere:** Baked and runtime GI probes, local reflections, cascaded shadow maps, atmospheric sky, volumetric clouds, and fog.
- **Visual effects:** Raymarching, GPU particles, CPU path tracing, MSAA, camera and object motion blur, bloom, exposure, and tone mapping.
- **Worlds and simulation:** Component-based scenes, ECS, skeletal animation, Jolt physics, and landscape and vegetation streaming.
- **Core:** Task-based multithreading, parallel render preparation, custom memory allocators, and engine-owned containers.
- **Editor:** C# / .NET MAUI, component and asset inspectors, landscape authoring, and workspace build and reload tools.
- **AI-assisted workflows:** MCP tools for scene creation, asset inspection, component editing, and editor automation.
- **Engine development:** C++20 with MSVC and Clang; extensible components, ECS systems, and rendering nodes for project-specific behavior.

## Dependencies

This project uses [vcpkg](https://github.com/microsoft/vcpkg) to manage C++ dependencies. Run `python update_deps.py` to install the required packages.

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
depth sample against drawing without culling. Use `xvfb-run -a` on headless Linux.

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
Debug or Release, choose folders or ZIP assets, select compression and optionally
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
separate from the authoring project's binaries. All project/engine content is
included initially, together with project settings and license notices.
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
on the target development machine; use Release for distribution. Each platform
is built on its native host.

## Asset archives

The engine executable can create ZIP assets without starting the renderer:

```sh
SailorEngine-Release --pack-assets /project/Content /package/Content.zip balanced
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
    { "root": "Content", "file": "Content.zip" },
    { "root": "Engine/Content", "file": "EngineContent.zip" }
  ]
}
```

Keep the content root directories themselves in the package, even when empty. Mount
roots must match the workspace or engine content directories, and all archive paths
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
SailorEngine-Release --pack-cache /path/to/workspace /path/to/game/InitialCache.zip balanced
```

Keep an empty `InitialCache` directory in the package and set `initialCache` to
`"InitialCache.zip"`. On the first launch the runtime installs the seed into writable
user storage and binds its shader envelope to the running workspace. Subsequent
launches preserve that cache. The seed includes `ShaderCache.yaml`, `CompiledShaders`,
`CompiledShadersWithDebug` and `Lods`. Shader fingerprints use mount kind, virtual
paths and source revisions so moving the package does not invalidate them. Asset
indexes containing absolute source paths, editor state, native driver caches and
build files are rebuilt locally. Warm the cache using the runtime configuration
being exported; an incompatible or invalid shader envelope fails cache export.

## Sponsorship and Contribution

**Support Sailor Engine's Development**  
Your support can help drive the development of new features, improved performance, and expanded documentation. Become a sponsor and play a role in shaping the future of Sailor Engine.

- [Sponsor the Project](https://github.com/sponsors/aantropov)

**Contribute to Sailor Engine**  
We welcome contributions from developers of all levels! Whether you want to fix a bug, add a feature, or improve documentation, your help is invaluable.

- Fork the repository and submit a Pull Request.
- Discuss features, ideas, or issues.
