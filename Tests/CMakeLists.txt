find_package(Python3 COMPONENTS Interpreter REQUIRED)

if(WIN32)
    find_package(VulkanLoader CONFIG REQUIRED)

    function(sailor_stage_workspace_test_runtime target)
        add_custom_command(TARGET ${target} POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                $<TARGET_RUNTIME_DLLS:${target}>
                $<TARGET_FILE:Vulkan::Loader>
                $<TARGET_FILE_DIR:${target}>
            COMMAND_EXPAND_LISTS
            VERBATIM
        )
    endfunction()
endif()

add_executable(RemoteViewportFoundationTests
    RemoteViewportFoundationTests.cpp
)

add_executable(RemoteViewportContractTests
    RemoteViewportContractTests.cpp
)

add_executable(RemoteViewportRuntimeTests
    RemoteViewportRuntimeTests.cpp
)

add_executable(EditorViewportSessionTests
    EditorViewportSessionTests.cpp
)

add_library(WorkspaceModuleFixture SHARED
    WorkspaceModuleFixture.cpp
)

add_library(WorkspaceModuleIncompatibleFixture SHARED
    WorkspaceModuleIncompatibleFixture.cpp
)

add_library(WorkspaceModuleMissingEntryFixture SHARED
    WorkspaceModuleMissingEntryFixture.cpp
)

function(add_workspace_malformed_metadata_fixture target output_name mode)
    add_library(${target} SHARED
        WorkspaceModuleMalformedMetadataFixture.cpp
    )
    target_compile_features(${target} PRIVATE cxx_std_20)
    target_compile_definitions(${target} PRIVATE
        SAILOR_WORKSPACE_MODULE
        SAILOR_WORKSPACE_MODULE_EXPORTS
        SAILOR_TEST_METADATA_MODE=${mode}
    )
    target_link_libraries(${target} PRIVATE Sailor::Runtime)
    target_include_directories(${target} PRIVATE
        ${SAILOR_RUNTIME_DIR}
    )
    set_target_properties(${target} PROPERTIES OUTPUT_NAME ${output_name})
endfunction()

add_workspace_malformed_metadata_fixture(
    WorkspaceModulePropertyMismatchFixture PropertyMismatchFixture 1)
add_workspace_malformed_metadata_fixture(
    WorkspaceModuleDuplicateCdoFixture DuplicateCdoFixture 2)
add_workspace_malformed_metadata_fixture(
    WorkspaceModuleUnknownDefaultFixture UnknownDefaultFixture 3)
add_workspace_malformed_metadata_fixture(
    WorkspaceModuleInvalidDefaultTypeFixture InvalidDefaultTypeFixture 4)
add_workspace_malformed_metadata_fixture(
    WorkspaceModuleMissingDefaultFixture MissingDefaultFixture 5)
add_workspace_malformed_metadata_fixture(
    WorkspaceModuleInvalidEnumDefaultFixture InvalidEnumDefaultFixture 6)
add_workspace_malformed_metadata_fixture(
    WorkspaceModuleInvalidStructuredDefaultFixture InvalidStructuredDefaultFixture 7)
add_workspace_malformed_metadata_fixture(
    WorkspaceModuleMissingCdoFixture MissingCdoFixture 8)
add_workspace_malformed_metadata_fixture(
    WorkspaceModuleOversizedStructuredDefaultFixture OversizedStructuredDefaultFixture 9)
add_workspace_malformed_metadata_fixture(
    WorkspaceModuleShadowedPropertyFixture ShadowedPropertyFixture 10)
add_workspace_malformed_metadata_fixture(
    WorkspaceModuleMissingEmptyPropertySchemaFixture MissingEmptyPropertySchemaFixture 11)
add_workspace_malformed_metadata_fixture(
    WorkspaceModuleAliasExpansionFixture AliasExpansionFixture 12)

add_executable(WorkspaceModuleContractTests
    WorkspaceModuleContractTests.cpp
    WorkspaceModuleFixture.cpp
)

add_executable(WorkspaceModuleRuntimeTests
    WorkspaceModuleRuntimeTests.cpp
)

if(WIN32)
    add_executable(RemoteViewportWindowsTransportTests
        RemoteViewportWindowsTransportTests.cpp
    )
endif()

if(APPLE)
    add_executable(RemoteViewportMacTransportTests
        RemoteViewportMacTransportTests.cpp
        ${SAILOR_RUNTIME_DIR}/Submodules/EditorRemote/RemoteViewportMacNativeBridge.mm
    )

    add_executable(RemoteViewportMacNativeBridgeTests
        RemoteViewportMacNativeBridgeTests.mm
        ${SAILOR_RUNTIME_DIR}/Submodules/EditorRemote/RemoteViewportMacNativeBridge.mm
    )
endif()

target_compile_features(RemoteViewportFoundationTests PRIVATE cxx_std_20)
target_compile_features(RemoteViewportContractTests PRIVATE cxx_std_20)
target_compile_features(RemoteViewportRuntimeTests PRIVATE cxx_std_20)
target_compile_features(EditorViewportSessionTests PRIVATE cxx_std_20)
target_compile_features(WorkspaceModuleFixture PRIVATE cxx_std_20)
target_compile_features(WorkspaceModuleIncompatibleFixture PRIVATE cxx_std_20)
target_compile_features(WorkspaceModuleMissingEntryFixture PRIVATE cxx_std_20)
target_compile_features(WorkspaceModuleContractTests PRIVATE cxx_std_20)
target_compile_features(WorkspaceModuleRuntimeTests PRIVATE cxx_std_20)
target_compile_definitions(WorkspaceModuleFixture PRIVATE
    SAILOR_WORKSPACE_MODULE
    SAILOR_WORKSPACE_MODULE_EXPORTS
)
target_compile_definitions(WorkspaceModuleContractTests PRIVATE
    SAILOR_WORKSPACE_MODULE
)
target_compile_definitions(WorkspaceModuleIncompatibleFixture PRIVATE
    SAILOR_WORKSPACE_NO_ENTRY_POINT_DECLARATIONS
    SAILOR_WORKSPACE_MODULE_EXPORTS
)
target_compile_definitions(WorkspaceModuleMissingEntryFixture PRIVATE
    SAILOR_WORKSPACE_NO_ENTRY_POINT_DECLARATIONS
    SAILOR_WORKSPACE_MODULE_EXPORTS
)
target_link_libraries(WorkspaceModuleFixture PRIVATE Sailor::Runtime)
target_link_libraries(WorkspaceModuleIncompatibleFixture PRIVATE Sailor::Runtime)
target_link_libraries(WorkspaceModuleContractTests PRIVATE
    Sailor::Runtime
)
target_link_libraries(WorkspaceModuleRuntimeTests PRIVATE Sailor::Runtime)
add_dependencies(WorkspaceModuleRuntimeTests
    WorkspaceModuleFixture
    WorkspaceModuleIncompatibleFixture
    WorkspaceModuleMissingEntryFixture
    WorkspaceModulePropertyMismatchFixture
    WorkspaceModuleDuplicateCdoFixture
    WorkspaceModuleUnknownDefaultFixture
    WorkspaceModuleInvalidDefaultTypeFixture
    WorkspaceModuleMissingDefaultFixture
    WorkspaceModuleInvalidEnumDefaultFixture
    WorkspaceModuleInvalidStructuredDefaultFixture
    WorkspaceModuleMissingCdoFixture
    WorkspaceModuleOversizedStructuredDefaultFixture
    WorkspaceModuleShadowedPropertyFixture
    WorkspaceModuleMissingEmptyPropertySchemaFixture
    WorkspaceModuleAliasExpansionFixture
)
set_target_properties(WorkspaceModuleFixture PROPERTIES OUTPUT_NAME WorkspaceFixture)
set_target_properties(WorkspaceModuleIncompatibleFixture PROPERTIES OUTPUT_NAME IncompatibleFixture)
set_target_properties(WorkspaceModuleMissingEntryFixture PROPERTIES OUTPUT_NAME MissingEntryFixture)
if(WIN32)
    sailor_stage_workspace_test_runtime(WorkspaceModuleContractTests)
endif()
if(WIN32)
    target_compile_features(RemoteViewportWindowsTransportTests PRIVATE cxx_std_20)
endif()
if(APPLE)
    target_compile_features(RemoteViewportMacTransportTests PRIVATE cxx_std_20)
    target_compile_features(RemoteViewportMacNativeBridgeTests PRIVATE cxx_std_20)
endif()
target_include_directories(RemoteViewportFoundationTests PRIVATE
    ${SAILOR_RUNTIME_DIR}
)
target_include_directories(RemoteViewportContractTests PRIVATE
    ${SAILOR_RUNTIME_DIR}
)
target_include_directories(RemoteViewportRuntimeTests PRIVATE
    ${SAILOR_RUNTIME_DIR}
)
target_include_directories(EditorViewportSessionTests PRIVATE
    ${SAILOR_RUNTIME_DIR}
)
target_include_directories(WorkspaceModuleFixture PRIVATE
    ${SAILOR_RUNTIME_DIR}
)
target_include_directories(WorkspaceModuleIncompatibleFixture PRIVATE
    ${SAILOR_RUNTIME_DIR}
)
target_include_directories(WorkspaceModuleMissingEntryFixture PRIVATE
    ${SAILOR_RUNTIME_DIR}
)
target_include_directories(WorkspaceModuleContractTests PRIVATE
    ${SAILOR_RUNTIME_DIR}
)
target_include_directories(WorkspaceModuleRuntimeTests PRIVATE
    ${SAILOR_RUNTIME_DIR}
)
if(WIN32)
    target_include_directories(RemoteViewportWindowsTransportTests PRIVATE
        ${SAILOR_RUNTIME_DIR}
    )
endif()

if(APPLE)
    target_include_directories(RemoteViewportMacTransportTests PRIVATE
        ${SAILOR_RUNTIME_DIR}
    )
    target_include_directories(RemoteViewportMacNativeBridgeTests PRIVATE
        ${SAILOR_RUNTIME_DIR}
    )
    target_link_libraries(RemoteViewportMacTransportTests PRIVATE
        "-framework Cocoa"
        "-framework QuartzCore"
        "-framework Metal"
        "-framework IOSurface")
    target_link_libraries(RemoteViewportMacNativeBridgeTests PRIVATE
        "-framework Cocoa"
        "-framework QuartzCore"
        "-framework Metal"
        "-framework IOSurface")
endif()

add_test(
    NAME RemoteViewportFoundationTests
    COMMAND RemoteViewportFoundationTests
)

add_test(
    NAME RemoteViewportContractTests
    COMMAND RemoteViewportContractTests
)

add_test(
    NAME RemoteViewportRuntimeTests
    COMMAND RemoteViewportRuntimeTests
)

add_test(
    NAME EditorViewportSessionTests
    COMMAND EditorViewportSessionTests
)

add_test(
    NAME WorkspaceModuleContractTests
    COMMAND WorkspaceModuleContractTests
)

if(WIN32 AND CMAKE_VERSION VERSION_GREATER_EQUAL "3.22" AND VCPKG_INSTALLED_DIR AND VCPKG_TARGET_TRIPLET)
    set_tests_properties(WorkspaceModuleContractTests PROPERTIES
        ENVIRONMENT_MODIFICATION
            "Path=path_list_prepend:${VCPKG_INSTALLED_DIR}/${VCPKG_TARGET_TRIPLET}/$<IF:$<CONFIG:Debug>,debug/bin,bin>"
    )
endif()

add_test(
    NAME WorkspaceModuleRuntimeTests
    COMMAND WorkspaceModuleRuntimeTests
        $<TARGET_FILE:WorkspaceModuleFixture>
        $<TARGET_FILE:WorkspaceModuleIncompatibleFixture>
        $<TARGET_FILE:WorkspaceModuleMissingEntryFixture>
        $<TARGET_FILE:WorkspaceModulePropertyMismatchFixture>
        $<TARGET_FILE:WorkspaceModuleDuplicateCdoFixture>
        $<TARGET_FILE:WorkspaceModuleUnknownDefaultFixture>
        $<TARGET_FILE:WorkspaceModuleInvalidDefaultTypeFixture>
        $<TARGET_FILE:WorkspaceModuleMissingDefaultFixture>
        $<TARGET_FILE:WorkspaceModuleInvalidEnumDefaultFixture>
        $<TARGET_FILE:WorkspaceModuleInvalidStructuredDefaultFixture>
        $<TARGET_FILE:WorkspaceModuleMissingCdoFixture>
        $<TARGET_FILE:WorkspaceModuleOversizedStructuredDefaultFixture>
        $<TARGET_FILE:WorkspaceModuleShadowedPropertyFixture>
        $<TARGET_FILE:WorkspaceModuleMissingEmptyPropertySchemaFixture>
        $<TARGET_FILE:WorkspaceModuleAliasExpansionFixture>
        $<CONFIG>
)
set_tests_properties(
    WorkspaceModuleContractTests
    WorkspaceModuleRuntimeTests
    PROPERTIES TIMEOUT 30
)

if(WIN32)
    add_test(
        NAME RemoteViewportWindowsTransportTests
        COMMAND RemoteViewportWindowsTransportTests
    )
endif()

if(APPLE)
    add_test(
        NAME RemoteViewportMacTransportTests
        COMMAND RemoteViewportMacTransportTests
    )

    add_test(
        NAME RemoteViewportMacNativeBridgeTests
        COMMAND RemoteViewportMacNativeBridgeTests
    )
endif()

add_test(
    NAME ProcessNoCrash
    COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/process_no_crash_test.py
)

add_test(
    NAME ContainerBenchmarks
    COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/container_benchmarks_test.py
)
