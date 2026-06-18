#!/usr/bin/env bash
# Build and smoke-test the llvm-tally self-contained random-walk workload.
set -euo pipefail

LLVM_TALLY_DIR="$1"
BUILD_DIR="$2"
PASS_PLUGIN="$3"
HOST_BINARY="$4"

WORKLOAD_SO="$("${LLVM_TALLY_DIR}/scripts/build-rust-workload.sh" examples/self-walk "${BUILD_DIR}" "${PASS_PLUGIN}")"
OUTPUT="$("${HOST_BINARY}" "${WORKLOAD_SO}" 300 100 100)"

echo "${OUTPUT}"

row_count="$(printf '%s\n' "${OUTPUT}" | awk -F, 'NR > 1 && $1 ~ /^[0-9]+$/ { count++ } END { print count + 0 }')"
if [ "${row_count}" -ne 10 ]; then
    echo "expected 10 CSV data rows, got ${row_count}" >&2
    exit 1
fi

if ! printf '%s\n' "${OUTPUT}" | awk -F, '
    /^thread,/ {
        if ($1 != "thread" || $2 != "budget_per_metacycle" || $4 != "vertices_walked") exit 1
        next
    }
    $1 ~ /^[0-9]+$/ {
        if ($4 <= 0) exit 1
        if (seen > 0 && $2 <= prev_budget) exit 1
        if (seen > 0 && $4 < prev_work) exit 1
        prev_budget = $2
        prev_work = $4
        seen++
    }
'; then
    echo "self-walk CSV failed shape or monotonicity checks" >&2
    exit 1
fi
