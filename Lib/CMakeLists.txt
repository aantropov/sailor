set(SAILOR_RUNTIME_DIR "${PROJECT_SOURCE_DIR}/Runtime")

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

# Properties->C/C++->General->Additional Include Directories
include_directories (.)

add_library(SailorLib SHARED ${SAILOR_RUNTIME_SOURCES} ${SAILOR_LIB_SOURCES})
set_property(TARGET SailorLib PROPERTY FOLDER "Libraries")

add_compile_definitions($<$<CONFIG:Release>:_SHIPPING>)

# Easy profiler
if(SAILOR_BUILD_WITH_EASY_PROFILER)
    target_link_libraries(SailorLib "${PROJECT_SOURCE_DIR}/External/easy_profiler/bin/easy_profiler.lib")
    add_custom_command(
        TARGET SailorLib POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy
                ${PROJECT_SOURCE_DIR}/External/easy_profiler/bin/easy_profiler.dll
               ${CMAKE_BINARY_DIR}/Binaries/$<CONFIG>/easy_profiler.dll)
endif(SAILOR_BUILD_WITH_EASY_PROFILER)

# Vulkan
find_package(Vulkan 1.3 REQUIRED)
include_directories ("${Vulkan_INCLUDE_DIR}")
target_link_libraries(SailorLib "${Vulkan_LIBRARIES}")

get_filename_component(VulkanLibPath "${Vulkan_LIBRARIES}" DIRECTORY )

target_link_libraries(SailorLib debug "${VulkanLibPath}/shaderc_combinedd.lib" 
                                optimized "${VulkanLibPath}/shaderc_combined.lib")
target_link_libraries(SailorLib winmm.lib)

# YAML
include_directories(SailorLib "${PROJECT_SOURCE_DIR}/External/yaml-cpp/include/")
target_link_libraries(SailorLib debug "${PROJECT_SOURCE_DIR}/External/yaml-cpp/Built/Debug/yaml-cppd.lib" 
                                optimized "${PROJECT_SOURCE_DIR}/External/yaml-cpp/Built/Release/yaml-cpp.lib")

# ASSIMP
IF(MSVC_TOOLSET_VERSION)
  SET(MSVC_PREFIX "vc${MSVC_TOOLSET_VERSION}")
  SET(ASSIMP_MSVC_VERSION ${MSVC_PREFIX})
ELSE()
  IF(MSVC12)
    SET(ASSIMP_MSVC_VERSION "vc120")
  ELSEIF(MSVC14)
    SET(ASSIMP_MSVC_VERSION "vc140")
  ELSEIF(MSVC15)
    SET(ASSIMP_MSVC_VERSION "vc141")
  ELSEIF(MSV142)
    SET(ASSIMP_MSVC_VERSION "vc142")
  ELSEIF(MSVC143)
    SET(ASSIMP_MSVC_VERSION "vc143")
  ENDIF()
ENDIF()
  
include_directories(SailorLib "${PROJECT_SOURCE_DIR}/External/assimp/include/")
include_directories(SailorLib "${PROJECT_SOURCE_DIR}/External/assimp/Built/include/")
target_link_libraries(SailorLib debug "${PROJECT_SOURCE_DIR}/External/assimp/Built/lib/Debug/assimp-${ASSIMP_MSVC_VERSION}-mtd.lib" 
                                optimized "${PROJECT_SOURCE_DIR}/External/assimp/Built/lib/Release/assimp-${ASSIMP_MSVC_VERSION}-mt.lib")

add_custom_command(
    TARGET SailorLib POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E $<IF:$<CONFIG:Release>,copy,true> ARGS ${PROJECT_SOURCE_DIR}/External/assimp/Built/bin/Release/assimp-${ASSIMP_MSVC_VERSION}-mt.dll ${CMAKE_BINARY_DIR}/Binaries/Release/assimp-${ASSIMP_MSVC_VERSION}-mt.dll
	COMMAND ${CMAKE_COMMAND} -E $<IF:$<CONFIG:Debug>,copy,true> ARGS ${PROJECT_SOURCE_DIR}/External/assimp/Built/bin/Debug/assimp-${ASSIMP_MSVC_VERSION}-mtd.dll ${CMAKE_BINARY_DIR}/Binaries/Debug/assimp-${ASSIMP_MSVC_VERSION}-mtd.dll
	COMMAND ${CMAKE_COMMAND} -E $<IF:$<CONFIG:RelWithDebInfo>,copy,true> ARGS ${PROJECT_SOURCE_DIR}/External/assimp/Built/bin/Release/assimp-${ASSIMP_MSVC_VERSION}-mt.dll ${CMAKE_BINARY_DIR}/Binaries/RelWithDebInfo/assimp-${ASSIMP_MSVC_VERSION}-mt.dll
	VERBATIM
	COMMAND_EXPAND_LISTS)

# Properties->General->Output Directory
set_target_properties(SailorLib PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${PROJECT_BINARY_DIR}/Binaries)

#install (TARGETS SailorLib
#         RUNTIME DESTINATION ${CMAKE_SOURCE_DIR}/Binaries/${CMAKE_CONFIGURATION_TYPE}
#         LIBRARY DESTINATION ${CMAKE_SOURCE_DIR}/Binaries/${CMAKE_CONFIGURATION_TYPE})


# ImGui
include_directories(SailorLib "${PROJECT_SOURCE_DIR}/External/imgui/")
target_sources(SailorLib PRIVATE
"${PROJECT_SOURCE_DIR}/External/imgui/imgui.h"
"${PROJECT_SOURCE_DIR}/External/imgui/imgui.cpp"

"${PROJECT_SOURCE_DIR}/External/imgui/imgui_demo.cpp"
"${PROJECT_SOURCE_DIR}/External/imgui/imgui_draw.cpp"
"${PROJECT_SOURCE_DIR}/External/imgui/imgui_widgets.cpp"

#"${PROJECT_SOURCE_DIR}/External/imgui/backends/imgui_impl_vulkan.cpp"
"${PROJECT_SOURCE_DIR}/External/imgui/backends/imgui_impl_win32.cpp"
"${PROJECT_SOURCE_DIR}/External/imgui/imgui_tables.cpp"
)

set_target_properties(SailorLib PROPERTIES PREFIX "")
set_target_properties(SailorLib PROPERTIES OUTPUT_NAME "Sailor")
set_target_properties(SailorLib PROPERTIES COMPILE_FLAGS "/Zc:wchar_t")

if(MSVC)
 target_compile_options(SailorLib PRIVATE "/MP")
endif()