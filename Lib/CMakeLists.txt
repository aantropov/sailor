include(GNUInstallDirs)
include(CMakePackageConfigHelpers)

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
set(SAILOR_CONFIGURED_WITH_DEPENDENCY_STUBS OFF)

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

find_package(TBB CONFIG QUIET)
if(NOT TBB_FOUND)
	list(APPEND SAILOR_MISSING_DEPS "TBB")
	sailor_create_stub_target(TBB::tbb)
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
	set(SAILOR_CONFIGURED_WITH_DEPENDENCY_STUBS ON)
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

# Project files and Engine library
file(GLOB_RECURSE SAILOR_RUNTIME_SOURCES
  "${SAILOR_RUNTIME_DIR}/*.h"
  "${SAILOR_RUNTIME_DIR}/*.cpp"
  "${SAILOR_RUNTIME_DIR}/*.mm"
  "${SAILOR_RUNTIME_DIR}/*.hpp"
  "${SAILOR_RUNTIME_DIR}/*.natvis")

file(GLOB_RECURSE SAILOR_LIB_SOURCES
  "*.h"
  "*.cpp")

if(APPLE)
	list(REMOVE_ITEM SAILOR_RUNTIME_SOURCES
		"${SAILOR_RUNTIME_DIR}/Platform/Win32/Window.cpp"
		"${SAILOR_RUNTIME_DIR}/Platform/Win32/ConsoleWindow.cpp")
endif()

if(NOT WIN32)
	list(REMOVE_ITEM SAILOR_LIB_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/DllMain.cpp")
endif()

source_group("" FILES ${SAILOR_LIB_SOURCES})
 
foreach(Source IN ITEMS ${SAILOR_RUNTIME_SOURCES})
  get_filename_component(SourcePath "${Source}" PATH)
  file(RELATIVE_PATH SourcePathRelative "${SAILOR_RUNTIME_DIR}" "${SourcePath}")
  string(REPLACE "/" "\\" GroupPath "${SourcePathRelative}")
  source_group("Runtime/${GroupPath}" FILES "${Source}")
endforeach()

add_library(SailorLib SHARED ${SAILOR_RUNTIME_SOURCES} ${SAILOR_LIB_SOURCES})
add_library(Sailor::Runtime ALIAS SailorLib)
set_property(TARGET SailorLib PROPERTY FOLDER "Libraries")
set_property(TARGET SailorLib PROPERTY EXPORT_NAME Runtime)

# Dependencies
target_link_libraries(SailorLib PUBLIC
  yaml-cpp::yaml-cpp
  nlohmann_json::nlohmann_json
  magic_enum::magic_enum
  imgui::imgui
  glm::glm
  TBB::tbb
  unofficial::spirv-reflect
)

find_path(REFL_CPP_INCLUDE_DIRS "refl.hpp")
find_path(TINYGLTF_INCLUDE_DIRS "tiny_gltf.h")

if(REFL_CPP_INCLUDE_DIRS MATCHES "NOTFOUND")
  if(SAILOR_ALLOW_MISSING_DEPS)
    set(SAILOR_CONFIGURED_WITH_DEPENDENCY_STUBS ON)
    set(REFL_CPP_INCLUDE_DIRS "")
  else()
    message(FATAL_ERROR "Missing refl-cpp headers. Install dependencies (python update_deps.py) or use -DSAILOR_ALLOW_MISSING_DEPS=ON.")
  endif()
endif()

if(TINYGLTF_INCLUDE_DIRS MATCHES "NOTFOUND")
  if(SAILOR_ALLOW_MISSING_DEPS)
    set(SAILOR_CONFIGURED_WITH_DEPENDENCY_STUBS ON)
    set(TINYGLTF_INCLUDE_DIRS "")
  else()
    message(FATAL_ERROR "Missing tinygltf headers. Install dependencies (python update_deps.py) or use -DSAILOR_ALLOW_MISSING_DEPS=ON.")
  endif()
endif()

target_include_directories(SailorLib PUBLIC
	$<BUILD_INTERFACE:${SAILOR_RUNTIME_DIR}>
	$<BUILD_INTERFACE:${SAILOR_EXTERNAL_DIR}>
	$<BUILD_INTERFACE:${REFL_CPP_INCLUDE_DIRS}>
	$<BUILD_INTERFACE:${Stb_INCLUDE_DIR}>
	$<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}/Sailor/Runtime>
	$<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}/Sailor/External>
)
target_include_directories(SailorLib PRIVATE ${TINYGLTF_INCLUDE_DIRS})

# Definitions
target_compile_features(SailorLib PUBLIC cxx_std_20)
target_compile_definitions(SailorLib PUBLIC GLM_FORCE_CXX11)
target_compile_definitions(SailorLib PUBLIC NOMINMAX)

target_compile_definitions(SailorLib PUBLIC $<$<CONFIG:Release>:_SHIPPING>)

target_compile_definitions(SailorLib INTERFACE _SAILOR_IMPORT_)

if(MSVC)
  target_compile_options(SailorLib PUBLIC /permissive-)
  target_compile_options(SailorLib PUBLIC $<$<NOT:$<CONFIG:DEBUG>>:/Oi>)
  target_compile_options(SailorLib PUBLIC $<$<NOT:$<CONFIG:DEBUG>>:/Ot>)
  target_compile_options(SailorLib PUBLIC /GF)
  target_compile_options(SailorLib PUBLIC /arch:AVX2)
  target_compile_options(SailorLib PUBLIC /Zc:wchar_t)
  target_compile_options(SailorLib PUBLIC /Zc:preprocessor)
  target_compile_options(SailorLib PUBLIC /external:anglebrackets)
  target_compile_options(SailorLib PUBLIC /external:W0)
  target_compile_options(SailorLib PRIVATE /WX)
  target_compile_options(SailorLib PUBLIC $<$<NOT:$<CONFIG:DEBUG>>:/GT>)
elseif(CMAKE_CXX_COMPILER_ID MATCHES "Clang|AppleClang")
  target_compile_options(SailorLib PUBLIC $<$<NOT:$<CONFIG:DEBUG>>:-O2>)
endif()

if(WIN32)
  target_compile_definitions(SailorLib PUBLIC NOMINMAX WIN32_LEAN_AND_MEAN)
  target_link_libraries(SailorLib PRIVATE Winmm.lib)
endif()

if(APPLE)
	find_library(SAILOR_COCOA_FRAMEWORK Cocoa REQUIRED)
	find_library(SAILOR_QUARTZCORE_FRAMEWORK QuartzCore REQUIRED)
	find_library(SAILOR_METAL_FRAMEWORK Metal REQUIRED)
	find_library(SAILOR_IOSURFACE_FRAMEWORK IOSurface REQUIRED)

	target_link_libraries(SailorLib PRIVATE
		${SAILOR_COCOA_FRAMEWORK}
		${SAILOR_QUARTZCORE_FRAMEWORK}
		${SAILOR_METAL_FRAMEWORK}
		${SAILOR_IOSURFACE_FRAMEWORK})
endif()

if(SAILOR_MEMORY_USE_LOCK_FREE_HEAP_ALLOCATOR_AS_DEFAULT)
  target_compile_definitions(SailorLib PUBLIC SAILOR_MEMORY_USE_LOCK_FREE_HEAP_ALLOCATOR_AS_DEFAULT)
endif()

if(SAILOR_MEMORY_HEAP_DISABLE_FREE)
  add_compile_definitions(SAILOR_MEMORY_HEAP_DISABLE_FREE)
endif()

if(SAILOR_BUILD_WITH_RENDER_DOC)
  target_compile_definitions(SailorLib PUBLIC SAILOR_BUILD_WITH_RENDER_DOC)
endif()

set(SAILOR_PACKAGE_NEEDS_TRACY OFF)
if(SAILOR_BUILD_WITH_TRACY_PROFILER)
  target_compile_definitions(SailorLib PUBLIC SAILOR_PROFILING_ENABLE BUILD_WITH_TRACY_PROFILER)
  target_link_libraries(SailorLib PUBLIC Tracy::TracyClient)
  set(SAILOR_PACKAGE_NEEDS_TRACY ON)
endif()

set(SAILOR_PACKAGE_USES_VULKAN OFF)
set(SAILOR_PACKAGE_NEEDS_VULKAN_TARGETS OFF)
if(SAILOR_BUILD_WITH_VULKAN)
  find_package(Vulkan 1.3 COMPONENTS shaderc_combined QUIET)

  set(SAILOR_SHADERC_FALLBACK "")
  set(SAILOR_MOLTENVK_FALLBACK "")
  if(APPLE)
    if(EXISTS "/opt/homebrew/lib/libshaderc_combined.a")
      set(SAILOR_SHADERC_FALLBACK "/opt/homebrew/lib/libshaderc_combined.a")
    endif()
    if(EXISTS "/opt/homebrew/lib/libMoltenVK.dylib")
      set(SAILOR_MOLTENVK_FALLBACK "/opt/homebrew/lib/libMoltenVK.dylib")
    endif()
  endif()
  if(NOT Vulkan_FOUND)
    find_package(Vulkan 1.3 QUIET)
    if(Vulkan_FOUND)
      find_library(SAILOR_SHADERC_FALLBACK NAMES shaderc_combined PATHS /opt/homebrew/lib /usr/local/lib)
    endif()
  endif()

  if(NOT Vulkan_FOUND AND APPLE)
    find_library(SAILOR_MOLTENVK_FALLBACK NAMES MoltenVK PATHS /opt/homebrew/lib /usr/local/lib)
    find_library(SAILOR_SHADERC_FALLBACK NAMES shaderc_combined PATHS /opt/homebrew/lib /usr/local/lib)
    if(SAILOR_MOLTENVK_FALLBACK AND SAILOR_SHADERC_FALLBACK)
      set(Vulkan_FOUND TRUE)
    endif()
  endif()

  if(Vulkan_FOUND AND (TARGET Vulkan::shaderc_combined OR SAILOR_SHADERC_FALLBACK))
    set(SAILOR_PACKAGE_USES_VULKAN ON)
    target_compile_definitions(SailorLib PUBLIC SAILOR_BUILD_WITH_VULKAN)
    if(TARGET Vulkan::Vulkan)
      target_link_libraries(SailorLib PUBLIC Vulkan::Vulkan)
      set(SAILOR_PACKAGE_NEEDS_VULKAN_TARGETS ON)
    elseif(APPLE AND SAILOR_MOLTENVK_FALLBACK)
      target_include_directories(SailorLib PUBLIC
		$<BUILD_INTERFACE:/opt/homebrew/include>
		$<BUILD_INTERFACE:/usr/local/include>)
      target_link_libraries(SailorLib PRIVATE ${SAILOR_MOLTENVK_FALLBACK})
      target_compile_definitions(SailorLib PUBLIC SAILOR_VULKAN_DIRECT_MOLTENVK_FALLBACK)
    endif()

    if(TARGET Vulkan::shaderc_combined)
      target_link_libraries(SailorLib PRIVATE Vulkan::shaderc_combined)
    else()
      target_link_libraries(SailorLib PRIVATE ${SAILOR_SHADERC_FALLBACK})
    endif()
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
set_target_properties(SailorLib PROPERTIES OUTPUT_NAME "Sailor")
set_target_properties(SailorLib PROPERTIES OUTPUT_NAME_DEBUG "Sailor-Debug")
set_target_properties(SailorLib PROPERTIES OUTPUT_NAME_RELEASE "Sailor-Release")
set_target_properties(SailorLib PROPERTIES OUTPUT_NAME_RELWITHDEBINFO "Sailor-RelWithDebInfo")
set_target_properties(SailorLib PROPERTIES OUTPUT_NAME_MINSIZEREL "Sailor-MinSizeRel")

if(SAILOR_CONFIGURED_WITH_DEPENDENCY_STUBS)
	install(CODE [[
		message(FATAL_ERROR "Sailor cannot be installed from a configuration that uses dependency stubs. Install dependencies and configure again with SAILOR_ALLOW_MISSING_DEPS=OFF.")
	]])
endif()

install(TARGETS SailorLib
	EXPORT SailorTargets
	RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
	LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
	ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR})

install(DIRECTORY "${SAILOR_RUNTIME_DIR}/"
	DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}/Sailor/Runtime"
	FILES_MATCHING
	PATTERN "*.h"
	PATTERN "*.hpp")

install(FILES
	"${SAILOR_EXTERNAL_DIR}/renderdoc/renderdoc/api/app/renderdoc_app.h"
	DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}/Sailor/External/renderdoc/renderdoc/api/app")

set(SAILOR_INSTALL_CMAKEDIR "${CMAKE_INSTALL_LIBDIR}/cmake/Sailor")
configure_package_config_file(
	"${PROJECT_SOURCE_DIR}/CMake/SailorConfig.cmake.in"
	"${CMAKE_CURRENT_BINARY_DIR}/SailorConfig.cmake"
	INSTALL_DESTINATION "${SAILOR_INSTALL_CMAKEDIR}")

write_basic_package_version_file(
	"${CMAKE_CURRENT_BINARY_DIR}/SailorConfigVersion.cmake"
	VERSION "${PROJECT_VERSION}"
	COMPATIBILITY SameMajorVersion)

install(EXPORT SailorTargets
	FILE SailorTargets.cmake
	NAMESPACE Sailor::
	DESTINATION "${SAILOR_INSTALL_CMAKEDIR}")

install(FILES
	"${CMAKE_CURRENT_BINARY_DIR}/SailorConfig.cmake"
	"${CMAKE_CURRENT_BINARY_DIR}/SailorConfigVersion.cmake"
	DESTINATION "${SAILOR_INSTALL_CMAKEDIR}")
