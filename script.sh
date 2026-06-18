#!/bin/bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

# $1- func name
INSTRUMENT_FLAGS="-ffixed-r15 -I${SCRIPT_DIR}/src -fplugin=${SCRIPT_DIR}/bin/gcc-tally.so -nostdlib -g -W"
MINITHREAD_FLAGS="-L${SCRIPT_DIR}/bin -l:minithread.a"
LINK_FLAGS="-Wl,-R -Wl,. -Wl,--export-dynamic"

#echo "path"
#pwd

#echo "compiling with"
#if ! [ -f "./../dl/$1.so" ]; then
gcc -fno-pie $INSTRUMENT_FLAGS -fPIC -shared -fPIC $LINK_FLAGS $MINITHREAD_FLAGS "${SCRIPT_DIR}/code/$1.c" -o "${SCRIPT_DIR}/dl/$1.so"

#echo "end loading"
