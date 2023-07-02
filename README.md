# Sailor Engine
## About
`Sailor` is a high-performance game engine prototype, designed with an emphasis on optimization and usability. It boasts a bindless graphics renderer built on `Vulkan 1.3` and uses multithreading effectively. Designed to be render agnostic, the engine doesn't rely on any specific rendering technology (some limitations notwithstanding). The code adheres to the C++ coding style and conforms to the `C++20` standard for the `x64` platform. Please note, the code has been tested only on Windows operating system with the `MSVC` compiler (`Microsoft Visual Studio`).

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
  - [Repository Infrastructure](#RepoInfra)
  - [Core Functionality](#CoreFunc)
- [Asset Management](#AssetManagement)
  - [AssetInfo and AssetFile](#AssetInfo)
  - [Asset Importers](#AssetImporters)
- [Memory Management](#MemoryManagement)
  - [Memory Allocators](#MemoryAllocators)
    - [Global Allocators](#GlobalAllocators)
    - [Heap Allocator](#HeapAllocator)
    - [Pointer Agnostic Allocators](#PointerAgnosticAllocators)
    - [Object Allocator](#ObjectAllocator)
  - [Smart Pointers](#SmartPointers)
- [Multi-Threading](#MultiThreading)
  - [Scheduler](#Scheduler)
  - [Tasks](#Tasks)
    - [Task Scheduling and Execution](#TaskScheduling)
    - [Task Dependencies and Continuations](#TaskDependencies)
    - [Benefits of Using Tasks](#TasksBenefits)
- [Game Code](#GameCode)
  - [GameObjects and Components](#GameObjectsandComponents)
    - [World](#World)
    - [GameObject](#GameObject)
    - [Component](#Component)
  - [ECS](#ECS)
    - [Adding Custom ECS Systems](#ECSCustom)
- [Rendering](#Rendering)
  - [Vulkan Graphics Driver](#VulkanGraphicsDriver)
  - [GPU Memory Management](#GPUMemory)
  - [Frame Graph](#FrameGraph)
- [Feature List](#FeatureList)
  - [Rendering Features](#RenderingFeatures)
  - [Lighting and Shadows](#LightingFeatures)
  - [Performance and Optimization](#PerformanceFeatures)
- [Third Parties](#ThirdParties)

## <a name="Concept"></a> Concept
`Sailor` is a purposefully designed engine that emphasizes usability for both engine and gameplay programmers. While the engine editor is currently not implemented, the project boasts a range of features that streamline development. 

Adhering to `Unity's` ideology, `Sailor` employs strategies such as Asset Management, GameObjects, and Components. Furthermore, it offers a hot reload for assets, including tracking of outdated shader permutations. Gameplay code, written in `C++`, easily aligns with ECS or Components (single responsibility) approaches. Multithreading is available in game code, and the game instance's memory is tracked by explicit allocators.

The high-level rendering utilizes FrameGraph, mirroring the approach of the `Frostbite` engine. Lighting is calculated using Tile-Based Forward Rendering (Forward+), with the overall renderer designed around the concept of parallelism. Compute shaders are heavily utilized for general graphics calculations.

The coding standard mirrors that of `CryEngine`, with the contract of containers drawing inspiration from `Unreal Engine`. The codebase aligns with the belief that C++ code should be simultaneously readable and highly optimized. Templates are employed where they resolve issues, and overall, the code is somewhat simplified. Additionally, the Task's contract resembles that of C#'s Tasks.

### <a name="Why"></a> Why?
Why is the engine named '`Sailor`'?
- `Sailor`, as a tool, assists you in setting your game development voyage underway. `Sailor` is the best in shipping.

Why develop 'yet another renderer'?
- `Sailor` extends beyond a simple renderer. It's conceived as a comprehensive game engine and incorporates functionalities often overlooked in 'mere renderer' projects, such as Tasks, Hot reloading, Shader compilation & reflection, Material system, and Memory allocators.

Why write the fundamental code such as containers, window creation, etc. on your own?
- This decision was driven by technical and conceptual considerations.

### <a name="WindowsBuild"></a> Windows Build Instructions
Follow the steps below to build the `Sailor` project on a Windows platform:

1. [Download and install the Windows Vulkan SDK](https://vulkan.lunarg.com/): Vulkan SDK provides all necessary tools, libraries and headers to develop Vulkan applications.
2. [Download and install the Windows cmake x64 installer](https://cmake.org/download/): CMake is a tool that helps manage the build process of software using compiler-independent methods.
    - During the installation process, make sure to select "Add cmake to the system PATH for all users".
3. Download the `Sailor` repository including all its submodules: The repository contains all the source code and assets needed to build and run the `Sailor` project.

#### Building for Visual Studio 2019/2022

To build the project for Visual Studio 2019/2022, execute the following commands in Windows PowerShell:

```
 cd Sailor
 mkdir Build
 cmake -S . -B .\Build\
```

Upon successful completion of the cmake process, a `Sailor.sln` file will be generated within the `Build` directory. You can open this file with Visual Studio. To run the project:

1. In Visual Studio, right-click the `SailorExec` project and select "Set as Startup Project".
2. Switch the project configuration from "Debug" to "Release".
3. Finally, select "Build Solution" to build the project and then "Start Without Debugging" to run it.

## <a name="Infrastructure"></a> Infrastructure
### <a name="RepoInfra"></a> Repository Infrastructure

The MSVC solution contains two main projects, namely `SailorLib` and `SailorExec`, resulting in two binary outputs: `Sailor.lib` and `Sailor.exe`, respectively. The repository is structured as follows:

- `/Runtime`: This directory contains the game engine source code.
- `/Content`: This directory stores assets.
- `/Content/Shaders`: This directory is for shaders.
- `/Cache`: This directory is designated for temporary objects such as compiled shader binaries, precalculated data, etc.
- `/External`: Third-party dependencies are located under this directory and are included in the repository as git submodules.

### <a name="CoreFunc"></a> Core Functionality

The core functionality of Sailor is implemented using a set of `TSubmodule<T>` classes. These allow for control over initialization order/lifetime and reduce coupling between components. The design philosophy behind these internal submodules is to:

- Provide a high-level layer of abstraction with instances adhering to the 'single responsibility' principle.
- Avoid the use of singletons to make the code more test-friendly.
- Develop a simple mechanism to facilitate the addition of new core functionalities.

The following code snippet illustrates how to create an instance of `CustomSubmodule`:

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

`Sailor`'s asset management system draws inspiration from `Unity`'s approach. For each asset, the game engine generates a `.asset` metafile in the same directory. This file stores detailed information about the asset, including how it should be imported, its unique identifier (UID), import time, and other relevant parameters.

The engine manages file timestamps in metafiles and uses these to detect outdated assets. The game engine API operates with UIDs, which are generated during asset import and stored in the '.asset' files.

### <a name="AssetInfo"></a> AssetInfo and AssetFile

The `AssetInfo` class serves as a base for storing metadata such as the UID, timestamps, and the asset filename. Instances of `AssetInfo` are serialized/deserialized into a human-readable `Yaml` format.

Each type of asset (e.g., model, texture, render config, material, shader) has its own derived meta class that extends the base POD class with extra properties. Examples include `ModelAssetInfo`, `TextureAssetInfo`, `MaterialAssetInfo`, and more.

### <a name="AssetImporters"></a> Asset Importers

The `AssetRegistry` submodule handles scanning, manages the asset info library, and registers/unregisters `AssetInfoHandlers`. The `IAssetInfoHandler` interface provides callbacks for fundamental asset importer logic, like dispatching by extension during asset importing/loading. The `IAssetInfoHandlerListener` interface provides callbacks for resolving outdated assets.

Asset importers, such as `ModelImporter`, `TextureImporter`, and `MaterialImporter`, contain loading logic. These instances manage loaded assets and prevent their destruction. While there's no base class for importers, they all follow a common pattern:

- Each has an asynchronous loading method: `Tasks::TaskPtr<bool> AssetImporter::LoadAsset(UID uid, AssetPtr& outAsset);`
- Each has an immediate loading method: `bool AssetImporter::LoadAsset_Immediate(UID uid, AssetPtr& outAsset);`
- Each contains an `ObjectAllocator`, which handles all assets of the same type in one place.
- Each resolves loading promises and hot reload logic.

The following code snippet illustrates the asynchronous loading of the `Sponza.obj` model. Note that the 'contract' returns a valid `ObjectPtr` instance immediately.

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
Memory Management in `Sailor` encompasses several different systems, including Memory Allocators, Smart Pointers, and Standard C++ Memory Management. The engine is purposefully designed to leverage these varied memory management strategies to optimize both performance and efficiency. All related code for memory management is conveniently located in the '/Runtime/Memory' folder.

### <a name="MemoryAllocators"></a> Memory Allocators
The `Sailor` engine incorporates a diverse range of memory allocators designed to address specific requirements and optimize memory usage. These allocators play a crucial role in enhancing performance and efficiency within the engine.

#### <a name="GlobalAllocators"></a> Global Allocators
The engine includes global allocators that offer both static and non-static allocation methods, enabling developers to leverage different allocation strategies, including inline allocators. The following global allocators are available:

- `MallocAllocator`: This global allocator provides a contract for utilizing the malloc and free functions from the standard library. It offers a familiar interface for memory management and allocation.
- `LockFreeHeapAllocator`: As a thread-safe global allocator, the `LockFreeHeapAllocator` is based on the `HeapAllocator` design, with each thread having its own dedicated instance. This ensures efficient and secure memory allocation in a multi-threaded environment.

#### <a name="HeapAllocator"></a> Heap Allocator
`HeapAllocator` in `Sailor` draws inspiration from the `id Tech 4` engine. It utilizes distinct allocation strategies based on the size of the allocations. This allocator employs the following strategies:

- Pools: For small-sized allocations, `HeapAllocator` employs pool-based allocation, which efficiently manages memory for these allocations.
- Intrusive Lists: Medium-sized allocations utilize intrusive lists, enabling optimized memory usage for this category of allocations.
- `malloc`/`free`: Large-sized allocations are handled using the standard malloc and free functions from the `C++` standard library.

#### <a name="PointerAgnosticAllocators"></a> Pointer Agnostic Allocators
The engine also includes pointer agnostic allocators that are primarily used for handling various types of memory, particularly for GPU memory management. These allocators are slower in performance but offer versatility in managing different memory types. The pointer agnostic allocators in `Sailor` include:

- `TBlockAllocator`: This allocator employs a block allocation strategy and serves as a universal allocator for GPU memory management. While it may have lower performance characteristics, it offers an economical solution for managing GPU memory.
- `TPoolAllocator`: Using pooling as its main allocation strategy, the `TPoolAllocator` outperforms the `TBlockAllocator` in terms of speed. It is well-suited for GPU texture memory management, albeit with a slightly higher memory footprint.
- `TMultiPoolAllocator`: The `TMultiPoolAllocator` takes pooling to the next level by utilizing multiple pools per allocation size. This results in improved performance compared to the `TPoolAllocator`, although it does come with increased memory consumption.

These pointer agnostic allocators leverage `TMemoryPtr<T>` and `TManagedMemory<T>` to provide a standardized interface for various memory operations, including pointer shifting and offset calculations.

#### <a name="ObjectAllocator"></a> Object Allocator
The `ObjectAllocator` serves as the primary allocator for game objects and asset instances within the 'game-thread'. It offers thread-safe tracking and management of these entities. All high-level entities, such as components, game objects, textures, and models, must be created using an instance of the `ObjectAllocator`. The allocation of objects within the ObjectAllocator is organized based on scopes, following a similar approach used in the `BitSquid`/`Stingray` engines. This strategy allows for efficient memory control, reducing fragmentation and optimizing cache utilization.

The careful selection and implementation of these memory allocators in `Sailor` contribute to its overall performance, efficiency, and scalability.

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

The folowing code creates the instances of objects and stores them into the pointers.
```
// RefPtr
RHI::RHIFencePtr fenceUpdateRes = RHI::RHIFencePtr::Make();

...

// ObjectPtr
Memory::ObjectAllocatorPtr allocator = Memory::ObjectAllocatorPtr::Make();
ModelPtr model = ModelPtr::Make(allocator, uid);
```

## <a name="MultiThreading"></a> Multi-Threading
The `Sailor` Engine embraces the power of multi-threading to maximize performance and responsiveness. 
This section explores the multi-threading capabilities of the engine and highlights two key components: the Scheduler and Tasks.

### <a name="Scheduler"></a> Scheduler
The Scheduler in the `Sailor` Engine is designed to efficiently manage and distribute tasks across multiple threads, leveraging the available hardware resources to their fullest extent. The engine dynamically adjusts the number of threads based on the logical cores available on the system, ensuring optimal performance.

The Scheduler categorizes threads into different types to handle specific aspects of the engine's operations. These include:
- Main Thread: Responsible for handling critical game logic, input processing, and high-level control flow.
- Render Thread: Handles rendering-related tasks, such as updating the GPU resources, rendering graphics, and managing the rendering pipeline.
- RHI Thread: Focused on the Render Hardware Interface (RHI), responsible for managing low-level rendering operations and interacting with the underlying graphics API.
- Worker Threads: Dedicated threads that handle background tasks, parallel computations, and other non-rendering related operations.

Tasks within the `Sailor` Engine are synchronized using `std::condition_variables`, enabling efficient coordination and synchronization between threads. This synchronization mechanism ensures that tasks are executed in the correct order and that dependencies between tasks are properly managed.

Additionally, the `Sailor` Engine provides flexibility in task execution. Developers have the ability to launch a task on a specific thread or a group of threads, enabling fine-grained control over task allocation and workload distribution. This level of control allows for efficient utilization of resources and can optimize performance for specific scenarios or workloads.

### <a name="Tasks"></a> Tasks
Tasks are a fundamental feature in the `Sailor` Engine, providing a powerful mechanism for managing asynchronous operations and synchronizing dependent tasks. With tasks, you can efficiently handle complex workflows and parallelize computations, leading to improved performance and responsiveness.
#### <a name="TaskScheduling"></a> Task Scheduling and Execution
The `Sailor` Engine utilizes a task scheduler to manage the execution of tasks. The scheduler dynamically distributes tasks across available threads, maximizing parallelism and utilizing the available processing power of the system.

Tasks can be created using the `Tasks::CreateTask()` function, which takes a lambda function representing the task's operation. You can specify the desired thread type for the task, such as `EThreadType::Worker` or `EThreadType::Render`, depending on the nature of the task and its requirements.

#### <a name="TaskDependencies"></a> Task Dependencies and Continuations
Tasks can have dependencies on other tasks, allowing you to create complex task graphs and enforce order of execution. You can use the `Then()` method to specify a continuation function that will be executed once the dependent task completes successfully.

Here's an example that demonstrates the loading of a texture on the `EThreadType::Worker` thread and subsequent initialization on the `EThreadType::Render thread`:
```
newPromise = Tasks::CreateTaskWithResult<TSharedPtr<Data>>("Load Texture",
  [pTexture, assetInfo, this]() mutable
	{
		TSharedPtr<Data> pData = TSharedPtr<Data>::Make();
	  ...
		return pData;
	})->Then<TexturePtr, TSharedPtr<Data>>([pTexture, assetInfo, this](TSharedPtr<Data> data) mutable
		{
			...
			return pTexture;
		}, "Create RHI texture", Tasks::EThreadType::RHI)->ToTaskWithResult()->Run();
```
#### <a name="TasksBenefits"></a> Benefits of Using Tasks
Using tasks in the `Sailor` Engine offers several benefits:
- Improved Performance: Tasks enable parallel execution of independent operations, making efficient use of available CPU resources and reducing processing time.
- Simplified Workflow: Task dependencies and continuations provide a structured way to define the order of operations, making complex workflows easier to manage and reason about.
- Concurrency Control: Tasks provide a built-in mechanism for synchronizing and coordinating dependent operations, ensuring that tasks execute in the correct order.
- Thread Flexibility: The ability to specify the desired thread type for each task allows for fine-grained control over the execution environment, optimizing resource utilization.

Tasks are a powerful tool in the `Sailor` Engine, facilitating the implementation of scalable and efficient systems. Whether it's loading assets, performing calculations, or initializing game systems, tasks enable you to harness the full potential of multi-threading and asynchronous execution.

## <a name="GameCode"></a> Game Code
The "Game Code" section of the `Sailor` engine refers to the user-defined code that implements the specific gameplay and functionality of a game built using the engine. This section includes game objects, components, and the ECS (Entity-Component-System) architecture.

### <a name="GameObjectsandComponents"></a> GameObjects and Components
In the `Sailor` engine, game objects and components play a crucial role in defining the entities and their behavior within the game world. Following the architectural approach similar to `Unity`, `Sailor` promotes the principle of Single Responsibility, where each component has a specific purpose and encapsulates a particular aspect of an entity's behavior.

#### <a name="World"></a> World

The `World` class acts as a container for game objects and takes charge of their lifecycle management within the `Sailor` engine. It offers essential functionalities for creating, updating, and destroying game objects. Notably, `Sailor` supports the existence of multiple independent Worlds, each having its own dedicated `ObjectAllocator`. This ensures efficient memory management by keeping the game memory utilized by the objects within each world separate and organized.

#### <a name="GameObject"></a> GameObject

The `GameObject` class represents an individual game object or entity within the game world. It serves as a container for components and encompasses the behavior and properties of entities. In line with the design pattern observed in `Unity`, each `GameObject` in `Sailor` includes a TransformComponent. This component defines the position, rotation, and scale of the GameObject, allowing for the establishment and management of the scene hierarchy. `GameObject`s can be created, updated, and destroyed dynamically during gameplay.

#### <a name="Component"></a> Component

The `Component` class plays a crucial role in specifying a specific aspect of an entity's behavior. `Component`s in `Sailor` define various properties and behaviors, such as rendering, physics, input handling, and more. These components are attached to game objects and assume the responsibility of updating the entity's state and behavior throughout gameplay. Adhering to the principle of Single Responsibility, each component focuses on a distinct aspect of an entity's behavior, ensuring modular and maintainable code structure.

By leveraging inheritance from the `Object` class, both `GameObject`s and `Components` conform to the requirements of the `ObjectAllocator` in `Sailor`. This inheritance enables efficient memory management and resource allocation for game objects and components, while providing a unified programming interface and shared functionalities, such as memory management and reference counting, across the `Sailor` engine.

The utilization of `GameObject`s and `Component`s, inheriting from the `Object` class, empowers developers with a robust and scalable game architecture. It ensures optimized memory management and resource allocation, while offering a consistent programming interface for the creation and management of game entities.

### <a name="ECS"></a> ECS (Entity-Component-System)

The Entity-Component-System (ECS) architecture in the `Sailor` engine provides a powerful approach for organizing and managing game logic. It separates entities (game objects), their behavior (components), and the systems responsible for updating and processing component data. This architecture promotes modularity, performance, and flexibility in game development.
In addition to managing game objects, the `World` also contains instances of ECS systems. These systems are responsible for processing and updating component data, enabling efficient and structured game logic.

#### <a name="ECSCustom"></a> Adding Custom ECS Systems
Extending the functionality of the ECS architecture in `Sailor` is straightforward. To add a custom ECS system, developers can simply inherit from the `TBaseSystem<TComponentData>` class. Here, `TComponentData` represents a plain-old-data (POD) structure that stores the necessary properties required by the system. By creating a new `TComponentData` structure and inheriting from `TBaseSystem`, developers can define their own custom systems tailored to their game's specific requirements.

The `TBaseSystem<TComponentData>` class provides a solid foundation for creating custom ECS systems. It handles the processing and updating of component data, allowing developers to focus on implementing the specific behavior and functionality required by their game. This modular approach enables easy integration of custom systems into the broader ECS architecture of the `Sailor` engine.

By leveraging the `World` class and extending `TBaseSystem<TComponentData>`, developers can seamlessly incorporate custom ECS systems into their games. This empowers developers to efficiently manage and update component data, enabling the creation of diverse and complex gameplay experiences. The `Sailor` engine's ECS architecture provides the necessary tools and flexibility to build scalable and performant game systems while maintaining code organization and reusability.

## <a name="Rendering"></a> Rendering
`Sailor`'s rendering architecture is designed around a multi-layered approach, aiming to reduce coupling and enhance modularity. At the heart of this architecture lies the Render Hardware Interface (RHI). This abstracts away specifics of the underlying rendering implementation, serving as a central hub that interfaces with the different layers.

The `VulkanGraphicsDriver` layer is an essential component of this system. It is tasked with the high-level orchestration of `Vulkan` rendering, handling `Vulkan` objects directly. These objects, along with the `VulkanAPI`, manifest as RAII (Resource Acquisition Is Initialization) objects and a wrapper over the Raw `Vulkan` API. This layered setup enforces a principle where each layer only interacts with its immediate neighbours. For instance, the `VulkanGraphicsDriver` communicates with `Vulkan` and `RHI` objects, while `Vulkan` objects remain oblivious to the `RHI`. This division of responsibilities fosters a well-organized and robust rendering architecture.

### <a name="VulkanGraphicsDriver"></a> Vulkan Graphics Driver
`Sailor`'s Vulkan Graphics Driver layer is responsible for the high-level orchestration of Vulkan rendering. It manages Vulkan objects and oversees the interactions between the application and the Vulkan API.

Beyond these functionalities, the VulkanGraphicsDriver also resolves any inconsistent DescriptorSets, maintaining an internal cache for their efficient utilization. This feature ensures the smooth operation of rendering processes and contributes significantly to the overall performance of the engine.

### <a name="GPUMemory"></a> GPU Memory Management
Contemporary rendering APIs offer direct memory management access, enabling the grouping of objects and memory sharing among them to minimize GPU allocations. `Sailor` harnesses this capacity and implements its GPU memory management based on Pointer Agnostic Allocators. This strategy closely aligns with monads, with the dereference operator (`*`) being a central concept.

The Vulkan Graphics Driver in `Sailor` accommodates VulkanMemory pointers' implementations and global allocators. These permit various operations like the allocation of diverse memory types and the placement of GPU objects in them, creating sub-buffers within buffers, and sharing buffers across multiple GPU objects.

Moreover, the Vulkan Graphics Driver encompasses a variety of GPU allocators, thereby aiding fragmentation reduction:
```cpp
TConcurrentMap<std::string, TSharedPtr<VulkanBufferAllocator>> m_uniformBuffers;
TSharedPtr<VulkanBufferAllocator> m_materialSsboAllocator;
TSharedPtr<VulkanBufferAllocator> m_generalSsboAllocator;
TSharedPtr<VulkanBufferAllocator> m_meshSsboAllocator;
```

An advanced GPU memory management system eases bindless rendering implementation. The current codebase facilitates the tracking of object placement within the memory.

### <a name="FrameGraph"></a> Frame Graph
At the core of `Sailor`'s scene rendering lies the `FrameGraph`. The `FrameGraph` comprises `RenderNode`s, creating a flexible rendering pipeline. To forge a custom `RenderNode`, it's as straightforward as extending the `FrameGraphNode<YourNodeType, YourNodeName>` base class, utilizing the Curiously Recurring Template Pattern (CRTP). `RenderNode`s register themselves and dynamically generate instances with all necessary parameters upon parsing the '.renderer' file. 

The `Sailor` engine provides a wide array of nodes to empower your rendering pipeline, including but not limited to:
- BlitNode 
- BloomNode 
- ClearNode 
- DebugDrawNode 
- DepthPrepassNode 
- EnvironmentNode 
- EyeAdaptationNode 
- FrameGraphNode 
- LightCullingNode 
- LinearizeDepthNode 
- PostProcessNode 
- RenderImGuiNode 
- RenderSceneNode 
- ShadowPrepassNode 
- SkyNode

Below is a snippet showcasing a sample `FrameGraph` configuration:

```text
samplers: ~
float:
- PI: 3.1415926
renderTargets:
- name: Main
  format: R16G16B16A16_SFLOAT
  width: ViewportWidth
  height: ViewportHeight

frame:
- name: DepthPrepass
  string:
  - Tag: Opaque
  - ClearDepth: true
  renderTargets:
  - depthStencil: DepthBuffer
 
- name: Clear
  vec4:
  - clearColor: [1.502, 2.08, 2.61, 2.5]
  renderTargets:
  - target: Main

- name: RenderScene
  string:
  - Tag: Opaque
  renderTargets:
  - color: Main
  - depthStencil: DepthBuffer
  
- name: Blit
  renderTargets:
  - src: Main
  - dst: BackBuffer

- name: RenderImGui
  renderTargets:
  - color: BackBuffer
  - depthStencil: DepthBuffer
```

## <a name="FeatureList"></a> Feature List
The `Sailor` engine offers a wide range of features designed to enhance game development and provide a solid foundation for building immersive experiences. Here are some key features grouped into different categories:

### <a name="RenderingFeatures"></a> Rendering Features:
- Data Driven Renderer: The engine employs a data-driven approach to rendering, enabling flexible and customizable rendering techniques.
- Bindless Renderer: The engine utilizes a bindless graphics renderer, leveraging Vulkan 1.3 for efficient and high-performance rendering.
- FrameGraph: The engine incorporates a FrameGraph system, facilitating high-level rendering and resource management for efficient rendering pipelines.
- HDR (High Dynamic Range) and Bloom: The engine supports HDR rendering and bloom effects, enhancing visual quality and realism.
- Clouds Rendering: The engine includes advanced clouds rendering techniques, allowing for realistic and dynamic rendering of clouds in the game world.
- Eye Adaptation: The engine includes eye adaptation techniques, allowing for realistic adjustment of exposure and brightness based on scene lighting conditions.
  
### <a name="LightingFeatures"></a> Lighting and Shadows:
- Forward+ Tile Based Rendering: `Sailor` leverages the Forward+ Tile Based Rendering technique, which ensures optimized lighting computations and superior performance even within intricate and detail-rich scenes.
- Cascaded Shadow Maps (CSM) and Exponential Variance Shadow Maps (EVSM): `Sailor` incorporates the methodologies of both CSM and EVSM, resulting in precise, high-quality shadow rendering that enriches visual depth and detail.
- Horizon-Based Ambient Occlusion (HBAO): By supporting HBAO, `Sailor` enhances depth perception and scene realism. This technique improves the overall visual quality by accurately rendering ambient occlusion, thereby enhancing user immersion.
- Physically Based Rendering (PBR): Inspired by Unreal Engine and Disney's material model, `Sailor` employs PBR for realistic rendering. This leads to the creation of materials and lighting effects that mimic real-world physical properties, enhancing the lifelikeness of the rendered scenes.

### <a name="PerformanceFeatures"></a> Performance and Optimization:
- Multi-Threading Support: The engine provides multi-threading capabilities, allowing for parallel execution of tasks and efficient utilization of CPU resources.
- Advanced CPU/GPU Memory Management: The engine incorporates advanced memory management techniques for both CPU and GPU, optimizing memory usage and improving performance.

## <a name="ThirdParties"></a> Third Parties
Here are some of the notable third-party libraries used in the `Sailor` project:

- `Vulkan SDK`: Provides the necessary tools, libraries, and headers to develop Vulkan applications.
- `Assimp`: A library for importing various 3D model formats into the `Sailor` engine.
- `nlohmann/json`: A C++ library for JSON serialization and deserialization.
- `yaml-cpp`: A C++ library for YAML parsing and generation.
- `EasyProfiler`: A lightweight profiling library for performance analysis and optimization.
- `ImGui`: A graphical user interface library for creating interactive tools and interfaces.
- `magic_enum`: A header-only library for working with C++ enums, providing reflection and iteration capabilities.
- `RenderDoc`: A powerful graphics debugger and profiler for capturing and analyzing rendering frames.
- `SPIRV-Reflect`: A library for parsing and analyzing SPIR-V shader reflection data.
