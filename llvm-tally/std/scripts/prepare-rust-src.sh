#!/usr/bin/env bash
# Verify Rust standard-library sources and record local toolchain metadata.
set -euo pipefail

usage() {
    cat >&2 <<'USAGE'
usage: prepare-rust-src.sh [--rust-src PATH] [--build-dir DIR] [--target TRIPLE]
                           [--pass-plugin PATH] [--crates LIST]

Finds a Rust source tree whose library/ directory matches the local Rust
installation, verifies the std crate set, and writes toolchain.json under DIR.

On Arch Linux, the preferred source for local std is the rust-src package:
  sudo pacman -S rust-src

You can also pass --rust-src /path/to/rust or set LLVM_TALLY_RUST_SRC.
USAGE
}

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/../../.." && pwd)"
BUILD_DIR="${REPO_ROOT}/build/llvm-tally/std"
TARGET="$(rustc -vV | awk '/^host:/ { print $2 }')"
PASS_PLUGIN=""
CRATES="core,alloc,std,panic_abort"
RUST_SRC="${LLVM_TALLY_RUST_SRC:-}"

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

mkdir -p "${BUILD_DIR}"

resolve_rust_src() {
    local candidate="$1"
    if [ -z "${candidate}" ]; then
        return 1
    fi

    if [ -f "${candidate}/library/std/Cargo.toml" ]; then
        cd "${candidate}" && pwd
        return 0
    fi

    if [ -f "${candidate}/std/Cargo.toml" ] && [ "$(basename "${candidate}")" = "library" ]; then
        cd "${candidate}/.." && pwd
        return 0
    fi

    return 1
}

SYSROOT="$(rustc --print sysroot)"
SYSROOT_RUST_SRC="${SYSROOT}/lib/rustlib/src/rust"

RUST_SRC_RESOLVED=""
for candidate in \
    "${RUST_SRC}" \
    "${REPO_ROOT}/third_party/rust" \
    "${SYSROOT_RUST_SRC}" \
    "/usr/lib/rustlib/src/rust"
do
    if RUST_SRC_RESOLVED="$(resolve_rust_src "${candidate}")"; then
        break
    fi
done

if [ -z "${RUST_SRC_RESOLVED}" ]; then
    cat >&2 <<EOF
Rust standard-library sources were not found.

This branch is meant to instrument the std version matching the local rustc:
$(rustc --version)

On Arch Linux, install the matching source package:
  sudo pacman -S rust-src

Or pass a matching rust-lang/rust checkout:
  llvm-tally/std/scripts/prepare-rust-src.sh --rust-src /path/to/rust
EOF
    exit 3
fi

IFS=',' read -r -a CRATE_ARRAY <<< "${CRATES}"
for crate in "${CRATE_ARRAY[@]}"; do
    crate="${crate// /}"
    if [ -z "${crate}" ]; then
        continue
    fi
    if [ ! -f "${RUST_SRC_RESOLVED}/library/${crate}/Cargo.toml" ]; then
        echo "missing Rust library crate source: library/${crate}/Cargo.toml" >&2
        exit 4
    fi
done

RUST_SOURCE_COMMIT="not-a-git-checkout"
if git -C "${RUST_SRC_RESOLVED}" rev-parse --is-inside-work-tree >/dev/null 2>&1; then
    RUST_SOURCE_COMMIT="$(git -C "${RUST_SRC_RESOLVED}" rev-parse HEAD)"
fi

RUSTC_VERSION="$(rustc -Vv 2>&1 || true)"
CARGO_VERSION="$(cargo -Vv 2>&1 || true)"
OPT_VERSION="$(opt --version 2>&1 | head -n 1 || true)"
LLVM_CONFIG_VERSION="$(llvm-config --version 2>&1 || true)"
RUSTC_RELEASE="$(rustc -Vv | awk '/^release:/ { print $2 }')"
METADATA="${BUILD_DIR}/toolchain.json"

export METADATA RUST_SRC_RESOLVED RUST_SOURCE_COMMIT RUSTC_VERSION CARGO_VERSION
export SYSROOT TARGET PASS_PLUGIN CRATES OPT_VERSION LLVM_CONFIG_VERSION RUSTC_RELEASE
python3 - <<'PY'
import json
import os

data = {
    "rust_source_path": os.environ["RUST_SRC_RESOLVED"],
    "rust_source_commit": os.environ["RUST_SOURCE_COMMIT"],
    "rustc_release": os.environ["RUSTC_RELEASE"],
    "rustc_version_verbose": os.environ["RUSTC_VERSION"],
    "cargo_version_verbose": os.environ["CARGO_VERSION"],
    "sysroot": os.environ["SYSROOT"],
    "target": os.environ["TARGET"],
    "selected_std_crates": [
        item.strip()
        for item in os.environ["CRATES"].split(",")
        if item.strip()
    ],
    "pass_plugin": os.environ["PASS_PLUGIN"],
    "opt_version": os.environ["OPT_VERSION"],
    "llvm_config_version": os.environ["LLVM_CONFIG_VERSION"],
}

with open(os.environ["METADATA"], "w", encoding="utf-8") as handle:
    json.dump(data, handle, indent=2, sort_keys=True)
    handle.write("\n")
PY

echo "${METADATA}"
