#!/usr/bin/env python3
"""Stress GCC/C and LLVM/Rust Tally with very large minithread counts."""

import argparse
import csv
import html
import math
import os
import statistics
import subprocess
import sys
import time
from pathlib import Path


DEFAULT_BUDGETS = "10,20,50,100,200,500,1000,5000,10000"
DEFAULT_TARGET_EDGES = 10_000_000_000
DEFAULT_THREAD_COUNT = 1_000_000
DEFAULT_STACK_BUDGET_GIB = 16.0


def parse_args():
    parser = argparse.ArgumentParser(
        description=(
            "Run a high-k stress benchmark for GCC/C Tally and LLVM/Rust Tally. "
            "The default is one million stackful minithreads and 10B total edges."
        )
    )
    parser.add_argument("--repo-root", type=Path, required=True)
    parser.add_argument("--build-root", type=Path, required=True)
    parser.add_argument("--gcc-binary", type=Path, required=True)
    parser.add_argument("--llvm-host", type=Path, required=True)
    parser.add_argument("--llvm-pass", type=Path, required=True)
    parser.add_argument("--thread-count", type=int, default=DEFAULT_THREAD_COUNT)
    parser.add_argument("--target-edges", type=int, default=DEFAULT_TARGET_EDGES)
    parser.add_argument("--budgets", default=DEFAULT_BUDGETS)
    parser.add_argument("--repetitions", type=int, default=1)
    parser.add_argument(
        "--stack-budget-gib",
        type=float,
        default=DEFAULT_STACK_BUDGET_GIB,
        help=(
            "Approximate stack-memory budget per implementation. The script "
            "divides this by --thread-count and passes the result to each host."
        ),
    )
    parser.add_argument(
        "--output-dir",
        type=Path,
        default=None,
        help="Directory for CSV and HTML reports. Defaults to <repo>/results/million-thread-stress.",
    )
    return parser.parse_args()


def parse_number_list(value):
    numbers = []
    for token in value.split(","):
        token = token.strip()
        if not token:
            continue
        number = int(token)
        if number <= 0:
            raise ValueError(f"invalid positive integer: {token}")
        numbers.append(number)
    return sorted(dict.fromkeys(numbers))


def build_llvm_workload(repo_root, build_root, llvm_pass):
    script = repo_root / "llvm-tally" / "scripts" / "build-rust-workload.sh"
    completed = subprocess.run(
        [str(script), "examples/self-walk", str(build_root / "llvm-tally"), str(llvm_pass)],
        cwd=repo_root,
        check=True,
        text=True,
        stdout=subprocess.PIPE,
    )
    return Path(completed.stdout.strip().splitlines()[-1])


def warm_gcc_workload(args):
    subprocess.run(
        [str(args.gcc_binary), "1", "10", "1", "1024", "1"],
        cwd=args.repo_root / "gcc-tally",
        check=True,
        stdout=subprocess.DEVNULL,
        stderr=subprocess.PIPE,
        text=True,
    )


def stack_sizes(thread_count, stack_budget_gib):
    total_bytes = int(stack_budget_gib * 1024**3)
    per_thread = max(4096, total_bytes // thread_count)
    per_thread = (per_thread // 4096) * 4096
    per_thread = max(4096, per_thread)
    gcc_stack_words = max(512, per_thread // 8)
    gcc_stack_bytes = gcc_stack_words * 8
    rust_stack_bytes = max(4096, per_thread)
    return gcc_stack_words, gcc_stack_bytes, rust_stack_bytes


def run_timed(command, *, cwd, env=None):
    start = time.perf_counter()
    completed = subprocess.run(
        command,
        cwd=cwd,
        env=env,
        check=False,
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
    )
    elapsed = time.perf_counter() - start
    if completed.returncode != 0:
        raise RuntimeError(
            "command failed with status "
            f"{completed.returncode}: {' '.join(command)}\n{completed.stderr}"
        )
    return elapsed, completed.stdout


def parse_summary_output(output, expected_target_edges):
    metadata = {}
    for line in output.splitlines():
        if ":" not in line:
            continue
        key, value = line.split(":", 1)
        metadata[key.strip()] = value.strip()

    total_edges = int(metadata["total_edges"])
    if total_edges < expected_target_edges:
        raise ValueError(
            f"total edges {total_edges} below requested target {expected_target_edges}"
        )

    return {
        "total_edges": total_edges,
        "run_seconds": float(metadata["run_seconds"]),
        "scheduler_cycles": int(metadata["scheduler_cycles"]),
        "thread_cycles": int(metadata["thread_cycles"]),
        "stack_bytes": int(metadata["stack_bytes"]),
    }


def measure(label, command, cwd, env, repetitions, target_edges):
    elapsed_values = []
    run_second_values = []
    edge_values = []
    scheduler_cycle_values = []
    thread_cycle_values = []
    stack_bytes = 0

    for _ in range(repetitions):
        elapsed, output = run_timed(command, cwd=cwd, env=env)
        parsed = parse_summary_output(output, target_edges)
        elapsed_values.append(elapsed)
        run_second_values.append(parsed["run_seconds"])
        edge_values.append(parsed["total_edges"])
        scheduler_cycle_values.append(parsed["scheduler_cycles"])
        thread_cycle_values.append(parsed["thread_cycles"])
        stack_bytes = parsed["stack_bytes"]

    mean_seconds = statistics.fmean(elapsed_values)
    mean_run_seconds = statistics.fmean(run_second_values)
    mean_edges = statistics.fmean(edge_values)
    return {
        "implementation": label,
        "mean_wall_seconds": mean_seconds,
        "mean_run_seconds": mean_run_seconds,
        "stdev_seconds": statistics.stdev(elapsed_values) if len(elapsed_values) > 1 else 0.0,
        "total_edges": mean_edges,
        "edges_per_second": mean_edges / mean_run_seconds,
        "wall_edges_per_second": mean_edges / mean_seconds,
        "scheduler_cycles": statistics.fmean(scheduler_cycle_values),
        "thread_cycles": statistics.fmean(thread_cycle_values),
        "stack_bytes": stack_bytes,
    }


def run_stress(args, budgets, workload_so, gcc_stack_words, gcc_stack_bytes, rust_stack_bytes):
    env = os.environ.copy()
    env["TALLY_ASSUME_COMPILED"] = "1"

    detail_rows = []
    comparison_rows = []
    for budget in budgets:
        gcc_command = [
            str(args.gcc_binary),
            str(args.thread_count),
            str(budget),
            str(args.target_edges),
            str(gcc_stack_words),
            "1",
        ]
        llvm_command = [
            str(args.llvm_host),
            str(workload_so),
            str(args.thread_count),
            str(budget),
            str(args.target_edges),
            str(rust_stack_bytes),
            "1",
        ]

        gcc = measure(
            "gcc-c",
            gcc_command,
            args.repo_root / "gcc-tally",
            env,
            args.repetitions,
            args.target_edges,
        )
        llvm = measure(
            "llvm-rust",
            llvm_command,
            args.repo_root,
            None,
            args.repetitions,
            args.target_edges,
        )

        for result in (gcc, llvm):
            detail_rows.append(
                {
                    "threads": args.thread_count,
                    "budget": budget,
                    **result,
                    "repetitions": args.repetitions,
                }
            )

        comparison = {
            "threads": args.thread_count,
            "budget": budget,
            "target_edges": args.target_edges,
            "gcc_wall_seconds": gcc["mean_wall_seconds"],
            "llvm_wall_seconds": llvm["mean_wall_seconds"],
            "gcc_run_seconds": gcc["mean_run_seconds"],
            "llvm_run_seconds": llvm["mean_run_seconds"],
            "gcc_edges_per_second": gcc["edges_per_second"],
            "llvm_edges_per_second": llvm["edges_per_second"],
            "gcc_wall_edges_per_second": gcc["wall_edges_per_second"],
            "llvm_wall_edges_per_second": llvm["wall_edges_per_second"],
            "llvm_vs_gcc_throughput": llvm["edges_per_second"] / gcc["edges_per_second"],
            "gcc_scheduler_cycles": gcc["scheduler_cycles"],
            "llvm_scheduler_cycles": llvm["scheduler_cycles"],
            "gcc_thread_cycles": gcc["thread_cycles"],
            "llvm_thread_cycles": llvm["thread_cycles"],
            "gcc_stack_bytes": gcc_stack_bytes,
            "llvm_stack_bytes": rust_stack_bytes,
        }
        comparison_rows.append(comparison)

        print(
            f"k={args.thread_count} b={budget:5d} "
            f"gcc={gcc['edges_per_second'] / 1_000_000:8.2f}M edges/s "
            f"llvm={llvm['edges_per_second'] / 1_000_000:8.2f}M edges/s "
            f"ratio={comparison['llvm_vs_gcc_throughput']:.3f}x",
            flush=True,
        )

    return detail_rows, comparison_rows


def write_csv(path, rows, fieldnames):
    path.parent.mkdir(parents=True, exist_ok=True)
    with path.open("w", newline="") as handle:
        writer = csv.DictWriter(handle, fieldnames=fieldnames)
        writer.writeheader()
        writer.writerows(rows)


def write_reports(output_dir, detail_rows, comparison_rows, args, budgets):
    output_dir.mkdir(parents=True, exist_ok=True)
    detail_csv = output_dir / "million-thread-stress-detail.csv"
    comparison_csv = output_dir / "million-thread-stress-comparison.csv"
    html_report = output_dir / "million-thread-stress-report.html"

    write_csv(
        detail_csv,
        detail_rows,
        [
            "threads",
            "budget",
            "implementation",
            "mean_wall_seconds",
            "mean_run_seconds",
            "stdev_seconds",
            "total_edges",
            "edges_per_second",
            "wall_edges_per_second",
            "scheduler_cycles",
            "thread_cycles",
            "stack_bytes",
            "repetitions",
        ],
    )
    write_csv(
        comparison_csv,
        comparison_rows,
        [
            "threads",
            "budget",
            "target_edges",
            "gcc_wall_seconds",
            "llvm_wall_seconds",
            "gcc_run_seconds",
            "llvm_run_seconds",
            "gcc_edges_per_second",
            "llvm_edges_per_second",
            "gcc_wall_edges_per_second",
            "llvm_wall_edges_per_second",
            "llvm_vs_gcc_throughput",
            "gcc_scheduler_cycles",
            "llvm_scheduler_cycles",
            "gcc_thread_cycles",
            "llvm_thread_cycles",
            "gcc_stack_bytes",
            "llvm_stack_bytes",
        ],
    )
    write_html_report(html_report, comparison_rows, args, budgets)
    return detail_csv, comparison_csv, html_report


def write_html_report(path, rows, args, budgets):
    max_gcc = max(row["gcc_edges_per_second"] for row in rows)
    max_llvm = max(row["llvm_edges_per_second"] for row in rows)
    max_ratio = max(row["llvm_vs_gcc_throughput"] for row in rows)
    best_gcc = max(rows, key=lambda row: row["gcc_edges_per_second"])
    best_llvm = max(rows, key=lambda row: row["llvm_edges_per_second"])
    best_ratio = max(rows, key=lambda row: row["llvm_vs_gcc_throughput"])
    min_ratio = min(rows, key=lambda row: row["llvm_vs_gcc_throughput"])
    width = 820
    height = 260

    html_text = f"""<!doctype html>
<html lang="en">
<head>
<meta charset="utf-8">
<title>Million-Thread Tally Stress</title>
<style>
body {{
  margin: 0;
  padding: 32px;
  font-family: Inter, ui-sans-serif, system-ui, -apple-system, BlinkMacSystemFont, "Segoe UI", sans-serif;
  background: #f5f7fb;
  color: #162033;
}}
h1, h2 {{ margin: 0 0 12px; }}
p {{ color: #5c667a; max-width: 980px; }}
.summary {{
  display: grid;
  grid-template-columns: repeat(auto-fit, minmax(220px, 1fr));
  gap: 12px;
  margin: 22px 0 28px;
}}
.metric {{
  background: white;
  border: 1px solid #d8deea;
  border-radius: 8px;
  padding: 14px 16px;
}}
.metric span {{
  display: block;
  color: #5c667a;
  font-size: 12px;
  text-transform: uppercase;
  letter-spacing: .04em;
  margin-bottom: 6px;
}}
.metric strong {{ font-size: 22px; }}
.section {{
  background: white;
  border: 1px solid #d8deea;
  border-radius: 8px;
  padding: 18px;
  margin: 18px 0;
  overflow-x: auto;
}}
table {{
  border-collapse: collapse;
  font-size: 13px;
}}
th, td {{
  border: 1px solid #e1e6f0;
  padding: 8px 10px;
  text-align: right;
  white-space: nowrap;
  font-variant-numeric: tabular-nums;
}}
th {{ background: #eef2f8; }}
svg {{ max-width: 100%; height: auto; }}
.gcc {{ color: #2563eb; }}
.llvm {{ color: #16a34a; }}
.ratio {{ color: #0f766e; }}
</style>
</head>
<body>
<h1>Million-Thread Tally Stress</h1>
<p>
Summary-only run with {args.thread_count:,} stackful minithreads. Each budget
cell traverses {args.target_edges:,} total synthetic graph edges per
implementation.
</p>
<div class="summary">
  {metric("Threads", f"{args.thread_count:,}")}
  {metric("Target edges", f"{args.target_edges:,}")}
  {metric("Budgets", ", ".join(str(b) for b in budgets))}
  {metric("Best GCC", describe_best(best_gcc, "gcc_edges_per_second"))}
  {metric("Best LLVM", describe_best(best_llvm, "llvm_edges_per_second"))}
  {metric("Max LLVM/GCC", describe_ratio(best_ratio))}
  {metric("Min LLVM/GCC", describe_ratio(min_ratio))}
  {metric("Stack per thread", f"GCC {best_gcc['gcc_stack_bytes']:,} B / LLVM {best_gcc['llvm_stack_bytes']:,} B")}
</div>
<div class="section">
<h2>Throughput by Budget</h2>
{line_chart(rows, width, height, max(max_gcc, max_llvm))}
</div>
<div class="section">
<h2>LLVM/GCC Throughput Ratio</h2>
{ratio_chart(rows, width, height, max_ratio)}
</div>
<div class="section">
<h2>Raw Results</h2>
{results_table(rows)}
</div>
</body>
</html>
"""
    path.write_text(html_text)


def metric(label, value):
    return f'<div class="metric"><span>{html.escape(label)}</span><strong>{html.escape(value)}</strong></div>'


def describe_best(row, field):
    return f"{row[field] / 1_000_000:.1f}M/s at b={int(row['budget'])}"


def describe_ratio(row):
    return f"{row['llvm_vs_gcc_throughput']:.2f}x at b={int(row['budget'])}"


def line_chart(rows, width, height, max_value):
    margin_left = 54
    margin_bottom = 34
    plot_w = width - margin_left - 18
    plot_h = height - 26 - margin_bottom
    budgets = [row["budget"] for row in rows]
    min_log = math.log10(min(budgets))
    max_log = math.log10(max(budgets))

    def x_for(budget):
        if max_log == min_log:
            return margin_left + plot_w / 2
        return margin_left + (math.log10(budget) - min_log) / (max_log - min_log) * plot_w

    def y_for(value):
        return 14 + plot_h - (value / max_value) * plot_h

    gcc_points = " ".join(
        f"{x_for(row['budget']):.1f},{y_for(row['gcc_edges_per_second']):.1f}" for row in rows
    )
    llvm_points = " ".join(
        f"{x_for(row['budget']):.1f},{y_for(row['llvm_edges_per_second']):.1f}" for row in rows
    )
    labels = "".join(
        f'<text x="{x_for(b):.1f}" y="{height - 8}" font-size="11" text-anchor="middle">{b}</text>'
        for b in budgets
    )
    return f"""
<svg viewBox="0 0 {width} {height}" role="img" aria-label="Throughput line chart">
  <line x1="{margin_left}" y1="14" x2="{margin_left}" y2="{height - margin_bottom}" stroke="#9aa5b5"/>
  <line x1="{margin_left}" y1="{height - margin_bottom}" x2="{width - 18}" y2="{height - margin_bottom}" stroke="#9aa5b5"/>
  <text x="8" y="18" font-size="11">{max_value / 1_000_000:.0f}M/s</text>
  <text x="{width - 18}" y="{height - 8}" font-size="11" text-anchor="end">budget</text>
  {labels}
  <polyline fill="none" stroke="#2563eb" stroke-width="3" points="{gcc_points}"/>
  <polyline fill="none" stroke="#16a34a" stroke-width="3" points="{llvm_points}"/>
  <text x="{margin_left + 8}" y="34" font-size="13" fill="#2563eb">GCC</text>
  <text x="{margin_left + 8}" y="54" font-size="13" fill="#16a34a">LLVM/Rust</text>
</svg>
"""


def ratio_chart(rows, width, height, max_ratio):
    margin_left = 54
    margin_bottom = 34
    plot_w = width - margin_left - 18
    plot_h = height - 26 - margin_bottom
    budgets = [row["budget"] for row in rows]
    min_log = math.log10(min(budgets))
    max_log = math.log10(max(budgets))

    def x_for(budget):
        if max_log == min_log:
            return margin_left + plot_w / 2
        return margin_left + (math.log10(budget) - min_log) / (max_log - min_log) * plot_w

    def y_for(value):
        return 14 + plot_h - (value / max_ratio) * plot_h

    points = " ".join(
        f"{x_for(row['budget']):.1f},{y_for(row['llvm_vs_gcc_throughput']):.1f}" for row in rows
    )
    labels = "".join(
        f'<text x="{x_for(b):.1f}" y="{height - 8}" font-size="11" text-anchor="middle">{b}</text>'
        for b in budgets
    )
    one_y = y_for(1.0)
    return f"""
<svg viewBox="0 0 {width} {height}" role="img" aria-label="LLVM to GCC throughput ratio chart">
  <line x1="{margin_left}" y1="14" x2="{margin_left}" y2="{height - margin_bottom}" stroke="#9aa5b5"/>
  <line x1="{margin_left}" y1="{height - margin_bottom}" x2="{width - 18}" y2="{height - margin_bottom}" stroke="#9aa5b5"/>
  <line x1="{margin_left}" y1="{one_y:.1f}" x2="{width - 18}" y2="{one_y:.1f}" stroke="#cbd5e1" stroke-dasharray="5 5"/>
  <text x="8" y="18" font-size="11">{max_ratio:.1f}x</text>
  <text x="16" y="{one_y - 4:.1f}" font-size="11">1x</text>
  {labels}
  <polyline fill="none" stroke="#0f766e" stroke-width="3" points="{points}"/>
</svg>
"""


def results_table(rows):
    output = [
        "<table><thead><tr>",
        "<th>budget</th><th>GCC M/s</th><th>LLVM M/s</th><th>LLVM/GCC</th>",
            "<th>GCC run seconds</th><th>LLVM run seconds</th>",
            "<th>GCC wall seconds</th><th>LLVM wall seconds</th>",
        "<th>GCC scheduler cycles</th><th>LLVM scheduler cycles</th>",
        "</tr></thead><tbody>",
    ]
    for row in rows:
        output.append(
            "<tr>"
            f"<td>{int(row['budget'])}</td>"
            f"<td>{row['gcc_edges_per_second'] / 1_000_000:.2f}</td>"
            f"<td>{row['llvm_edges_per_second'] / 1_000_000:.2f}</td>"
            f"<td>{row['llvm_vs_gcc_throughput']:.3f}x</td>"
            f"<td>{row['gcc_run_seconds']:.3f}</td>"
            f"<td>{row['llvm_run_seconds']:.3f}</td>"
            f"<td>{row['gcc_wall_seconds']:.3f}</td>"
            f"<td>{row['llvm_wall_seconds']:.3f}</td>"
            f"<td>{row['gcc_scheduler_cycles']:.0f}</td>"
            f"<td>{row['llvm_scheduler_cycles']:.0f}</td>"
            "</tr>"
        )
    output.append("</tbody></table>")
    return "".join(output)


def print_comparison_csv(rows):
    fieldnames = [
        "threads",
        "budget",
        "target_edges",
        "gcc_wall_seconds",
        "llvm_wall_seconds",
        "gcc_run_seconds",
        "llvm_run_seconds",
        "gcc_edges_per_second",
        "llvm_edges_per_second",
        "llvm_vs_gcc_throughput",
    ]
    writer = csv.DictWriter(sys.stdout, fieldnames=fieldnames, extrasaction="ignore")
    writer.writeheader()
    writer.writerows(rows)


def main():
    args = parse_args()
    if args.thread_count <= 0:
        raise ValueError("--thread-count must be positive")
    if args.target_edges <= 0:
        raise ValueError("--target-edges must be positive")
    if args.repetitions <= 0:
        raise ValueError("--repetitions must be positive")
    if args.stack_budget_gib <= 0:
        raise ValueError("--stack-budget-gib must be positive")

    args.repo_root = args.repo_root.resolve()
    args.build_root = args.build_root.resolve()
    args.gcc_binary = args.gcc_binary.resolve()
    args.llvm_host = args.llvm_host.resolve()
    args.llvm_pass = args.llvm_pass.resolve()
    output_dir = (
        args.output_dir or args.repo_root / "results" / "million-thread-stress"
    ).resolve()
    budgets = parse_number_list(args.budgets)
    gcc_stack_words, gcc_stack_bytes, rust_stack_bytes = stack_sizes(
        args.thread_count, args.stack_budget_gib
    )

    print(
        f"stack_budget_gib,{args.stack_budget_gib:.3f}\n"
        f"gcc_stack_bytes_per_thread,{gcc_stack_bytes}\n"
        f"llvm_stack_bytes_per_thread,{rust_stack_bytes}\n"
        f"thread_count,{args.thread_count}\n"
        f"target_edges,{args.target_edges}",
        flush=True,
    )

    workload_so = build_llvm_workload(args.repo_root, args.build_root, args.llvm_pass)
    warm_gcc_workload(args)
    detail_rows, comparison_rows = run_stress(
        args, budgets, workload_so, gcc_stack_words, gcc_stack_bytes, rust_stack_bytes
    )
    detail_csv, comparison_csv, html_report = write_reports(
        output_dir, detail_rows, comparison_rows, args, budgets
    )

    print()
    print_comparison_csv(comparison_rows)
    print()
    print(f"wrote_detail_csv,{detail_csv}")
    print(f"wrote_comparison_csv,{comparison_csv}")
    print(f"wrote_html_report,{html_report}")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except Exception as exc:
        print(f"compare-million-threads failed: {exc}", file=sys.stderr)
        raise
