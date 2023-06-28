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
- [Infrastructure](#Infrastructure)
- [Asset Management](#AssetManagement)
  - [AssetInfo and AssetFile](#AssetInfo)
  - [Asset Importers](#AssetImporters)
- [Memory Management](#MemoryManagement)
  - [Memory Allocators](#MemoryAllocators)
  - [Smart Pointers](#SmartPointers)
- [Multi-Threading](#MultiThreading)
  - [Threads](#MTThreads)
  - [ECS](#ECS)
  - [Tasks](#Tasks)
- [Game Code](#GameCode)
  - [ECS](#ECS)
  - [GameObjects and Components](#Components)
- [Rendering](#Rendering)
  - [Vulkan Graphics Driver](#Vulkan)
  - [Shaders Compilation](#Shader)
  - [GPU Memory Management](#GPUMM)
    - [GPU Allocator](#GPUMMAllocator)
    - [GPU Managed Memory Pointers](#GPUMMMM)
  - [Frame Graph](#FrameGraph)
- [Feature List](#FeatureList)
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

## <a name="Infrastructure"></a> Infrastructure
#### Repository infrastructure
There are 2 main projects in MSVC solution, `SailorLib` and `SailorExec`, which have 2 binary outputs, `Sailor.lib` and `Sailor.exe`, correspondingly.
- `/Runtime` folder contains the game engine source code
- `/Content` folder contains assets.
- `/Content/Shaders` contains shaders.
- `/Cache` folder consists of temporary objects, such as compiled shader binaries, precalculated data, etc..
- Third parties are located under `/External` folder and added to the repository as git submodules.

#### Core functionaly
The `Sailor's` core functionality is implemented with a number of `TSubmodule<T>` that allows to control lifetime/order of initialization and decrease the coupling. The idea under internal submodules is based on:
- Implement a high level layer of abstraction with 'single responsibility' instances.
- Avoid the usage of singletons and make the code more friendly for test writing.
- Create a simple mechanism that allows easy add new core functionality.

This code creates the instance of `CustomSubmodule`
```
// Declaration
class CustomSubmodule : public TSubmodule<CustomSubmodule> { ... };

...

// Usage
App::AddSubmodule(TSubmodule<CustomSubmodule>::Make());
App::GetSubmodule<CustomSubmodule>()->Function();
App::RemoveSubmodule<CustomSubmodule>();
```

## <a name="AssetManagement"></a> Asset Management
The idea under `Sailor's` `AssetManagement` is similar to `Unity's` approach. For each asset, game engine generates `.asset` meta file under the same folder, which stores detailed information about the asset, how the asset should be imported, id, import time and other parameters.

The engine handles timestamps of files in meta, and uses them to find outdated assets.
The game engine API operates with unique ids `UIDs` which are generated during asset import and stored in the '.asset' files.

### <a name="AssetInfo"></a> AssetInfo and AssetFile
`AssetInfo` class is a base class of meta information which stores just `UID`, timestamps and asset filename. 
`AssetInfo` instances are serialized/deserialized to `Yaml` format which is chosen by its human readability.

Each asset type (model, texture, render config, material, shader and others) has derived meta, which 'extends' the base POD class with extra properties, 
`ModelAssetInfo`, `TextureAssetInfo`, `MaterialAssetInfo` and others.

### <a name="AssetImporters"></a> Asset Importers

`AssetRegistry` is a main submodule which contains scan functionality, handles the asset infos library and registers/unregisters `AssetInfoHandlers`.
`IAssetInfoHandler` is an interface which provides callbacks for basic asset importers logic, such as dispatcherization by extension during asset importing/loading.
`IAssetInfoHandlerListener` is an interface which provides callbacks for resolving outdated assets.

`ModelImporter`, `TextureImporter`, `MaterialImporter` and others are submodules with loading logic, also these instances handle the loaded assets and prevent them from being destructed.
There is no basic class for importers, but all of them follows the general approach:
- They have async loading method `Tasks::TaskPtr<bool> AssetImporter::LoadAsset(UID uid, AssetPtr& outAsset);`
- They have instant loading method `bool AssetImporter::LoadAsset_Immediate(UID uid, AssetPtr& outAsset);`
- They contain `ObjectAllocator`, which handle all assets of the same type in one place.
- They resolve loading promises and hot reload logic.

This code loads `Sponza.obj` model asynchroniously, while the 'contract' returns a valid `ObjectPtr` instance immediately. 
```
ModelPtr pModel = nullptr;
if (auto modelUID = App::GetSubmodule<AssetRegistry>()->GetAssetInfoPtr<ModelAssetInfoPtr>("Models/Sponza/sponza.obj"))
{
  App::GetSubmodule<ModelImporter>()->LoadModel(modelUID->GetUID(), pModel);
}

...

if(!pModel->IsReady())
{
  SAILOR_LOG("We should wait a bit");
}
```

## <a name="MemoryManagement"></a> Memory Management
A explanation of the few different memory management systems in `Sailor`: Memory Allocators, Smart Pointers, and Standard C++ Memory Management.
The engine is designed to use a number of memory management approaches to achieve the best results in aspect of Memory Management and all memory management related code is located under `/Runtime/Memory` folder. 

### <a name="MemoryAllocators"></a> Memory Allocators
There are different types of allocators persist in the engine, and all of them are designed to solve different problems.

Global allocators contain static 'allocate/free' functions together with non static 'Allocate/Free' methods to allow engine developers use different allocation strategies, including the inline allocators. 
- `MallocAllocator` - that is a global allocator that contains contract for the usage of malloc/free from standart library.
- `LockFreeHeapAllocator` - that is a thread-safe global allocator based on `HeapAllocator` (by design we have an instance of HeapAllocator per thread).

The main CPU allocator is
- `HeapAllocator` - that is an allocator inspired by `id Tech 4` engine. The allocator uses different allocation strategies for different sizes of allocations: pools for small, intrusive lists for medium and 'malloc/free' for huge.

The pointer agnostic allocators are slow but used to handle any kind of memory, especially GPU memory management is based on them:
- `TBlockAllocator` - Uses block allocation strategy. Designed as universal allocator for GPU memory management. Economic but slow.
- `TPoolAllocator` - Uses pooling as main allocation strategy. Faster than `TBlockAllocator` but have more memory consumption. Could be used for GPU texture memory management.
- `TMultiPoolAllocator` - Uses multi pools per allocation size. Faster than `TPoolAllocator`, but have much more memory consumption.

The pointer agnostic allocators are based on `TMemoryPtr<T>` and `TManagedMemory<T>` which provides the contract for memory operations such as 'shift', 'offset' pointers and others.

- `ObjectAllocator` - That is a main 'game-thread' allocator which is used for tracking of game objects and asset instances. All high level entities such as `Components`, `GameObjects`, `Textures`, `Models`, and others must be created with 
the instance of the `ObjectAllocator`. The allocator is thread safe.
The core idea is to have many `ObjectAllocators` which store the objects by its scopes: `AssetImporters` stores all `Assets` by its types in the same memory, `World` also has the main `ObjectAllocator` which stores all world's game objects and its data together, etc..
That allows directly splits memory by different logic and better hit the cache.

### <a name="SmartPointers"></a> Smart Pointers
`Sailor` has a custom implementation of C++11 smart pointers designed to ease the burden of memory allocation and tracking. 
This implementation includes the industry standard `Shared Pointers`, `Weak Pointers`, and `Unique Pointers`.
It also adds `Object Pointer` which is designed for high-level, game memory management and `Ref Pointer` which is fast, simple pointer used in the renderer's code.

- `TSharedPtr` - A Shared Pointer owns the object it references, indefinitely preventing deletion of that object, and ultimately handling its deletion when no Shared Pointer references it. 
A Shared Pointer can be empty, meaning it doesn't reference any object.
- `TWeakPtr` - Weak Pointers are similar to Shared Pointers, but do not own the object they reference, and therefore do not affect its lifecycle. This property can be very useful, as it breaks reference cycles, but it also means that a Weak Pointer can become null at any time, without warning. 
For this reason, a Weak Pointer can produce a Shared Pointer to the object it references, ensuring programmers safe access to the object on a temporary basis.
- `TUniquePtr` - A Unique Pointer solely and explicitly owns the object it references. Since there can only be one Unique Pointer to a given resource, 
Unique Pointers can transfer ownership, but cannot share it. Any attempts to copy a Unique Pointer will result in a compile error. 
When a Unique Pointer is goes out of scope, it will automatically delete the object it references.
- `TRefPtr` - A Reference Pointer owns the object in the similar way of `TSharedPtr`, but the technical realisation is intrusive and stored objects must be derived from `TRefPtrBase`. The pointer occupies only 8 bytes on x64 platform.
- `TObjectPtr` - An Object Pointer owns the object in the similar way of `TSharedPtr`, but allows programmer forcely delete the object without producing of dangling pointers. 
There must be specified the instance of `ObjectAllocator` to create the instance of `ObjectPtr`.

## <a name="MultiThreading"></a> Multi-Threading
### <a name="MTThreads"></a> Threads
### <a name="ECS"></a> ECS
### <a name="Tasks"></a> Tasks

## <a name="GameCode"></a> Game Code
### <a name="ECS"></a> ECS
### <a name="GameObjects and Components"></a> Components

## <a name="Rendering"></a> Rendering
## <a name="FeatureList"></a> Feature List
## <a name="ThirdParties"></a> Third Parties
