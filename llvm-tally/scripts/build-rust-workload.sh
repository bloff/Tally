#!/usr/bin/env bash
# Build a controlled Rust workload through rustc -> opt -> clang.
set -euo pipefail

if [ "$#" -lt 1 ]; then
    echo "usage: $0 <workload-rel-dir> [build-dir] [pass-plugin]" >&2
    exit 2
fi

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
LLVM_TALLY_DIR="$(cd "${SCRIPT_DIR}/.." && pwd)"
WORKLOAD_REL="${1%/}"
BUILD_DIR="${2:-/tmp/tally-llvm-build}"
PASS_PLUGIN="${3:-${BUILD_DIR}/llvm-tally-pass.so}"

SOURCE_FILE="${LLVM_TALLY_DIR}/${WORKLOAD_REL}/workload/src/lib.rs"
WORK_DIR="${BUILD_DIR}/workloads/${WORKLOAD_REL}"
OUTPUT_DIR="${LLVM_TALLY_DIR}/dl/${WORKLOAD_REL}"

if ! [ -f "${SOURCE_FILE}" ]; then
    echo "missing workload source: ${SOURCE_FILE}" >&2
    exit 1
fi

if ! [ -f "${PASS_PLUGIN}" ]; then
    echo "missing LLVM Tally pass plugin: ${PASS_PLUGIN}" >&2
    exit 1
fi

mkdir -p "${WORK_DIR}" "${OUTPUT_DIR}"

RAW_BC="${WORK_DIR}/random_walk.bc"
INSTRUMENTED_BC="${WORK_DIR}/random_walk.instrumented.bc"
OUTPUT_SO="${OUTPUT_DIR}/random_walk.so"

rustc "${SOURCE_FILE}" \
    --crate-name random_walk_budget \
    --crate-type lib \
    --target x86_64-unknown-linux-gnu \
    --emit=llvm-bc \
    -C panic=abort \
    -C codegen-units=1 \
    -C no-redzone=yes \
    -C relocation-model=pic \
    -C opt-level=1 \
    -o "${RAW_BC}"

opt \
    -load-pass-plugin="${PASS_PLUGIN}" \
    -passes=tally-instrument \
    "${RAW_BC}" \
    -o "${INSTRUMENTED_BC}"

clang \
    -shared \
    -fPIC \
    "${INSTRUMENTED_BC}" \
    -Wl,--unresolved-symbols=ignore-all \
    -o "${OUTPUT_SO}"

echo "${OUTPUT_SO}"
