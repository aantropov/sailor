add_executable (SailorExec WIN32 Main.cpp)
add_dependencies(SailorExec SailorLib)
target_link_libraries (SailorExec SailorLib)

set_property(TARGET SailorExec PROPERTY FOLDER "Executables")
set_property(DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR} PROPERTY VS_STARTUP_PROJECT SailorExec)

add_compile_definitions(_SAILOR_IMPORT_)

# Properties->General->Output Directory
set_target_properties(SailorExec PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${PROJECT_BINARY_DIR}/Binaries) 
set_property(TARGET SailorExec PROPERTY VS_DEBUGGER_WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}")

# Adds logic to INSTALL.vcproj to copy app.exe to destination directory
#install (TARGETS SailorExec RUNTIME DESTINATION ${CMAKE_SOURCE_DIR}/Binaries/${CMAKE_CONFIGURATION_TYPE})

set_target_properties(SailorExec PROPERTIES OUTPUT_NAME "Sailor_exec")
set_target_properties(SailorExec PROPERTIES COMPILE_FLAGS "/Zc:wchar_t")
#set_target_properties(SailorExec PROPERTIES COMPILE_FLAGS "/EHsc")