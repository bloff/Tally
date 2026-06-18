# Build the Tally runtime, GCC plugin, demos, experiments, and tests.
cmake_minimum_required(VERSION 3.16...4.3)
project(gcc-tally C CXX)

set(THREADS_PREFER_PTHREAD_FLAG ON)
find_package(Threads)

set(CMAKE_POSITION_INDEPENDENT_CODE ON)

set(TALLY_OUTPUT_DIR "${CMAKE_CURRENT_SOURCE_DIR}/bin")
set(TALLY_INCLUDE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/include")
set(TALLY_MODULE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/src/tally/modules")

set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -fms-extensions")
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -fms-extensions")

set(CMAKE_DISABLE_IN_SOURCE_BUILD ON)
set(CMAKE_DISABLE_SOURCE_CHANGES  ON)

if ("${CMAKE_SOURCE_DIR}" STREQUAL "${CMAKE_BINARY_DIR}")
    message(SEND_ERROR "In-source builds are not allowed.")
endif ()

set(CMAKE_SHARED_LIBRARY_PREFIX "")
set(CMAKE_STATIC_LIBRARY_PREFIX "")
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${TALLY_OUTPUT_DIR}")
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY "${TALLY_OUTPUT_DIR}")
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY "${TALLY_OUTPUT_DIR}")

set(CMAKE_VERBOSE_MAKEFILE ON)
set(CMAKE_COLOR_MAKEFILE   ON)

# Locate the GCC plugin headers used by the Tally instrumentation pass.
execute_process(COMMAND gcc -print-file-name=plugin/include
                    OUTPUT_VARIABLE GCCPLUGINS_INCLUDE_DIR) # a GCC plugin must know where the includes are
string(STRIP "${GCCPLUGINS_INCLUDE_DIR}" GCCPLUGINS_INCLUDE_DIR)

# Runtime library that jumps to and from instrumented code.
add_library(minithread STATIC
    src/tally/runtime/minithread.c
    src/tally/runtime/cycles_probe.c
)
set_target_properties(minithread PROPERTIES COMPILE_FLAGS "-save-temps -fPIC -no-pie -rdynamic -Wl,--export-dynamic")
set_target_properties(minithread PROPERTIES LINK_FLAGS "-Wl,--export-dynamic")
target_include_directories(minithread PRIVATE "${TALLY_INCLUDE_DIR}")
target_compile_definitions(minithread PRIVATE TALLY_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}")

add_library(gcc-tally SHARED src/tally/plugin/gcc-tally.cpp)
set_target_properties(gcc-tally PROPERTIES COMPILE_FLAGS "-save-temps -fPIC -no-pie -fno-rtti -I${GCCPLUGINS_INCLUDE_DIR}")
target_include_directories(gcc-tally PRIVATE "${TALLY_INCLUDE_DIR}")

# Compilation flags for instrumented module code.
set(INSTRUMENTATION_COMPILE_FLAGS "-ffixed-r15 -fplugin=${TALLY_OUTPUT_DIR}/gcc-tally.so -nostdlib")

file(GLOB_RECURSE module_src_clean "${TALLY_MODULE_DIR}/clean/*.c")
file(GLOB_RECURSE module_src_instrumented "${TALLY_MODULE_DIR}/instrumented/*.c")
set_source_files_properties(${module_src_instrumented} PROPERTIES COMPILE_FLAGS "${INSTRUMENTATION_COMPILE_FLAGS}")
add_executable(main examples/demo/main.c ${module_src_clean} ${module_src_instrumented})
set_target_properties(main PROPERTIES COMPILE_FLAGS "-ldl -rdynamic -g -lpthread -no-pie -fno-pie -fPIC -save-temps")
set_target_properties(main PROPERTIES LINK_FLAGS "-Wl,-R -Wl,. -ldl -Wl,--export-dynamic")
add_dependencies(main gcc-tally)
target_include_directories(main PRIVATE "${TALLY_INCLUDE_DIR}")
target_compile_definitions(main PRIVATE TALLY_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(main minithread dl Threads::Threads)

add_executable(budget_walk experiments/budget_walk/budget_walk.c ${module_src_clean} ${module_src_instrumented})
set_target_properties(budget_walk PROPERTIES COMPILE_FLAGS "-ldl -rdynamic -g -lpthread -no-pie -fno-pie -fPIC -save-temps")
set_target_properties(budget_walk PROPERTIES LINK_FLAGS "-Wl,-R -Wl,. -ldl -Wl,--export-dynamic")
add_dependencies(budget_walk gcc-tally)
target_include_directories(budget_walk PRIVATE "${TALLY_INCLUDE_DIR}")
target_compile_definitions(budget_walk PRIVATE TALLY_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(budget_walk minithread dl Threads::Threads)

enable_testing()

add_executable(test_hash tests/test_hash.c)
target_include_directories(test_hash PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/include")
set_target_properties(test_hash PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/tests")
add_test(NAME hash_macros COMMAND test_hash)

add_executable(test_graph_load tests/test_graph_load.c src/tally/modules/clean/graph/graph.c)
target_include_directories(test_graph_load PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_compile_definitions(test_graph_load PRIVATE TALLY_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}")
set_target_properties(test_graph_load PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/tests")
add_test(NAME graph_load COMMAND test_graph_load)

add_test(NAME demo_runs COMMAND main)
set_tests_properties(demo_runs PROPERTIES TIMEOUT 20)

add_test(NAME budget_walk_runs COMMAND budget_walk 100 100 100)
set_tests_properties(budget_walk_runs PROPERTIES TIMEOUT 20)
