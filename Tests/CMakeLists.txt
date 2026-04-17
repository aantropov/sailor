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

add_executable(RemoteViewportWindowsTransportTests
    RemoteViewportWindowsTransportTests.cpp
)

add_executable(RemoteViewportMacTransportTests
    RemoteViewportMacTransportTests.cpp
    ${SAILOR_RUNTIME_DIR}/Submodules/EditorRemote/RemoteViewportMacNativeBridge.mm
)

add_executable(RemoteViewportMacNativeBridgeTests
    RemoteViewportMacNativeBridgeTests.mm
    ${SAILOR_RUNTIME_DIR}/Submodules/EditorRemote/RemoteViewportMacNativeBridge.mm
)

target_compile_features(RemoteViewportFoundationTests PRIVATE cxx_std_20)
target_compile_features(RemoteViewportContractTests PRIVATE cxx_std_20)
target_compile_features(RemoteViewportRuntimeTests PRIVATE cxx_std_20)
target_compile_features(EditorViewportSessionTests PRIVATE cxx_std_20)
target_compile_features(RemoteViewportWindowsTransportTests PRIVATE cxx_std_20)
target_compile_features(RemoteViewportMacTransportTests PRIVATE cxx_std_20)
target_compile_features(RemoteViewportMacNativeBridgeTests PRIVATE cxx_std_20)
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
target_include_directories(RemoteViewportWindowsTransportTests PRIVATE
    ${SAILOR_RUNTIME_DIR}
)
target_include_directories(RemoteViewportMacTransportTests PRIVATE
    ${SAILOR_RUNTIME_DIR}
)
target_include_directories(RemoteViewportMacNativeBridgeTests PRIVATE
    ${SAILOR_RUNTIME_DIR}
)

if(APPLE)
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
    NAME RemoteViewportWindowsTransportTests
    COMMAND RemoteViewportWindowsTransportTests
)

add_test(
    NAME RemoteViewportMacTransportTests
    COMMAND RemoteViewportMacTransportTests
)

add_test(
    NAME RemoteViewportMacNativeBridgeTests
    COMMAND RemoteViewportMacNativeBridgeTests
)

add_test(
    NAME ProcessNoCrash
    COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/process_no_crash_test.py
)

add_test(
    NAME ContainerBenchmarks
    COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/container_benchmarks_test.py
)

