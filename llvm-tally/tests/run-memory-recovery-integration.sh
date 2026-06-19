#!/usr/bin/env bash
# Build and run the llvm-tally memory-recovery integration workload.
set -euo pipefail

LLVM_TALLY_DIR="$1"
BUILD_DIR="$2"
PASS_PLUGIN="$3"
HOST_BINARY="$4"

WORKLOAD_SO="$("${LLVM_TALLY_DIR}/scripts/build-rust-workload.sh" examples/memory-recovery "${BUILD_DIR}" "${PASS_PLUGIN}")"
OUTPUT="$("${HOST_BINARY}" "${WORKLOAD_SO}")"

echo "${OUTPUT}"

if ! printf '%s\n' "${OUTPUT}" | grep -q '^memory recovery integration: ok$'; then
    echo "memory recovery integration did not report success" >&2
    exit 1
fi
