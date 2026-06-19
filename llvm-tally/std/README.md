# LLVM Tally Rust Std Tooling

This directory holds the first tooling for building toward an instrumented Rust
standard library. The intended source version is the one matching the local
Rust installation, not an arbitrary nightly std.

On the current Arch setup, `rustc` is `1.95.0` and `rustup` is not installed.
Install matching std sources with:

```sh
sudo pacman -S rust-src
```

The local `rustc` rejects direct `-load-pass-plugin` injection through
`-C llvm-args`, so the working path is a bitcode/rlib fallback:

1. build local std crates with `-Z build-std` and `--emit=llvm-bc,link`;
2. run `opt -passes=tally-instrument` over selected crate bitcode;
3. compile the instrumented bitcode back to object files;
4. repack copied `.rlib` files while preserving `lib.rmeta`;
5. assemble a private sysroot under the build directory;
6. compile std-using workloads against that private sysroot and the tiny
   namespace-local `libtally_abi_bridge.so`.

The ABI bridge is loaded with `dlmopen` into a dedicated linker namespace for
the instrumented std profile. The host manager keeps using the process's normal
Rust `std` in the base namespace, while workloads loaded into the std namespace
resolve `__tally_charge` and future allocation hooks through the bridge. Many
workloads can share the same std namespace; the design does not allocate one
namespace per workload.

The normal build path is:

```sh
cmake -S llvm-tally -B /tmp/tally-llvm-build
cmake --build /tmp/tally-llvm-build
llvm-tally/std/scripts/prepare-rust-src.sh \
  --pass-plugin /tmp/tally-llvm-build/llvm-tally-pass.so
llvm-tally/std/scripts/probe-rustc-pass-injection.sh \
  --pass-plugin /tmp/tally-llvm-build/llvm-tally-pass.so
llvm-tally/std/scripts/build-instrumented-std.sh \
  --pass-plugin /tmp/tally-llvm-build/llvm-tally-pass.so
```

That produces `instrumented-std-report.json`, instrumented bitcode/object files,
repacked rlibs, and a private sysroot. To build and run the first std-heavy
workload:

```sh
WORKLOAD_SO="$(llvm-tally/std/scripts/build-std-workload.sh \
  std/examples/std-vec-workload \
  /tmp/tally-llvm-build \
  /tmp/tally-llvm-build/llvm-tally-pass.so \
  /tmp/tally-llvm-build/bin/libtally_abi_bridge.so)"
/tmp/tally-llvm-build/bin/llvm-tally-std-vec-host \
  "${WORKLOAD_SO}" 4 512 10000 \
  /tmp/tally-llvm-build/bin/libtally_abi_bridge.so
```

The host creates one std environment, loads the workload into that namespace,
runs it as Tally minithreads, and can unload namespace-local workload handles
after all minithreads are returned or errored.

Fast CI coverage is provided by `llvm_std_tooling_smoke`, which checks source
metadata generation, pass-injection probing, and clean prerequisite reporting
without requiring a full std build. `llvm_abi_bridge_symbols` verifies that the
bridge exports the namespace ABI required by instrumented workloads.

The full local-std workload smoke is intentionally opt-in because it rebuilds
local std and takes tens of seconds:

```sh
LLVM_TALLY_RUN_STD_SLOW=1 \
ctest --test-dir /tmp/tally-llvm-build --output-on-failure -R llvm_std_workload_smoke
```

The slow smoke test rebuilds local std, checks that the workload has a
`DT_NEEDED` dependency on `libtally_abi_bridge.so`, then runs two
load/run/unload rounds through the namespace-aware host.
