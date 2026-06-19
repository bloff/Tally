#!/usr/bin/env bash
# Build a std-using minithread workload when instrumented std support is ready.
set -euo pipefail

usage() {
    cat >&2 <<'USAGE'
usage: build-std-workload.sh <workload-rel-dir> <build-dir> <pass-plugin> [abi-bridge-so]

This uses the same local-std path as build-instrumented-std.sh. The first
implementation supports a single-file cdylib workload with no extra Cargo
dependencies and links it against the private instrumented sysroot. When
abi-bridge-so is provided, the workload records a DT_NEEDED dependency on the
namespace-local Tally ABI bridge.
USAGE
}

if [ "$#" -lt 3 ]; then
    usage
    exit 2
fi

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
LLVM_TALLY_DIR="$(cd "${SCRIPT_DIR}/../.." && pwd)"
WORKLOAD_REL="${1%/}"
BUILD_DIR="$2"
PASS_PLUGIN="$3"
ABI_BRIDGE="${4:-}"
TARGET="$(rustc -vV | awk '/^host:/ { print $2 }')"

REPORT="$("${SCRIPT_DIR}/build-instrumented-std.sh" \
    --build-dir "${BUILD_DIR}/std" \
    --pass-plugin "${PASS_PLUGIN}" \
    --target "${TARGET}")"

WORKLOAD_DIR="${LLVM_TALLY_DIR}/${WORKLOAD_REL}"
if [ ! -f "${WORKLOAD_DIR}/Cargo.toml" ]; then
    echo "missing workload Cargo.toml: ${WORKLOAD_DIR}/Cargo.toml" >&2
    exit 2
fi
if [ ! -f "${WORKLOAD_DIR}/src/lib.rs" ]; then
    echo "missing workload source: ${WORKLOAD_DIR}/src/lib.rs" >&2
    exit 2
fi

WORKLOAD_NAME="$(basename "${WORKLOAD_REL}")"
OUTPUT_DIR="${LLVM_TALLY_DIR}/dl/${WORKLOAD_REL}"
mkdir -p "${OUTPUT_DIR}"
OUTPUT_SO="${OUTPUT_DIR}/${WORKLOAD_NAME//-/_}.so"
PRIVATE_SYSROOT="$(python3 -c 'import json,sys; print(json.load(open(sys.argv[1]))["private_sysroot"])' "${REPORT}")"
CRATE_NAME="${WORKLOAD_NAME//-/_}"
RUSTC_LINK_ARGS=()

if [ -n "${ABI_BRIDGE}" ]; then
    if [ ! -f "${ABI_BRIDGE}" ]; then
        echo "missing ABI bridge shared object: ${ABI_BRIDGE}" >&2
        exit 2
    fi
    ABI_BRIDGE_DIR="$(cd "$(dirname "${ABI_BRIDGE}")" && pwd)"
    RUSTC_LINK_ARGS+=(
        -L "native=${ABI_BRIDGE_DIR}"
        -l dylib=tally_abi_bridge
        -C "link-arg=-Wl,-rpath,${ABI_BRIDGE_DIR}"
    )
fi

export RUSTC_BOOTSTRAP=1
rustc "${WORKLOAD_DIR}/src/lib.rs" \
    --crate-name "${CRATE_NAME}" \
    --crate-type cdylib \
    --edition=2021 \
    --target "${TARGET}" \
    --sysroot "${PRIVATE_SYSROOT}" \
    -Z unstable-options \
    -C panic=immediate-abort \
    -C no-redzone=yes \
    -C relocation-model=pic \
    "${RUSTC_LINK_ARGS[@]}" \
    -o "${OUTPUT_SO}"

echo "${OUTPUT_SO}"
