#!/usr/bin/env bash
# Fast smoke test for Rust std instrumentation tooling.
set -euo pipefail

LLVM_TALLY_DIR="$1"
BUILD_DIR="$2"
PASS_PLUGIN="$3"

STD_BUILD_DIR="${BUILD_DIR}/std-tooling-smoke"
FAKE_RUST_SRC="${STD_BUILD_DIR}/fake-rust-src"
mkdir -p \
    "${FAKE_RUST_SRC}/library/core" \
    "${FAKE_RUST_SRC}/library/alloc" \
    "${FAKE_RUST_SRC}/library/std" \
    "${FAKE_RUST_SRC}/library/panic_abort"

for crate in core alloc std panic_abort; do
    printf '[package]\nname = "%s"\nversion = "0.0.0"\nedition = "2021"\n' "${crate}" \
        > "${FAKE_RUST_SRC}/library/${crate}/Cargo.toml"
done

METADATA="$("${LLVM_TALLY_DIR}/std/scripts/prepare-rust-src.sh" \
    --rust-src "${FAKE_RUST_SRC}" \
    --build-dir "${STD_BUILD_DIR}" \
    --pass-plugin "${PASS_PLUGIN}")"

test -f "${METADATA}"
grep -q '"rust_source_path"' "${METADATA}"
grep -q '"selected_std_crates"' "${METADATA}"

PROBE_OUTPUT="$("${LLVM_TALLY_DIR}/std/scripts/probe-rustc-pass-injection.sh" \
    --build-dir "${STD_BUILD_DIR}" \
    --pass-plugin "${PASS_PLUGIN}")"
printf '%s\n' "${PROBE_OUTPUT}"

test -f "${STD_BUILD_DIR}/rustc-pass-injection.json"
grep -Eq '"status": "(supported|unsupported)"' "${STD_BUILD_DIR}/rustc-pass-injection.json"

"${LLVM_TALLY_DIR}/std/scripts/build-instrumented-std.sh" \
    --rust-src "${FAKE_RUST_SRC}" \
    --build-dir "${STD_BUILD_DIR}" \
    --pass-plugin "${PASS_PLUGIN}" \
    --check-only

grep -q '"status": "unsupported"' "${STD_BUILD_DIR}/rustc-pass-injection.json"

echo "std tooling smoke: ok"
