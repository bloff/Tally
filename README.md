# Tally

Tally is an experimental AMD64/GCC runtime for running C functions as
preemptible minithreads with approximate CPU-cycle budgets.

## Layout

- `src/tally/`: Tally library implementation.
  - `runtime/`: minithread manager, context switching, dynamic loading.
  - `plugin/`: GCC plugin that instruments GIMPLE basic blocks.
  - `modules/`: clean host-side modules and instrumented module APIs.
- `include/`: runtime, plugin, module, and experiment headers.
- `examples/`: demo host program and instrumented example workloads.
- `experiments/`: benchmark/measurement programs such as `budget_walk`.
- `tests/`: CTest units plus extra instrumented compiler/runtime probes.
- `data/`: graph fixture used by examples and tests.
- `scripts/`: helper scripts for dynamic instrumentation and plotting.
- `thesis/`: Andre Manada thesis reference PDF, source, and images.

For a deeper guide, see `SOURCE-OVERVIEW.md`.

## Build And Test

```sh
cmake -S . -B build
cmake --build build
ctest --test-dir build --output-on-failure
```

The build writes executables, the GCC plugin, and the runtime static library to
`bin/`.

## Run The Demo

```sh
./bin/main
```

The demo dynamically instruments `examples/instrumented/random_walk.c` into
`dl/examples/instrumented/random_walk.so`, loads `data/graph.txt`, and runs the
workload through the minithread scheduler.

## Run The Budget-Walk Experiment

```sh
./bin/budget_walk
python3 scripts/plot_budget_walk_scaling.py
```

The plotting script runs the experiment, writes CSV data, and saves a PNG graph
under `results/`.
