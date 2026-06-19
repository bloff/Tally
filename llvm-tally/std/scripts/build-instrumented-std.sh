#!/usr/bin/env bash
# Build a private std-using target with Tally instrumentation when supported.
set -euo pipefail

usage() {
    cat >&2 <<'USAGE'
usage: build-instrumented-std.sh --pass-plugin PATH [options]

Options:
  --rust-src PATH      Rust source tree matching the local rustc.
  --build-dir DIR      Build output directory.
  --target TRIPLE      Target triple, default local host.
  --crates LIST        Comma-separated std crates, default core,alloc,std,panic_abort.
  --check-only         Verify metadata/probes without attempting the std build.

This script intentionally targets the std version matching the local rustc. On
Arch Linux that normally means installing:
  sudo pacman -S rust-src

If local rustc cannot load the LLVM pass directly, the script falls back to
building local std bitcode, running opt over selected crates, and repacking
instrumented rlib copies.
USAGE
}

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/../../.." && pwd)"
BUILD_DIR="${REPO_ROOT}/build/llvm-tally/std"
TARGET="$(rustc -vV | awk '/^host:/ { print $2 }')"
PASS_PLUGIN=""
RUST_SRC=""
CRATES="core,alloc,std,panic_abort"
CHECK_ONLY=0

while [ "$#" -gt 0 ]; do
    case "$1" in
        --rust-src)
            RUST_SRC="${2:?missing --rust-src value}"
            shift 2
            ;;
        --build-dir)
            BUILD_DIR="${2:?missing --build-dir value}"
            shift 2
            ;;
        --target)
            TARGET="${2:?missing --target value}"
            shift 2
            ;;
        --pass-plugin)
            PASS_PLUGIN="${2:?missing --pass-plugin value}"
            shift 2
            ;;
        --crates)
            CRATES="${2:?missing --crates value}"
            shift 2
            ;;
        --check-only)
            CHECK_ONLY=1
            shift
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

PREPARE_ARGS=(--build-dir "${BUILD_DIR}" --target "${TARGET}" --pass-plugin "${PASS_PLUGIN}" --crates "${CRATES}")
if [ -n "${RUST_SRC}" ]; then
    PREPARE_ARGS+=(--rust-src "${RUST_SRC}")
fi
"${SCRIPT_DIR}/prepare-rust-src.sh" "${PREPARE_ARGS[@]}" >/dev/null
"${SCRIPT_DIR}/probe-rustc-pass-injection.sh" --build-dir "${BUILD_DIR}" --pass-plugin "${PASS_PLUGIN}" >/dev/null

if [ "${CHECK_ONLY}" -eq 1 ]; then
    echo "std instrumentation prerequisites checked"
    exit 0
fi

SYSROOT_RUST_SRC="$(rustc --print sysroot)/lib/rustlib/src/rust"
if [ ! -f "${SYSROOT_RUST_SRC}/library/std/Cargo.toml" ]; then
    cat >&2 <<EOF
Cargo build-std needs Rust sources in the active sysroot:
  ${SYSROOT_RUST_SRC}

On Arch Linux, install the matching local std sources:
  sudo pacman -S rust-src
EOF
    exit 5
fi

PROBE_STATUS="$(python3 -c 'import json,sys; print(json.load(open(sys.argv[1]))["status"])' "${BUILD_DIR}/rustc-pass-injection.json")"
if [ "${PROBE_STATUS}" != "supported" ]; then
    echo "direct rustc pass injection unsupported; using bitcode/rlib fallback" >&2
    exec "${SCRIPT_DIR}/instrument-std-artifacts.sh" \
        --build-dir "${BUILD_DIR}" \
        --pass-plugin "${PASS_PLUGIN}" \
        --target "${TARGET}" \
        --crates "${CRATES}"
fi

PROBE_CRATE="${BUILD_DIR}/std-build-probe"
mkdir -p "${PROBE_CRATE}/src"
cat > "${PROBE_CRATE}/Cargo.toml" <<'TOML'
[package]
name = "llvm-tally-std-build-probe"
version = "0.1.0"
edition = "2021"
TOML
cat > "${PROBE_CRATE}/src/main.rs" <<'RS'
fn main() {
    let mut values = Vec::new();
    for i in 0..32_u64 {
        values.push(i);
    }
    println!("{}", values.iter().copied().sum::<u64>());
}
RS

export RUSTC_BOOTSTRAP=1
export RUSTFLAGS="-Zunstable-options -Cllvm-args=-load-pass-plugin=${PASS_PLUGIN} -Cpasses=tally-instrument -Cpanic=immediate-abort -Cno-redzone=yes -Cembed-bitcode=yes"
cargo build \
    -Z "build-std=${CRATES}" \
    --target "${TARGET}" \
    --release \
    --target-dir "${BUILD_DIR}/cargo" \
    --manifest-path "${PROBE_CRATE}/Cargo.toml"

touch "${BUILD_DIR}/instrumented-std-ready"
echo "${BUILD_DIR}/instrumented-std-ready"
