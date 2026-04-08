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

target_compile_features(RemoteViewportFoundationTests PRIVATE cxx_std_20)
target_compile_features(RemoteViewportContractTests PRIVATE cxx_std_20)
target_compile_features(RemoteViewportRuntimeTests PRIVATE cxx_std_20)
target_include_directories(RemoteViewportFoundationTests PRIVATE
    ${SAILOR_RUNTIME_DIR}
)
target_include_directories(RemoteViewportContractTests PRIVATE
    ${SAILOR_RUNTIME_DIR}
)
target_include_directories(RemoteViewportRuntimeTests PRIVATE
    ${SAILOR_RUNTIME_DIR}
)

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
    NAME ProcessNoCrash
    COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/process_no_crash_test.py
)

add_test(
    NAME ContainerBenchmarks
    COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/container_benchmarks_test.py
)

