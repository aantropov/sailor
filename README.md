# Sailor Engine

**An independent C++20 game engine with a Vulkan renderer and a desktop editor.**

[Download](https://github.com/aantropov/sailor/releases) · [Watch the showcase](https://youtu.be/WgifakC2LU0) · [Documentation](https://github.com/aantropov/sailor/wiki) · [Build from source](BUILDING.md)

[![Sailor Engine — terrain, vegetation, water and atmospheric lighting in ForestRiver](.github/media/forest-river.jpg)](.github/media/forest-river.jpg)

Sailor brings together real-time rendering, component-based worlds and tools for
building games, visual effects and interactive scenes. The native runtime is
written in C++; the editor uses C# and .NET MAUI, with an experimental GTK4 head
on Linux.

This is a long-running personal project rooted in computer graphics. The engine's
core was originally written by hand, with an emphasis on extensible rendering,
multithreading and engine systems.

**In active development.** Desktop packages are available for Windows x64,
macOS Apple Silicon and Linux x64. Linux editor support is experimental.

## Inside the engine

- **Rendering:** Bindless Vulkan 1.3, a data-driven frame graph, Forward+ lighting,
  physically based materials, GPU frustum/occlusion culling and mesh LODs.
- **Lighting:** Baked and runtime GI probes, local reflections, cascaded shadows,
  atmospheric sky, volumetric clouds and fog.
- **Effects:** Raymarching, GPU particles, CPU path tracing, MSAA, camera and object
  motion blur, bloom, exposure and tone mapping.
- **Worlds:** Components and ECS, skeletal animation, Jolt physics, landscape
  authoring and vegetation streaming.
- **Runtime:** Task-based multithreading, parallel render preparation, custom
  memory allocators and engine-owned containers.
- **Extensibility:** C++ components, ECS systems and rendering nodes, plus MCP
  tools for editor automation and AI-assisted scene work.

## Scene editor

Create scenes, inspect components and materials, edit landscapes, work with
prefabs, and build standalone games. Projects have their own content and C++
logic modules, with build and reload tools in the editor.

[![Sailor Editor — the Light Pavilion scene, Content browser and Atmosphere inspector](.github/media/editor-pavilion.jpg)](.github/media/editor-pavilion.jpg)

## More from Sailor

| Lighting and global illumination | Raymarching and interactive visuals |
| --- | --- |
| [![Light Pavilion — physically based materials, reflections and GI](.github/media/light-pavilion.jpg)](.github/media/light-pavilion.jpg) | [![SwarmAlien — a raymarched humanoid form](.github/media/swarm-alien.jpg)](.github/media/swarm-alien.jpg) |

The [showcase video](https://youtu.be/WgifakC2LU0) brings these scenes together.
Click any screenshot to view the original 4K capture.

## Get started

Download the matching archive from [GitHub Releases](https://github.com/aantropov/sailor/releases),
extract the whole package, and start the editor:

| Platform | Launcher |
| --- | --- |
| Windows x64 | `SailorEditor.cmd` |
| macOS Apple Silicon | `SailorEditor.command` |
| Linux x64 | `./SailorEditor.sh` |

Packages include the editor, runtime, engine content and matching C++ SDK.
See the [installation and build guide](BUILDING.md) for system requirements,
[macOS first launch](BUILDING.md#macos-first-launch), source builds and game exports.
The [Wiki](https://github.com/aantropov/sailor/wiki) covers engine design and usage.

## Contribute

[Report a bug](https://github.com/aantropov/sailor/issues), open a pull request,
or [support the project](https://github.com/sponsors/aantropov).
Development, CI and release instructions are in [CONTRIBUTING.md](CONTRIBUTING.md).

<details>
<summary>Screenshot notes and asset credits</summary>

Captured with the Ultra preset and resolution factor 1.0. MSAA 8x applies to the
relevant raster passes; SwarmAlien uses its own raymarching pass. Its audience
input is synthetic in this capture. The editor screenshot shows the complete
application window.

ForestRiver includes [Poly Haven](https://polyhaven.com) assets (CC0). The pavilion
includes [RobotExpressive](https://github.com/mrdoob/three.js/tree/dev/examples/models/gltf/RobotExpressive)
by Tomas Laulhe / Quaternius, with modifications by Don McCurdy (CC0).

</details>
