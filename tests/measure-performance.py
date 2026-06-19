#!/usr/bin/env python3
"""Run the LLVM/Rust self-walk benchmark matrix."""

import argparse
import csv
import html
import math
import statistics
import subprocess
import sys
import time
from pathlib import Path


DEFAULT_THREAD_COUNTS = "1-10,20,30,40,50,100,200,300,400,500,1000"
DEFAULT_BUDGETS = "10,20,50,100,200,500,1000"
DEFAULT_TARGET_EDGES = 50_000_000


def parse_args():
    parser = argparse.ArgumentParser(
        description="Run a matrix of finite self-contained LLVM/Rust Tally benchmarks."
    )
    parser.add_argument("--repo-root", type=Path, required=True)
    parser.add_argument("--build-root", type=Path, required=True)
    parser.add_argument("--llvm-host", type=Path, required=True)
    parser.add_argument("--llvm-pass", type=Path, required=True)
    parser.add_argument("--target-edges", type=int, default=DEFAULT_TARGET_EDGES)
    parser.add_argument("--thread-counts", default=DEFAULT_THREAD_COUNTS)
    parser.add_argument("--budgets", default=DEFAULT_BUDGETS)
    parser.add_argument("--repetitions", type=int, default=1)
    parser.add_argument(
        "--output-dir",
        type=Path,
        default=None,
        help="Directory for CSV and HTML reports. Defaults to <repo>/results/performance-matrix.",
    )
    return parser.parse_args()


def parse_number_list(value):
    numbers = []
    for token in value.split(","):
        token = token.strip()
        if not token:
            continue
        if "-" in token:
            start_text, end_text = token.split("-", 1)
            start = int(start_text)
            end = int(end_text)
            if start <= 0 or end < start:
                raise ValueError(f"invalid range: {token}")
            numbers.extend(range(start, end + 1))
        else:
            number = int(token)
            if number <= 0:
                raise ValueError(f"invalid positive integer: {token}")
            numbers.append(number)
    return sorted(dict.fromkeys(numbers))


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


def parse_host_output(output, expected_threads, expected_target_edges):
    lines = output.splitlines()
    metadata = {}
    for line in lines:
        if line.startswith("thread,"):
            break
        if ":" not in line:
            continue
        key, value = line.split(":", 1)
        metadata[key.strip()] = value.strip()

    try:
        header_index = next(i for i, line in enumerate(lines) if line.startswith("thread,"))
    except StopIteration as exc:
        raise ValueError("output did not contain a CSV header") from exc

    rows = list(csv.DictReader(lines[header_index:]))
    if len(rows) != expected_threads:
        raise ValueError(f"expected {expected_threads} thread rows, got {len(rows)}")

    total_edges = 0
    total_thread_cycles = 0
    for row in rows:
        target = int(row["target_edges"])
        vertices = int(row["vertices_walked"])
        if vertices < target:
            raise ValueError(f"thread {row['thread']} stopped before its target")
        total_edges += vertices
        total_thread_cycles += int(row["cycles_run"])

    if total_edges < expected_target_edges:
        raise ValueError(
            f"total edges {total_edges} below requested target {expected_target_edges}"
        )

    metadata_total = int(metadata.get("total_edges", total_edges))
    if metadata_total != total_edges:
        raise ValueError("metadata total_edges does not match thread rows")

    return {
        "total_edges": total_edges,
        "scheduler_cycles": int(metadata.get("scheduler_cycles", 0)),
        "thread_cycles": total_thread_cycles,
    }


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


def measure(command, cwd, repetitions, thread_count, target_edges):
    elapsed_values = []
    edge_values = []
    scheduler_cycle_values = []
    thread_cycle_values = []

    for _ in range(repetitions):
        elapsed, output = run_timed(command, cwd=cwd)
        parsed = parse_host_output(output, thread_count, target_edges)
        elapsed_values.append(elapsed)
        edge_values.append(parsed["total_edges"])
        scheduler_cycle_values.append(parsed["scheduler_cycles"])
        thread_cycle_values.append(parsed["thread_cycles"])

    mean_seconds = statistics.fmean(elapsed_values)
    mean_edges = statistics.fmean(edge_values)
    return {
        "implementation": "llvm-rust",
        "mean_seconds": mean_seconds,
        "stdev_seconds": statistics.stdev(elapsed_values) if len(elapsed_values) > 1 else 0.0,
        "total_edges": mean_edges,
        "edges_per_second": mean_edges / mean_seconds,
        "scheduler_cycles": statistics.fmean(scheduler_cycle_values),
        "thread_cycles": statistics.fmean(thread_cycle_values),
    }


def run_matrix(args, thread_counts, budgets, workload_so):
    detail_rows = []
    summary_rows = []
    for thread_count in thread_counts:
        for budget in budgets:
            command = [
                str(args.llvm_host),
                str(workload_so),
                str(thread_count),
                str(budget),
                str(args.target_edges),
            ]
            result = measure(
                command,
                args.repo_root,
                args.repetitions,
                thread_count,
                args.target_edges,
            )
            row = {
                "threads": thread_count,
                "budget": budget,
                **result,
                "repetitions": args.repetitions,
            }
            detail_rows.append(row)
            summary_rows.append(
                {
                    "threads": thread_count,
                    "budget": budget,
                    "llvm_seconds": result["mean_seconds"],
                    "llvm_edges_per_second": result["edges_per_second"],
                    "llvm_scheduler_cycles": result["scheduler_cycles"],
                    "llvm_thread_cycles": result["thread_cycles"],
                }
            )

            print(
                f"k={thread_count:4d} b={budget:4d} "
                f"llvm={result['edges_per_second'] / 1_000_000:8.2f}M edges/s",
                flush=True,
            )

    return detail_rows, summary_rows


def write_csv(path, rows, fieldnames):
    path.parent.mkdir(parents=True, exist_ok=True)
    with path.open("w", newline="") as handle:
        writer = csv.DictWriter(handle, fieldnames=fieldnames)
        writer.writeheader()
        writer.writerows(rows)


def write_reports(output_dir, detail_rows, summary_rows, thread_counts, budgets, args):
    output_dir.mkdir(parents=True, exist_ok=True)
    detail_csv = output_dir / "performance-matrix-detail.csv"
    summary_csv = output_dir / "performance-matrix-summary.csv"
    html_report = output_dir / "performance-matrix-report.html"

    write_csv(
        detail_csv,
        detail_rows,
        [
            "threads",
            "budget",
            "implementation",
            "mean_seconds",
            "stdev_seconds",
            "total_edges",
            "edges_per_second",
            "scheduler_cycles",
            "thread_cycles",
            "repetitions",
        ],
    )
    write_csv(
        summary_csv,
        summary_rows,
        [
            "threads",
            "budget",
            "llvm_seconds",
            "llvm_edges_per_second",
            "llvm_scheduler_cycles",
            "llvm_thread_cycles",
        ],
    )
    write_html_report(html_report, summary_rows, thread_counts, budgets, args)
    return detail_csv, summary_csv, html_report


def write_html_report(path, rows, thread_counts, budgets, args):
    by_combo = {(int(row["threads"]), int(row["budget"])): row for row in rows}
    max_llvm = max(row["llvm_edges_per_second"] for row in rows)
    best_llvm = max(rows, key=lambda row: row["llvm_edges_per_second"])
    fastest = min(rows, key=lambda row: row["llvm_seconds"])
    most_cycles = max(rows, key=lambda row: row["llvm_thread_cycles"])

    html_text = f"""<!doctype html>
<html lang="en">
<head>
<meta charset="utf-8">
<title>LLVM Tally Performance Matrix</title>
<style>
:root {{
  color-scheme: light;
  --ink: #172033;
  --muted: #5d667a;
  --line: #d8deea;
  --panel: #ffffff;
  --page: #f4f6fa;
}}
body {{
  margin: 0;
  padding: 32px;
  font-family: Inter, ui-sans-serif, system-ui, -apple-system, BlinkMacSystemFont, "Segoe UI", sans-serif;
  background: var(--page);
  color: var(--ink);
}}
h1, h2 {{ margin: 0 0 12px; }}
p {{ color: var(--muted); max-width: 980px; }}
.summary {{
  display: grid;
  grid-template-columns: repeat(auto-fit, minmax(220px, 1fr));
  gap: 12px;
  margin: 22px 0 28px;
}}
.metric {{
  background: var(--panel);
  border: 1px solid var(--line);
  border-radius: 8px;
  padding: 14px 16px;
  box-shadow: 0 1px 2px rgba(20, 28, 45, 0.04);
}}
.metric span {{
  display: block;
  color: var(--muted);
  font-size: 12px;
  text-transform: uppercase;
  letter-spacing: 0.04em;
  margin-bottom: 6px;
}}
.metric strong {{ font-size: 22px; }}
.section {{
  background: var(--panel);
  border: 1px solid var(--line);
  border-radius: 8px;
  padding: 18px;
  margin: 18px 0;
  overflow-x: auto;
}}
table {{
  border-collapse: collapse;
  font-size: 13px;
  min-width: max-content;
}}
th, td {{
  border: 1px solid #e1e6f0;
  padding: 7px 8px;
  text-align: right;
  white-space: nowrap;
}}
th {{
  background: #eef2f8;
  color: #26344f;
  font-weight: 650;
  position: sticky;
  top: 0;
}}
th:first-child, td:first-child {{
  position: sticky;
  left: 0;
  z-index: 1;
  background: #f8fafd;
  text-align: right;
  font-weight: 650;
}}
td {{ font-variant-numeric: tabular-nums; }}
.note {{ font-size: 13px; }}
</style>
</head>
<body>
<h1>LLVM Tally Performance Matrix</h1>
<p>
Single-process benchmark over the self-contained synthetic graph walk. Each cell
runs until all minithreads collectively traverse {args.target_edges:,} edges.
Thread count is <code>k</code>; budget is per minithread scheduler cycle.
</p>
<div class="summary">
  {summary_card("Target edges", f"{args.target_edges:,}")}
  {summary_card("Thread counts", f"{min(thread_counts)}..{max(thread_counts)} ({len(thread_counts)} values)")}
  {summary_card("Budgets", ", ".join(str(b) for b in budgets))}
  {summary_card("Repetitions", str(args.repetitions))}
  {summary_card("Best LLVM", describe_best(best_llvm, "llvm_edges_per_second"))}
  {summary_card("Fastest cell", describe_time(fastest))}
  {summary_card("Most thread cycles", describe_cycles(most_cycles))}
</div>
<div class="section">
<h2>LLVM/Rust Throughput (M edges/s)</h2>
{heatmap_table(thread_counts, budgets, by_combo, lambda row: row["llvm_edges_per_second"] / 1_000_000, lambda value: f"{value:.1f}", lambda value: throughput_color(value * 1_000_000, max_llvm))}
</div>
<div class="section">
<h2>Run Time (seconds)</h2>
{heatmap_table(thread_counts, budgets, by_combo, lambda row: row["llvm_seconds"], lambda value: f"{value:.3f}", lambda value: time_color(value, rows))}
</div>
<div class="section">
<h2>Scheduler Cycles</h2>
{heatmap_table(thread_counts, budgets, by_combo, lambda row: row["llvm_scheduler_cycles"], lambda value: f"{value:.0f}", lambda value: cycles_color(value, rows))}
</div>
</body>
</html>
"""
    path.write_text(html_text)


def summary_card(label, value):
    return f'<div class="metric"><span>{html.escape(label)}</span><strong>{html.escape(value)}</strong></div>'


def describe_best(row, field):
    return (
        f"{row[field] / 1_000_000:.1f}M/s "
        f"at k={int(row['threads'])}, b={int(row['budget'])}"
    )


def describe_time(row):
    return f"{row['llvm_seconds']:.3f}s at k={int(row['threads'])}, b={int(row['budget'])}"


def describe_cycles(row):
    return f"{row['llvm_thread_cycles']:.0f} at k={int(row['threads'])}, b={int(row['budget'])}"


def heatmap_table(thread_counts, budgets, by_combo, value_fn, label_fn, color_fn):
    output = ['<table><thead><tr><th>k \\ b</th>']
    output.extend(f"<th>{budget}</th>" for budget in budgets)
    output.append("</tr></thead><tbody>")
    for thread_count in thread_counts:
        output.append(f"<tr><td>{thread_count}</td>")
        for budget in budgets:
            row = by_combo[(thread_count, budget)]
            value = value_fn(row)
            tooltip = (
                f"k={thread_count}, b={budget}; "
                f"LLVM {row['llvm_edges_per_second'] / 1_000_000:.2f}M/s; "
                f"time {row['llvm_seconds']:.6f}s; "
                f"scheduler cycles {row['llvm_scheduler_cycles']:.0f}"
            )
            output.append(
                f'<td title="{html.escape(tooltip)}" style="background:{color_fn(value)}">'
                f"{html.escape(label_fn(value))}</td>"
            )
        output.append("</tr>")
    output.append("</tbody></table>")
    return "".join(output)


def throughput_color(value, max_value):
    scale = 0.0 if max_value <= 0 else min(1.0, max(0.0, value / max_value))
    scale = math.sqrt(scale)
    return interpolate_hex("#ffffff", "#16a34a", scale)


def time_color(value, rows):
    values = [row["llvm_seconds"] for row in rows]
    min_value = min(values)
    max_value = max(values)
    scale = 0.0 if max_value == min_value else (value - min_value) / (max_value - min_value)
    return interpolate_hex("#dcfce7", "#f97316", scale)


def cycles_color(value, rows):
    max_value = max(row["llvm_scheduler_cycles"] for row in rows)
    scale = 0.0 if max_value <= 0 else min(1.0, max(0.0, value / max_value))
    return interpolate_hex("#ffffff", "#2563eb", math.sqrt(scale))


def interpolate_hex(low, high, scale):
    low_rgb = tuple(int(low[i : i + 2], 16) for i in (1, 3, 5))
    high_rgb = tuple(int(high[i : i + 2], 16) for i in (1, 3, 5))
    mixed = tuple(round(a + (b - a) * scale) for a, b in zip(low_rgb, high_rgb))
    return f"#{mixed[0]:02x}{mixed[1]:02x}{mixed[2]:02x}"


def print_summary_csv(rows):
    fieldnames = [
        "threads",
        "budget",
        "llvm_seconds",
        "llvm_edges_per_second",
        "llvm_scheduler_cycles",
        "llvm_thread_cycles",
    ]
    writer = csv.DictWriter(sys.stdout, fieldnames=fieldnames, extrasaction="ignore")
    writer.writeheader()
    writer.writerows(rows)


def main():
    args = parse_args()
    if args.repetitions <= 0:
        raise ValueError("--repetitions must be positive")
    if args.target_edges <= 0:
        raise ValueError("--target-edges must be positive")

    args.repo_root = args.repo_root.resolve()
    args.build_root = args.build_root.resolve()
    args.llvm_host = args.llvm_host.resolve()
    args.llvm_pass = args.llvm_pass.resolve()

    thread_counts = parse_number_list(args.thread_counts)
    budgets = parse_number_list(args.budgets)
    output_dir = (args.output_dir or args.repo_root / "results" / "performance-matrix").resolve()

    workload_so = build_llvm_workload(args.repo_root, args.build_root, args.llvm_pass)
    detail_rows, summary_rows = run_matrix(args, thread_counts, budgets, workload_so)
    detail_csv, summary_csv, html_report = write_reports(
        output_dir, detail_rows, summary_rows, thread_counts, budgets, args
    )

    print()
    print_summary_csv(summary_rows)
    print()
    print(f"wrote_detail_csv,{detail_csv}")
    print(f"wrote_summary_csv,{summary_csv}")
    print(f"wrote_html_report,{html_report}")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except Exception as exc:
        print(f"measure-performance failed: {exc}", file=sys.stderr)
        raise
