# LLVM Tally

`llvm-tally` is a Rust/LLVM prototype of Tally. It compiles a controlled Rust
workload to LLVM bitcode, instruments that bitcode with an LLVM FunctionPass,
links the result as a shared object, and runs it inside a Rust minithread
runtime.

The runtime deliberately uses a memory-backed per-minithread budget. That keeps
the Rust experiment independent from rustc backend changes and easy to inspect.

## Build And Test

```sh
cmake -S llvm-tally -B /tmp/tally-llvm-build
cmake --build /tmp/tally-llvm-build
ctest --test-dir /tmp/tally-llvm-build --output-on-failure
```

The test suite includes `llvm_memory_recovery_integration`, which builds a
`#![no_std]` workload through the LLVM pass and verifies that per-minithread
heap exhaustion, deep recursion, and oversized stack frames mark only the
offending minithread as errored while the scheduler keeps running.

## Run The Random-Walk Demo

```sh
llvm-tally/scripts/build-rust-workload.sh examples/random-walk /tmp/tally-llvm-build /tmp/tally-llvm-build/llvm-tally-pass.so
/tmp/tally-llvm-build/bin/llvm-tally-random-walk
```

The demo prints CSV rows with one row per minithread.

## Run The Self-Contained Walk Benchmark

```sh
llvm-tally/scripts/build-rust-workload.sh examples/self-walk /tmp/tally-llvm-build /tmp/tally-llvm-build/llvm-tally-pass.so
/tmp/tally-llvm-build/bin/llvm-tally-self-walk llvm-tally/dl/examples/self-walk/self_walk.so 10 100 50000000
```

This variant keeps the graph algorithm inside the instrumented Rust workload so
performance measurements avoid host graph calls in the measured loop. Its
arguments after the shared object path are `thread_count`, `budget_per_cycle`,
and `target_edges`.

## Memory Limits

`TallyManager::spawn_with_limits` accepts `MemoryLimits` with separate stack and
heap controls. `MemoryLimits::new(stack_bytes, heap_bytes)` gives the
minithread a fixed per-minithread heap, `MemoryLimits::stack_only(stack_bytes)`
keeps heap allocation unavailable, and
`MemoryLimits::unlimited_heap(stack_bytes)` delegates heap allocation to the
host allocator while still tracking live and peak bytes. Stacks are mapped with
guard pages, `__tally_charge` records stack usage and catches cooperative soft
overflow, and guard-page faults from a running minithread are converted into
scheduler-visible `ThreadState::Errored` results.

The runtime also exports direct heap hooks:

```c
void * __tally_alloc(uint64_t size, uint64_t align);
void   __tally_dealloc(void *ptr, uint64_t size, uint64_t align);
void * __tally_realloc(void *ptr, uint64_t old_size, uint64_t align, uint64_t new_size);
```

For fixed heap budgets these hooks use a simple per-minithread bump heap. Heap
exhaustion records `ThreadError::HeapLimit` on only the offending minithread and
returns control to the scheduler. For unlimited heaps, the same hooks delegate
to the app's allocator.

Std-using workloads loaded through `std/abi/libtally_abi_bridge.so` also route
allocator symbols such as `malloc`, `calloc`, `realloc`, `free`, and
`__rust_alloc*` through these hooks. That means `Vec`, `Box`, `String`, and
ordinary Rust `std` allocation in an instrumented workload are charged against
the current minithread's fixed heap, or use the app heap when that minithread
was created with `MemoryLimits::unlimited_heap`.
