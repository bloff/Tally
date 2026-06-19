# Build both Tally implementations from the repository root.
cmake_minimum_required(VERSION 3.16...4.3)
project(tally-all)

enable_testing()
find_package(Python3 COMPONENTS Interpreter REQUIRED)

add_subdirectory(gcc-tally)
add_subdirectory(llvm-tally)

add_test(
    NAME compare_gcc_llvm_self_walk_performance
    COMMAND Python3::Interpreter
            "${CMAKE_CURRENT_SOURCE_DIR}/tests/compare-performance.py"
            --repo-root "${CMAKE_CURRENT_SOURCE_DIR}"
            --build-root "${CMAKE_CURRENT_BINARY_DIR}"
            --gcc-binary "${CMAKE_CURRENT_SOURCE_DIR}/gcc-tally/bin/self_walk"
            --llvm-host "${CMAKE_CURRENT_BINARY_DIR}/llvm-tally/bin/llvm-tally-self-walk"
            --llvm-pass "$<TARGET_FILE:llvm-tally-pass>"
            --target-edges 10000
            --thread-counts 1,2,5
            --budgets 10,100
            --repetitions 1
            --output-dir "${CMAKE_CURRENT_BINARY_DIR}/performance-smoke"
)
set_tests_properties(compare_gcc_llvm_self_walk_performance PROPERTIES TIMEOUT 60)

add_test(
    NAME calibrate_virtual_budget_smoke
    COMMAND Python3::Interpreter
            "${CMAKE_CURRENT_SOURCE_DIR}/tests/calibrate-virtual-budget.py"
            --repo-root "${CMAKE_CURRENT_SOURCE_DIR}"
            --build-root "${CMAKE_CURRENT_BINARY_DIR}"
            --gcc-binary "${CMAKE_CURRENT_SOURCE_DIR}/gcc-tally/bin/self_walk"
            --llvm-host "${CMAKE_CURRENT_BINARY_DIR}/llvm-tally/bin/llvm-tally-self-walk"
            --llvm-pass "$<TARGET_FILE:llvm-tally-pass>"
            --target-edges 10000
            --thread-counts 1,2
            --budgets 100,1000
            --repetitions 1
            --output-dir "${CMAKE_CURRENT_BINARY_DIR}/virtual-budget-calibration-smoke"
)
set_tests_properties(calibrate_virtual_budget_smoke PROPERTIES TIMEOUT 60)
