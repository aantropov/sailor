add_executable(SailorMemoryBenchmark MemoryBenchmark.cpp)
target_compile_features(SailorMemoryBenchmark PRIVATE cxx_std_20)
target_link_libraries(SailorMemoryBenchmark PRIVATE Sailor::Runtime)

find_package(Threads REQUIRED)
add_executable(SailorContainerBenchmark ContainerBenchmark.cpp)
target_compile_features(SailorContainerBenchmark PRIVATE cxx_std_20)
target_link_libraries(SailorContainerBenchmark PRIVATE Sailor::Runtime Threads::Threads)

if(WIN32)
    find_package(VulkanLoader CONFIG REQUIRED)
    target_link_libraries(SailorMemoryBenchmark PRIVATE Psapi)
    foreach(benchmark SailorMemoryBenchmark SailorContainerBenchmark)
        add_custom_command(TARGET ${benchmark} POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                $<TARGET_RUNTIME_DLLS:${benchmark}>
                $<TARGET_FILE:Vulkan::Loader>
                $<TARGET_FILE_DIR:${benchmark}>
            COMMAND_EXPAND_LISTS
            VERBATIM)
    endforeach()
endif()
