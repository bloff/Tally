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
/tmp/tally-llvm-build/bin/llvm-tally-self-walk llvm-tally/dl/examples/self-walk/self_walk.so
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
budget. The script reports timing and vertices/second, but does not assert a
winner because benchmark results are machine- and load-sensitive.
