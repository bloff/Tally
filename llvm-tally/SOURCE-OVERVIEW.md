# LLVM Tally Source Overview

## Summary

LLVM Tally proves that the Tally model can be expressed for Rust by
instrumenting LLVM IR and yielding through a Rust runtime, without changing
rustc itself.

## Components

- `pass/`: LLVM new-pass-manager plugin. The `tally-instrument` FunctionPass
  inserts `__tally_charge(i64 cost)` calls into eligible basic blocks.
- `runtime/`: Rust minithread manager, stackful context switching, dynamic
  loading, CPU budget accounting, memory-limit accounting, and exported C ABI
  hooks such as `__tally_charge`, `__tally_alloc`, `__tally_dealloc`, and
  `__tally_realloc`.
- `examples/random-walk/workload/`: controlled `#![no_std]` Rust workload that
  calls host graph functions and runs forever.
- `examples/random-walk/host/`: Rust host executable that loads the instrumented
  workload and runs ten minithreads with linearly increasing budgets.
- `examples/self-walk/workload/`: controlled `#![no_std]` Rust workload that
  performs a synthetic graph walk without host graph calls.
- `examples/self-walk/host/`: Rust host executable for the self-contained
  benchmark; it reports the workload-owned vertex counters after fixed
  scheduler metacycles.
- `examples/memory-recovery/workload/`: controlled `#![no_std]` workload that
  intentionally exhausts heap, recursive stack, and large-frame stack limits.
- `examples/memory-recovery/host/`: Rust host executable that verifies memory
  failures mark only the offending minithread `Errored` and the scheduler can
  continue running healthy minithreads.
- `std/abi/`: tiny C ABI bridge built as `libtally_abi_bridge.so`. Std-using
  workloads link against this bridge so `__tally_charge` and allocator symbols
  such as `malloc`, `free`, and `__rust_alloc*` resolve inside a `dlmopen`
  namespace instead of the manager's base namespace.
- `std/scripts/`: local-Rust-source tooling that builds and instruments a
  private std sysroot, then compiles std-using workloads against it.
- `std/examples/std-vec-*`: first std-heavy workload and host pair. The workload
  uses `Vec`, `Box<[u64]>`, iterators, and sorting; the host loads it through a
  namespace-backed std environment.
- `scripts/`: rustc/opt/clang pipeline for turning workload Rust into an
  instrumented shared object.
- `tests/`: pass fixtures and integration scripts.

## Runtime Model

The scheduler adds each thread's per-cycle budget to a signed remaining-budget
counter. Instrumented code calls `__tally_charge(cost)`, which subtracts from the
active thread. If the remaining budget is zero or negative, the runtime saves
the instrumented stack/register context and switches back to the scheduler. The
negative value is retained as debt and is subtracted from the next cycle's
effective budget.

The runtime does not reserve a machine register for budget accounting. That
keeps the Rust path independent from rustc backend changes, while making the
budget check easy to inspect and evolve.

Std-using workloads use a separate loader path. `TallyManager` creates a std
environment by loading `libtally_abi_bridge.so` with `dlmopen(LM_ID_NEWLM, ...)`,
installs a table of base-namespace runtime function pointers into that bridge,
and then loads all workloads for the same instrumented/pruned std profile into
that namespace. The manager's own Rust `std` remains the normal process copy in
the base namespace. Workload handles can be unloaded after all minithreads are
returned or errored; the first implementation refuses unload while any
minithread is still active.

## Memory Accounting Model

`spawn_with_limits` creates minithreads with separate stack and heap limits.
Stacks are anonymous `mmap` regions with protected guard pages. Cooperative
checks in `__tally_charge` read the current stack pointer, update current and
peak stack usage, and convert soft overflow into `ThreadState::Errored`.
Guard-page `SIGSEGV` faults from the currently running minithread are handled on
an alternate signal stack, recognized as stack faults, and switched back to the
scheduler instead of terminating the host process.

Each minithread can have either a fixed heap budget or an unlimited heap.
`MemoryLimits::new(stack_bytes, heap_bytes)` creates a fixed per-minithread bump
heap, `MemoryLimits::stack_only(stack_bytes)` makes heap allocation fail, and
`MemoryLimits::unlimited_heap(stack_bytes)` delegates allocation to the app's
allocator while still tracking live bytes, peak live bytes, and allocation
counts. Fixed heap exhaustion records a `HeapLimit` error and yields to the
scheduler.

Controlled no-std workloads can call `__tally_alloc`, `__tally_dealloc`, and
`__tally_realloc` directly. Std-using workloads go through the namespace bridge:
`libtally_abi_bridge.so` interposes libc-style allocator symbols plus the Rust
allocator ABI, stores a small header beside each allocation, and forwards the
request to the current minithread's heap hooks in the manager. As a result,
`Vec`, `Box`, `String`, and similar std allocation are now charged against the
current minithread heap when the workload is loaded through a Tally std
environment.

## Performance Workload

The repository-level performance matrix uses `examples/self-walk`, not the
graph ABI demo. The workload uses a linear-congruential random number
generator, four synthetic neighbor offsets, and per-iteration state updates.
Each benchmark run gives `k` minithreads a common per-cycle budget and stops
when the workload-owned counters reach the requested total edge count. That
keeps graph work inside instrumented code.

## Limitations

- Linux/x86-64 only.
- External pipeline only: `rustc --emit=llvm-bc`, then `opt`, then `clang`.
- The original v1 workload is a controlled `#![no_std]` crate with
  `panic=abort`.
- Std instrumentation is currently a local private-sysroot artifact pipeline,
  not a Cargo-native replacement toolchain.
- The per-minithread heap is a bump allocator; freed blocks are accounted for
  but not reused yet.
- The std allocator bridge interposes normal allocation paths, but it does not
  yet intercept direct large-allocation system calls such as `mmap`.
- The pass uses a simple LLVM IR instruction cost model rather than a calibrated
  machine-instruction model.
