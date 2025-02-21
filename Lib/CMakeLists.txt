# External dependencies
find_package(yaml-cpp CONFIG REQUIRED)
find_package(tracy CONFIG REQUIRED)
find_package(imgui CONFIG REQUIRED)
find_package(nlohmann_json CONFIG REQUIRED)
find_package(magic_enum CONFIG REQUIRED)
find_package(glm CONFIG REQUIRED)
find_package(unofficial-spirv-reflect CONFIG REQUIRED)
find_package(Stb REQUIRED)

if(SAILOR_BUILD_WITH_TRACY_PROFILER)
  find_package(tracy CONFIG REQUIRED)
endif()

# Header only dependencies
#find_package(tinygltf CONFIG REQUIRED)  
#find_package(refl-cpp CONFIG REQUIRED)

# Project files and Engine library
file(GLOB_RECURSE SAILOR_RUNTIME_SOURCES
  "${SAILOR_RUNTIME_DIR}/*.h"
  "${SAILOR_RUNTIME_DIR}/*.cpp"
  "${SAILOR_RUNTIME_DIR}/*.hpp"
  "${SAILOR_RUNTIME_DIR}/*.natvis")

file(GLOB_RECURSE SAILOR_LIB_SOURCES
  "*.h"
  "*.cpp")

source_group("" FILES ${SAILOR_LIB_SOURCES})
 
foreach(Source IN ITEMS ${SAILOR_RUNTIME_SOURCES})
  get_filename_component(SourcePath "${Source}" PATH)
  file(RELATIVE_PATH SourcePathRelative "${SAILOR_RUNTIME_DIR}" "${SourcePath}")
  string(REPLACE "/" "\\" GroupPath "${SourcePathRelative}")
  source_group("Runtime/${GroupPath}" FILES "${Source}")
endforeach()

add_library(SailorLib SHARED ${SAILOR_RUNTIME_SOURCES} ${SAILOR_LIB_SOURCES})
set_property(TARGET SailorLib PROPERTY FOLDER "Libraries")

# Dependencies
target_link_libraries(SailorLib PUBLIC
  yaml-cpp::yaml-cpp
  nlohmann_json::nlohmann_json
  magic_enum::magic_enum
  imgui::imgui
  glm::glm
  unofficial::spirv-reflect
)

find_path(REFL_CPP_INCLUDE_DIRS "refl.hpp")
find_path(TINYGLTF_INCLUDE_DIRS "tiny_gltf.h")

target_include_directories(SailorLib PUBLIC 
    ${SAILOR_RUNTIME_DIR}
    ${SAILOR_EXTERNAL_DIR}
    ${REFL_CPP_INCLUDE_DIRS} 
    ${TINYGLTF_INCLUDE_DIRS}
    ${Stb_INCLUDE_DIR}
)

# Definitions
target_compile_features(SailorLib PUBLIC cxx_std_20)
target_compile_definitions(SailorLib PUBLIC GLM_FORCE_CXX11)
target_compile_definitions(SailorLib PUBLIC NOMINMAX)

if(${CMAKE_BUILD_TYPE} MATCHES Release)
  target_compile_definitions(SailorLib PUBLIC _SHIPPING)
endif()

target_compile_options(SailorLib PUBLIC /permissive-)
target_compile_options(SailorLib PUBLIC $<$<NOT:$<CONFIG:DEBUG>>:/Oi>)
target_compile_options(SailorLib PUBLIC $<$<NOT:$<CONFIG:DEBUG>>:/Ot>)
target_compile_options(SailorLib PUBLIC /GF)
target_compile_options(SailorLib PUBLIC /arch:AVX2)
target_compile_options(SailorLib PUBLIC /Zc:wchar_t)
target_compile_definitions(SailorLib INTERFACE _SAILOR_IMPORT_)
target_compile_options(SailorLib PUBLIC /WX)

if(CMAKE_CXX_COMPILER_ID STREQUAL "Clang")
  target_compile_options(SailorLib PUBLIC $<$<NOT:$<CONFIG:DEBUG>>:-O2>)
elseif(CMAKE_CXX_COMPILER_ID STREQUAL "MSVC")
  target_compile_options(SailorLib PUBLIC $<$<NOT:$<CONFIG:DEBUG>>:/GT>)
endif()

if(WIN32)
  target_compile_definitions(SailorLib PUBLIC NOMINMAX WIN32_LEAN_AND_MEAN)
  target_link_libraries(SailorLib PUBLIC Winmm.lib)
endif()

if(SAILOR_MEMORY_USE_LOCK_FREE_HEAP_ALLOCATOR_AS_DEFAULT)
  target_compile_definitions(SailorLib PUBLIC SAILOR_MEMORY_USE_LOCK_FREE_HEAP_ALLOCATOR_AS_DEFAULT)
endif()

if(SAILOR_MEMORY_HEAP_DISABLE_FREE)
  add_compile_definitions(SAILOR_MEMORY_HEAP_DISABLE_FREE)
endif()

if(SAILOR_BUILD_WITH_RENDER_DOC)
  add_compile_definitions(SAILOR_BUILD_WITH_RENDER_DOC)
endif()

if(SAILOR_BUILD_WITH_TRACY_PROFILER)
  target_compile_definitions(SailorLib PUBLIC SAILOR_PROFILING_ENABLE BUILD_WITH_TRACY_PROFILER)
  target_link_libraries(SailorLib PRIVATE Tracy::TracyClient)
endif()

if(SAILOR_BUILD_WITH_VULKAN)
  target_compile_definitions(SailorLib PUBLIC SAILOR_BUILD_WITH_VULKAN)
  find_package(Vulkan 1.3 COMPONENTS shaderc_combined REQUIRED)
  target_link_libraries(SailorLib PUBLIC Vulkan::Vulkan Vulkan::shaderc_combined)
endif()

if(SAILOR_VULKAN_SHARE_DEVICE_MEMORY_FOR_STAGING_BUFFERS)
  target_compile_definitions(SailorLib PUBLIC SAILOR_VULKAN_SHARE_DEVICE_MEMORY_FOR_STAGING_BUFFERS)
endif()

if(SAILOR_VULKAN_STAGING_BUFFERS_COMBINE)
  target_compile_definitions(SailorLib PUBLIC SAILOR_VULKAN_COMBINE_STAGING_BUFFERS)
endif()

if(SAILOR_VULKAN_STORE_VERTICES_INDICES_IN_SSBO)
  target_compile_definitions(SailorLib PUBLIC SAILOR_VULKAN_STORE_VERTICES_INDICES_IN_SSBO)
endif()

if(SAILOR_VULKAN_MSAA_IMPACTS_TEXTURE_SAMPLING)
  target_compile_definitions(SailorLib PUBLIC SAILOR_VULKAN_MSAA_IMPACTS_TEXTURE_SAMPLING)
endif()

set_target_properties(SailorLib PROPERTIES PREFIX "")
set_target_properties(SailorLib PROPERTIES DEBUG_POSTFIX "")
set_target_properties(SailorLib PROPERTIES OUTPUT_NAME "Sailor-${CMAKE_BUILD_TYPE}")
