add_executable (SailorExec WIN32 Main.cpp)
add_dependencies(SailorExec SailorLib)
target_link_libraries (SailorExec SailorLib)

target_compile_definitions(SailorExec PUBLIC NOMINMAX)

set_property(TARGET SailorExec PROPERTY FOLDER "Executables")
set_property(DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR} PROPERTY VS_STARTUP_PROJECT SailorExec)

set_property(TARGET SailorExec PROPERTY VS_DEBUGGER_WORKING_DIRECTORY "${SAILOR_BINARIES_DIR}")

set_target_properties(SailorExec PROPERTIES OUTPUT_NAME "SailorEngine-${CMAKE_BUILD_TYPE}")
