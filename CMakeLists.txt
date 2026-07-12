cmake_minimum_required(VERSION 3.21)
project(Sailor VERSION 0.1)

set_property(GLOBAL PROPERTY USE_FOLDERS ON)

if(CMAKE_CURRENT_SOURCE_DIR STREQUAL CMAKE_SOURCE_DIR)
  set(SAILOR_IS_TOP_LEVEL ON)
else()
  set(SAILOR_IS_TOP_LEVEL OFF)
endif()

option(SAILOR_BUILD_EXECUTABLE "Build the standalone Sailor executable" ${SAILOR_IS_TOP_LEVEL})
option(SAILOR_BUILD_TESTS "Build Sailor tests" ${SAILOR_IS_TOP_LEVEL})

option(SAILOR_BUILD_WITH_TRACY_PROFILER "Build with tracy profile" OFF)
option(SAILOR_MEMORY_USE_LOCK_FREE_HEAP_ALLOCATOR_AS_DEFAULT "Use LockFreeHeapAllocator as default" ON)
option(SAILOR_MEMORY_HEAP_DISABLE_FREE "Custom allocator disable free memory" OFF)
option(SAILOR_BUILD_WITH_RENDER_DOC "Build with RenderDoc" ON)
option(SAILOR_BUILD_WITH_VULKAN "Build with Vulkan" ON)
option(SAILOR_VULKAN_SHARE_DEVICE_MEMORY_FOR_STAGING_BUFFERS "Vulkan share device memory between staging buffers" OFF)
option(SAILOR_VULKAN_STAGING_BUFFERS_COMBINE "Vulkan combine staging buffers" ON)
option(SAILOR_VULKAN_STORE_VERTICES_INDICES_IN_SSBO "Vulkan store all meshes in one ssbo buffer" ON)
option(SAILOR_VULKAN_MSAA_IMPACTS_TEXTURE_SAMPLING "Vulkan MSAA impacts texture sampling" OFF)

set(SAILOR_RUNTIME_DIR "${PROJECT_SOURCE_DIR}/Runtime/")
set(SAILOR_EXTERNAL_DIR "${PROJECT_SOURCE_DIR}/External/")
set(SAILOR_BINARIES_DIR "${PROJECT_SOURCE_DIR}/Binaries/")
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${SAILOR_BINARIES_DIR}")

if (NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

set(BUILD_SHARED_LIBS OFF)

if (MSVC)
  add_compile_options(/Zc:preprocessor)
endif()

message("cxx Flags:" ${CMAKE_CXX_FLAGS})

if(SAILOR_BUILD_EXECUTABLE)
  add_subdirectory(Exec)
endif()

add_subdirectory(Lib)

if(SAILOR_BUILD_TESTS)
  enable_testing()
  add_subdirectory(Tests)
endif()
