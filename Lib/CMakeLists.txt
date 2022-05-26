set(SAILOR_RUNTIME_DIR "${PROJECT_SOURCE_DIR}/Runtime")

file (GLOB_RECURSE SAILOR_RUNTIME_SOURCES
      "${SAILOR_RUNTIME_DIR}/*.h"
      "${SAILOR_RUNTIME_DIR}/*.cpp"
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

# Easy profiler
if(BUILD_WITH_EASY_PROFILER)
    target_link_libraries(SailorLib "${PROJECT_SOURCE_DIR}/External/easy_profiler/bin/easy_profiler.lib")
    add_custom_command(
        TARGET SailorLib POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy
                ${PROJECT_SOURCE_DIR}/External/easy_profiler/bin/easy_profiler.dll
               ${CMAKE_BINARY_DIR}/Binaries/$<CONFIG>/easy_profiler.dll)
endif(BUILD_WITH_EASY_PROFILER)

# Vulkan
find_package(Vulkan REQUIRED)
include_directories ("${Vulkan_INCLUDE_DIR}")
target_link_libraries(SailorLib "${Vulkan_LIBRARIES}")

get_filename_component(VulkanLibPath "${Vulkan_LIBRARIES}" DIRECTORY )

target_link_libraries(SailorLib debug "${VulkanLibPath}/shaderc_combinedd.lib" optimized "${VulkanLibPath}/shaderc_combined.lib")

set_property(TARGET SailorLib PROPERTY FOLDER "Libraries")

# Properties->General->Output Directory
set_target_properties(SailorLib PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${PROJECT_BINARY_DIR}/Binaries)

#install (TARGETS SailorLib
#         RUNTIME DESTINATION ${CMAKE_SOURCE_DIR}/Binaries/${CMAKE_CONFIGURATION_TYPE}
#         LIBRARY DESTINATION ${CMAKE_SOURCE_DIR}/Binaries/${CMAKE_CONFIGURATION_TYPE})

set_target_properties(SailorLib PROPERTIES PREFIX "")
set_target_properties(SailorLib PROPERTIES OUTPUT_NAME "Sailor")
set_target_properties(SailorLib PROPERTIES COMPILE_FLAGS "/Zc:wchar_t")
