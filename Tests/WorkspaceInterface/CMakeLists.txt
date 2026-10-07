cmake_minimum_required(VERSION 3.21)
project(WorkspaceInterfaceFixture LANGUAGES CXX)
include("${SAILOR_SOURCE_DIR}/CMake/WorkspaceInterface.cmake")
add_executable(InterfaceProbe InterfaceProbe.cpp)
sailor_generate_workspace_interface(InterfaceProbe "${SDK_ROOT}"
    "${CMAKE_CURRENT_BINARY_DIR}/Generated" "${INTERFACE_SETTINGS}")
file(GENERATE OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/probe-path-$<CONFIG>.txt" CONTENT "$<TARGET_FILE:InterfaceProbe>")
