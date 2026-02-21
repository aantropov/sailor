if(NOT DEFINED SAILOR_ALLOW_MISSING_DEPS)
	if(WIN32)
		set(SAILOR_ALLOW_MISSING_DEPS OFF CACHE BOOL "Allow CMake configure without installed dependencies")
	else()
		set(SAILOR_ALLOW_MISSING_DEPS ON CACHE BOOL "Allow CMake configure without installed dependencies")
	endif()
endif()

function(sailor_create_stub_target target_name)
	if(NOT TARGET ${target_name})
		add_library(${target_name} INTERFACE IMPORTED)
	endif()
endfunction()

set(SAILOR_MISSING_DEPS "")

# External dependencies
find_package(yaml-cpp CONFIG QUIET)
if(NOT yaml-cpp_FOUND)
	list(APPEND SAILOR_MISSING_DEPS "yaml-cpp")
	sailor_create_stub_target(yaml-cpp::yaml-cpp)
endif()

find_package(tracy CONFIG QUIET)
if(NOT tracy_FOUND)
	list(APPEND SAILOR_MISSING_DEPS "tracy")
	sailor_create_stub_target(Tracy::TracyClient)
endif()

find_package(imgui CONFIG QUIET)
if(NOT imgui_FOUND)
	list(APPEND SAILOR_MISSING_DEPS "imgui")
	sailor_create_stub_target(imgui::imgui)
endif()

find_package(nlohmann_json CONFIG QUIET)
if(NOT nlohmann_json_FOUND)
	list(APPEND SAILOR_MISSING_DEPS "nlohmann_json")
	sailor_create_stub_target(nlohmann_json::nlohmann_json)
endif()

find_package(magic_enum CONFIG QUIET)
if(NOT magic_enum_FOUND)
	list(APPEND SAILOR_MISSING_DEPS "magic_enum")
	sailor_create_stub_target(magic_enum::magic_enum)
endif()

find_package(glm CONFIG QUIET)
if(NOT glm_FOUND)
	list(APPEND SAILOR_MISSING_DEPS "glm")
	sailor_create_stub_target(glm::glm)
endif()

find_package(unofficial-spirv-reflect CONFIG QUIET)
if(NOT unofficial-spirv-reflect_FOUND)
	list(APPEND SAILOR_MISSING_DEPS "unofficial-spirv-reflect")
	sailor_create_stub_target(unofficial::spirv-reflect)
endif()

find_package(Stb QUIET)
if(NOT Stb_FOUND)
	list(APPEND SAILOR_MISSING_DEPS "Stb")
	set(Stb_INCLUDE_DIR "")
endif()

if(SAILOR_MISSING_DEPS)
	list(JOIN SAILOR_MISSING_DEPS ", " SAILOR_MISSING_DEPS_MSG)
	if(SAILOR_ALLOW_MISSING_DEPS)
		message(WARNING "Missing dependencies: ${SAILOR_MISSING_DEPS_MSG}. CMake configure will continue with stub targets, but build may fail until dependencies are installed.")
	else()
		message(FATAL_ERROR "Missing dependencies: ${SAILOR_MISSING_DEPS_MSG}. Run 'python update_deps.py' after initializing submodules, or configure with -DSAILOR_ALLOW_MISSING_DEPS=ON.")
	endif()
endif()

# Header only dependencies
#find_package(tinygltf CONFIG REQUIRED)  
#find_package(refl-cpp CONFIG REQUIRED)

# Ensure tinygltf implementation is always included
list(APPEND SAILOR_RUNTIME_SOURCES
    "${SAILOR_RUNTIME_DIR}/Submodules/ExternalDeps.cpp")

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

if(REFL_CPP_INCLUDE_DIRS MATCHES "NOTFOUND")
  if(SAILOR_ALLOW_MISSING_DEPS)
    set(REFL_CPP_INCLUDE_DIRS "")
  else()
    message(FATAL_ERROR "Missing refl-cpp headers. Install dependencies (python update_deps.py) or use -DSAILOR_ALLOW_MISSING_DEPS=ON.")
  endif()
endif()

if(TINYGLTF_INCLUDE_DIRS MATCHES "NOTFOUND")
  if(SAILOR_ALLOW_MISSING_DEPS)
    set(TINYGLTF_INCLUDE_DIRS "")
  else()
    message(FATAL_ERROR "Missing tinygltf headers. Install dependencies (python update_deps.py) or use -DSAILOR_ALLOW_MISSING_DEPS=ON.")
  endif()
endif()

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
  find_package(Vulkan 1.3 COMPONENTS shaderc_combined QUIET)

  if(Vulkan_FOUND)
    target_compile_definitions(SailorLib PUBLIC SAILOR_BUILD_WITH_VULKAN)
    target_link_libraries(SailorLib PUBLIC Vulkan::Vulkan Vulkan::shaderc_combined)
  elseif(SAILOR_ALLOW_MISSING_DEPS)
    message(WARNING "Vulkan SDK 1.3+ with shaderc_combined not found. Vulkan integration is disabled for this configuration.")
  else()
    message(FATAL_ERROR "Vulkan SDK 1.3+ with shaderc_combined is required. Install dependencies (python update_deps.py) or use -DSAILOR_ALLOW_MISSING_DEPS=ON.")
  endif()
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
