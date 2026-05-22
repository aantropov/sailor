# Sailor Engine

**Sailor Engine** is a high-performance game engine prototype with a bindless graphics renderer based on `Vulkan 1.3` and efficient multithreading. It features task management, advanced memory allocators, and a data-driven design for modern game development.

Please visit the Wiki to explore the engine's technical design in detail: [**Technical Overview and Details**](https://github.com/aantropov/sailor/wiki).

## Get Started

The **Sailor** Wiki provides a centralized resource for all aspects of the engine [Build Instructions and Documentation](https://github.com/aantropov/sailor/wiki)
Visit the Wiki to explore everything you need to get started, understand the engine's internals, and contribute to its development.

**Set sail on your game development journey with Sailor Engine!**

## Screenshots

### Realtime (PBR, Forward+, Raymarching, CSSM, Tone Mapping GPGPU, etc)
<p float="left">
  <img src="https://github.com/aantropov/sailor/assets/3637761/2135ae32-b95b-4bdb-bbbb-ec1e01b40946" width="400" />
  <img src="https://github.com/aantropov/sailor/assets/3637761/2ca20784-5784-4e77-a824-8e3032919785" width="400" />
  <img src="https://github.com/aantropov/sailor/assets/3637761/6ac3e042-7519-43c3-b7ab-a1e14b7b4df2" width="400" />
  <img src="https://user-images.githubusercontent.com/3637761/216842844-0312267c-52a6-41d4-ba6d-a2b785fb7725.png" width="400" />
</p>

### Pathtracer (CPU, GLTF)
<p float="left">
  <img src="https://github.com/aantropov/sailor/assets/3637761/086c1dc0-f7f4-47a4-bd0f-d2cef4e84fd8" width="400" />
  <img src="https://github.com/aantropov/sailor/assets/3637761/84e58446-65d2-4358-90ec-8d6a59a81076" width="400" />
</p>

### Editor
<img width="1131" height="837" alt="Screenshot 2026-05-22 at 22 51 40" src="https://github.com/user-attachments/assets/1b313ae5-1eda-42eb-8a0e-99cdf976d932" />


## Features

- **Rendering:** Bindless Renderer, Frame Graph, Forward+ Lighting, PBR, Raymarching, CPU-based Path Tracing.
- **Core:** Multithreading, custom memory allocators.
- **Editor:** Built with `.NET MAUI`.
- **Engine:** Built with `C++20` (`MSVC` and `clang`).

## Dependencies

This project uses [vcpkg](https://github.com/microsoft/vcpkg) to manage C++ dependencies. Run `python update_deps.py` to install the required packages.

## Sponsorship and Contribution

**Support Sailor Engine's Development**  
Your support can help drive the development of new features, improved performance, and expanded documentation. Become a sponsor and play a role in shaping the future of Sailor Engine.

- [Sponsor the Project](https://github.com/sponsors/aantropov)

**Contribute to Sailor Engine**  
We welcome contributions from developers of all levels! Whether you want to fix a bug, add a feature, or improve documentation, your help is invaluable.

- Fork the repository and submit a Pull Request.
- Discuss features, ideas, or issues.
