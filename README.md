# Sailor Engine
## About
Game Engine engine prototype made with the focus on high-performance and usability, based on multi-threading and bindless renderer.
The engine is render agnostic (with limitations) by design but currently have the implementation of `Vulkan 1.3` only.
Code is written using a `C++` coding style. Project is written for the `C++20` standard and `x64` platform. 
Currently the code is tested only on `Windows`, using `MSVC` (Visual Studio). 

## Table of Contents
- [Concept](#Concept)
  - [Why?](#Why)
  - [Building for Windows](#WindowsBuild)
- [Asset Management](#AssetManagement)
  - [Concept](#AssetManagementConcept)
  - [AssetInfo and AssetFile](#AssetInfo)
  - [Asset Importers](#AssetImporters)
- [Rendering](#Rendering)
  - [Concept](#RenderingConcept)
  - [Vulkan Graphics Driver](#Vulkan)
  - [Shaders Compilation](#Shader)
  - [GPU Memory Management](#GPUMM)
    - [GPU Allocator](#GPUMMAllocator)
    - [GPU Managed Memory Pointers](#GPUMMMM)
  - [Frame Graph](#FrameGraph)
- [Memory Management](#MemoryManagement)
  - [Concept](#MMConcept)
  - [Memory Allocator](#MemoryAllocator)
  - [Smart Pointers](#SmartPointers)
- [Multi-Threading](#MultiThreading)
  - [Concept](#MTConcept)
  - [Threads](#MTThreads)
  - [ECS](#ECS)
  - [Tasks](#Tasks)
- [Game Code](#GameCode)
  - [Concept](#GameCodeConcept)
  - [ECS](#ECS)
  - [GameObjects and Components](#Components)
- [Third Parties](#ThirdParties)
  
## Screenshots
![image](https://github.com/aantropov/sailor/assets/3637761/352593a9-ee55-4444-b884-6bd30ece53bd)
![image](https://github.com/aantropov/sailor/assets/3637761/2ca20784-5784-4e77-a824-8e3032919785)
![3](https://user-images.githubusercontent.com/3637761/216842844-0312267c-52a6-41d4-ba6d-a2b785fb7725.png)
![image](https://user-images.githubusercontent.com/3637761/216842799-a3d871fe-0f46-4cb5-9c9f-fb9cbc713f8d.png)
![2](https://user-images.githubusercontent.com/3637761/216842838-920cb66e-a79b-4ddd-8883-314eb60ae958.png)
