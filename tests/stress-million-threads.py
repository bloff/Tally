#!/usr/bin/env python3
"""Stress LLVM/Rust Tally with very large minithread counts."""

import argparse
import csv
import html
import math
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
            "Run a high-k stress benchmark for LLVM/Rust Tally. The default "
            "is one million stackful minithreads and 10B total edges."
        )
    )
    parser.add_argument("--repo-root", type=Path, required=True)
    parser.add_argument("--build-root", type=Path, required=True)
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
            "Approximate stack-memory budget for the run. The script divides "
            "this by --thread-count and passes the result to the LLVM host."
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
    seen = set()
    for token in value.split(","):
        token = token.strip()
        if not token:
            continue
        number = int(token)
        if number <= 0:
            raise ValueError(f"invalid positive integer: {token}")
        if number not in seen:
            numbers.append(number)
            seen.add(number)
    return numbers


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


def stack_size(thread_count, stack_budget_gib):
    total_bytes = int(stack_budget_gib * 1024**3)
    per_thread = max(4096, total_bytes // thread_count)
    per_thread = (per_thread // 4096) * 4096
    return max(4096, per_thread)


def run_timed(command, *, cwd):
    start = time.perf_counter()
    completed = subprocess.run(
        command,
        cwd=cwd,
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


def measure(command, cwd, repetitions, target_edges):
    elapsed_values = []
    run_second_values = []
    edge_values = []
    scheduler_cycle_values = []
    thread_cycle_values = []
    stack_bytes = 0

    for _ in range(repetitions):
        elapsed, output = run_timed(command, cwd=cwd)
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
        "implementation": "llvm-rust",
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


def run_stress(args, budgets, workload_so, rust_stack_bytes):
    rows = []
    for budget in budgets:
        command = [
            str(args.llvm_host),
            str(workload_so),
            str(args.thread_count),
            str(budget),
            str(args.target_edges),
            str(rust_stack_bytes),
            "1",
        ]

        result = measure(command, args.repo_root, args.repetitions, args.target_edges)
        row = {
            "threads": args.thread_count,
            "budget": budget,
            "target_edges": args.target_edges,
            **result,
            "repetitions": args.repetitions,
        }
        rows.append(row)

        print(
            f"k={args.thread_count} b={budget:5d} "
            f"llvm={result['edges_per_second'] / 1_000_000:8.2f}M edges/s "
            f"run_seconds={result['mean_run_seconds']:.3f}",
            flush=True,
        )

    return rows


def write_csv(path, rows, fieldnames):
    path.parent.mkdir(parents=True, exist_ok=True)
    with path.open("w", newline="") as handle:
        writer = csv.DictWriter(handle, fieldnames=fieldnames, extrasaction="ignore")
        writer.writeheader()
        writer.writerows(rows)


def write_reports(output_dir, rows, args, budgets):
    output_dir.mkdir(parents=True, exist_ok=True)
    detail_csv = output_dir / "million-thread-stress-detail.csv"
    summary_csv = output_dir / "million-thread-stress-summary.csv"
    html_report = output_dir / "million-thread-stress-report.html"

    fieldnames = [
        "threads",
        "budget",
        "target_edges",
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
    ]
    write_csv(detail_csv, rows, fieldnames)
    write_csv(
        summary_csv,
        rows,
        [
            "threads",
            "budget",
            "target_edges",
            "mean_wall_seconds",
            "mean_run_seconds",
            "edges_per_second",
            "wall_edges_per_second",
            "scheduler_cycles",
            "thread_cycles",
            "stack_bytes",
            "repetitions",
        ],
    )
    write_html_report(html_report, rows, args, budgets)
    return detail_csv, summary_csv, html_report


def write_html_report(path, rows, args, budgets):
    max_llvm = max(row["edges_per_second"] for row in rows)
    best_llvm = max(rows, key=lambda row: row["edges_per_second"])
    fastest = min(rows, key=lambda row: row["mean_run_seconds"])
    width = 820
    height = 260

    html_text = f"""<!doctype html>
<html lang="en">
<head>
<meta charset="utf-8">
<title>LLVM Tally Million-Thread Stress</title>
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
.llvm {{ color: #16a34a; }}
</style>
</head>
<body>
<h1>LLVM Tally Million-Thread Stress</h1>
<p>
Summary-only run with {args.thread_count:,} stackful minithreads. Each budget
cell traverses {args.target_edges:,} total synthetic graph edges.
</p>
<div class="summary">
  {metric("Threads", f"{args.thread_count:,}")}
  {metric("Target edges", f"{args.target_edges:,}")}
  {metric("Budgets", ", ".join(str(b) for b in budgets))}
  {metric("Best LLVM", describe_best(best_llvm))}
  {metric("Fastest run", describe_time(fastest))}
  {metric("Stack per thread", f"{best_llvm['stack_bytes']:,} B")}
</div>
<div class="section">
<h2>Throughput by Budget</h2>
{line_chart(rows, width, height, max_llvm)}
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


def describe_best(row):
    return f"{row['edges_per_second'] / 1_000_000:.1f}M/s at b={int(row['budget'])}"


def describe_time(row):
    return f"{row['mean_run_seconds']:.3f}s at b={int(row['budget'])}"


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

    llvm_points = " ".join(
        f"{x_for(row['budget']):.1f},{y_for(row['edges_per_second']):.1f}" for row in rows
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
  <polyline fill="none" stroke="#16a34a" stroke-width="3" points="{llvm_points}"/>
  <text x="{margin_left + 8}" y="34" font-size="13" fill="#16a34a">LLVM/Rust</text>
</svg>
"""


def results_table(rows):
    output = [
        "<table><thead><tr>",
        "<th>budget</th><th>LLVM M/s</th><th>run seconds</th>",
        "<th>wall seconds</th><th>scheduler cycles</th><th>thread cycles</th>",
        "</tr></thead><tbody>",
    ]
    for row in rows:
        output.append(
            "<tr>"
            f"<td>{int(row['budget'])}</td>"
            f"<td>{row['edges_per_second'] / 1_000_000:.2f}</td>"
            f"<td>{row['mean_run_seconds']:.3f}</td>"
            f"<td>{row['mean_wall_seconds']:.3f}</td>"
            f"<td>{row['scheduler_cycles']:.0f}</td>"
            f"<td>{row['thread_cycles']:.0f}</td>"
            "</tr>"
        )
    output.append("</tbody></table>")
    return "".join(output)


def print_summary_csv(rows):
    fieldnames = [
        "threads",
        "budget",
        "target_edges",
        "mean_wall_seconds",
        "mean_run_seconds",
        "edges_per_second",
        "wall_edges_per_second",
        "scheduler_cycles",
        "thread_cycles",
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
    args.llvm_host = args.llvm_host.resolve()
    args.llvm_pass = args.llvm_pass.resolve()
    output_dir = (
        args.output_dir or args.repo_root / "results" / "million-thread-stress"
    ).resolve()
    budgets = parse_number_list(args.budgets)
    rust_stack_bytes = stack_size(args.thread_count, args.stack_budget_gib)

    print(
        f"stack_budget_gib,{args.stack_budget_gib:.3f}\n"
        f"llvm_stack_bytes_per_thread,{rust_stack_bytes}\n"
        f"thread_count,{args.thread_count}\n"
        f"target_edges,{args.target_edges}",
        flush=True,
    )

    workload_so = build_llvm_workload(args.repo_root, args.build_root, args.llvm_pass)
    rows = run_stress(args, budgets, workload_so, rust_stack_bytes)
    detail_csv, summary_csv, html_report = write_reports(output_dir, rows, args, budgets)

    print()
    print_summary_csv(rows)
    print()
    print(f"wrote_detail_csv,{detail_csv}")
    print(f"wrote_summary_csv,{summary_csv}")
    print(f"wrote_html_report,{html_report}")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except Exception as exc:
        print(f"stress-million-threads failed: {exc}", file=sys.stderr)
        raise
