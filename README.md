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

Package a workspace into a new directory outside that workspace:

```sh
python3 package_linux.py --workspace /path/to/workspace --output /path/to/Sailor-Linux
/path/to/Sailor-Linux/Sailor.sh --world Example.world
```

The package includes the executable, engine library, workspace content, project settings
and third-party license notices. Manifest workspaces also include the selected configuration
of their logic module and engine content. Build that module before packaging. A Vulkan-capable driver and the system libraries used
by the build are required on the destination machine.

## Sponsorship and Contribution

**Support Sailor Engine's Development**  
Your support can help drive the development of new features, improved performance, and expanded documentation. Become a sponsor and play a role in shaping the future of Sailor Engine.

- [Sponsor the Project](https://github.com/sponsors/aantropov)

**Contribute to Sailor Engine**  
We welcome contributions from developers of all levels! Whether you want to fix a bug, add a feature, or improve documentation, your help is invaluable.

- Fork the repository and submit a Pull Request.
- Discuss features, ideas, or issues.
