# Source Overview

## Summary

Tally is an experimental C/C++ runtime for running C functions in small,
preemptible "minithreads" governed by an approximate CPU-cycle budget. The core
idea is:

1. Compile target C code with a GCC plugin.
2. Instrument each GIMPLE basic block so it subtracts a cost from the reserved
   `r15` register.
3. When the counter reaches zero or below, jump back into the minithread runtime.
4. Save the instrumented function's stack/register state and resume it later
   from the inserted continuation label.

The project is Linux/x86-64 specific. It depends on GCC plugin internals,
inline assembly, manual stack switching, dynamic loading, and a reserved `r15`
register. The main executable currently demonstrates the mechanism with a graph
random-walk workload.

At a high level, the repository contains:

- `src/gcc-tally.cpp`: the GCC plugin that instruments target code.
- `src/minithread.c`: the runtime that allocates stacks, loads instrumented
  functions, switches contexts, and resumes execution.
- `include/`: runtime, module, graph, allocator, and helper headers.
- `modules/clean/`: normal, non-instrumented module setup and backing state.
- `modules/instrumented/`: module APIs callable from instrumented code.
- `code/`: sample functions and compiler/runtime probes that can be compiled
  into shared objects and run as minithread bodies.
- `src/main.c`: the current demo driver.
- `script.sh`: helper for compiling one `code/*.c` file into `dl/*.so` with the
  GCC plugin.
- `bin/`, `dl/`, and `build/`: generated artifacts from previous builds.
- `Tally_André_Manada_Thesis.pdf` and `Tally_André_Manada.zip`: André Manada's
  thesis and thesis source, which explain the intended design and benchmark
  context behind the current source tree.

## Detailed Description

### Thesis Context

The thesis describes Tally as a library for scheduling many tiny C
"microprocesses" with software-enforced CPU-instruction budgets rather than
timer-based preemption. The intended use case is not OS-grade isolation or
shared-memory threading, but a language/runtime-level setting where many small
computations can share an address space and only need small private execution
contexts.

The implementation is split into the two pieces visible in this repository:

- a GCC plugin that instruments target C code at compile time, and
- a minithread manager that compiles or loads instrumented code, switches stacks,
  preserves registers, and resumes execution later.

The thesis source zip contains the LaTeX source and rendered figures used to
produce the PDF. It includes the diagrams and performance plots, but not the raw
benchmark data files or data-collection scripts mentioned in the thesis text.

### Build Model

`CMakeLists.txt` builds three main targets:

- `minithread`: a static library from `src/minithread.c`.
- `gcc-tally`: a GCC plugin shared library from `src/gcc-tally.cpp`.
- `main`: the demo executable from `src/main.c`, `src/cycles_probe.c`, all
  clean modules, and all instrumented modules.

The build intentionally writes output into the repository's `bin/` directory by
setting `CMAKE_RUNTIME_OUTPUT_DIRECTORY`, `CMAKE_LIBRARY_OUTPUT_DIRECTORY`, and
`CMAKE_ARCHIVE_OUTPUT_DIRECTORY` to `bin`.

Instrumented source files under `modules/instrumented/**/*.c` are compiled with:

```text
-ffixed-r15 -fplugin=bin/gcc-tally.so -nostdlib
```

`-ffixed-r15` reserves `r15` so generated code does not allocate it for ordinary
work. The minithread runtime then treats `r15` as the current cycle counter via
the global register variable `mt_arg`.

`script.sh` performs the same kind of instrumentation for dynamic workloads in
`code/`. For example, `random_walk.c` is compiled into `dl/random_walk.so`, and
`src/minithread.c` loads that shared object at runtime with `dlopen`. The script
resolves paths relative to its own location, so this path works from ordinary
out-of-source builds as well as from the checked-in `build/` directory.

### GCC Plugin

`src/gcc-tally.cpp` defines a GCC GIMPLE pass named `GCC Tally Plugin`. It is
registered after the `optimized` pass:

```c++
pass_info.reference_pass_name = "optimized";
pass_info.pos_op = PASS_POS_INSERT_AFTER;
```

For each function with a control-flow graph, the pass walks every basic block,
estimates a tally cost for each statement, and inserts inline assembly into the
block.

The cost model is intentionally simple and statement-type based:

- `GIMPLE_ASSIGN`: number of operands minus one.
- `GIMPLE_CALL`: base cost plus argument count.
- `GIMPLE_COND`: one unit.
- `GIMPLE_SWITCH`: proportional to operand count.
- `GIMPLE_RETURN`: one unit.
- labels and prediction statements are effectively free.

The inserted assembly subtracts the block's tally from `r15`. If the counter is
still positive, execution continues. If it is exhausted, the instrumentation
pushes a generated continuation label and `_minithread_break` onto the stack,
then executes `ret`. That transfers control to the runtime while preserving the
address where the instrumented code should resume.

`include/flags.h` currently keeps the branch-prediction variant enabled and
makes debug output opt-in through compiler definitions:

```c
#ifdef GCCTALLY_DEBUG
#define gcctally_DEBUG
#endif

#ifdef GCCTALLY_DEBUG_RUNTIME
#define gcctally_DEBUG_RUNTIME
#endif

#define gcctally_branch_prediction
```

With branch prediction enabled, the plugin emits a separate switch block of
return paths and appends it near the end of the function. Without it, each
instrumented block contains its own direct break/continue sequence.

The thesis explains why this optimization exists: Intel's documented static
branch-prediction rule predicts forward conditional branches as not taken. The
optimized layout keeps the common "continue minithread code" path as the
fallthrough and moves the uncommon switch-out code to a later block. In André's
benchmarks this did not produce a significant performance gain or loss, so it is
best understood as an experimental optimization rather than a proven win.

### Minithread Runtime

The runtime is centered on `struct stMinithread` in
`include/minithread_struct.h`. Each minithread tracks:

- its private stack allocation and current saved stack pointer,
- module memory carved out near the bottom of the stack region,
- the dynamically loaded function body,
- an initial argument pointer,
- lifecycle state,
- the intended cycle budget and remaining cycles.

The lifecycle states are:

```c
MINITHREAD_NEW
MINITHREAD_INTERRUPTED
MINITHREAD_RUNNING
MINITHREAD_FORCE_YIELD
MINITHREAD_VOLUNTARY_YIELD
MINITHREAD_ERRORED
MINITHREAD_RETURNED
```

`minithread_init` performs setup:

1. Allocate or reuse a `struct stMinithread`.
2. Allocate and zero the private stack.
3. Reserve module memory and initialize module wrappers.
4. Sort modules by hash for lookup.
5. Load the target instrumented function with `_load_func`.

`_load_func` can call `script.sh` to compile `code/<file>.c` into
`dl/<file>.so`, then loads it with `dlopen` and resolves the entry function with
`dlsym`.

`minithread_run_cycle` is the scheduler entry point. It:

1. Stores the active minithread in thread-local `threadInUse`.
2. Loads `r15` with either the full cycle budget or the previously saved
   remainder, depending on state.
3. Saves host registers.
4. Switches `rsp` to the minithread stack.
5. Calls the minithread body for a new thread, or restores saved registers and
   executes `ret` to resume an interrupted one.
6. Receives control at `_minithread_break` when instrumentation or a runtime
   macro yields.
7. Saves the minithread registers and stack pointer.
8. Restores the host stack/register context.
9. Updates the minithread state and remaining cycle count.

The runtime-side assembly helpers live mostly in `include/minithread.h`:

- `PUSH_REGISTERS` saves general-purpose registers and XMM registers.
- `POP_REGISTERS` restores them.
- `_minithread_break` is exported as a global label so instrumented code can
  return into the scheduler.

`include/minithread_api.h` exposes macros and helpers for code that runs inside
the minithread:

- `MINITHREAD_YEILD` marks a voluntary yield.
- `MINITHREAD_ERROR` marks an error.
- `minithread_find` and `minithread_m_find` locate loaded modules by hash.
- `HASH` and `HASH_S` provide runtime and compile-time string hashing helpers.

### Module System

Modules are intended to provide services to instrumented minithread code. Each
module has a runtime descriptor:

```c
struct minithreadModuleOpt {
    char* module_name;
    void (*init)(void*, void*);
    void (*clean)();
    void* init_args;
    uint64_t unique_id;
    uint64_t struct_size;
};
```

During minithread initialization, `_minithread_load_module` allocates the
module's per-thread struct, writes a `gcctally_module_wrapper` at the beginning
of that struct, hashes the module name, assigns the init/clean callbacks, and
sets `module.start` to the current module-memory boundary inside the
minithread's stack region.

The pattern is:

- Clean module files are normal C and can perform initialization, allocation,
  I/O, and global setup.
- Instrumented module files provide APIs callable from instrumented code.
- Instrumented APIs use `minithread_find(HASH_S(...))` to retrieve their
  current minithread's module struct through `threadInUse`.

There are currently two concrete module families: graph and `shmall`.

### Graph Module

The graph module is split between:

- `modules/clean/graph/graph.c`
- `modules/instrumented/graph/graph.c`
- `include/graph.h`
- `include/graph_api.h`
- `include/graph_func.h`

The clean side owns global graph storage:

- `N_G`: number of nodes.
- `G`: adjacency arrays.
- `Glen`: degree/count for each node.

`load_graph` reads the repository's adjacency-list format:

```text
<number-of-nodes>
<degree-of-node-0> <neighbor> <neighbor> ...
<degree-of-node-1> <neighbor> <neighbor> ...
...
```

The root `graph.txt` is an example graph in this format. The first line is
`1000`, and each following line begins with a neighbor count.

The instrumented graph API exposes:

- `get_n_nodes`
- `get_n_neighbours`
- `get_x_neighbour`

The public header also declares `add_hop`, and `struct graph_struct` contains a
`hops` field. The instrumented graph implementation increments the current
minithread's graph module when `add_hop` is called.

`modules/clean/graph/gen_graph.py` is a NetworkX-based graph generator adapted
to print adjacency-list data compatible with `load_graph`.

### Shmall Module

`shmall` is a small heap allocator designed to live inside a minithread's module
memory. It is split between:

- `modules/clean/shmall/shmall.c`
- `modules/instrumented/shmall/shmall.c`
- `modules/instrumented/shmall/heap.c`
- `modules/instrumented/shmall/llist.c`
- `include/shmall.h`
- `include/shmall_api.h`
- `include/llist.h`

The clean initializer `init_shmall` receives a `struct shmall_args` containing
the requested heap size. It creates bin headers, marks the minithread module
memory as a free allocation region, writes a footer, and inserts the initial
free node into the appropriate size bin.

The instrumented API exposes:

- `shmall_alloc`
- `shmall_free`

Internally it uses:

- segregated bins,
- linked lists ordered by block size,
- headers and footers for coalescing,
- wilderness block checks for future expansion/contraction.

`expand` and `contract` are currently stubs, so the allocator is bounded by the
initial module-memory region supplied during initialization.

### Demo Driver

`src/main.c` currently demonstrates the graph random-walk workload:

1. Configure a `minithreadFuncOpt` for `random_walk` / `run_walk`.
2. Configure a graph module descriptor.
3. Load `graph.txt`.
4. Create one minithread with a private stack size of `1 << 10` pointer slots.
5. Divide `1e9` total cycles into 100 metacycles.
6. Repeatedly call `minithread_run_cycle`.
7. Print elapsed wall-clock time in milliseconds.

The current graph module descriptor sets `init = NULL`, so the global graph is
loaded, but the `init_graph` routine is not called for the per-minithread graph
struct in this demo path.

### Sample Workloads and Probes

The `code/` directory is not one coherent application. It is a mixed collection
of benchmark kernels, example minithread bodies, and compiler/runtime probes.
Files with `run_*` or `void f(void*)` style entry points are intended to be
compiled by `script.sh` or a similar command into `dl/*.so` and loaded as
minithread bodies. The `test_*` files are mostly small probes used to exercise
specific compiler constructs or helper APIs.

The thesis identifies `random_walk.c` as the main performance benchmark kernel.
It was chosen because it is a simple infinite workload over the graph module:
start at a graph node, choose a random neighbor, jump there, and repeat forever.
The benchmark used the repository's 1000-node `graph.txt` so the graph would fit
comfortably in cache and avoid turning the benchmark into a graph-memory stress
test.

The files are:

- `random_walk.c`: the thesis benchmark kernel; an infinite random walk over the
  graph module, entered through `run_walk`.
- `bfs.c`: breadth-first search using graph APIs and `add_hop`, entered through
  `run_bfs`.
- `dfs.c` and `graph_func1.c`: depth-first search variants using graph APIs,
  both entered through `run_dfs`.
- `func.c`: simple arithmetic/printing loop entered through `f`; allocator calls
  are present only as commented-out experimentation.
- `_test_dp_large.c`: dynamic-programming stress example from competitive
  programming style code. The thesis mentions this kind of more-linear code when
  discussing larger average basic-block costs.
- `test_switch.c`: switch instrumentation probe, useful because switches stress
  control-flow handling in the GCC pass.
- `test_hash.c`: standalone compile-time hash probe for `HASH_S`.
- `test_enmu.c`: standalone enum/switch behavior probe.
- `test_time.c`: standalone clock overhead probe.

Only the function selected by `src/main.c` or a caller-provided
`minithreadFuncOpt` is loaded as the minithread body.

### Performance Context From The Thesis

The thesis performance chapter uses `code/random_walk.c` as the standard
workload and compares several variables around it:

- **Code size:** small instrumented functions roughly doubled or tripled in
  binary size in André's measurements.
- **Multiple files:** Tally loads instrumented code through `dlopen`, so André
  tested many minithreads using the same shared object versus many copied shared
  objects with different filenames. With 10,000 minithreads, 100 cycles per
  minithread per metacycle, and 1,000 metacycles, using four orders of magnitude
  more files increased runtime by about 18%. Because that penalty was modest,
  later measurements used the same instrumented file for every minithread.
- **Multiple minithreads:** André varied the number of minithreads while keeping
  total context switches constant. The goal was to isolate cache effects from
  switch-count effects. Runtime stayed proportional to total work for one
  minithread; multiple minithreads had a larger relative penalty at lower work
  sizes. Shuffling minithread execution order did not materially change the
  result.
- **Branch-prediction layout:** the alternate switch-out layout in the plugin was
  benchmarked head-to-head with the direct layout. The thesis found no
  significant gain or loss.
- **Overwork and granularity:** because Tally only checks the counter at
  basic-block boundaries, very small cycle budgets overshoot. For the random
  walk workload, the thesis estimates an average basic-block cost of about 6
  cycles, so asking for 1 cycle per metacycle can perform roughly 6 times the
  requested work unless corrected.
- **Context switch cost:** after correcting for overwork, the thesis estimates
  the switch cost at roughly 355 cycles for 1 minithread, 375 cycles for 128
  minithreads, and 441 cycles for 8192 minithreads. It concludes that practical
  granularity is dominated more by context-switch cost than by basic-block
  resolution, with a rough usable granularity around 1500 cycles once switch cost
  and overwork are considered.

These benchmarks are useful historical context for the source tree, but they are
not automatically reproduced by the current CTest suite. The zip includes the
plots as thesis graphics, not a runnable benchmark harness.

### Generated Artifacts and Data

The repository currently includes build and binary artifacts:

- `build/`: CMake cache and generated build files.
- `bin/main`: built demo executable.
- `bin/gcc-tally.so`: built GCC plugin.
- `bin/minithread.a`: built minithread static library.
- `bin/*.s`, `bin/*.i`, `bin/*.o`: saved intermediate build outputs.
- `dl/*.so`: instrumented dynamic workloads produced from `code/*.c`.
- `Tally_André_Manada_Thesis.pdf`: thesis PDF.
- `Tally_André_Manada.zip`: thesis LaTeX source and rendered thesis graphics.

These files are useful for seeing the current build state, but the source of
truth is the C/C++/header/module code.

`graph.txt` is runtime input data for the graph examples.

### Tests

CTest is enabled in `CMakeLists.txt`. The current tests are:

- `hash_macros`: verifies the runtime and static hash macros agree for module
  names used by the project.
- `graph_load`: loads `graph.txt` and checks representative adjacency data.
- `demo_runs`: runs the main minithread demo, including dynamic compilation of
  `code/random_walk.c` through `script.sh`.

### Important Constraints and Caveats

- The project is explicitly AMD64-only in `include/minithread.h`.
- The runtime and plugin assume `r15` is reserved for the cycle counter.
- Instrumented code depends on `_minithread_break` being exported by the main
  executable/runtime.
- The plugin depends on GCC internal APIs and declares a GCC base version of
  `6`, so portability across GCC versions may require fixes.
- `minithread_alternetive.c` appears to be an older alternate implementation; it
  references state names that do not match the current enum in
  `minithread_struct.h`.
- The module wrapper appears in both `include/minithread_api.h` and
  `include/modules.h`, with a spelling difference (`wrapper` vs `wraper`).
- `MINITHREAD_YEILD` is consistently misspelled in the public macro name.
- Some module/workload combinations are still experimental and should be tested
  before being treated as supported examples.
- `shmall` expansion/contraction hooks are present but not implemented.
- The formal test harness is small; examples and probes under `code/` are still
  useful for ad hoc compiler/runtime checks but are not all wired into CTest.

## Mental Model for Future Changes

When changing this project, it helps to keep three execution domains separate:

1. **Host/runtime domain**: normal C code in `src/` and `modules/clean/` that
   can allocate, call libc, load files, and manage minithread structs.
2. **Instrumented domain**: C code compiled with the GCC plugin and `-ffixed-r15`
   that may be preempted at inserted continuation labels.
3. **Generated artifact domain**: `bin/`, `dl/`, and `build/`, which are outputs
   of the plugin/runtime build process rather than design sources.

Most architectural bugs in this codebase are likely to live at the boundaries:
stack layout, register preservation, continuation-label stack discipline,
module memory ownership, and symbol visibility between the main executable,
runtime library, plugin-generated code, and dynamically loaded `.so` files.
