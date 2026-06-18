# Source Overview

## Summary

Tally is an experimental C/C++ runtime for running C functions in small,
preemptible "minithreads" governed by an approximate CPU-cycle budget. It is
Linux/x86-64 specific and depends on GCC plugin internals, inline assembly,
manual stack switching, dynamic loading, and a reserved `r15` register.

The repository is now organized around a clear split:

- `src/tally/` contains the Tally implementation itself.
- `include/` contains the headers used by the runtime, plugin, modules, and
  instrumented code.
- `examples/` contains programs and instrumented workloads that use Tally.
- `experiments/` contains measurement code, including the budget linearity
  random-walk experiment.
- `tests/` contains CTest-backed checks and additional compiler/runtime probes.
- `data/` contains runtime input data such as the graph fixture.
- `scripts/` contains helper scripts for dynamic instrumentation and plotting.
- `../thesis/` contains Andre Manada's thesis PDF, source, and figures as project
  reference material.

The core idea is:

1. Compile target C code with the Tally GCC plugin.
2. Instrument each GIMPLE basic block so it subtracts an estimated cost from the
   reserved `r15` register.
3. When the counter reaches zero or below, jump back into the minithread
   runtime.
4. Save the instrumented function's stack/register state and resume it later
   from the inserted continuation label.

The main executable demonstrates the mechanism with a graph random-walk
workload. The `budget_walk` experiment runs ten minithreads over the same graph
with linearly increasing per-cycle budgets, reports completed graph hops, and can
be plotted by `scripts/plot_budget_walk_scaling.py`.

## Detailed Description

### Thesis Context

Andre Manada's thesis describes Tally as a library for scheduling many tiny C
"microprocesses" with software-enforced CPU-instruction budgets rather than
timer-based preemption. The intended use case is a language/runtime-level
setting where many small computations share an address space and only need small
private execution contexts.

The thesis design maps directly onto the implementation in this repository:

- a GCC plugin instruments target C code at compile time, and
- a minithread manager compiles or loads instrumented code, switches stacks,
  preserves registers, and resumes execution later.

The thesis source zip is unpacked under `../thesis/` together with the PDF and
graphics. It is reference material; the runnable benchmark and test harnesses
live in this repository's normal `examples/`, `experiments/`, and `tests/`
folders.

### Repository Layout

#### Tally Library Code

`src/tally/runtime/`

- `minithread.c`: core runtime. It loads instrumented shared objects, allocates
  minithread stacks, initializes modules, switches contexts, and records
  remaining budget between cycles.
- `cycles_probe.c`: low-level cycle counter helper used by the runtime.
- `minithread_alternative.c`: older alternate runtime implementation kept for
  reference. It is not part of the current build and references obsolete state
  names.

`src/tally/plugin/`

- `gcc-tally.cpp`: GCC GIMPLE plugin that inserts the budget-accounting and
  switch-out assembly into instrumented functions.

`src/tally/modules/`

- `clean/`: normal host-side module setup code. Clean modules can allocate,
  load files, and initialize per-minithread module state.
- `instrumented/`: module APIs callable from plugin-instrumented code. These
  files are compiled with `-ffixed-r15` and the GCC plugin.

`include/`

- Runtime headers: `minithread.h`, `minithread_api.h`,
  `minithread_struct.h`, `cycles_probe.h`, `flags.h`, `modules.h`.
- Graph module headers: `graph.h`, `graph_api.h`, `graph_func.h`.
- Allocator module headers: `shmall.h`, `shmall_api.h`, `llist.h`.
- Experiment header: `random_walk_budget.h`.

#### Code That Uses Tally

`examples/demo/`

- `main.c`: host-side demo program that loads `data/graph.txt`, creates a
  graph-backed minithread, dynamically compiles `examples/instrumented/random_walk.c`,
  and runs it through repeated scheduler cycles.

`examples/instrumented/`

- `random_walk.c`: instrumented infinite random-walk workload used by the demo
  and thesis-style graph benchmarks.
- `bfs.c`: breadth-first graph traversal example.
- `dfs.c` and `graph_dfs.c`: depth-first graph traversal examples.
- `func.c`: small instrumented playground workload for calls, printing, and
  allocator experiments.

`experiments/budget_walk/`

- `budget_walk.c`: host program for the budget linearity experiment. It runs ten
  minithreads on the same graph, assigns linearly increasing budgets, and prints
  work-per-budget metrics.
- `instrumented/random_walk_budget.c`: instrumented random-walk body used by the
  experiment. It increments the graph module hop counter on each transition.
- `scripts/plot_budget_walk_scaling.py`: runs `budget_walk`, writes CSV output,
  and produces the PNG plot.

`tests/`

- `test_hash.c`: CTest unit check for runtime/static module hashes.
- `test_graph_load.c`: CTest unit check for loading `data/graph.txt`.
- `instrumented/*.c`: additional instrumented compiler/runtime probes retained
  from the original `code/` directory. They are useful ad hoc coverage for
  switches, enums, hashing, timing loops, and large control-flow examples.

`data/`

- `graph.txt`: 1000-node graph fixture used by the demo, graph load test, and
  budget-walk experiment.

### Build Model

`CMakeLists.txt` builds these main targets:

- `minithread`: static runtime library from `src/tally/runtime/minithread.c` and
  `src/tally/runtime/cycles_probe.c`.
- `gcc-tally`: shared GCC plugin from `src/tally/plugin/gcc-tally.cpp`.
- `main`: demo executable from `examples/demo/main.c`, clean modules, and
  instrumented modules.
- `budget_walk`: experiment executable from `experiments/budget_walk/budget_walk.c`,
  clean modules, and instrumented modules.
- `test_hash` and `test_graph_load`: CTest unit executables.

The build writes its runtime, library, and plugin outputs to `bin/`. It compiles
instrumented module sources under `src/tally/modules/instrumented/**/*.c` with:

```text
-ffixed-r15 -fplugin=bin/gcc-tally.so -nostdlib
```

`-ffixed-r15` reserves `r15` so GCC does not allocate it for ordinary program
work. The minithread runtime then treats `r15` as the current cycle counter via
the global register variable `mt_arg`.

Dynamic minithread bodies are built by `scripts/build-instrumented.sh`. The
runtime passes it a repository-relative source stem such as
`examples/instrumented/random_walk`, and the script produces the matching shared
object under `dl/examples/instrumented/random_walk.so`.

### GCC Plugin

`src/tally/plugin/gcc-tally.cpp` defines a GCC GIMPLE pass named
`GCC Tally Plugin`, registered after the `optimized` pass.

For each function with a control-flow graph, the pass walks every basic block,
estimates a tally cost for each statement, and inserts inline assembly into the
block. The cost model is intentionally simple and statement-type based:

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

`include/flags.h` keeps the branch-prediction layout enabled and makes debug
output opt-in through compiler definitions. The thesis explains the motivation:
keep the common "continue minithread code" path as the fallthrough and move the
uncommon switch-out code to a later block. Andre's benchmarks did not show a
significant gain or loss, so this is best treated as an experimental layout
choice rather than a proven optimization.

### Minithread Runtime

The runtime is centered on `struct stMinithread` in
`include/minithread_struct.h`. Each minithread tracks:

- its private stack allocation and saved stack pointer,
- module memory carved out near the bottom of the stack region,
- the dynamically loaded function body,
- an initial argument pointer,
- lifecycle state,
- the intended cycle budget and any remaining budget.

`minithread_init` performs setup:

1. Allocate or reuse a `struct stMinithread`.
2. Allocate and zero the private stack.
3. Reserve module memory and initialize module wrappers.
4. Sort modules by hash for lookup.
5. Load the target instrumented function with `_load_func`.

`minithread_run_cycle` is the scheduler entry point. It stores the active
minithread in thread-local state, loads `r15` with the next cycle budget,
switches to the minithread stack, calls or resumes the instrumented function,
receives control at `_minithread_break`, saves the minithread state, restores
the host stack/register context, and updates the remaining cycle count.

The current runtime includes the "carry over uncharged work" adjustment: when a
cycle overshoots or undershoots the nominal budget because checks only occur at
basic-block boundaries, the saved remaining counter is used to adjust the next
cycle's starting budget. This is what the budget-walk experiment uses to make
longer runs converge toward the requested budget rather than repeatedly granting
the same boundary overshoot.

### Module System

Modules provide services to instrumented minithread code. Each module has a
runtime descriptor:

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
module's per-thread struct, writes a wrapper at the beginning of that struct,
hashes the module name, assigns callbacks, and places the module inside the
minithread's module-memory region.

Clean module files are normal C. Instrumented module files provide APIs callable
from instrumented code. Those APIs use `minithread_find(HASH_S(...))` to locate
the current minithread's module struct through `threadInUse`.

There are currently two concrete module families:

- Graph: loads and queries `data/graph.txt`, and tracks graph hops for
  instrumented workloads that call `add_hop`.
- Shmall: small per-minithread allocator with segregated bins and coalescing.
  Expansion/contraction hooks are present but not implemented, so it is bounded
  by the initial module-memory region.

### Tests And Experiments

CTest currently covers:

- `hash_macros`: verifies runtime and static hash macros agree for module names.
- `graph_load`: loads `data/graph.txt` and checks representative adjacency data.
- `demo_runs`: runs the main minithread demo, including dynamic compilation of
  `examples/instrumented/random_walk.c`.
- `budget_walk_runs`: runs a short budget-walk smoke test.

The budget-walk experiment can also be run through the plotting script:

```text
python3 scripts/plot_budget_walk_scaling.py
```

It writes CSV data and a plot under `results/` by default.

### Generated Artifacts

These directories are build outputs rather than source design inputs:

- `build/`: CMake cache and generated build files.
- `bin/`: built executables, libraries, GCC plugin, and saved compiler
  intermediates.
- `dl/`: dynamically compiled instrumented shared objects.
- `results/`: generated CSV and PNG outputs from experiments.

The repository historically tracked some generated artifacts. The source of
truth is now the organized source tree above.

### Important Constraints And Caveats

- The project is AMD64-only in `include/minithread.h`.
- The runtime and plugin assume `r15` is reserved for the cycle counter.
- Instrumented code depends on `_minithread_break` being exported by the host
  executable/runtime.
- The plugin depends on GCC internal APIs, so portability across GCC versions may
  require small fixes.
- `src/tally/runtime/minithread_alternative.c` is retained only as old reference
  code and is not built.
- `MINITHREAD_YEILD` is consistently misspelled in the public macro name.
- `shmall` expansion/contraction hooks are stubs.
- The formal test harness is intentionally small; the instrumented files under
  `tests/instrumented/` are useful probes but are not all wired into CTest.

## Mental Model For Future Changes

Keep three execution domains separate:

1. Host/runtime domain: normal C code in `src/tally/runtime/`,
   `src/tally/modules/clean/`, and host programs under `examples/` or
   `experiments/`.
2. Instrumented domain: C code compiled with the GCC plugin and `-ffixed-r15`,
   including instrumented modules and dynamic minithread bodies.
3. Generated artifact domain: `bin/`, `dl/`, `build/`, and `results/`, which are
   outputs of the build and experiment flow.

Most architectural bugs in this codebase are likely to live at the boundaries:
stack layout, register preservation, continuation-label stack discipline,
module memory ownership, and symbol visibility between the host executable,
runtime library, plugin-generated code, and dynamically loaded `.so` files.
