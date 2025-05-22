find_package(Python3 COMPONENTS Interpreter REQUIRED)

add_test(
    NAME ProcessNoCrash
    COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/process_no_crash_test.py
)

add_test(
    NAME ContainerBenchmarks
    COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/container_benchmarks_test.py
)

add_test(
    NAME SchedulerTests
    COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tasks_scheduler_test.py
)

