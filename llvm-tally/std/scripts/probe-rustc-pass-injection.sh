#!/usr/bin/env bash
# Probe whether the local rustc can load the llvm-tally pass directly.
set -euo pipefail

usage() {
    cat >&2 <<'USAGE'
usage: probe-rustc-pass-injection.sh --pass-plugin PATH [--build-dir DIR]

This is a probe, not a hard requirement test. It writes
rustc-pass-injection.json and exits successfully whether direct pass injection
is supported or not. Unsupported means the std path must use bitcode artifact
rewriting or another build driver.
USAGE
}

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/../../.." && pwd)"
BUILD_DIR="${REPO_ROOT}/build/llvm-tally/std"
PASS_PLUGIN=""

while [ "$#" -gt 0 ]; do
    case "$1" in
        --pass-plugin)
            PASS_PLUGIN="${2:?missing --pass-plugin value}"
            shift 2
            ;;
        --build-dir)
            BUILD_DIR="${2:?missing --build-dir value}"
            shift 2
            ;;
        -h|--help)
            usage
            exit 0
            ;;
        *)
            echo "unknown argument: $1" >&2
            usage
            exit 2
            ;;
    esac
done

if [ -z "${PASS_PLUGIN}" ] || [ ! -f "${PASS_PLUGIN}" ]; then
    echo "missing pass plugin: ${PASS_PLUGIN:-<unset>}" >&2
    exit 2
fi

PROBE_DIR="${BUILD_DIR}/probes/rustc-pass-injection"
mkdir -p "${PROBE_DIR}"
SOURCE="${PROBE_DIR}/main.rs"
OUTPUT_LL="${PROBE_DIR}/main.ll"
STDERR_LOG="${PROBE_DIR}/rustc.stderr"
RESULT_JSON="${BUILD_DIR}/rustc-pass-injection.json"

cat > "${SOURCE}" <<'RS'
fn main() {
    let mut values = Vec::new();
    values.push(1_u64);
    println!("{}", values[0]);
}
RS

STATUS="unsupported"
REASON="rustc did not inject __tally_charge"
set +e
rustc "${SOURCE}" \
    --emit=llvm-ir \
    -C panic=abort \
    -C codegen-units=1 \
    -C no-redzone=yes \
    -C "llvm-args=-load-pass-plugin=${PASS_PLUGIN}" \
    -C passes=tally-instrument \
    -o "${OUTPUT_LL}" \
    2> "${STDERR_LOG}"
RUSTC_STATUS="$?"
set -e

if [ "${RUSTC_STATUS}" -eq 0 ] && grep -q "__tally_charge" "${OUTPUT_LL}"; then
    STATUS="supported"
    REASON="rustc accepted the LLVM pass plugin and emitted __tally_charge"
elif [ "${RUSTC_STATUS}" -ne 0 ]; then
    REASON="$(head -n 8 "${STDERR_LOG}" | tr '\n' ' ')"
fi

export RESULT_JSON STATUS REASON RUSTC_STATUS PASS_PLUGIN OUTPUT_LL STDERR_LOG
python3 - <<'PY'
import json
import os

data = {
    "status": os.environ["STATUS"],
    "reason": os.environ["REASON"],
    "rustc_status": int(os.environ["RUSTC_STATUS"]),
    "pass_plugin": os.environ["PASS_PLUGIN"],
    "llvm_ir": os.environ["OUTPUT_LL"],
    "stderr_log": os.environ["STDERR_LOG"],
}

with open(os.environ["RESULT_JSON"], "w", encoding="utf-8") as handle:
    json.dump(data, handle, indent=2, sort_keys=True)
    handle.write("\n")
PY

echo "${RESULT_JSON}"
if [ "${STATUS}" = "supported" ]; then
    echo "rustc pass injection: supported"
else
    echo "rustc pass injection: unsupported"
    echo "${REASON}"
fi
