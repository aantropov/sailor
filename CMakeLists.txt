cmake_minimum_required(VERSION 3.10)

project(Sailor VERSION 0.1)

set_property(GLOBAL PROPERTY USE_FOLDERS ON)

include_directories ("${PROJECT_SOURCE_DIR}/Runtime/")
include_directories ("${PROJECT_SOURCE_DIR}/External/")
include_directories ("${PROJECT_SOURCE_DIR}/External/easy_profiler/include/")

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED True)
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} /W3")

#set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_SOURCE_DIR}/Binaries/${CMAKE_CONFIGURATION_TYPE}/")
#set(CMAKE_LIBRARY_OUTPUT_DIRECTORY "${CMAKE_SOURCE_DIR}/Binaries/${CMAKE_CONFIGURATION_TYPE}/")
#set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_SOURCE_DIR}/Binaries/${CMAKE_CONFIGURATION_TYPE}/")

add_compile_options(/permissive-)
add_compile_options($<$<NOT:$<CONFIG:DEBUG>>:/Oi>)
add_compile_options($<$<NOT:$<CONFIG:DEBUG>>:/Ot>)
add_compile_options($<$<NOT:$<CONFIG:DEBUG>>:/GT>)
add_compile_options(/GF)
add_compile_options(/WX)
add_compile_options(/arch:AVX2)

option(CONSOLE "Enable console" ON)
if(CONSOLE)
	add_compile_definitions(SAILOR_WITH_CONSOLE)
endif(CONSOLE)

option(BUILD_WITH_EASY_PROFILER "Build with easy profile" ON)
if(BUILD_WITH_EASY_PROFILER)
	add_compile_definitions(SAILOR_PROFILING_ENABLE)
	add_compile_definitions(BUILD_WITH_EASY_PROFILER)
endif(BUILD_WITH_EASY_PROFILER)

option(MEMORY_USE_LOCK_FREE_HEAP_ALLOCATOR_AS_DEFAULT "Use LockFreeHeapAllocator as default" ON)
if(MEMORY_USE_LOCK_FREE_HEAP_ALLOCATOR_AS_DEFAULT)	
	add_compile_definitions(SAILOR_MEMORY_USE_LOCK_FREE_HEAP_ALLOCATOR_AS_DEFAULT)
endif(MEMORY_USE_LOCK_FREE_HEAP_ALLOCATOR_AS_DEFAULT)

option(MEMORY_HEAP_DISABLE_FREE "Custom allocator disable free memory" OFF)
if(MEMORY_HEAP_DISABLE_FREE)	
	add_compile_definitions(SAILOR_MEMORY_HEAP_DISABLE_FREE)
endif(MEMORY_HEAP_DISABLE_FREE)

option(BUILD_WITH_RENDER_DOC "Build with RenderDoc" ON)
if(BUILD_WITH_RENDER_DOC)
	add_compile_definitions(BUILD_WITH_RENDER_DOC)
endif(BUILD_WITH_RENDER_DOC)

option(BUILD_WITH_VULKAN "Build with Vulkan" ON)
if(BUILD_WITH_VULKAN)	
	add_compile_definitions(SAILOR_BUILD_WITH_VULKAN)
endif(BUILD_WITH_VULKAN)

option(VULKAN_SHARE_DEVICE_MEMORY_FOR_STAGING_BUFFERS "Vulkan share device memory between staging buffers" OFF)
if(VULKAN_SHARE_DEVICE_MEMORY_FOR_STAGING_BUFFERS)	
	add_compile_definitions(SAILOR_VULKAN_SHARE_DEVICE_MEMORY_FOR_STAGING_BUFFERS)
endif(VULKAN_SHARE_DEVICE_MEMORY_FOR_STAGING_BUFFERS)

option(VULKAN_STAGING_BUFFERS_COMBINE "Vulkan combine staging buffers" ON)
if(VULKAN_STAGING_BUFFERS_COMBINE)	
	add_compile_definitions(SAILOR_VULKAN_COMBINE_STAGING_BUFFERS)
endif(VULKAN_STAGING_BUFFERS_COMBINE)

option(VULKAN_STORE_VERTICES_INDICES_IN_SSBO "Vulkan store all meshes in one ssbo buffer" ON)
if(VULKAN_STORE_VERTICES_INDICES_IN_SSBO)	
	add_compile_definitions(SAILOR_VULKAN_STORE_VERTICES_INDICES_IN_SSBO)
endif(VULKAN_STORE_VERTICES_INDICES_IN_SSBO)

option(VULKAN_MSAA_IMPACTS_TEXTURE_SAMPLING "Vulkan MSAA impacts texture sampling" OFF)
if(VULKAN_MSAA_IMPACTS_TEXTURE_SAMPLING)	
	add_compile_definitions(SAILOR_VULKAN_MSAA_IMPACTS_TEXTURE_SAMPLING)
endif(VULKAN_MSAA_IMPACTS_TEXTURE_SAMPLING)

#option(VULKAN_ENABLE_VALIDATION_LAYER "Enable Vulkan validation layer" ON)
#if(VULKAN_ENABLE_VALIDATION_LAYER)
#	add_compile_definitions(SAILOR_VULKAN_ENABLE_VALIDATION_LAYER)
#endif(VULKAN_ENABLE_VALIDATION_LAYER)

add_compile_definitions($<$<NOT:$<CONFIG:RELEASE>>:SAILOR_VULKAN_ENABLE_VALIDATION_LAYER>)
add_compile_definitions(NOMINMAX)
add_compile_definitions(WIN32_LEAN_AND_MEAN)

message ("cxx Flags:" ${CMAKE_CXX_FLAGS})

add_subdirectory(Exec)
add_subdirectory(Lib)