#!/bin/bash
# Compile one repo-relative instrumented C source into the matching dl/*.so.
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
SOURCE_REL="${1%.c}"
SOURCE_FILE="${REPO_ROOT}/${SOURCE_REL}.c"
OUTPUT_FILE="${REPO_ROOT}/dl/${SOURCE_REL}.so"

if ! [ -f "${SOURCE_FILE}" ]; then
    echo "missing instrumented source: ${SOURCE_FILE}" >&2
    exit 1
fi

mkdir -p "$(dirname "${OUTPUT_FILE}")"

INSTRUMENT_FLAGS="-ffixed-r15 -I${REPO_ROOT}/include -fplugin=${REPO_ROOT}/bin/gcc-tally.so -nostdlib -g -W"
MINITHREAD_FLAGS="-L${REPO_ROOT}/bin -l:minithread.a"
LINK_FLAGS="-Wl,-R -Wl,. -Wl,--export-dynamic"

gcc -fno-pie $INSTRUMENT_FLAGS -fPIC -shared -fPIC $LINK_FLAGS $MINITHREAD_FLAGS "${SOURCE_FILE}" -o "${OUTPUT_FILE}"
