# Rust Standard Library Instrumentation Plan

## Goal

Build a private Rust sysroot whose target-side standard-library crates are
instrumented by LLVM Tally, while keeping upstream Rust source code as a clean
reference dependency. The host scheduler/runtime should continue to use the
normal system Rust standard library. Only code that runs inside Tally
minithread contexts should link against the instrumented standard library.

## Guiding Constraints

- Do not fork Rust as part of this repository.
- Prefer zero source edits to Rust. If source edits become necessary, keep them
  as a tiny patch queue applied to a temporary worktree.
- Compile only standard-library crates first, not the entire Rust compiler.
- Keep updates easy: advancing Rust should mostly mean moving an upstream git
  reference, rebuilding, and rerunning compatibility tests.
- Keep the instrumented sysroot separate from the host toolchain sysroot.
- Do not instrument the host scheduler, host runtime, build scripts, proc
  macros, or normal command-line tooling.

## Repository Shape

Use upstream Rust as a pinned reference:

```text
third_party/rust/                 # git submodule or external checkout of rust-lang/rust
llvm-tally/std/
  plan.md                         # this plan
  patches/                        # ideally empty; tiny format-patch queue if needed
  scripts/
    prepare-rust-src.sh           # verify or fetch/check out matching Rust source
    build-instrumented-std.sh     # build private instrumented target sysroot
    build-std-workload.sh         # build std-using minithread workload against it
    update-rust-upstream.sh       # move pinned Rust source and reapply patches
  examples/
    std-walk/                     # first workload that uses Vec/Box/iterators
  tests/
    smoke-std-sysroot.sh
    smoke-std-workload.sh
```

`third_party/rust/` should be a submodule if we want a reproducible pinned
upstream commit in Git. If we want to avoid submodule friction, use an external
checkout path plus a lock file that records the expected Rust commit. In either
case, generated build trees and sysroots should stay ignored.

## Rust Sources

Source acquisition should support two modes:

1. User-provided:

   ```sh
   llvm-tally/std/scripts/prepare-rust-src.sh --rust-src /path/to/rust
   ```

2. Managed reference:

   ```sh
   git submodule add https://github.com/rust-lang/rust third_party/rust
   git -C third_party/rust checkout <matching-rust-commit-or-tag>
   ```

The script should verify:

- `library/std/Cargo.toml` exists.
- `library/core`, `library/alloc`, `library/std`, and panic crates are present.
- the Rust source version is compatible with the local `rustc`.
- local toolchain metadata is recorded, including `rustc -Vv`, `cargo -Vv`,
  `rustc --print sysroot`, `opt --version`, and `llvm-config --version`.

Current local observation when this plan was written:

- `rustc 1.95.0`, distro install, sysroot `/usr`.
- `rustup` is not installed.
- no installed `rust-src` tree was found under the usual `/usr` locations.
- `rustc` reports LLVM `22.1.3`; system `opt`/`clang`/`llvm-config` report
  LLVM `22.1.5`.

That means the first implementation should not assume `rustup component add
rust-src`; it should accept or fetch sources explicitly.

## Preferred Build Strategy

### Primary route: std-aware Cargo with a private target sysroot

Cargo's `-Z build-std` is the intended light path for compiling standard
library crates as part of a build graph. It can build `core`, `alloc`, `std`,
`proc_macro`, and, for tests, `test`; it also supports selecting a subset such
as `core,alloc`. The documented requirements are nightly Cargo/rustc, `rust-src`
sources, and passing `-Z build-std` to all relevant Cargo invocations.

For Tally, the first target should be:

```sh
cargo +nightly build \
  -Z build-std=core,alloc,std,panic_abort \
  -Z build-std-features=panic_immediate_abort \
  --target x86_64-unknown-linux-gnu
```

The host runtime should not be built with this sysroot. Only the instrumented
workload crate should be.

### Fallback route: rustc wrapper around std crate compilation

If direct pass injection through Cargo/rustc is not viable, use a `RUSTC_WRAPPER`
or `RUSTC_WORKSPACE_WRAPPER`-style tool for the target build only. The wrapper
should:

- detect standard-library crates by crate name and source path;
- leave host artifacts, build scripts, proc macros, and the Tally host alone;
- force or capture LLVM bitcode for target std crates;
- run `opt -load-pass-plugin ... -passes=tally-instrument`;
- feed the instrumented bitcode back into the artifact creation flow.

This is more fragile than direct rustc pass injection, especially for `.rlib`
packing and metadata. It should be a fallback, not the first design.

### Heavy fallback: Rust bootstrap

Avoid full bootstrap unless the above paths fail. Rust's `./x build library`
builds a working stage1 compiler and standard libraries, which is much heavier
than we want. It is useful as a reference for how upstream assembles sysroots,
but not as the default Tally workflow.

## Prerequisites To Add On The Instrumentation Branch

Before std instrumentation is pleasant, the LLVM instrumentation branch should
grow a few features that are useful beyond std:

1. **Pass options**

   Add configurable include/exclude rules:

   - skip crate/function/module names matching `__tally_*`;
   - optionally instrument only functions whose source path is under selected
     roots;
   - optionally skip panic/unwind/personality/runtime symbols;
   - emit a summary of instrumented/skipped functions.

2. **Rustc pass-injection probe**

   Add a script/test that asks whether the current `rustc` can load the Tally
   LLVM pass directly through codegen LLVM args. This should fail cleanly with a
   diagnostic if the rustc LLVM ABI and system LLVM plugin ABI are incompatible.

3. **Bitcode artifact tooling**

   Add helpers for:

   - finding emitted `.bc`/`.ll` for a crate;
   - running the pass over bitcode;
   - linking or repacking artifacts;
   - dumping before/after instrumentation counts.

4. **Runtime reentrancy guard**

   `__tally_charge` currently returns immediately when no Tally thread is active.
   That is good for host safety. We should add tests for this behavior and make
   the intended contract explicit: instrumented std code may exist in a process,
   but it only charges/yields while executing inside a Tally minithread.

5. **Symbol and linkage sanity checks**

   Add tests that an instrumented shared object can resolve `__tally_charge`,
   and that ordinary host calls into Rust `std` do not accidentally use the
   instrumented sysroot.

6. **Cost-model knobs**

   Standard-library code will have very different basic-block shapes from the
   toy workloads. The pass should expose a small set of stable cost-model
   options so we can compare:

   - flat per-basic-block charge;
   - current simple instruction-kind cost;
   - call-heavy weighting;
   - optional no-charge for tiny prologue/cleanup blocks.

## Implementation Phases

### Phase 0: Branch and documentation

- Keep this plan on `codex/rust-std-tally`.
- Return to the LLVM instrumentation branch for prerequisite pass/runtime
  features.
- Merge or rebase this std branch after those features are ready.

### Phase 1: Toolchain and source discovery

Add `prepare-rust-src.sh` and a small metadata file under the build directory:

```text
build/llvm-tally/std/toolchain.json
```

It should record:

- Rust source path and commit;
- rustc/cargo versions;
- sysroot;
- target triple;
- LLVM tool versions;
- selected std crates;
- selected instrumentation pass path.

### Phase 2: Rustc pass-injection experiment

Try a tiny ordinary `std` program:

```rust
fn main() {
    let mut v = Vec::new();
    v.push(1);
    println!("{}", v[0]);
}
```

Build it with a private target directory and attempt to pass the LLVM plugin to
rustc. Success criteria:

- build succeeds;
- generated IR/object contains calls to `__tally_charge` in application code;
- host execution outside a Tally thread does not crash;
- `__tally_charge` no-op path is exercised when no current Tally thread exists.

If pass loading fails, preserve the error in docs and move to the wrapper route.

### Phase 3: Build an instrumented private std sysroot

Build a private sysroot containing target-side instrumented:

- `core`
- `alloc`
- `std`
- `panic_abort`

Avoid `panic_unwind`, `backtrace`, and `test` in the first pass. Prefer aborting
panic behavior so minithread stack switching does not have to interact with Rust
unwinding.

Outputs should live under:

```text
build/llvm-tally/std/sysroot/
```

The scripts should never overwrite `/usr` or any rustup sysroot.

### Phase 4: std-using minithread workload

Add a workload that is impossible to satisfy with the current `#![no_std]`
pipeline, for example:

- allocate `Vec<u64>`;
- perform repeated push/pop/sort or iterator work;
- optionally use `Box<[u64]>`;
- export counters through the existing C ABI pattern.

Build this workload against the private instrumented sysroot and load it into
the existing Tally runtime.

Success criteria:

- the workload runs inside Tally minithreads;
- `__tally_charge` count increases from both workload and std/alloc code;
- host runtime still uses normal `std`;
- budget proportionality remains measurable.

### Phase 5: Tests and reports

Add tests:

- source discovery smoke test;
- rustc pass-injection smoke test;
- private sysroot build smoke test, allowed to be opt-in/slow;
- std workload integration test;
- host safety test where ordinary host `Vec`/I/O does not charge;
- benchmark comparing no-std self-walk vs std-heavy workload.

Add reports:

- total functions instrumented by crate;
- charge count by workload phase;
- throughput and fairness under std-heavy code;
- sysroot build metadata.

### Phase 6: Update workflow

Add `update-rust-upstream.sh`:

1. fetch upstream Rust;
2. move the submodule/reference to a requested tag or commit;
3. create a temporary worktree;
4. apply `llvm-tally/std/patches/*.patch` if any;
5. rebuild private sysroot;
6. run smoke tests;
7. write an update report.

If patches fail to apply, the script should stop with a clear message and leave
the temporary worktree intact for manual inspection.

## Open Questions

- Can the local rustc load the system-built LLVM pass plugin despite the LLVM
  patch-version mismatch? Answer from the first implementation: no, local
  rustc rejects `-load-pass-plugin` through `-C llvm-args`. The working route is
  build-std bitcode capture plus `opt` artifact rewriting.
- Do we need nightly installed locally, or can distro Rust's unstable Cargo
  flags be used with a bootstrap escape hatch? Answer from the first
  implementation: distro Rust 1.95 works with `RUSTC_BOOTSTRAP=1`, local
  `rust-src`, and `-Cpanic=immediate-abort`.
- What is the minimal crate set for a useful `std` workload: `core,alloc,std`
  only, or do we need `panic_abort` and compiler-builtins details immediately?
- Should instrumented std be linked into shared-object workloads only, or do we
  also want full executables using the private sysroot?
- How much of `std` should be charged? For example, should allocator and panic
  paths count the same way as normal library code?

## Current Implementation Snapshot

The first working implementation now builds the local Rust 1.95 std sources
from `/usr/lib/rustlib/src/rust`, emits LLVM bitcode for build-std crates,
instruments `core`, `alloc`, `std`, and `panic_abort` with the existing
`tally-instrument` `opt` pass, compiles the instrumented bitcode back to object
files, and repacks copied `.rlib` artifacts while preserving metadata.

The generated private sysroot is build-local and does not modify `/usr`. The
first std-heavy workload uses `Vec`, `Box<[u64]>`, iterators, and sorting, then
loads as a shared object into the normal Tally runtime. The opt-in
`llvm_std_workload_smoke` test verifies that this workload runs inside Tally
minithreads and accumulates nonzero charge counts from instrumented std code.

## References

- Cargo unstable `build-std` documentation:
  https://doc.rust-lang.org/cargo/reference/unstable.html#build-std
- Cargo `build-std-features` documentation:
  https://doc.rust-lang.org/cargo/reference/unstable.html#build-std-features
- Rust compiler development guide, building with `x`:
  https://rustc-dev-guide.rust-lang.org/building/how-to-build-and-run.html
- Rust compiler development guide, bootstrapping stages:
  https://rustc-dev-guide.rust-lang.org/building/bootstrapping/what-bootstrapping-does.html
