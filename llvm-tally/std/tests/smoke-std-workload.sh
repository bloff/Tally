#!/usr/bin/env bash
# Optional slow smoke test for a std-using workload linked to instrumented std.
set -euo pipefail

LLVM_TALLY_DIR="$1"
BUILD_DIR="$2"
PASS_PLUGIN="$3"
HOST_BINARY="$4"

if [ "${LLVM_TALLY_RUN_STD_SLOW:-0}" != "1" ]; then
    echo "std workload smoke skipped; set LLVM_TALLY_RUN_STD_SLOW=1 to run"
    exit 0
fi

WORKLOAD_SO="$("${LLVM_TALLY_DIR}/std/scripts/build-std-workload.sh" \
    std/examples/std-vec-workload \
    "${BUILD_DIR}" \
    "${PASS_PLUGIN}")"
OUTPUT="$("${HOST_BINARY}" "${WORKLOAD_SO}" 4 512 10000)"

echo "${OUTPUT}"

if ! printf '%s\n' "${OUTPUT}" | awk -F, '
    NR == 1 {
        if ($1 != "thread" || $5 != "charges") exit 1
        next
    }
    $1 ~ /^[0-9]+$/ {
        if ($3 != $2) exit 1
        if ($5 <= 0) exit 1
        if ($6 != "Returned") exit 1
        seen++
    }
    END { if (seen != 4) exit 1 }
'; then
    echo "std workload smoke failed" >&2
    exit 1
fi

echo "std workload smoke: ok"
