
file (GLOB_RECURSE SAILOR_RUNTIME_SOURCES
    "${SAILOR_RUNTIME_DIR}/*.h"
    "${SAILOR_RUNTIME_DIR}/*.cpp"
    "${SAILOR_RUNTIME_DIR}/*.hpp"
    "${SAILOR_RUNTIME_DIR}/*.natvis")

file (GLOB_RECURSE SAILOR_LIB_SOURCES
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
target_include_directories(SailorLib PUBLIC ${SAILOR_RUNTIME_DIR} ${SAILOR_EXTERNAL_DIR})
target_compile_features(SailorLib PUBLIC cxx_std_20)
target_link_libraries(SailorLib yaml-cpp::yaml-cpp assimp::assimp nlohmann_json::nlohmann_json refl-cpp::refl-cpp)

target_compile_definitions(SailorLib PUBLIC NOMINMAX)

if(NOT ${CMAKE_BUILD_TYPE} MATCHES Release)
    target_compile_definitions(SailorLib PUBLIC SAILOR_VULKAN_ENABLE_VALIDATION_LAYER)    
else()
    target_compile_definitions(SailorLib PUBLIC _SHIPPING)
endif()

if(MSVC)
    target_compile_options(SailorLib PUBLIC /permissive-)
    target_compile_options(SailorLib PUBLIC $<$<NOT:$<CONFIG:DEBUG>>:/Oi>)
    target_compile_options(SailorLib PUBLIC $<$<NOT:$<CONFIG:DEBUG>>:/Ot>)
    target_compile_options(SailorLib PUBLIC $<$<NOT:$<CONFIG:DEBUG>>:/GT>)
    target_compile_options(SailorLib PUBLIC /GF)
    target_compile_options(SailorLib PUBLIC /WX)
    target_compile_options(SailorLib PUBLIC /arch:AVX2)
    target_compile_options(SailorLib PUBLIC /Zc:wchar_t)
    target_compile_definitions(SailorLib INTERFACE _SAILOR_IMPORT_)
endif()

if(WIN32)
    target_compile_definitions(SailorLib PUBLIC NOMINMAX WIN32_LEAN_AND_MEAN)
    target_link_libraries(SailorLib Winmm.lib)
endif()

if(SAILOR_CONSOLE)
    target_compile_definitions(SailorLib PUBLIC SAILOR_WITH_CONSOLE)
endif(SAILOR_CONSOLE)

if(SAILOR_MEMORY_USE_LOCK_FREE_HEAP_ALLOCATOR_AS_DEFAULT)
    target_compile_definitions(SailorLib PUBLIC SAILOR_MEMORY_USE_LOCK_FREE_HEAP_ALLOCATOR_AS_DEFAULT)
endif(SAILOR_MEMORY_USE_LOCK_FREE_HEAP_ALLOCATOR_AS_DEFAULT)

if(SAILOR_MEMORY_HEAP_DISABLE_FREE)	
	add_compile_definitions(SAILOR_MEMORY_HEAP_DISABLE_FREE)
endif(SAILOR_MEMORY_HEAP_DISABLE_FREE)

if(SAILOR_BUILD_WITH_RENDER_DOC)
	add_compile_definitions(SAILOR_BUILD_WITH_RENDER_DOC)
endif(SAILOR_BUILD_WITH_RENDER_DOC)

if(SAILOR_BUILD_WITH_TRACY_PROFILER)
    target_compile_definitions(SailorLib PUBLIC SAILOR_PROFILING_ENABLE)
    target_compile_definitions(SailorLib PUBLIC BUILD_WITH_TRACY_PROFILER)
    target_link_libraries(SailorLib TracyClient)
endif(SAILOR_BUILD_WITH_TRACY_PROFILER)

if(SAILOR_BUILD_WITH_VULKAN)
    target_compile_definitions(SailorLib PUBLIC SAILOR_BUILD_WITH_VULKAN)

    find_package(Vulkan 1.3 COMPONENTS shaderc_combined REQUIRED)
    target_link_libraries(SailorLib Vulkan::Vulkan Vulkan::shaderc_combined)
endif(SAILOR_BUILD_WITH_VULKAN)

if(SAILOR_VULKAN_SHARE_DEVICE_MEMORY_FOR_STAGING_BUFFERS)
    target_compile_definitions(SailorLib PUBLIC SAILOR_VULKAN_SHARE_DEVICE_MEMORY_FOR_STAGING_BUFFERS)
endif(SAILOR_VULKAN_SHARE_DEVICE_MEMORY_FOR_STAGING_BUFFERS)

if(SAILOR_VULKAN_STAGING_BUFFERS_COMBINE)
    target_compile_definitions(SailorLib PUBLIC SAILOR_VULKAN_COMBINE_STAGING_BUFFERS)
endif(SAILOR_VULKAN_STAGING_BUFFERS_COMBINE)

if(SAILOR_VULKAN_STORE_VERTICES_INDICES_IN_SSBO)
    target_compile_definitions(SailorLib PUBLIC SAILOR_VULKAN_STORE_VERTICES_INDICES_IN_SSBO)
endif(SAILOR_VULKAN_STORE_VERTICES_INDICES_IN_SSBO)

if(SAILOR_VULKAN_MSAA_IMPACTS_TEXTURE_SAMPLING)
    target_compile_definitions(SailorLib PUBLIC SAILOR_VULKAN_MSAA_IMPACTS_TEXTURE_SAMPLING)
endif(SAILOR_VULKAN_MSAA_IMPACTS_TEXTURE_SAMPLING)

# ImGui
include_directories(SailorLib "${PROJECT_SOURCE_DIR}/External/imgui/")
target_sources(SailorLib PRIVATE
    "${PROJECT_SOURCE_DIR}/External/imgui/imgui.h"
    "${PROJECT_SOURCE_DIR}/External/imgui/imgui.cpp"

    "${PROJECT_SOURCE_DIR}/External/imgui/imgui_demo.cpp"
    "${PROJECT_SOURCE_DIR}/External/imgui/imgui_draw.cpp"
    "${PROJECT_SOURCE_DIR}/External/imgui/imgui_widgets.cpp"

    "${PROJECT_SOURCE_DIR}/External/imgui/backends/imgui_impl_win32.cpp"
    "${PROJECT_SOURCE_DIR}/External/imgui/imgui_tables.cpp"
)

set_target_properties(SailorLib PROPERTIES PREFIX "")
set_target_properties(SailorLib PROPERTIES DEBUG_POSTFIX "")
set_target_properties(SailorLib PROPERTIES OUTPUT_NAME "Sailor-${CMAKE_BUILD_TYPE}")