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

add_test(
    NAME native_virtual_budget_fairness_smoke
    COMMAND Python3::Interpreter
            "${CMAKE_CURRENT_SOURCE_DIR}/tests/native-virtual-budget-fairness.py"
            --repo-root "${CMAKE_CURRENT_SOURCE_DIR}"
            --gcc-binary "${CMAKE_CURRENT_SOURCE_DIR}/gcc-tally/bin/virtual_budget_fairness"
            --llvm-binary "${CMAKE_CURRENT_BINARY_DIR}/llvm-tally/bin/llvm-tally-virtual-budget-fairness"
            --thread-count 100
            --rounds 1
            --min-round-seconds 0.05
            --max-round-seconds 0.05
            --total-virtual-budget 0.5
            --calibration-seconds 0
            --calibration-work 1000
            --gcc-stack-words 1024
            --llvm-stack-bytes 16384
            --output-dir "${CMAKE_CURRENT_BINARY_DIR}/native-virtual-budget-fairness-smoke"
)
set_tests_properties(native_virtual_budget_fairness_smoke PROPERTIES TIMEOUT 60)
