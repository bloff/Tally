#!/usr/bin/env python3
"""Calibrate virtual-budget constants for GCC Tally and LLVM Tally."""

import argparse
import csv
import datetime as dt
import html
import json
import math
import os
import statistics
import subprocess
import sys
from pathlib import Path


DEFAULT_THREAD_COUNTS = "1,2,5,10,20,50"
DEFAULT_BUDGETS = "20,50,100,200,500,1000,5000"
DEFAULT_TARGET_EDGES = 5_000_000


def parse_args():
    parser = argparse.ArgumentParser(
        description=(
            "Run self-contained random-walk measurements and fit a linear "
            "virtual-budget calibration for both Tally runtimes."
        )
    )
    parser.add_argument("--repo-root", type=Path, required=True)
    parser.add_argument("--build-root", type=Path, required=True)
    parser.add_argument("--gcc-binary", type=Path, required=True)
    parser.add_argument("--llvm-host", type=Path, required=True)
    parser.add_argument("--llvm-pass", type=Path, required=True)
    parser.add_argument("--thread-counts", default=DEFAULT_THREAD_COUNTS)
    parser.add_argument("--budgets", default=DEFAULT_BUDGETS)
    parser.add_argument("--target-edges", type=int, default=DEFAULT_TARGET_EDGES)
    parser.add_argument(
        "--target-edge-counts",
        default=None,
        help=(
            "Comma/range list of total edge counts to include in one fit. "
            "When omitted, --target-edges is used as a single target."
        ),
    )
    parser.add_argument("--repetitions", type=int, default=3)
    parser.add_argument("--gcc-stack-words", type=int, default=1024)
    parser.add_argument("--llvm-stack-bytes", type=int, default=64 * 1024)
    parser.add_argument(
        "--output-dir",
        type=Path,
        default=None,
        help="Defaults to <repo>/results/virtual-budget-calibration.",
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


def run_command(command, *, cwd, env=None):
    completed = subprocess.run(
        command,
        cwd=cwd,
        env=env,
        check=False,
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
    )
    if completed.returncode != 0:
        raise RuntimeError(
            "command failed with status "
            f"{completed.returncode}: {' '.join(str(part) for part in command)}\n"
            f"{completed.stderr}"
        )
    return completed.stdout


def parse_metadata(output):
    metadata = {}
    for line in output.splitlines():
        if not line.strip():
            if metadata:
                break
            continue
        if ":" not in line:
            continue
        key, value = line.split(":", 1)
        metadata[key.strip()] = value.strip()
    return metadata


def metadata_number(metadata, key, cast):
    try:
        return cast(metadata[key])
    except KeyError as exc:
        raise ValueError(f"benchmark output missing metadata field {key}") from exc


def build_llvm_workload(repo_root, build_root, llvm_pass):
    script = repo_root / "llvm-tally" / "scripts" / "build-rust-workload.sh"
    output = run_command(
        [str(script), "examples/self-walk", str(build_root / "llvm-tally"), str(llvm_pass)],
        cwd=repo_root,
    )
    return Path(output.strip().splitlines()[-1])


def warm_gcc_workload(args):
    env = os.environ.copy()
    run_command(
        [str(args.gcc_binary), "1", "10", "1", str(args.gcc_stack_words), "1"],
        cwd=args.repo_root / "gcc-tally",
        env=env,
    )


def measure(command, *, cwd, env, repetitions):
    samples = []
    for _ in range(repetitions):
        output = run_command(command, cwd=cwd, env=env)
        metadata = parse_metadata(output)
        run_seconds = metadata_number(metadata, "run_seconds", float)
        total_edges = metadata_number(metadata, "total_edges", int)
        scheduler_cycles = metadata_number(metadata, "scheduler_cycles", int)
        thread_cycles = metadata_number(metadata, "thread_cycles", int)
        budget_per_cycle = metadata_number(metadata, "budget_per_cycle", int)
        budget_units_consumed = int(
            metadata.get("budget_units_consumed", thread_cycles * budget_per_cycle)
        )
        samples.append(
            {
                "run_seconds": run_seconds,
                "total_edges": total_edges,
                "scheduler_cycles": scheduler_cycles,
                "thread_cycles": thread_cycles,
                "budget_units_consumed": budget_units_consumed,
                "edges_per_second": total_edges / run_seconds,
            }
        )
    return {
        key: statistics.fmean(sample[key] for sample in samples)
        for key in samples[0].keys()
    } | {
        "stdev_seconds": statistics.stdev(sample["run_seconds"] for sample in samples)
        if len(samples) > 1
        else 0.0
    }


def run_measurements(args, thread_counts, budgets, target_edge_counts, workload_so):
    gcc_env = os.environ.copy()
    gcc_env["TALLY_ASSUME_COMPILED"] = "1"
    rows = []

    for target_edges in target_edge_counts:
        for thread_count in thread_counts:
            for budget in budgets:
                rows.extend(
                    run_one_combination(
                        args,
                        thread_count,
                        budget,
                        target_edges,
                        workload_so,
                        gcc_env,
                    )
                )

    return rows


def run_one_combination(args, thread_count, budget, target_edges, workload_so, gcc_env):
    rows = []
    commands = [
        (
            "gcc-c",
            [
                str(args.gcc_binary),
                str(thread_count),
                str(budget),
                str(target_edges),
                str(args.gcc_stack_words),
                "1",
            ],
            args.repo_root / "gcc-tally",
            gcc_env,
        ),
        (
            "llvm-rust",
            [
                str(args.llvm_host),
                str(workload_so),
                str(thread_count),
                str(budget),
                str(target_edges),
                str(args.llvm_stack_bytes),
                "1",
            ],
            args.repo_root,
            None,
        ),
    ]

    for implementation, command, cwd, env in commands:
        measurement = measure(command, cwd=cwd, env=env, repetitions=args.repetitions)
        budget_units_granted = measurement["thread_cycles"] * budget
        row = {
            "implementation": implementation,
            "target_edges_requested": target_edges,
            "threads": thread_count,
            "budget": budget,
            "budget_units_granted": budget_units_granted,
            **measurement,
            "repetitions": args.repetitions,
        }
        rows.append(row)
        print(
            f"{implementation:9s} edges={target_edges:9d} "
            f"k={thread_count:4d} b={budget:5d} "
            f"time={measurement['run_seconds']:.6f}s "
            f"throughput={measurement['edges_per_second'] / 1_000_000:8.2f}M edges/s",
            flush=True,
        )

    return rows


def fit_calibration(rows):
    by_implementation = {}
    for row in rows:
        by_implementation.setdefault(row["implementation"], []).append(row)

    return {
        implementation: fit_one_implementation(implementation, impl_rows)
        for implementation, impl_rows in sorted(by_implementation.items())
    }


def fit_one_implementation(implementation, rows):
    feature_rows = [
        [
            float(row["budget_units_consumed"]),
            float(row["thread_cycles"]),
            float(row["scheduler_cycles"]),
            1.0,
        ]
        for row in rows
    ]
    y_values = [float(row["run_seconds"]) for row in rows]
    raw_coefficients = least_squares(feature_rows, y_values)
    names = [
        "seconds_per_budget_unit",
        "seconds_per_activation",
        "seconds_per_scheduler_round",
        "intercept_seconds",
    ]
    raw = dict(zip(names, raw_coefficients))

    calibration = {
        "seconds_per_budget_unit": max(raw["seconds_per_budget_unit"], 1.0e-12),
        "seconds_per_activation": max(raw["seconds_per_activation"], 0.0),
        "seconds_per_scheduler_round": max(raw["seconds_per_scheduler_round"], 0.0),
        "min_internal_budget": 1,
        "max_internal_budget": 2**62 - 1,
    }
    calibration["context_switch_budget_units"] = (
        calibration["seconds_per_activation"] / calibration["seconds_per_budget_unit"]
    )
    calibration["scheduler_round_budget_units"] = (
        calibration["seconds_per_scheduler_round"] / calibration["seconds_per_budget_unit"]
    )

    predictions = [dot(row, raw_coefficients) for row in feature_rows]
    residuals = [observed - predicted for observed, predicted in zip(y_values, predictions)]
    rmse = math.sqrt(statistics.fmean(residual * residual for residual in residuals))
    mean_y = statistics.fmean(y_values)
    ss_total = sum((value - mean_y) ** 2 for value in y_values)
    ss_residual = sum(residual * residual for residual in residuals)
    r_squared = 1.0 - (ss_residual / ss_total) if ss_total > 0.0 else 1.0

    return {
        "implementation": implementation,
        "calibration": calibration,
        "raw_fit": raw,
        "fit_rmse_seconds": rmse,
        "fit_rmse_percent_of_mean": (rmse / mean_y) * 100.0 if mean_y > 0.0 else 0.0,
        "fit_r_squared": r_squared,
        "sample_count": len(rows),
    }


def least_squares(feature_rows, y_values):
    if not feature_rows:
        raise ValueError("cannot fit an empty calibration")

    width = len(feature_rows[0])
    scales = []
    for column in range(width):
        scale = math.sqrt(sum(row[column] * row[column] for row in feature_rows))
        scales.append(scale if scale > 0.0 else 1.0)

    matrix = [[0.0 for _ in range(width)] for _ in range(width)]
    vector = [0.0 for _ in range(width)]
    for features, y_value in zip(feature_rows, y_values):
        scaled = [features[i] / scales[i] for i in range(width)]
        for i in range(width):
            vector[i] += scaled[i] * y_value
            for j in range(width):
                matrix[i][j] += scaled[i] * scaled[j]

    ridge = 1.0e-10
    for i in range(width):
        matrix[i][i] += ridge

    scaled_solution = solve_linear_system(matrix, vector)
    return [scaled_solution[i] / scales[i] for i in range(width)]


def solve_linear_system(matrix, vector):
    n = len(vector)
    augmented = [row[:] + [vector[i]] for i, row in enumerate(matrix)]

    for column in range(n):
        pivot = max(range(column, n), key=lambda row: abs(augmented[row][column]))
        if abs(augmented[pivot][column]) < 1.0e-18:
            raise ValueError("calibration fit is singular; use more thread/budget points")
        augmented[column], augmented[pivot] = augmented[pivot], augmented[column]

        pivot_value = augmented[column][column]
        for j in range(column, n + 1):
            augmented[column][j] /= pivot_value

        for row in range(n):
            if row == column:
                continue
            factor = augmented[row][column]
            if factor == 0.0:
                continue
            for j in range(column, n + 1):
                augmented[row][j] -= factor * augmented[column][j]

    return [augmented[row][n] for row in range(n)]


def dot(left, right):
    return sum(a * b for a, b in zip(left, right))


def write_outputs(output_dir, rows, calibrations, args, thread_counts, budgets, target_edge_counts):
    output_dir.mkdir(parents=True, exist_ok=True)
    detail_csv = output_dir / "virtual-budget-calibration-detail.csv"
    json_path = output_dir / "virtual-budget-calibration.json"
    html_path = output_dir / "virtual-budget-calibration-report.html"

    write_csv(
        detail_csv,
        rows,
        [
            "implementation",
            "target_edges_requested",
            "threads",
            "budget",
            "run_seconds",
            "stdev_seconds",
            "total_edges",
            "edges_per_second",
            "scheduler_cycles",
            "thread_cycles",
            "budget_units_granted",
            "budget_units_consumed",
            "repetitions",
        ],
    )

    payload = {
        "generated_at": dt.datetime.now(dt.timezone.utc).isoformat(),
        "model": (
            "run_seconds ~= seconds_per_budget_unit * budget_units_consumed "
            "+ seconds_per_activation * thread_cycles "
            "+ seconds_per_scheduler_round * scheduler_cycles + intercept_seconds"
        ),
        "target_edge_counts": target_edge_counts,
        "thread_counts": thread_counts,
        "budgets": budgets,
        "repetitions": args.repetitions,
        "calibrations": calibrations,
    }
    json_path.write_text(json.dumps(payload, indent=2, sort_keys=True) + "\n")
    write_html_report(html_path, rows, calibrations, args, thread_counts, budgets, target_edge_counts)
    return detail_csv, json_path, html_path


def write_csv(path, rows, fieldnames):
    with path.open("w", newline="") as handle:
        writer = csv.DictWriter(handle, fieldnames=fieldnames)
        writer.writeheader()
        writer.writerows(rows)


def write_html_report(path, rows, calibrations, args, thread_counts, budgets, target_edge_counts):
    by_impl = {}
    for row in rows:
        by_impl.setdefault(row["implementation"], []).append(row)

    sections = []
    for implementation, impl_rows in sorted(by_impl.items()):
        fit = calibrations[implementation]
        sections.append(
            f"""
<section>
<h2>{html.escape(implementation)}</h2>
<div class="summary">
  {summary_card("Seconds / budget unit", f"{fit['calibration']['seconds_per_budget_unit']:.3e}")}
  {summary_card("Activation", f"{fit['calibration']['seconds_per_activation'] * 1_000_000:.3f} us")}
  {summary_card("Context units", f"{fit['calibration']['context_switch_budget_units']:.1f}")}
  {summary_card("Scheduler round", f"{fit['calibration']['seconds_per_scheduler_round'] * 1_000_000:.3f} us")}
  {summary_card("Fit RMSE", f"{fit['fit_rmse_seconds'] * 1000:.3f} ms")}
  {summary_card("Fit R^2", f"{fit['fit_r_squared']:.4f}")}
</div>
<h3>Throughput (M edges/s)</h3>
{throughput_table(impl_rows, thread_counts, budgets)}
</section>
"""
        )

    path.write_text(
        f"""<!doctype html>
<html lang="en">
<head>
<meta charset="utf-8">
<title>Tally Virtual Budget Calibration</title>
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
h1, h2, h3 {{ margin: 0 0 12px; }}
p {{ color: var(--muted); max-width: 980px; }}
section {{
  background: var(--panel);
  border: 1px solid var(--line);
  border-radius: 8px;
  padding: 18px;
  margin: 18px 0;
  overflow-x: auto;
}}
.summary {{
  display: grid;
  grid-template-columns: repeat(auto-fit, minmax(190px, 1fr));
  gap: 12px;
  margin: 14px 0 22px;
}}
.metric {{
  border: 1px solid var(--line);
  border-radius: 8px;
  padding: 12px 14px;
}}
.metric span {{
  display: block;
  color: var(--muted);
  font-size: 12px;
  text-transform: uppercase;
  letter-spacing: 0.04em;
  margin-bottom: 6px;
}}
.metric strong {{ font-size: 20px; }}
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
  font-variant-numeric: tabular-nums;
}}
th {{ background: #eef2f8; color: #26344f; }}
th:first-child, td:first-child {{
  position: sticky;
  left: 0;
  background: #f8fafd;
  font-weight: 650;
}}
code {{ font-family: ui-monospace, SFMono-Regular, Menlo, Consolas, monospace; }}
</style>
</head>
<body>
<h1>Tally Virtual Budget Calibration</h1>
<p>
This report fits a first-order model from the self-contained random-walk
benchmark. The generated JSON constants can be passed to the C and Rust
virtual-budget APIs to translate a virtual CPU share into implementation-local
budget units.
</p>
<section>
<h2>Run Shape</h2>
<div class="summary">
  {summary_card("Target edges", ", ".join(f"{value:,}" for value in target_edge_counts))}
  {summary_card("Thread counts", ", ".join(str(value) for value in thread_counts))}
  {summary_card("Budgets", ", ".join(str(value) for value in budgets))}
  {summary_card("Repetitions", str(args.repetitions))}
</div>
<p><code>run_seconds ~= unit_cost * budget_units_consumed + activation_cost * thread_cycles + scheduler_round_cost * scheduler_cycles + intercept</code></p>
</section>
{''.join(sections)}
</body>
</html>
"""
    )


def summary_card(label, value):
    return f'<div class="metric"><span>{html.escape(label)}</span><strong>{html.escape(value)}</strong></div>'


def throughput_table(rows, thread_counts, budgets):
    by_combo = {(int(row["threads"]), int(row["budget"])): row for row in rows}
    max_value = max(row["edges_per_second"] for row in rows)
    output = ['<table><thead><tr><th>k \\ b</th>']
    output.extend(f"<th>{budget}</th>" for budget in budgets)
    output.append("</tr></thead><tbody>")
    for thread_count in thread_counts:
        output.append(f"<tr><td>{thread_count}</td>")
        for budget in budgets:
            row = by_combo[(thread_count, budget)]
            value = row["edges_per_second"] / 1_000_000
            color = throughput_color(row["edges_per_second"], max_value)
            tooltip = (
                f"k={thread_count}, b={budget}; "
                f"time={row['run_seconds']:.6f}s; "
                f"thread_cycles={row['thread_cycles']:.0f}; "
                f"scheduler_cycles={row['scheduler_cycles']:.0f}"
            )
            output.append(
                f'<td title="{html.escape(tooltip)}" style="background:{color}">{value:.2f}</td>'
            )
        output.append("</tr>")
    output.append("</tbody></table>")
    return "".join(output)


def throughput_color(value, max_value):
    scale = 0.0 if max_value <= 0.0 else min(1.0, max(0.0, value / max_value))
    scale = math.sqrt(scale)
    return interpolate_hex("#ffffff", "#0f766e", scale)


def interpolate_hex(low, high, scale):
    low_rgb = tuple(int(low[i : i + 2], 16) for i in (1, 3, 5))
    high_rgb = tuple(int(high[i : i + 2], 16) for i in (1, 3, 5))
    mixed = tuple(round(a + (b - a) * scale) for a, b in zip(low_rgb, high_rgb))
    return f"#{mixed[0]:02x}{mixed[1]:02x}{mixed[2]:02x}"


def main():
    args = parse_args()
    if args.target_edges <= 0:
        raise ValueError("--target-edges must be positive")
    if args.repetitions <= 0:
        raise ValueError("--repetitions must be positive")

    args.repo_root = args.repo_root.resolve()
    args.build_root = args.build_root.resolve()
    args.gcc_binary = args.gcc_binary.resolve()
    args.llvm_host = args.llvm_host.resolve()
    args.llvm_pass = args.llvm_pass.resolve()

    thread_counts = parse_number_list(args.thread_counts)
    budgets = parse_number_list(args.budgets)
    target_edge_counts = (
        parse_number_list(args.target_edge_counts)
        if args.target_edge_counts
        else [args.target_edges]
    )
    output_dir = (
        args.output_dir or args.repo_root / "results" / "virtual-budget-calibration"
    ).resolve()

    workload_so = build_llvm_workload(args.repo_root, args.build_root, args.llvm_pass)
    warm_gcc_workload(args)
    rows = run_measurements(args, thread_counts, budgets, target_edge_counts, workload_so)
    calibrations = fit_calibration(rows)
    detail_csv, json_path, html_path = write_outputs(
        output_dir, rows, calibrations, args, thread_counts, budgets, target_edge_counts
    )

    print()
    for implementation, fit in calibrations.items():
        calibration = fit["calibration"]
        print(
            f"{implementation}: "
            f"seconds_per_budget_unit={calibration['seconds_per_budget_unit']:.6e}, "
            f"activation_us={calibration['seconds_per_activation'] * 1_000_000:.3f}, "
            f"context_units={calibration['context_switch_budget_units']:.2f}, "
            f"scheduler_round_us={calibration['seconds_per_scheduler_round'] * 1_000_000:.3f}, "
            f"rmse_ms={fit['fit_rmse_seconds'] * 1000:.3f}"
        )
    print()
    print(f"wrote_detail_csv,{detail_csv}")
    print(f"wrote_calibration_json,{json_path}")
    print(f"wrote_html_report,{html_path}")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except Exception as exc:
        print(f"calibrate-virtual-budget failed: {exc}", file=sys.stderr)
        raise
