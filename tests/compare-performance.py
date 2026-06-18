#!/usr/bin/env python3
"""Compare GCC Tally and LLVM/Rust Tally random-walk throughput."""

import argparse
import csv
import os
import statistics
import subprocess
import sys
import time
from pathlib import Path


def parse_args():
    parser = argparse.ArgumentParser(
        description="Time comparable GCC/C and LLVM/Rust Tally random-walk runs."
    )
    parser.add_argument("--repo-root", type=Path, required=True)
    parser.add_argument("--build-root", type=Path, required=True)
    parser.add_argument("--gcc-binary", type=Path, required=True)
    parser.add_argument("--llvm-host", type=Path, required=True)
    parser.add_argument("--llvm-pass", type=Path, required=True)
    parser.add_argument("--metacycles", type=int, default=3000)
    parser.add_argument("--base-budget", type=int, default=100)
    parser.add_argument("--budget-step", type=int, default=100)
    parser.add_argument("--repetitions", type=int, default=3)
    return parser.parse_args()


def run_timed(command, *, cwd, env=None):
    start = time.perf_counter()
    completed = subprocess.run(
        command,
        cwd=cwd,
        env=env,
        check=True,
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
    )
    elapsed = time.perf_counter() - start
    return elapsed, completed.stdout


def parse_rows(output):
    lines = output.splitlines()
    try:
        header_index = next(i for i, line in enumerate(lines) if line.startswith("thread,"))
    except StopIteration as exc:
        raise ValueError("output did not contain a CSV header") from exc

    rows = list(csv.DictReader(lines[header_index:]))
    if len(rows) != 10:
        raise ValueError(f"expected 10 thread rows, got {len(rows)}")

    previous_budget = 0
    previous_work = -1
    total_vertices = 0
    for row in rows:
        budget = int(row["budget_per_metacycle"])
        vertices = int(row["vertices_walked"])
        if budget <= previous_budget:
            raise ValueError("budgets are not strictly increasing")
        if vertices <= 0:
            raise ValueError("thread did no graph work")
        if previous_work >= 0 and vertices < previous_work:
            raise ValueError("work is not nondecreasing with budget")
        previous_budget = budget
        previous_work = vertices
        total_vertices += vertices

    return rows, total_vertices


def build_llvm_workload(repo_root, build_root, llvm_pass):
    script = repo_root / "llvm-tally" / "scripts" / "build-rust-workload.sh"
    completed = subprocess.run(
        [str(script), "examples/random-walk", str(build_root / "llvm-tally"), str(llvm_pass)],
        cwd=repo_root,
        check=True,
        text=True,
        stdout=subprocess.PIPE,
    )
    return Path(completed.stdout.strip().splitlines()[-1])


def warm_gcc_workload(args):
    subprocess.run(
        [
            str(args.gcc_binary),
            "1",
            str(args.base_budget),
            str(args.budget_step),
        ],
        cwd=args.repo_root / "gcc-tally",
        check=True,
        stdout=subprocess.DEVNULL,
        stderr=subprocess.PIPE,
        text=True,
    )


def measure_gcc(args):
    env = os.environ.copy()
    env["TALLY_ASSUME_COMPILED"] = "1"
    command = [
        str(args.gcc_binary),
        str(args.metacycles),
        str(args.base_budget),
        str(args.budget_step),
    ]
    return measure("gcc-c", command, args.repo_root / "gcc-tally", args.repetitions, env)


def measure_llvm(args, workload_so):
    command = [
        str(args.llvm_host),
        str(workload_so),
        str(args.repo_root / "gcc-tally" / "data" / "graph.txt"),
        str(args.metacycles),
        str(args.base_budget),
        str(args.budget_step),
    ]
    return measure("llvm-rust", command, args.repo_root, args.repetitions, None)


def measure(label, command, cwd, repetitions, env):
    elapsed_values = []
    vertex_values = []
    last_output = ""
    for _ in range(repetitions):
        elapsed, output = run_timed(command, cwd=cwd, env=env)
        _rows, total_vertices = parse_rows(output)
        elapsed_values.append(elapsed)
        vertex_values.append(total_vertices)
        last_output = output

    mean_seconds = statistics.fmean(elapsed_values)
    mean_vertices = statistics.fmean(vertex_values)
    return {
        "label": label,
        "mean_seconds": mean_seconds,
        "stdev_seconds": statistics.stdev(elapsed_values) if len(elapsed_values) > 1 else 0.0,
        "mean_vertices": mean_vertices,
        "vertices_per_second": mean_vertices / mean_seconds,
        "last_output": last_output,
    }


def main():
    args = parse_args()
    if args.repetitions <= 0:
        raise ValueError("--repetitions must be positive")

    workload_so = build_llvm_workload(args.repo_root, args.build_root, args.llvm_pass)
    warm_gcc_workload(args)

    gcc = measure_gcc(args)
    llvm = measure_llvm(args, workload_so)

    fastest_seconds = min(gcc["mean_seconds"], llvm["mean_seconds"])
    fastest_throughput = max(gcc["vertices_per_second"], llvm["vertices_per_second"])

    print(
        "implementation,mean_seconds,stdev_seconds,mean_vertices,"
        "vertices_per_second,relative_time,relative_throughput"
    )
    for result in (gcc, llvm):
        print(
            f"{result['label']},"
            f"{result['mean_seconds']:.6f},"
            f"{result['stdev_seconds']:.6f},"
            f"{result['mean_vertices']:.2f},"
            f"{result['vertices_per_second']:.2f},"
            f"{result['mean_seconds'] / fastest_seconds:.3f},"
            f"{result['vertices_per_second'] / fastest_throughput:.3f}"
        )

    if gcc["vertices_per_second"] >= llvm["vertices_per_second"]:
        winner = "gcc-c"
        ratio = gcc["vertices_per_second"] / llvm["vertices_per_second"]
    else:
        winner = "llvm-rust"
        ratio = llvm["vertices_per_second"] / gcc["vertices_per_second"]
    print(f"winner_by_throughput,{winner},{ratio:.3f}x")

    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except Exception as exc:
        print(f"compare-performance failed: {exc}", file=sys.stderr)
        raise
