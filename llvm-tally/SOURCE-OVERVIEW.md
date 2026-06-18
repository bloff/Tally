# LLVM Tally Source Overview

## Summary

LLVM Tally is a prototype sibling to `gcc-tally`. It proves that the Tally
model can be expressed for Rust by instrumenting LLVM IR and yielding through a
Rust runtime, without changing rustc itself.

## Components

- `pass/`: LLVM new-pass-manager plugin. The `tally-instrument` FunctionPass
  inserts `__tally_charge(i64 cost)` calls into eligible basic blocks.
- `runtime/`: Rust minithread manager, stackful context switching, dynamic
  loading, and the exported `__tally_charge` C ABI.
- `examples/random-walk/workload/`: controlled `#![no_std]` Rust workload that
  calls host graph functions and runs forever.
- `examples/random-walk/host/`: Rust host executable that loads the instrumented
  workload and runs ten minithreads with linearly increasing budgets.
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

Unlike `gcc-tally`, this prototype does not reserve `r15`. That keeps the Rust
path independent from rustc backend changes, but it means the budget check is
more expensive than the original register-based GCC instrumentation.

## Limitations

- Linux/x86-64 only.
- External pipeline only: `rustc --emit=llvm-bc`, then `opt`, then `clang`.
- The v1 workload is a controlled `#![no_std]` crate with `panic=abort`.
- Dependencies and the Rust standard library are not instrumented.
- The pass uses a simple LLVM IR instruction cost model rather than a calibrated
  machine-instruction model.
