# Tally

This repository contains two implementations of the Tally idea: running small
computations with software-enforced work budgets and cooperative preemption.

- `gcc-tally/`: the original C/GCC-plugin implementation.
- `llvm-tally/`: a Rust/LLVM prototype that instruments Rust-emitted LLVM
  bitcode with an LLVM FunctionPass and yields through a Rust minithread
  runtime.
- `thesis/`: Andre Manada's original thesis PDF, source, and figures.

## Build Everything

```sh
cmake -S . -B /tmp/tally-all-build
cmake --build /tmp/tally-all-build
ctest --test-dir /tmp/tally-all-build --output-on-failure
```

## Build The GCC Implementation

```sh
cmake -S gcc-tally -B /tmp/tally-gcc-build
cmake --build /tmp/tally-gcc-build
ctest --test-dir /tmp/tally-gcc-build --output-on-failure
```

## Build The LLVM/Rust Prototype

```sh
cmake -S llvm-tally -B /tmp/tally-llvm-build
cmake --build /tmp/tally-llvm-build
ctest --test-dir /tmp/tally-llvm-build --output-on-failure
```

The Rust workload pipeline can also be run manually:

```sh
llvm-tally/scripts/build-rust-workload.sh examples/random-walk /tmp/tally-llvm-build /tmp/tally-llvm-build/llvm-tally-pass.so
/tmp/tally-llvm-build/bin/llvm-tally-random-walk

llvm-tally/scripts/build-rust-workload.sh examples/self-walk /tmp/tally-llvm-build /tmp/tally-llvm-build/llvm-tally-pass.so
/tmp/tally-llvm-build/bin/llvm-tally-self-walk llvm-tally/dl/examples/self-walk/self_walk.so 10 100 50000000
```

## Compare GCC/C And LLVM/Rust Self-Contained Runtime Throughput

After building the root project, run:

```sh
python3 tests/compare-performance.py \
  --repo-root . \
  --build-root /tmp/tally-all-build \
  --gcc-binary gcc-tally/bin/self_walk \
  --llvm-host /tmp/tally-all-build/llvm-tally/bin/llvm-tally-self-walk \
  --llvm-pass /tmp/tally-all-build/llvm-tally/llvm-tally-pass.so
```

This compares runtime throughput for C/GCC and Rust/LLVM workloads that run the
same synthetic degree-4 random walk. Graph generation and vertex counting happen
inside the instrumented code in both languages; there are no host graph calls in
the measured loop. The runtimes still use their native accounting strategies:
GCC keeps its reserved-register budget and LLVM/Rust keeps its memory-backed
budget.

By default the comparison runs every combination of:

- `k = 1, 2, ..., 10, 20, 30, 40, 50, 100, 200, 300, 400, 500, 1000`
- `b = 10, 20, 50, 100, 200, 500, 1000`
- `target_edges = 50000000` per `(k, b, implementation)` run

The script writes:

- `results/performance-matrix/performance-matrix-detail.csv`
- `results/performance-matrix/performance-matrix-comparison.csv`
- `results/performance-matrix/performance-matrix-report.html`

The HTML report contains heatmaps for LLVM/GCC throughput ratio and absolute
throughput. Use smaller `--thread-counts`, `--budgets`, or `--target-edges`
values for quick smoke runs.

## Million-Thread Stress Benchmark

For very high minithread counts, use the separate summary-only stress runner:

```sh
python3 tests/compare-million-threads.py \
  --repo-root . \
  --build-root /tmp/tally-all-build \
  --gcc-binary gcc-tally/bin/self_walk \
  --llvm-host /tmp/tally-all-build/llvm-tally/bin/llvm-tally-self-walk \
  --llvm-pass /tmp/tally-all-build/llvm-tally/llvm-tally-pass.so
```

Defaults are `k = 1000000`, `target_edges = 10000000000`, and
`b = 10, 20, 50, 100, 200, 500, 1000, 5000, 10000`. The script passes
summary-only flags to the hosts so they do not print one million per-thread
rows. It also divides a stack-memory budget across the minithreads; the default
is `--stack-budget-gib 16`, which gives roughly 16 KiB of stack per minithread
at `k = 1000000`.

Outputs are written under `results/million-thread-stress/`.

## Virtual-Budget Calibration

The internal budget units are deliberately implementation-local: GCC/C counts
lower-level inserted charges using a reserved register, while LLVM/Rust counts
LLVM basic-block costs through a memory-backed runtime value. To map both of
those onto a user-facing "fraction of one CPU core" budget, use the calibration
harness:

```sh
python3 tests/calibrate-virtual-budget.py \
  --repo-root . \
  --build-root /tmp/tally-all-build \
  --gcc-binary gcc-tally/bin/self_walk \
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
coded globally. The C side exposes this mapping in
`gcc-tally/include/virtual_budget.h` through `TallyVirtualCalibration` and
`TallyVirtualThread`. The Rust side exposes the same concepts as
`VirtualCalibration` and `VirtualThread` in the `llvm_tally_runtime` crate.

Both libraries also provide native startup calibration:

- GCC/C: `tally_virtual_calibrate`, `tally_virtual_calibration_write_file`, and
  `tally_virtual_calibration_read_file`.
- LLVM/Rust: `VirtualCalibration::calibrate`,
  `VirtualCalibration::write_to_file`, and
  `VirtualCalibration::read_from_file`.

The default native calibration configuration is intended for a real startup
calibration run and targets roughly 30 seconds of amortized measurements. Tests
and quick experiments can pass a shorter config. The persisted file format is
the same small text format for both implementations, so calibration objects can
be inspected and compared easily.

In both APIs, a scheduler periodically grants each minithread
`elapsed_wall_seconds * cpu_share` virtual CPU seconds. The helper then converts
that accumulated credit into an internal budget only when the thread can afford
the calibrated activation/context-switch cost plus at least one usable slice.
Negative credit is allowed, which lets overshoot at instrumentation boundaries
carry forward as debt.

For runtime drift, use `TallyVirtualAdaptiveState` on the C side or
`VirtualAdaptiveState` on the Rust side. These objects deliberately do not time
individual minithread slices. Instead, the scheduler measures a window of wall
time around many activations, aggregates `budget_units_consumed`, activation
count, and scheduler-round count, then updates `seconds_per_budget_unit` with a
smoothed estimate. This preserves proportionality as well as the current machine
load and cache behavior allow, while avoiding noisy sub-microsecond timing.

To test whether the native calibrations produce proportional work allocation,
run the fairness harness:

```sh
python3 tests/native-virtual-budget-fairness.py \
  --repo-root . \
  --gcc-binary gcc-tally/bin/virtual_budget_fairness \
  --llvm-binary /tmp/tally-all-build/llvm-tally/bin/llvm-tally-virtual-budget-fairness
```

By default this calibrates each runtime natively, then runs three randomized
10-20 second rounds with 50,000 minithreads whose virtual budgets sum to `0.5`.
For each thread it compares the observed work share `W_i / sum(W_i)` against the
activation-corrected budget share `B_i / sum(B_i)`, where
`B_i = max(0, v_i * round_seconds - activations_i * activation_cost)`. It writes:

- `results/native-virtual-budget-fairness/native-virtual-budget-fairness-detail.csv`
- `results/native-virtual-budget-fairness/native-virtual-budget-fairness-summary.csv`
- `results/native-virtual-budget-fairness/native-virtual-budget-fairness-buckets.csv`
- `results/native-virtual-budget-fairness/native-virtual-budget-fairness-report.html`
