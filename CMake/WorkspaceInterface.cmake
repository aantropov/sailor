function(sailor_workspace_interface_id output source_root build_settings)
    file(GLOB_RECURSE headers RELATIVE "${source_root}"
        "${source_root}/Runtime/*.h"
        "${source_root}/Runtime/*.hpp"
        "${source_root}/Runtime/*.inl")
    list(APPEND headers "External/YamlExceptionBoundary.h")
    list(SORT headers)

    set(interface "${build_settings}\n")
    foreach(header IN LISTS headers)
        set(path "${source_root}/${header}")
        file(SHA256 "${path}" digest)
        string(APPEND interface "${header}=${digest}\n")
    endforeach()
    string(SHA256 identity "${interface}")
    set(${output} "${identity}" PARENT_SCOPE)
endfunction()

function(sailor_generate_workspace_interface target source_root output_dir build_settings)
    set(settings "${build_settings}\n${CMAKE_CXX_FLAGS}")
    set(configurations ${CMAKE_CONFIGURATION_TYPES} ${CMAKE_BUILD_TYPE})
    list(REMOVE_DUPLICATES configurations)
    list(SORT configurations)
    foreach(config IN LISTS configurations)
        string(TOUPPER "${config}" config)
        string(APPEND settings "\n${config}=${CMAKE_CXX_FLAGS_${config}}")
    endforeach()
    string(SHA256 settings "${settings}")
    sailor_workspace_interface_id(SAILOR_WORKSPACE_INTERFACE_ID "${source_root}" "${settings}")
    set(header "${output_dir}/Workspace/WorkspaceInterface.h")
    configure_file("${CMAKE_CURRENT_FUNCTION_LIST_DIR}/WorkspaceInterface.h.in" "${header}" @ONLY)
    # Hash on every build; configure-time timestamp checks can miss rapid header edits.
    add_custom_target(${target}WorkspaceInterface
        COMMAND ${CMAKE_COMMAND}
            "-DSAILOR_INTERFACE_SOURCE_DIR=${source_root}"
            "-DSAILOR_INTERFACE_OUTPUT_DIR=${output_dir}"
            "-DSAILOR_INTERFACE_BUILD_SETTINGS=${settings}"
            -P "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/WorkspaceInterface.cmake"
        BYPRODUCTS "${header}"
        VERBATIM)
    add_dependencies(${target} ${target}WorkspaceInterface)
    target_include_directories(${target} PUBLIC $<BUILD_INTERFACE:${output_dir}>)
endfunction()

if(CMAKE_SCRIPT_MODE_FILE)
    sailor_workspace_interface_id(SAILOR_WORKSPACE_INTERFACE_ID
        "${SAILOR_INTERFACE_SOURCE_DIR}" "${SAILOR_INTERFACE_BUILD_SETTINGS}")
    configure_file("${CMAKE_CURRENT_LIST_DIR}/WorkspaceInterface.h.in"
        "${SAILOR_INTERFACE_OUTPUT_DIR}/Workspace/WorkspaceInterface.h" @ONLY)
endif()
