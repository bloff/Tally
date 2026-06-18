#!/usr/bin/env bash
# Build and smoke-test the llvm-tally self-contained random-walk workload.
set -euo pipefail

LLVM_TALLY_DIR="$1"
BUILD_DIR="$2"
PASS_PLUGIN="$3"
HOST_BINARY="$4"

WORKLOAD_SO="$("${LLVM_TALLY_DIR}/scripts/build-rust-workload.sh" examples/self-walk "${BUILD_DIR}" "${PASS_PLUGIN}")"
OUTPUT="$("${HOST_BINARY}" "${WORKLOAD_SO}" 10 100 10000)"

echo "${OUTPUT}"

row_count="$(printf '%s\n' "${OUTPUT}" | awk -F, 'NR > 1 && $1 ~ /^[0-9]+$/ { count++ } END { print count + 0 }')"
if [ "${row_count}" -ne 10 ]; then
    echo "expected 10 CSV data rows, got ${row_count}" >&2
    exit 1
fi

if ! printf '%s\n' "${OUTPUT}" | awk -F, '
    /^thread,/ {
        if ($1 != "thread" || $2 != "budget_per_cycle" || $3 != "target_edges" || $4 != "vertices_walked") exit 1
        next
    }
    $1 ~ /^[0-9]+$/ {
        if ($3 <= 0) exit 1
        if ($4 < $3) exit 1
        total += $4
        seen++
    }
    END { if (total < 10000) exit 1 }
'; then
    echo "self-walk CSV failed shape or monotonicity checks" >&2
    exit 1
fi
