# Build both Tally implementations from the repository root.
cmake_minimum_required(VERSION 3.16...4.3)
project(tally-all)

enable_testing()
find_package(Python3 COMPONENTS Interpreter REQUIRED)

add_subdirectory(gcc-tally)
add_subdirectory(llvm-tally)

add_test(
    NAME compare_gcc_llvm_random_walk_performance
    COMMAND Python3::Interpreter
            "${CMAKE_CURRENT_SOURCE_DIR}/tests/compare-performance.py"
            --repo-root "${CMAKE_CURRENT_SOURCE_DIR}"
            --build-root "${CMAKE_CURRENT_BINARY_DIR}"
            --gcc-binary "${CMAKE_CURRENT_SOURCE_DIR}/gcc-tally/bin/budget_walk"
            --llvm-host "${CMAKE_CURRENT_BINARY_DIR}/llvm-tally/bin/llvm-tally-random-walk"
            --llvm-pass "$<TARGET_FILE:llvm-tally-pass>"
            --metacycles 3000
            --repetitions 3
)
set_tests_properties(compare_gcc_llvm_random_walk_performance PROPERTIES TIMEOUT 60)
