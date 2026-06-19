#!/usr/bin/env bash
# Build the Rust workload and smoke-test the llvm-tally random-walk host.
set -euo pipefail

LLVM_TALLY_DIR="$1"
BUILD_DIR="$2"
PASS_PLUGIN="$3"
HOST_BINARY="$4"
REPO_ROOT="$(cd "${LLVM_TALLY_DIR}/.." && pwd)"

WORKLOAD_SO="$("${LLVM_TALLY_DIR}/scripts/build-rust-workload.sh" examples/random-walk "${BUILD_DIR}" "${PASS_PLUGIN}")"
OUTPUT="$("${HOST_BINARY}" "${WORKLOAD_SO}" "${LLVM_TALLY_DIR}/data/graph.txt" 300 100 100)"

echo "${OUTPUT}"

row_count="$(printf '%s\n' "${OUTPUT}" | awk -F, 'NR > 1 && $1 ~ /^[0-9]+$/ { count++ } END { print count + 0 }')"
if [ "${row_count}" -ne 10 ]; then
    echo "expected 10 CSV data rows, got ${row_count}" >&2
    exit 1
fi

if ! printf '%s\n' "${OUTPUT}" | awk -F, '
    NR == 1 {
        if ($1 != "thread" || $2 != "budget_per_metacycle" || $3 != "vertices_walked") exit 1
        next
    }
    $1 ~ /^[0-9]+$/ {
        if ($3 <= 0) exit 1
        if (NR > 2 && $2 <= prev_budget) exit 1
        if (NR > 2 && $3 < prev_work) exit 1
        prev_budget = $2
        prev_work = $3
    }
'; then
    echo "random-walk CSV failed shape or monotonicity checks" >&2
    exit 1
fi
