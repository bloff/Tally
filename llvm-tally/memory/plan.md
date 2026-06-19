# LLVM Tally Minithread Memory Accounting Plan

## Goal

Extend `llvm-tally` so each minithread has an explicit memory budget and memory
accounting. The first target is stack usage, because the runtime already owns
each minithread stack. The second target is heap usage, first through runtime
allocation hooks and then, after Rust standard-library instrumentation is in
place, through allocator integration for `Vec`, `Box`, `String`, and other
standard-library allocation paths.

This work is LLVM/Rust-only. The current commit has been tagged
`gcc-tally-final`; GCC Tally should remain frozen except for documentation or
build-break fixes if absolutely necessary.

## What We Know

- Current `TallyThread` stores its stack as `Vec<u8>`.
- `thread_trampoline` runs instrumented code on that private stack after the
  assembly context switch restores the minithread stack pointer.
- `__tally_charge(cost)` is called by instrumented code at basic-block
  boundaries. It already has access to `CURRENT_THREAD`, so it is a natural
  cooperative checkpoint for budget and stack-limit checks.
- Instrumented Rust workloads are compiled with `-C no-redzone=yes`.
- Local `rustc -C help` reports that `-C no-stack-check` is deprecated and does
  nothing, so stack enforcement should not depend on rustc's old stack checking.
- Linux/x86-64 stacks grow downward.
- `mmap` can create anonymous virtual memory mappings; `mprotect(PROT_NONE)` can
  turn pages into inaccessible guard pages that raise `SIGSEGV` when touched.
- A process-wide `RLIMIT_AS` or `RLIMIT_DATA` is too coarse for minithreads.

## Design Principles

- Keep host runtime memory separate from minithread memory.
- Treat stack and heap budgets separately at first, then optionally expose a
  combined memory budget later.
- Prefer deterministic runtime errors over process aborts where possible.
- Use OS guard pages as hard safety boundaries; use cooperative checks for clean
  accounting and graceful yield/error behavior.
- Make the allocator path no-std-friendly and reentrancy-safe. Allocator hooks
  must not allocate using the same allocator.
- Accept that exact memory accounting is harder than CPU-budget accounting:
  compiler optimizations can remove or move allocations, and stack frames can
  grow between instrumentation points.

## Stack Accounting And Limits

### Runtime stack representation

Replace `TallyThread.stack: Vec<u8>` with an owned mapped stack object:

```rust
struct TallyStack {
    mapping_base: NonNull<u8>,
    mapping_len: usize,
    usable_base: NonNull<u8>,
    usable_len: usize,
    guard_low_len: usize,
    guard_high_len: usize,
}
```

For downward-growing stacks:

```text
low addresses
  [PROT_NONE guard page]
  [usable stack bytes]
  [optional high guard/slop page]
high addresses
```

Initial `rsp` should be aligned near the high end of the usable stack, as it is
today. The low guard page catches overflow beyond the configured stack budget.

Implementation details:

- Use `mmap` with `MAP_PRIVATE | MAP_ANONYMOUS | MAP_STACK` when available.
- Page-align requested sizes.
- Use at least one low `PROT_NONE` guard page via `mprotect`.
- Keep `-C no-redzone=yes` for instrumented workloads.
- Drop should `munmap` the full mapping.

### Cooperative stack checks

At each `__tally_charge`, read the current stack pointer and compare it to the
thread's usable stack bounds:

```text
used_stack = usable_high - current_rsp
if current_rsp < usable_low + soft_guard_slop:
    mark thread errored and yield to scheduler
```

This gives us:

- current stack usage;
- peak stack usage;
- a clean error before a guard page fault when possible.

Suggested fields:

```rust
stack_limit_bytes: usize,
stack_used_bytes: usize,
stack_peak_bytes: usize,
stack_overflowed: bool,
```

The exact current stack pointer can be read with small inline assembly on
x86-64, or with a local variable address as a conservative approximation. Inline
assembly is clearer and matches the existing assembly-heavy runtime.

### Hard overflow behavior

Guard pages remain necessary because cooperative checks only happen at
instrumented boundaries. A function with a huge stack frame may touch below the
limit before the next `__tally_charge`.

Phase 1 can let a guard-page hit terminate the process during tests, but the
runtime should move toward graceful handling:

- install a `SIGSEGV` handler on an alternate signal stack;
- detect whether the fault address belongs to the current minithread's guard
  page;
- mark that thread as `Errored`;
- switch back to the scheduler.

Signal recovery across custom context switching is delicate, so this should be
Phase 2, after mapped stacks and cooperative checks are stable.

## Heap Accounting And Limits

### Short-term heap model: runtime-owned heap arenas

Before instrumenting `std`, add an explicit per-minithread heap object managed
by the Tally runtime:

```rust
struct TallyHeap {
    limit_bytes: usize,
    committed_bytes: usize,
    live_bytes: usize,
    peak_live_bytes: usize,
    allocation_count: u64,
    deallocation_count: u64,
}
```

Start with a simple bump allocator arena per minithread:

- `mmap` a fixed-size region per minithread;
- optionally add guard pages around the arena;
- allocate by bumping a pointer with alignment;
- do not reuse freed memory in the first implementation;
- fail allocation when the arena cannot satisfy the request.

This gives clear isolation and a simple upper bound. It is not a general
allocator yet, but it is enough to test memory budgets and std-heavy workloads
that do not need long-running reuse.

### Runtime allocation ABI

Expose no-std-friendly C ABI functions:

```c
void * __tally_alloc(uint64_t size, uint64_t align);
void   __tally_dealloc(void *ptr, uint64_t size, uint64_t align);
void * __tally_realloc(void *ptr, uint64_t old_size, uint64_t align, uint64_t new_size);
```

Behavior:

- If `CURRENT_THREAD` is null, use the host/system allocator or return null,
  depending on which call site is expected.
- If a minithread is active, allocate from that thread's heap.
- On limit exceed, mark the thread as memory-exhausted and return null.
- Do not unwind from allocator hooks.
- Track requested bytes and actual aligned bytes separately.

### Later heap model: real per-minithread allocator

Once the bump allocator proves the integration:

- add free-list reuse;
- keep allocation headers with requested size and actual size;
- support `realloc`;
- optionally quarantine/detect double free in debug builds;
- optionally use `mmap` chunks for large allocations.

## Standard Library Allocation Integration

This should come after Rust standard-library instrumentation is in place.

There are two plausible routes:

### Route A: workload-level global allocator

For instrumented workloads, define:

```rust
#[global_allocator]
static TALLY_ALLOCATOR: TallyGlobalAllocator = TallyGlobalAllocator;
```

`TallyGlobalAllocator` implements `std::alloc::GlobalAlloc` and forwards to
`__tally_alloc`, `__tally_dealloc`, and `__tally_realloc`.

Advantages:

- Small local code.
- No Rust source edits.
- Works with `Vec`, `Box`, `String`, and most global allocation paths.

Constraints:

- `GlobalAlloc` is process-global for the linked artifact. We must ensure the
  host runtime is not linked against this allocator.
- Allocator functions must not unwind and must avoid accidentally allocating.
- Allocation counting is not semantically guaranteed for optimized-away
  allocations; Rust's allocator docs explicitly warn that programs must not rely
  on allocations actually occurring.

### Route B: instrumented std allocator shim

When building a private instrumented std sysroot, patch or configure the target
std/alloc path so the default target allocator calls Tally hooks.

Advantages:

- Workloads do not need to define their own global allocator.
- Better for transparent std instrumentation.

Constraints:

- More coupled to Rust internals.
- Requires patch queue or wrapper logic.
- More work to keep across Rust updates.

Preferred order:

1. Route A first.
2. Route B only if transparency is worth the maintenance cost.

## Memory Budget Semantics

Add memory budget configuration at spawn time:

```rust
pub struct MemoryLimits {
    pub stack_bytes: usize,
    pub heap_bytes: usize,
    pub combined_bytes: Option<usize>,
}
```

Initial policy:

- stack and heap are separate hard limits;
- heap allocation returns null on exhaustion;
- stack soft overflow marks thread errored at `__tally_charge`;
- guard-page overflow is a process-level failure until signal recovery exists.

Later policy:

- optional combined memory budget;
- optional dynamic virtual-memory budget, analogous to virtual CPU budget;
- optional memory pressure scheduler policy.

## Runtime API Changes

Add:

```rust
impl TallyManager {
    pub fn spawn_with_limits(
        &mut self,
        entry: TallyEntry,
        arg: *mut c_void,
        budget_per_cycle: i64,
        limits: MemoryLimits,
    ) -> Result<usize, TallyError>;

    pub fn memory_stats_for_thread(&self, id: usize) -> Result<MemoryStats, TallyError>;
}
```

Keep existing `spawn_with_stack` as a compatibility wrapper.

Suggested stats:

```rust
pub struct MemoryStats {
    pub stack_limit_bytes: usize,
    pub stack_used_bytes: usize,
    pub stack_peak_bytes: usize,
    pub heap_limit_bytes: usize,
    pub heap_live_bytes: usize,
    pub heap_peak_live_bytes: usize,
    pub heap_committed_bytes: usize,
    pub allocations: u64,
    pub deallocations: u64,
    pub allocation_failures: u64,
}
```

## Instrumentation Changes

The current LLVM pass does not need to change for the first stack check, because
`__tally_charge` is already called frequently. Later improvements:

- pass option to charge memory checks separately;
- optional stack-check call at function entry for more predictable stack
  accounting;
- optional call-site instrumentation for known allocation functions if allocator
  hooks are insufficient;
- pass summary that reports whether memory instrumentation helpers are present.

## Test Plan

### Stack tests

- Spawn a minithread with a small stack and verify it runs a shallow function.
- Spawn with too-small stack and verify spawn fails cleanly.
- Use recursion or large local arrays to approach the soft stack limit and
  verify `stack_peak_bytes` grows.
- Verify cooperative overflow marks the thread `Errored`.
- Later: verify guard-page overflow is caught on an alternate signal stack.

### Heap tests

- Allocate within the per-thread heap and verify live/peak counters.
- Allocate beyond limit and verify failure is reported without host corruption.
- Verify two minithreads cannot allocate from each other's heaps.
- Verify deallocation reduces live bytes once free-list support exists.
- Verify host allocations outside a minithread do not charge a minithread.

### Standard library tests

After std allocation integration:

- `Vec::push` consumes heap budget.
- `Box::new` consumes heap budget.
- `String` growth consumes heap budget.
- A workload that exceeds heap budget returns a controlled error or aborts only
  the minithread, not the host process.

### Fairness and performance tests

- Rerun CPU fairness with stack checks enabled to measure overhead.
- Add memory-heavy fairness runs with equal CPU budget but varied heap budgets.
- Add a stress test with many minithreads and small heaps to inspect metadata
  overhead.

## Implementation Phases

### Phase 1: Mapped stacks and stats

- Add `TallyStack`.
- Use `mmap`/`mprotect`/`munmap`.
- Add stack bounds and peak stack tracking at `__tally_charge`.
- Add memory stats API.
- Keep existing tests passing.

### Phase 2: Soft stack limits

- Add configurable soft stack limit/slop.
- Mark thread errored when a charge observes stack use beyond limit.
- Add recursion/large-frame tests.

### Phase 3: Per-minithread bump heap

- Add `TallyHeap`.
- Add `__tally_alloc`/`__tally_dealloc`/`__tally_realloc`.
- Add heap stats.
- Add no-std workload that calls the allocation ABI directly.

### Phase 4: Global allocator shim for workloads

- Add a small Rust allocator shim crate or module for instrumented workloads.
- Use `#[global_allocator]` to route `Vec`/`Box`/`String` to Tally heap.
- Keep host runtime on normal system allocator.

### Phase 5: Instrumented std integration

- Once the private instrumented std sysroot exists, test standard library code
  with the Tally allocator shim.
- Decide whether a private std allocator patch is needed.

### Phase 6: Guard-page recovery

- Install alternate signal stack.
- Add `SIGSEGV` handler that recognizes current-thread stack guard faults.
- Convert recoverable guard faults into `ThreadState::Errored`.

## Open Questions

- Should memory exhaustion yield to the scheduler, mark the thread errored, or
  return allocation failure to user code where possible?
- Should stack and heap share one virtual memory budget or remain separate?
- How much allocator reuse is needed before std-heavy workloads become useful?
- Can guard-page signal recovery safely swap back to the scheduler in all cases,
  or should we initially treat guard faults as fatal debug diagnostics?
- How should memory budgets interact with future multi-core scheduling?

## References

- Rust `GlobalAlloc` API:
  https://doc.rust-lang.org/std/alloc/trait.GlobalAlloc.html
- Rust `Allocator` API, currently nightly-only:
  https://doc.rust-lang.org/std/alloc/trait.Allocator.html
- Rust allocator API tracking feature:
  https://doc.rust-lang.org/unstable-book/library-features/allocator-api.html
- Rust `handle_alloc_error` behavior:
  https://doc.rust-lang.org/std/alloc/fn.handle_alloc_error.html
- Linux `mmap(2)`:
  https://man7.org/linux/man-pages/man2/mmap.2.html
- Linux `mprotect(2)`:
  https://man7.org/linux/man-pages/man2/mprotect.2.html
- Linux `sigaltstack(2)`:
  https://man7.org/linux/man-pages/man2/sigaltstack.2.html
- Linux `getrlimit(2)`:
  https://man7.org/linux/man-pages/man2/getrlimit.2.html
