find_package(Python3 COMPONENTS Interpreter REQUIRED)

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

add_executable(WorkspaceModuleContractTests
    WorkspaceModuleContractTests.cpp
    WorkspaceModuleFixture.cpp
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
target_compile_features(WorkspaceModuleContractTests PRIVATE cxx_std_20)
target_compile_definitions(WorkspaceModuleFixture PRIVATE
    SAILOR_WORKSPACE_MODULE
    SAILOR_WORKSPACE_MODULE_EXPORTS
)
target_compile_definitions(WorkspaceModuleContractTests PRIVATE
    SAILOR_WORKSPACE_MODULE
)
target_link_libraries(WorkspaceModuleFixture PRIVATE Sailor::Runtime)
target_link_libraries(WorkspaceModuleContractTests PRIVATE
    Sailor::Runtime
)
if(WIN32)
    add_custom_command(TARGET WorkspaceModuleContractTests POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            $<TARGET_RUNTIME_DLLS:WorkspaceModuleContractTests>
            $<TARGET_FILE_DIR:WorkspaceModuleContractTests>
        COMMAND_EXPAND_LISTS
        VERBATIM
    )
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
target_include_directories(WorkspaceModuleContractTests PRIVATE
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
            "PATH=path_list_prepend:${VCPKG_INSTALLED_DIR}/${VCPKG_TARGET_TRIPLET}/$<IF:$<CONFIG:Debug>,debug/bin,bin>"
    )
endif()

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
