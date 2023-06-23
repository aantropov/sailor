# Sailor Engine
## About
The `Sailor` is a game engine prototype made with the focus on high-performance and usability. The technical solution uses multithreading and has a bindless renderer made with `Vulkan 1.3`.

The engine is render agnostic (with limitations) by design.
The codebase is written using a `C++` coding style and the project is written for the `C++20` standard for `x64` platform. 
Currently the code is tested only on `Windows`, using `MSVC` (Microsoft Visual Studio). 

## Screenshots

<p float="left">
  <img src="https://github.com/aantropov/sailor/assets/3637761/2135ae32-b95b-4bdb-bbbb-ec1e01b40946" width="400" />
  <img src="https://github.com/aantropov/sailor/assets/3637761/2ca20784-5784-4e77-a824-8e3032919785" width="400" />
  <img src="https://github.com/aantropov/sailor/assets/3637761/6ac3e042-7519-43c3-b7ab-a1e14b7b4df2" width="400" />
  <img src="https://user-images.githubusercontent.com/3637761/216842844-0312267c-52a6-41d4-ba6d-a2b785fb7725.png" width="400" />
</p>

## Table of Contents
- [Concept](#Concept)
  - [Why?](#Why)
  - [Building for Windows](#WindowsBuild)
- [Asset Management](#AssetManagement)
  - [AssetInfo and AssetFile](#AssetInfo)
  - [Asset Importers](#AssetImporters)
- [Rendering](#Rendering)
  - [Vulkan Graphics Driver](#Vulkan)
  - [Shaders Compilation](#Shader)
  - [GPU Memory Management](#GPUMM)
    - [GPU Allocator](#GPUMMAllocator)
    - [GPU Managed Memory Pointers](#GPUMMMM)
  - [Frame Graph](#FrameGraph)
- [Memory Management](#MemoryManagement)
  - [Memory Allocator](#MemoryAllocator)
  - [Smart Pointers](#SmartPointers)
- [Multi-Threading](#MultiThreading)
  - [Threads](#MTThreads)
  - [ECS](#ECS)
  - [Tasks](#Tasks)
- [Game Code](#GameCode)
  - [ECS](#ECS)
  - [GameObjects and Components](#Components)
- [Third Parties](#ThirdParties)

## <a name="Concept"></a> Concept
The `Sailor` is an engine that is made with the focus on the usability as for engine programmers as for gameplay programmers.
In spite of the fact that the editor hasn't been implemented yet, the project contains a number of features that makes the development
fast and easy. 

The game engine follows `Unity's` ideology in aspects of Asset Management, GameObjects and Components. It has hot reload for assets including tracking of outdated shader permutations.
On the other hand, gameplay code is written in C++, and could be easy based on ECS or Components (single responsibility) approaches. Multithreading is available in game threads and the game instance's memory is tracked by explicit allocators.

The high-level rendering uses FrameGraph, similar to `Frostbite` engine. The lighting is calculated with Tile-Based Forward Rendering (Forward+). Overall the renderer is made with the idea of parallelism. The compute shaders are greatly used for many general graphics calculations.

The coding standard is similar to `CryEngine`, while the contract of containers is inspired by `Unreal Engine`. The codebase is written with the following idea under the hood: the C++ code should be readable and highly optimized simultaneously, templates are used where it solves the issues, and the code is a bit simplified overall. Also Task's contract is similar to C#'s Tasks.

### <a name="Why"></a> Why?
Why is the engine called 'Sailor'?
- The Sailor is a tool that helps you to ship.
   
Why do you need to 'write one more renderer'?
- The Sailor is not just a renderer. That's designed as a game engine and contains functionality that is usually avoided in 'just renderer' projects, for example: Tasks, Hot reloading, Shader compilation & reflection, Material system and Memory allocators.

Why did you decide to write the basic code such as containers, window creation, etc.. with your own?
- By technical and conceptual reasons.

### <a name="WindowsBuild"></a> Windows build instructions
- [Download and install Windows Vulkan sdk](https://vulkan.lunarg.com/)
- [Download and install Windows cmake x64 installer](https://cmake.org/download/)
  - Make sure to select "Add cmake to the system Path for all users"
- Download repository including submodules

#### Building for Visual Studio 2019/2022

- In windows powershell

```
 cd Sailor
 mkdir Build
 cmake -S . -B .\Build\
```

- If cmake finished successfully, it will create a Sailor.sln file in the build directory that can be opened with visual studio. Right click the SailorExec project -> set as startup project. Change from debug to release, and then build and start without debugging.
