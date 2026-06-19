#!/usr/bin/env bash
# Optional slow smoke test for a std-using workload linked to instrumented std.
set -euo pipefail

LLVM_TALLY_DIR="$1"
BUILD_DIR="$2"
PASS_PLUGIN="$3"
HOST_BINARY="$4"
ABI_BRIDGE="${5:-}"

if [ -z "${ABI_BRIDGE}" ]; then
    echo "usage: smoke-std-workload.sh <llvm-tally-dir> <build-dir> <pass-plugin> <host-binary> <abi-bridge-so>" >&2
    exit 2
fi

if [ "${LLVM_TALLY_RUN_STD_SLOW:-0}" != "1" ]; then
    echo "std workload smoke skipped; set LLVM_TALLY_RUN_STD_SLOW=1 to run"
    exit 0
fi

WORKLOAD_SO="$("${LLVM_TALLY_DIR}/std/scripts/build-std-workload.sh" \
    std/examples/std-vec-workload \
    "${BUILD_DIR}" \
    "${PASS_PLUGIN}" \
    "${ABI_BRIDGE}")"

if command -v readelf >/dev/null 2>&1; then
    if ! readelf -d "${WORKLOAD_SO}" | grep -q 'libtally_abi_bridge.so'; then
        echo "std workload is not linked against libtally_abi_bridge.so" >&2
        exit 1
    fi
fi

OUTPUT="$("${HOST_BINARY}" "${WORKLOAD_SO}" 4 512 10000 "${ABI_BRIDGE}" 2)"

echo "${OUTPUT}"

if ! printf '%s\n' "${OUTPUT}" | awk -F, '
    NR == 1 {
        if ($1 != "round" || $6 != "charges") exit 1
        next
    }
    $1 ~ /^[0-9]+$/ {
        if ($4 != $3) exit 1
        if ($6 <= 0) exit 1
        if ($7 != "Returned") exit 1
        rounds[$1]++
        seen++
    }
    END { if (seen != 8 || rounds[0] != 4 || rounds[1] != 4) exit 1 }
'; then
    echo "std workload smoke failed" >&2
    exit 1
fi

echo "std workload smoke: ok"
