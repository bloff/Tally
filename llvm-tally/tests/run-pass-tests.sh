#!/usr/bin/env bash
# Check that the LLVM pass instruments ordinary blocks and skips Tally internals.
set -euo pipefail

PASS_PLUGIN="$1"
LLVM_TALLY_DIR="$2"
TMP_DIR="$(mktemp -d)"
trap 'rm -rf "${TMP_DIR}"' EXIT

INPUT="${LLVM_TALLY_DIR}/tests/fixtures/basic.ll"
OUTPUT="${TMP_DIR}/basic.instrumented.ll"

opt -load-pass-plugin="${PASS_PLUGIN}" -passes=tally-instrument -S "${INPUT}" -o "${OUTPUT}"

charge_count="$(grep -c "call void @__tally_charge" "${OUTPUT}")"
if [ "${charge_count}" -ne 4 ]; then
    echo "expected 4 charge calls in @foo, got ${charge_count}" >&2
    cat "${OUTPUT}" >&2
    exit 1
fi

if awk '/define void @__tally_internal/,/^}/ { if ($0 ~ /__tally_charge/) found=1 } END { exit found ? 0 : 1 }' "${OUTPUT}"; then
    echo "__tally_internal should not have been instrumented" >&2
    cat "${OUTPUT}" >&2
    exit 1
fi

if ! awk '
    /merge:/ { in_merge=1; seen_phi=0; next }
    in_merge && /phi i32/ { seen_phi=1; next }
    in_merge && /call void @__tally_charge/ { exit seen_phi ? 0 : 1 }
    in_merge && /^}/ { exit 1 }
' "${OUTPUT}"; then
    echo "expected charge call after the merge phi node" >&2
    cat "${OUTPUT}" >&2
    exit 1
fi
