# Build the LLVM pass plugin, Rust runtime tests, and Rust random-walk host.
cmake_minimum_required(VERSION 3.16...4.3)
project(llvm-tally C CXX)

find_package(LLVM REQUIRED CONFIG)

list(APPEND CMAKE_MODULE_PATH "${LLVM_CMAKE_DIR}")
include(AddLLVM)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_llvm_pass_plugin(llvm-tally-pass
    pass/TallyInstrumentPass.cpp
)

set(LLVM_TALLY_BIN_DIR "${CMAKE_CURRENT_BINARY_DIR}/bin")
set(LLVM_TALLY_CARGO_TARGET_DIR "${CMAKE_CURRENT_BINARY_DIR}/cargo")
set(LLVM_TALLY_HOST_MANIFEST "${CMAKE_CURRENT_SOURCE_DIR}/examples/random-walk/host/Cargo.toml")
set(LLVM_TALLY_HOST_BINARY "${LLVM_TALLY_CARGO_TARGET_DIR}/release/llvm-tally-random-walk")
set(LLVM_TALLY_HOST_OUTPUT "${LLVM_TALLY_BIN_DIR}/llvm-tally-random-walk")
set(LLVM_TALLY_SELF_HOST_MANIFEST "${CMAKE_CURRENT_SOURCE_DIR}/examples/self-walk/host/Cargo.toml")
set(LLVM_TALLY_SELF_HOST_BINARY "${LLVM_TALLY_CARGO_TARGET_DIR}/release/llvm-tally-self-walk")
set(LLVM_TALLY_SELF_HOST_OUTPUT "${LLVM_TALLY_BIN_DIR}/llvm-tally-self-walk")
set(LLVM_TALLY_FAIRNESS_HOST_MANIFEST "${CMAKE_CURRENT_SOURCE_DIR}/examples/virtual-budget-fairness/host/Cargo.toml")
set(LLVM_TALLY_FAIRNESS_HOST_BINARY "${LLVM_TALLY_CARGO_TARGET_DIR}/release/llvm-tally-virtual-budget-fairness")
set(LLVM_TALLY_FAIRNESS_HOST_OUTPUT "${LLVM_TALLY_BIN_DIR}/llvm-tally-virtual-budget-fairness")
set(LLVM_TALLY_MEMORY_HOST_MANIFEST "${CMAKE_CURRENT_SOURCE_DIR}/examples/memory-recovery/host/Cargo.toml")
set(LLVM_TALLY_MEMORY_HOST_BINARY "${LLVM_TALLY_CARGO_TARGET_DIR}/release/llvm-tally-memory-recovery")
set(LLVM_TALLY_MEMORY_HOST_OUTPUT "${LLVM_TALLY_BIN_DIR}/llvm-tally-memory-recovery")
set(LLVM_TALLY_STD_VEC_HOST_MANIFEST "${CMAKE_CURRENT_SOURCE_DIR}/std/examples/std-vec-host/Cargo.toml")
set(LLVM_TALLY_STD_VEC_HOST_BINARY "${LLVM_TALLY_CARGO_TARGET_DIR}/release/llvm-tally-std-vec-host")
set(LLVM_TALLY_STD_VEC_HOST_OUTPUT "${LLVM_TALLY_BIN_DIR}/llvm-tally-std-vec-host")

add_custom_command(
    OUTPUT "${LLVM_TALLY_HOST_OUTPUT}"
    COMMAND "${CMAKE_COMMAND}" -E make_directory "${LLVM_TALLY_BIN_DIR}"
    COMMAND "${CMAKE_COMMAND}" -E env
            CARGO_TARGET_DIR="${LLVM_TALLY_CARGO_TARGET_DIR}"
            cargo build --release --manifest-path "${LLVM_TALLY_HOST_MANIFEST}"
    COMMAND "${CMAKE_COMMAND}" -E copy "${LLVM_TALLY_HOST_BINARY}" "${LLVM_TALLY_HOST_OUTPUT}"
    DEPENDS
        "${LLVM_TALLY_HOST_MANIFEST}"
        examples/random-walk/host/build.rs
        examples/random-walk/host/src/main.rs
        runtime/Cargo.toml
        runtime/src/lib.rs
)

add_custom_target(llvm-tally-random-walk-host ALL
    DEPENDS "${LLVM_TALLY_HOST_OUTPUT}"
)

add_custom_command(
    OUTPUT "${LLVM_TALLY_SELF_HOST_OUTPUT}"
    COMMAND "${CMAKE_COMMAND}" -E make_directory "${LLVM_TALLY_BIN_DIR}"
    COMMAND "${CMAKE_COMMAND}" -E env
            CARGO_TARGET_DIR="${LLVM_TALLY_CARGO_TARGET_DIR}"
            cargo build --release --manifest-path "${LLVM_TALLY_SELF_HOST_MANIFEST}"
    COMMAND "${CMAKE_COMMAND}" -E copy "${LLVM_TALLY_SELF_HOST_BINARY}" "${LLVM_TALLY_SELF_HOST_OUTPUT}"
    DEPENDS
        "${LLVM_TALLY_SELF_HOST_MANIFEST}"
        examples/self-walk/host/build.rs
        examples/self-walk/host/src/main.rs
        runtime/Cargo.toml
        runtime/src/lib.rs
)

add_custom_target(llvm-tally-self-walk-host ALL
    DEPENDS "${LLVM_TALLY_SELF_HOST_OUTPUT}"
)

add_custom_command(
    OUTPUT "${LLVM_TALLY_FAIRNESS_HOST_OUTPUT}"
    COMMAND "${CMAKE_COMMAND}" -E make_directory "${LLVM_TALLY_BIN_DIR}"
    COMMAND "${CMAKE_COMMAND}" -E env
            CARGO_TARGET_DIR="${LLVM_TALLY_CARGO_TARGET_DIR}"
            cargo build --release --manifest-path "${LLVM_TALLY_FAIRNESS_HOST_MANIFEST}"
    COMMAND "${CMAKE_COMMAND}" -E copy "${LLVM_TALLY_FAIRNESS_HOST_BINARY}" "${LLVM_TALLY_FAIRNESS_HOST_OUTPUT}"
    DEPENDS
        "${LLVM_TALLY_FAIRNESS_HOST_MANIFEST}"
        examples/virtual-budget-fairness/host/src/main.rs
        runtime/Cargo.toml
        runtime/src/lib.rs
)

add_custom_target(llvm-tally-virtual-budget-fairness-host ALL
    DEPENDS "${LLVM_TALLY_FAIRNESS_HOST_OUTPUT}"
)

add_custom_command(
    OUTPUT "${LLVM_TALLY_MEMORY_HOST_OUTPUT}"
    COMMAND "${CMAKE_COMMAND}" -E make_directory "${LLVM_TALLY_BIN_DIR}"
    COMMAND "${CMAKE_COMMAND}" -E env
            CARGO_TARGET_DIR="${LLVM_TALLY_CARGO_TARGET_DIR}"
            cargo build --release --manifest-path "${LLVM_TALLY_MEMORY_HOST_MANIFEST}"
    COMMAND "${CMAKE_COMMAND}" -E copy "${LLVM_TALLY_MEMORY_HOST_BINARY}" "${LLVM_TALLY_MEMORY_HOST_OUTPUT}"
    DEPENDS
        "${LLVM_TALLY_MEMORY_HOST_MANIFEST}"
        examples/memory-recovery/host/build.rs
        examples/memory-recovery/host/src/main.rs
        runtime/Cargo.toml
        runtime/src/lib.rs
)

add_custom_target(llvm-tally-memory-recovery-host ALL
    DEPENDS "${LLVM_TALLY_MEMORY_HOST_OUTPUT}"
)

add_custom_command(
    OUTPUT "${LLVM_TALLY_STD_VEC_HOST_OUTPUT}"
    COMMAND "${CMAKE_COMMAND}" -E make_directory "${LLVM_TALLY_BIN_DIR}"
    COMMAND "${CMAKE_COMMAND}" -E env
            CARGO_TARGET_DIR="${LLVM_TALLY_CARGO_TARGET_DIR}"
            cargo build --release --manifest-path "${LLVM_TALLY_STD_VEC_HOST_MANIFEST}"
    COMMAND "${CMAKE_COMMAND}" -E copy "${LLVM_TALLY_STD_VEC_HOST_BINARY}" "${LLVM_TALLY_STD_VEC_HOST_OUTPUT}"
    DEPENDS
        "${LLVM_TALLY_STD_VEC_HOST_MANIFEST}"
        std/examples/std-vec-host/build.rs
        std/examples/std-vec-host/src/main.rs
        runtime/Cargo.toml
        runtime/src/lib.rs
)

add_custom_target(llvm-tally-std-vec-host ALL
    DEPENDS "${LLVM_TALLY_STD_VEC_HOST_OUTPUT}"
)

enable_testing()

add_test(
    NAME llvm_runtime_tests
    COMMAND "${CMAKE_COMMAND}" -E env
            CARGO_TARGET_DIR="${LLVM_TALLY_CARGO_TARGET_DIR}"
            cargo test --manifest-path "${CMAKE_CURRENT_SOURCE_DIR}/runtime/Cargo.toml" -- --test-threads=1
)

add_test(
    NAME llvm_pass_basic
    COMMAND bash "${CMAKE_CURRENT_SOURCE_DIR}/tests/run-pass-tests.sh"
            "$<TARGET_FILE:llvm-tally-pass>"
            "${CMAKE_CURRENT_SOURCE_DIR}"
)

add_test(
    NAME llvm_random_walk_integration
    COMMAND bash "${CMAKE_CURRENT_SOURCE_DIR}/tests/run-random-walk-integration.sh"
            "${CMAKE_CURRENT_SOURCE_DIR}"
            "${CMAKE_CURRENT_BINARY_DIR}"
            "$<TARGET_FILE:llvm-tally-pass>"
            "${LLVM_TALLY_HOST_OUTPUT}"
)

add_test(
    NAME llvm_self_walk_integration
    COMMAND bash "${CMAKE_CURRENT_SOURCE_DIR}/tests/run-self-walk-integration.sh"
            "${CMAKE_CURRENT_SOURCE_DIR}"
            "${CMAKE_CURRENT_BINARY_DIR}"
            "$<TARGET_FILE:llvm-tally-pass>"
            "${LLVM_TALLY_SELF_HOST_OUTPUT}"
)

add_test(
    NAME llvm_memory_recovery_integration
    COMMAND bash "${CMAKE_CURRENT_SOURCE_DIR}/tests/run-memory-recovery-integration.sh"
            "${CMAKE_CURRENT_SOURCE_DIR}"
            "${CMAKE_CURRENT_BINARY_DIR}"
            "$<TARGET_FILE:llvm-tally-pass>"
            "${LLVM_TALLY_MEMORY_HOST_OUTPUT}"
)

add_test(
    NAME llvm_std_tooling_smoke
    COMMAND bash "${CMAKE_CURRENT_SOURCE_DIR}/std/tests/smoke-std-tooling.sh"
            "${CMAKE_CURRENT_SOURCE_DIR}"
            "${CMAKE_CURRENT_BINARY_DIR}"
            "$<TARGET_FILE:llvm-tally-pass>"
)

add_test(
    NAME llvm_std_workload_smoke
    COMMAND bash "${CMAKE_CURRENT_SOURCE_DIR}/std/tests/smoke-std-workload.sh"
            "${CMAKE_CURRENT_SOURCE_DIR}"
            "${CMAKE_CURRENT_BINARY_DIR}"
            "$<TARGET_FILE:llvm-tally-pass>"
            "${LLVM_TALLY_STD_VEC_HOST_OUTPUT}"
)

set_tests_properties(llvm_runtime_tests llvm_pass_basic llvm_random_walk_integration llvm_self_walk_integration llvm_memory_recovery_integration llvm_std_tooling_smoke
    PROPERTIES TIMEOUT 30
)
set_tests_properties(llvm_std_workload_smoke PROPERTIES TIMEOUT 120)
