# Tally

This branch contains the LLVM/Rust implementation of Tally: a prototype for
running small computations with software-enforced work budgets, cooperative
preemption, memory limits, virtual-budget calibration, and an instrumented Rust
standard-library path.

- `llvm-tally/`: LLVM pass plugin, Rust minithread runtime, examples, std
  instrumentation tooling, and integration tests.
- `tests/`: repository-level measurement and report harnesses for LLVM Tally.
- `thesis/`: Andre Manada's original thesis PDF, source, and figures.

The old GCC implementation has been removed from this branch. Historical GCC
work should be read from the frozen `gcc-tally-final` tag or older branches.

## Build And Test

```sh
cmake -S . -B /tmp/tally-all-build
cmake --build /tmp/tally-all-build
ctest --test-dir /tmp/tally-all-build --output-on-failure
```

The root build delegates to `llvm-tally/`. You can also build that subproject
directly:

```sh
cmake -S llvm-tally -B /tmp/tally-llvm-build
cmake --build /tmp/tally-llvm-build
ctest --test-dir /tmp/tally-llvm-build --output-on-failure
```

## Workload Pipeline

The no-std Rust workload pipeline can be run manually:

```sh
llvm-tally/scripts/build-rust-workload.sh examples/random-walk /tmp/tally-llvm-build /tmp/tally-llvm-build/llvm-tally-pass.so
/tmp/tally-llvm-build/bin/llvm-tally-random-walk

llvm-tally/scripts/build-rust-workload.sh examples/self-walk /tmp/tally-llvm-build /tmp/tally-llvm-build/llvm-tally-pass.so
/tmp/tally-llvm-build/bin/llvm-tally-self-walk llvm-tally/dl/examples/self-walk/self_walk.so 10 100 50000000
```

The random-walk demo uses `llvm-tally/data/graph.txt`. The self-walk benchmark
keeps synthetic graph generation inside instrumented Rust code, so there are no
host graph calls in the measured loop.

## Performance Matrix

After building the root project, run:

```sh
python3 tests/measure-performance.py \
  --repo-root . \
  --build-root /tmp/tally-all-build \
  --llvm-host /tmp/tally-all-build/llvm-tally/bin/llvm-tally-self-walk \
  --llvm-pass /tmp/tally-all-build/llvm-tally/llvm-tally-pass.so
```

By default this runs every combination of:

- `k = 1, 2, ..., 10, 20, 30, 40, 50, 100, 200, 300, 400, 500, 1000`
- `b = 10, 20, 50, 100, 200, 500, 1000`
- `target_edges = 50000000` per `(k, b)` run

The script writes:

- `results/performance-matrix/performance-matrix-detail.csv`
- `results/performance-matrix/performance-matrix-summary.csv`
- `results/performance-matrix/performance-matrix-report.html`

Use smaller `--thread-counts`, `--budgets`, or `--target-edges` values for quick
smoke runs.

## Million-Thread Stress Benchmark

For very high minithread counts, use the summary-only stress runner:

```sh
python3 tests/stress-million-threads.py \
  --repo-root . \
  --build-root /tmp/tally-all-build \
  --llvm-host /tmp/tally-all-build/llvm-tally/bin/llvm-tally-self-walk \
  --llvm-pass /tmp/tally-all-build/llvm-tally/llvm-tally-pass.so
```

Defaults are `k = 1000000`, `target_edges = 10000000000`, and
`b = 10, 20, 50, 100, 200, 500, 1000, 5000, 10000`. The script passes
summary-only flags to the host so it does not print one million per-thread
rows. It also divides a stack-memory budget across the minithreads; the default
is `--stack-budget-gib 16`, which gives roughly 16 KiB of stack per minithread
at `k = 1000000`.

Outputs are written under `results/million-thread-stress/`.

## Virtual-Budget Calibration

The internal budget units are deliberately implementation-local. To map them
onto a user-facing "fraction of one CPU core" budget, use the calibration
harness:

```sh
python3 tests/calibrate-virtual-budget.py \
  --repo-root . \
  --build-root /tmp/tally-all-build \
  --llvm-host /tmp/tally-all-build/llvm-tally/bin/llvm-tally-self-walk \
  --llvm-pass /tmp/tally-all-build/llvm-tally/llvm-tally-pass.so \
  --target-edge-counts 250000,1000000,5000000
```

The fitted model is:

```text
run_seconds ~= seconds_per_budget_unit * budget_units_consumed
            + seconds_per_activation * thread_cycles
            + seconds_per_scheduler_round * scheduler_cycles
            + intercept_seconds
```

The script writes:

- `results/virtual-budget-calibration/virtual-budget-calibration-detail.csv`
- `results/virtual-budget-calibration/virtual-budget-calibration.json`
- `results/virtual-budget-calibration/virtual-budget-calibration-report.html`

The JSON constants are meant to be copied into runtime configuration, not hard
coded globally. The Rust runtime exposes this mapping as `VirtualCalibration`
and `VirtualThread` in the `llvm_tally_runtime` crate.

For runtime drift, use `VirtualAdaptiveState`. It deliberately does not time
individual minithread slices. Instead, the scheduler measures a window of wall
time around many activations, aggregates `budget_units_consumed`, activation
count, and scheduler-round count, then updates `seconds_per_budget_unit` with a
smoothed estimate.

To test whether native calibration produces proportional work allocation, run:

```sh
python3 tests/native-virtual-budget-fairness.py \
  --repo-root . \
  --llvm-binary /tmp/tally-all-build/llvm-tally/bin/llvm-tally-virtual-budget-fairness
```

By default this calibrates the runtime natively, then runs three randomized
10-20 second rounds for each budget shape with 50,000 minithreads whose virtual
budgets sum to `0.5`. The default shapes are the original `random-log`
distribution and `tiered-50-35-15`, where 98% of threads are small and share
50% of the total virtual budget, 1.9% are medium and share 35%, and 0.1% are
large and share 15%.

The fairness script writes:

- `results/native-virtual-budget-fairness/native-virtual-budget-fairness-detail.csv`
- `results/native-virtual-budget-fairness/native-virtual-budget-fairness-summary.csv`
- `results/native-virtual-budget-fairness/native-virtual-budget-fairness-buckets.csv`
- `results/native-virtual-budget-fairness/native-virtual-budget-fairness-classes.csv`
- `results/native-virtual-budget-fairness/native-virtual-budget-fairness-report.html`

## Instrumented Rust Std

The `llvm-tally/std/` tooling builds the Rust standard-library sources that
match the local `rustc`, instruments selected std crates through the LLVM pass,
and assembles a private sysroot. Std-using workloads are loaded through a
`dlmopen` namespace and a tiny `libtally_abi_bridge.so`, so the manager keeps
using the normal host `std` while minithread workloads resolve their own
instrumented std profile.
