#!/usr/bin/env bash
# Build local std bitcode, instrument selected crates, and repack rlib copies.
set -euo pipefail

usage() {
    cat >&2 <<'USAGE'
usage: instrument-std-artifacts.sh --pass-plugin PATH [options]

Options:
  --build-dir DIR      Build output directory.
  --target TRIPLE      Target triple, default local host.
  --manifest PATH      Cargo manifest used to drive -Z build-std.
  --crates LIST        Comma-separated crates to instrument, default core,alloc,std,panic_abort.

The script builds the local rust-src standard-library crates with
--emit=llvm-bc,link, runs opt -passes=tally-instrument over selected crate
bitcode, compiles the instrumented bitcode to object files, and repacks copied
.rlib artifacts while preserving lib.rmeta.
USAGE
}

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/../../.." && pwd)"
BUILD_DIR="${REPO_ROOT}/build/llvm-tally/std"
TARGET="$(rustc -vV | awk '/^host:/ { print $2 }')"
PASS_PLUGIN=""
MANIFEST="${REPO_ROOT}/llvm-tally/std/examples/std-vec-workload/Cargo.toml"
CRATES="core,alloc,std,panic_abort"

while [ "$#" -gt 0 ]; do
    case "$1" in
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
        --manifest)
            MANIFEST="${2:?missing --manifest value}"
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

if [ -z "${PASS_PLUGIN}" ] || [ ! -f "${PASS_PLUGIN}" ]; then
    echo "missing pass plugin: ${PASS_PLUGIN:-<unset>}" >&2
    exit 2
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

TARGET_DIR="${BUILD_DIR}/cargo-bitcode"
REPORT_DIR="${BUILD_DIR}/instrumented-std"
INSTRUMENTED_DEPS="${REPORT_DIR}/${TARGET}/deps"
PRIVATE_SYSROOT="${REPORT_DIR}/sysroot"
PRIVATE_SYSROOT_LIB="${PRIVATE_SYSROOT}/lib/rustlib/${TARGET}/lib"
OBJECT_DIR="${REPORT_DIR}/objects"
BC_DIR="${REPORT_DIR}/bitcode"
REPORT_JSON="${REPORT_DIR}/instrumented-std-report.json"

mkdir -p "${INSTRUMENTED_DEPS}" "${PRIVATE_SYSROOT_LIB}" "${OBJECT_DIR}" "${BC_DIR}"

export RUSTC_BOOTSTRAP=1
export RUSTFLAGS="-Zunstable-options -Cpanic=immediate-abort --emit=llvm-bc,link -Cembed-bitcode=yes"
cargo build \
    -Z "build-std=${CRATES}" \
    --target "${TARGET}" \
    --target-dir "${TARGET_DIR}" \
    --manifest-path "${MANIFEST}"

DEPS_DIR="${TARGET_DIR}/${TARGET}/debug/deps"
if [ ! -d "${DEPS_DIR}" ]; then
    echo "missing Cargo deps directory: ${DEPS_DIR}" >&2
    exit 1
fi

cp "${DEPS_DIR}"/*.rlib "${DEPS_DIR}"/*.rmeta "${PRIVATE_SYSROOT_LIB}/"

export CRATES PASS_PLUGIN DEPS_DIR INSTRUMENTED_DEPS PRIVATE_SYSROOT PRIVATE_SYSROOT_LIB
export OBJECT_DIR BC_DIR REPORT_JSON TARGET
python3 - <<'PY'
import json
import os
import shutil
import subprocess
import sys
from pathlib import Path

crates = [item.strip().replace("-", "_") for item in os.environ["CRATES"].split(",") if item.strip()]
pass_plugin = Path(os.environ["PASS_PLUGIN"])
deps_dir = Path(os.environ["DEPS_DIR"])
instrumented_deps = Path(os.environ["INSTRUMENTED_DEPS"])
private_sysroot = Path(os.environ["PRIVATE_SYSROOT"])
private_sysroot_lib = Path(os.environ["PRIVATE_SYSROOT_LIB"])
object_dir = Path(os.environ["OBJECT_DIR"])
bc_dir = Path(os.environ["BC_DIR"])
report_json = Path(os.environ["REPORT_JSON"])

def run(command):
    subprocess.run(command, check=True)

def capture(command):
    return subprocess.check_output(command, text=True)

def charge_count(bitcode):
    text = capture(["llvm-dis", str(bitcode), "-o", "-"])
    return text.count("__tally_charge")

artifacts = []
for crate in crates:
    matches = sorted(deps_dir.glob(f"{crate}-*.bc"))
    if not matches:
        artifacts.append({"crate": crate, "status": "missing-bitcode"})
        continue
    bitcode = matches[0]
    stem = bitcode.stem
    rlib = deps_dir / f"lib{stem}.rlib"
    if not rlib.exists():
        artifacts.append({"crate": crate, "status": "missing-rlib", "bitcode": str(bitcode)})
        continue

    archive_members = capture(["ar", "t", str(rlib)]).splitlines()
    object_members = [member for member in archive_members if member.endswith(".o")]
    if len(object_members) != 1:
        artifacts.append({
            "crate": crate,
            "status": "unsupported-rlib-layout",
            "rlib": str(rlib),
            "object_members": object_members,
        })
        continue

    instrumented_bc = bc_dir / f"{stem}.instrumented.bc"
    object_path = object_dir / object_members[0]
    output_rlib = instrumented_deps / rlib.name
    output_rmeta = instrumented_deps / f"lib{stem}.rmeta"

    before_count = charge_count(bitcode)
    run([
        "opt",
        f"-load-pass-plugin={pass_plugin}",
        "-passes=tally-instrument",
        str(bitcode),
        "-o",
        str(instrumented_bc),
    ])
    after_count = charge_count(instrumented_bc)
    run(["clang", "-c", str(instrumented_bc), "-o", str(object_path)])

    shutil.copy2(rlib, output_rlib)
    run(["ar", "r", str(output_rlib), str(object_path)])
    run(["ranlib", str(output_rlib)])
    source_rmeta = deps_dir / f"lib{stem}.rmeta"
    if source_rmeta.exists():
        shutil.copy2(source_rmeta, output_rmeta)
        shutil.copy2(source_rmeta, private_sysroot_lib / source_rmeta.name)
    shutil.copy2(output_rlib, private_sysroot_lib / output_rlib.name)

    unresolved = capture(["nm", "-u", str(object_path)])
    artifacts.append({
        "crate": crate,
        "status": "instrumented",
        "source_bitcode": str(bitcode),
        "instrumented_bitcode": str(instrumented_bc),
        "instrumented_object": str(object_path),
        "source_rlib": str(rlib),
        "instrumented_rlib": str(output_rlib),
        "charge_count_before": before_count,
        "charge_count_after": after_count,
        "has_unresolved_tally_charge": "__tally_charge" in unresolved,
    })

report = {
    "target": os.environ.get("TARGET", ""),
    "deps_dir": str(deps_dir),
    "instrumented_deps": str(instrumented_deps),
    "private_sysroot": str(private_sysroot),
    "private_sysroot_lib": str(private_sysroot_lib),
    "artifacts": artifacts,
}
report_json.parent.mkdir(parents=True, exist_ok=True)
with report_json.open("w", encoding="utf-8") as handle:
    json.dump(report, handle, indent=2, sort_keys=True)
    handle.write("\n")

failed = [item for item in artifacts if item["status"] != "instrumented"]
if failed:
    print(json.dumps(report, indent=2, sort_keys=True), file=sys.stderr)
    sys.exit(1)
PY

echo "${REPORT_JSON}"
