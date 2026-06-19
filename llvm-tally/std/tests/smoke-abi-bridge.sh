#!/usr/bin/env bash
# Check that the namespace-local ABI bridge exports the symbols workloads need.
set -euo pipefail

ABI_BRIDGE="$1"

if [ ! -f "${ABI_BRIDGE}" ]; then
    echo "missing ABI bridge shared object: ${ABI_BRIDGE}" >&2
    exit 1
fi

if command -v nm >/dev/null 2>&1; then
    SYMBOLS="$(nm -D --defined-only "${ABI_BRIDGE}")"
elif command -v readelf >/dev/null 2>&1; then
    SYMBOLS="$(readelf -Ws "${ABI_BRIDGE}")"
else
    echo "neither nm nor readelf is available for ABI bridge inspection" >&2
    exit 1
fi

for symbol in \
    tally_abi_set_hooks \
    __tally_charge \
    __tally_alloc \
    __tally_dealloc \
    __tally_realloc
do
    if ! printf '%s\n' "${SYMBOLS}" | grep -Eq "[[:space:]]${symbol}$"; then
        echo "ABI bridge does not export ${symbol}" >&2
        exit 1
    fi
done

echo "ABI bridge symbols: ok"
