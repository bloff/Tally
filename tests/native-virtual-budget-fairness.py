#!/usr/bin/env python3
"""Run native LLVM virtual-budget calibration fairness experiments."""

import argparse
import csv
import html
import math
import statistics
import subprocess
import sys
from pathlib import Path


def parse_args():
    parser = argparse.ArgumentParser(
        description=(
            "Run native LLVM/Rust virtual-budget fairness experiments. Each "
            "run calibrates the Rust runtime, then uses random virtual budgets and "
            "reports how observed work shares compare to activation-corrected "
            "budget shares."
        )
    )
    parser.add_argument("--repo-root", type=Path, required=True)
    parser.add_argument("--llvm-binary", type=Path, required=True)
    parser.add_argument("--thread-count", type=int, default=50_000)
    parser.add_argument("--rounds", type=int, default=3)
    parser.add_argument("--min-round-seconds", type=float, default=10.0)
    parser.add_argument("--max-round-seconds", type=float, default=20.0)
    parser.add_argument("--total-virtual-budget", type=float, default=0.5)
    parser.add_argument("--seed", type=int, default=0x5EED1234)
    parser.add_argument("--calibration-seconds", type=float, default=30.0)
    parser.add_argument("--calibration-work", type=int, default=200_000)
    parser.add_argument("--llvm-stack-bytes", type=int, default=16 * 1024)
    parser.add_argument("--adaptive", action="store_true")
    parser.add_argument(
        "--budget-shapes",
        default="random-log,tiered-50-35-15",
        help=(
            "Comma-separated budget distributions. Defaults to the previous "
            "random log-uniform shape plus a tiered 50/35/15 budget split."
        ),
    )
    parser.add_argument(
        "--output-dir",
        type=Path,
        default=None,
        help="Defaults to <repo>/results/native-virtual-budget-fairness.",
    )
    parser.add_argument(
        "--fail-total-variation-above",
        type=float,
        default=None,
        help="Optional threshold for CTest-style gating.",
    )
    return parser.parse_args()


def run_command(command, *, cwd, env=None):
    completed = subprocess.run(
        command,
        cwd=cwd,
        env=env,
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        check=False,
    )
    if completed.returncode != 0:
        raise RuntimeError(
            "command failed with status "
            f"{completed.returncode}: {' '.join(str(part) for part in command)}\n"
            f"{completed.stderr}"
        )
    return completed.stdout


def run_native_hosts(args):
    common = [
        str(args.thread_count),
        str(args.rounds),
        f"{args.min_round_seconds:.9f}",
        f"{args.max_round_seconds:.9f}",
        f"{args.total_virtual_budget:.17g}",
        str(args.seed),
        f"{args.calibration_seconds:.9f}",
        str(args.calibration_work),
    ]

    runs = [
        (
            "llvm-rust",
            [
                str(args.llvm_binary),
                *common,
                str(args.llvm_stack_bytes),
                "1" if args.adaptive else "0",
                args.budget_shapes,
            ],
            args.repo_root,
            None,
        ),
    ]

    parsed = []
    for label, command, cwd, env in runs:
        print(f"running {label} native fairness experiment...", flush=True)
        output = run_command(command, cwd=cwd, env=env)
        parsed.append(parse_native_output(output))
    return parsed


def parse_native_output(output):
    lines = output.splitlines()
    metadata = {}
    header_index = None
    for index, line in enumerate(lines):
        if line.startswith("thread,") or line.startswith("scenario,"):
            header_index = index
            break
        if ":" in line:
            key, value = line.split(":", 1)
            metadata[key.strip()] = value.strip()

    if header_index is None:
        raise ValueError("native fairness output did not include thread CSV")

    rows = list(csv.DictReader(lines[header_index:]))
    for row in rows:
        row["implementation"] = metadata["implementation"]
        row["scenario"] = row.get("scenario") or metadata.get("budget_shapes", "random-log")
        row["budget_class"] = row.get("budget_class") or "unknown"
        row["thread"] = int(row["thread"])
        row["round"] = int(row["round"])
        row["virtual_budget"] = float(row["virtual_budget"])
        row["activation_corrected_budget_seconds"] = float(
            row["activation_corrected_budget_seconds"]
        )
        row["work"] = int(row["work"])
        row["activations"] = int(row["activations"])
        row["budget_units_consumed"] = int(row["budget_units_consumed"])
        row["round_seconds"] = float(row["round_seconds"])
    return metadata, rows


def summarize(parsed_outputs):
    all_rows = []
    summary_rows = []
    bucket_rows = []
    class_rows = []

    for metadata, rows in parsed_outputs:
        all_rows.extend(rows)
        implementation = metadata["implementation"]
        scenarios = sorted({row["scenario"] for row in rows})
        for scenario in scenarios:
            scenario_rows = [row for row in rows if row["scenario"] == scenario]
            rounds = sorted({row["round"] for row in scenario_rows})
            for round_index in rounds:
                round_rows = [row for row in scenario_rows if row["round"] == round_index]
                summary = summarize_round(implementation, scenario, round_index, round_rows)
                summary_rows.append(summary)
                bucket_rows.extend(summarize_buckets(summary, round_rows))
                class_rows.extend(summarize_classes(summary, round_rows))

    return all_rows, summary_rows, bucket_rows, class_rows


def summarize_round(implementation, scenario, round_index, rows):
    total_work = sum(row["work"] for row in rows)
    total_expected = sum(row["activation_corrected_budget_seconds"] for row in rows)
    total_activations = sum(row["activations"] for row in rows)
    total_budget_units = sum(row["budget_units_consumed"] for row in rows)
    round_seconds = statistics.fmean(row["round_seconds"] for row in rows)

    if total_work <= 0:
        raise ValueError(f"{implementation} round {round_index} did no work")
    if total_expected <= 0.0:
        raise ValueError(f"{implementation} round {round_index} had no positive expected budget")

    actual_shares = []
    expected_shares = []
    diffs = []
    for row in rows:
        actual_share = row["work"] / total_work
        expected_share = row["activation_corrected_budget_seconds"] / total_expected
        diff = actual_share - expected_share
        row["actual_share"] = actual_share
        row["expected_share"] = expected_share
        row["share_error"] = diff
        actual_shares.append(actual_share)
        expected_shares.append(expected_share)
        diffs.append(diff)

    total_variation = 0.5 * sum(abs(diff) for diff in diffs)
    rms_error = math.sqrt(statistics.fmean(diff * diff for diff in diffs))
    max_abs_error = max(abs(diff) for diff in diffs)
    correlation = pearson(actual_shares, expected_shares)
    weighted_relative_rmse = math.sqrt(
        sum(diff * diff for diff in diffs) / sum(share * share for share in expected_shares)
    )

    return {
        "implementation": implementation,
        "scenario": scenario,
        "round": round_index,
        "threads": len(rows),
        "round_seconds": round_seconds,
        "total_work": total_work,
        "total_expected_budget_seconds": total_expected,
        "total_activations": total_activations,
        "total_budget_units_consumed": total_budget_units,
        "total_variation": total_variation,
        "rms_share_error": rms_error,
        "max_abs_share_error": max_abs_error,
        "weighted_relative_rmse": weighted_relative_rmse,
        "correlation": correlation,
    }


def summarize_buckets(summary, rows, bucket_count=10):
    ordered = sorted(rows, key=lambda row: row["expected_share"])
    total_work = sum(row["work"] for row in rows)
    total_expected = sum(row["activation_corrected_budget_seconds"] for row in rows)
    buckets = []
    for bucket in range(bucket_count):
        start = (len(ordered) * bucket) // bucket_count
        end = (len(ordered) * (bucket + 1)) // bucket_count
        bucket_rows = ordered[start:end]
        if not bucket_rows:
            continue
        actual_share = sum(row["work"] for row in bucket_rows) / total_work
        expected_share = (
            sum(row["activation_corrected_budget_seconds"] for row in bucket_rows)
            / total_expected
        )
        buckets.append(
            {
                "implementation": summary["implementation"],
                "scenario": summary["scenario"],
                "round": summary["round"],
                "bucket": bucket,
                "threads": len(bucket_rows),
                "min_virtual_budget": min(row["virtual_budget"] for row in bucket_rows),
                "max_virtual_budget": max(row["virtual_budget"] for row in bucket_rows),
                "actual_share": actual_share,
                "expected_share": expected_share,
                "actual_over_expected": actual_share / expected_share
                if expected_share > 0
                else float("nan"),
            }
        )
    return buckets


def summarize_classes(summary, rows):
    total_work = sum(row["work"] for row in rows)
    total_expected = sum(row["activation_corrected_budget_seconds"] for row in rows)
    class_rows = []
    for budget_class in sorted({row["budget_class"] for row in rows}, key=budget_class_sort_key):
        members = [row for row in rows if row["budget_class"] == budget_class]
        actual_share = sum(row["work"] for row in members) / total_work
        expected_share = (
            sum(row["activation_corrected_budget_seconds"] for row in members) / total_expected
        )
        class_rows.append(
            {
                "implementation": summary["implementation"],
                "scenario": summary["scenario"],
                "round": summary["round"],
                "budget_class": budget_class,
                "threads": len(members),
                "min_virtual_budget": min(row["virtual_budget"] for row in members),
                "mean_virtual_budget": statistics.fmean(row["virtual_budget"] for row in members),
                "max_virtual_budget": max(row["virtual_budget"] for row in members),
                "actual_share": actual_share,
                "expected_share": expected_share,
                "actual_over_expected": actual_share / expected_share
                if expected_share > 0
                else float("nan"),
            }
        )
    return class_rows


def budget_class_sort_key(budget_class):
    order = {"random": 0, "small": 1, "medium": 2, "large": 3}
    return (order.get(budget_class, 99), budget_class)


def pearson(xs, ys):
    mean_x = statistics.fmean(xs)
    mean_y = statistics.fmean(ys)
    numerator = sum((x - mean_x) * (y - mean_y) for x, y in zip(xs, ys))
    denom_x = sum((x - mean_x) ** 2 for x in xs)
    denom_y = sum((y - mean_y) ** 2 for y in ys)
    denominator = math.sqrt(denom_x * denom_y)
    return numerator / denominator if denominator > 0.0 else 0.0


def write_outputs(output_dir, all_rows, summary_rows, bucket_rows, class_rows, args):
    output_dir.mkdir(parents=True, exist_ok=True)
    detail_csv = output_dir / "native-virtual-budget-fairness-detail.csv"
    summary_csv = output_dir / "native-virtual-budget-fairness-summary.csv"
    bucket_csv = output_dir / "native-virtual-budget-fairness-buckets.csv"
    class_csv = output_dir / "native-virtual-budget-fairness-classes.csv"
    html_report = output_dir / "native-virtual-budget-fairness-report.html"

    write_csv(
        detail_csv,
        all_rows,
        [
            "implementation",
            "scenario",
            "round",
            "thread",
            "budget_class",
            "virtual_budget",
            "activation_corrected_budget_seconds",
            "work",
            "activations",
            "budget_units_consumed",
            "round_seconds",
            "actual_share",
            "expected_share",
            "share_error",
        ],
    )
    write_csv(
        summary_csv,
        summary_rows,
        [
            "implementation",
            "scenario",
            "round",
            "threads",
            "round_seconds",
            "total_work",
            "total_expected_budget_seconds",
            "total_activations",
            "total_budget_units_consumed",
            "total_variation",
            "rms_share_error",
            "max_abs_share_error",
            "weighted_relative_rmse",
            "correlation",
        ],
    )
    write_csv(
        bucket_csv,
        bucket_rows,
        [
            "implementation",
            "scenario",
            "round",
            "bucket",
            "threads",
            "min_virtual_budget",
            "max_virtual_budget",
            "actual_share",
            "expected_share",
            "actual_over_expected",
        ],
    )
    write_csv(
        class_csv,
        class_rows,
        [
            "implementation",
            "scenario",
            "round",
            "budget_class",
            "threads",
            "min_virtual_budget",
            "mean_virtual_budget",
            "max_virtual_budget",
            "actual_share",
            "expected_share",
            "actual_over_expected",
        ],
    )
    write_html(html_report, summary_rows, bucket_rows, class_rows, args)
    return detail_csv, summary_csv, bucket_csv, class_csv, html_report


def write_csv(path, rows, fieldnames):
    with path.open("w", newline="") as handle:
        writer = csv.DictWriter(handle, fieldnames=fieldnames, extrasaction="ignore")
        writer.writeheader()
        writer.writerows(rows)


def write_html(path, summary_rows, bucket_rows, class_rows, args):
    rows_html = "\n".join(
        f"<tr><td>{html.escape(row['implementation'])}</td>"
        f"<td>{html.escape(row['scenario'])}</td>"
        f"<td>{row['round']}</td>"
        f"<td>{row['round_seconds']:.3f}</td>"
        f"<td>{row['total_work']:,}</td>"
        f"<td>{row['total_variation']:.4f}</td>"
        f"<td>{row['weighted_relative_rmse']:.4f}</td>"
        f"<td>{row['correlation']:.5f}</td></tr>"
        for row in summary_rows
    )
    bucket_sections = "\n".join(
        bucket_table(implementation, scenario, round_index, bucket_rows)
        for implementation, scenario, round_index in sorted(
            {(row["implementation"], row["scenario"], row["round"]) for row in bucket_rows}
        )
    )
    class_sections = "\n".join(
        class_table(implementation, scenario, round_index, class_rows)
        for implementation, scenario, round_index in sorted(
            {(row["implementation"], row["scenario"], row["round"]) for row in class_rows}
        )
    )
    path.write_text(
        f"""<!doctype html>
<html lang="en">
<head>
<meta charset="utf-8">
<title>Tally Native Virtual-Budget Fairness</title>
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
p {{ color: var(--muted); max-width: 1000px; }}
section {{
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
  font-variant-numeric: tabular-nums;
}}
th {{ background: #eef2f8; color: #26344f; }}
td:first-child, th:first-child {{ text-align: left; }}
.metric {{
  display: grid;
  grid-template-columns: repeat(auto-fit, minmax(220px, 1fr));
  gap: 12px;
}}
.metric div {{
  border: 1px solid var(--line);
  border-radius: 8px;
  padding: 12px;
}}
.metric span {{
  display: block;
  color: var(--muted);
  font-size: 12px;
  text-transform: uppercase;
  letter-spacing: 0.04em;
}}
</style>
</head>
<body>
<h1>Tally Native Virtual-Budget Fairness</h1>
<p>
The Rust runtime calibrated itself natively, then ran {args.thread_count:,}
minithreads with virtual budgets summing to {args.total_virtual_budget:g}.
Expected shares use activation-corrected virtual budget seconds:
<code>B_i = max(0, v_i * round_seconds - activations_i * activation_cost)</code>.
The default tiered scenario assigns 50% of the total virtual budget to many
small threads, 35% to medium threads, and 15% to a few large threads.
</p>
<section>
<h2>Run Shape</h2>
<div class="metric">
  {metric("Rounds", str(args.rounds))}
  {metric("Round seconds", f"{args.min_round_seconds:g} to {args.max_round_seconds:g}")}
  {metric("Calibration seconds", f"{args.calibration_seconds:g}")}
  {metric("Budget shapes", args.budget_shapes)}
  {metric("Adaptive", "yes" if args.adaptive else "no")}
</div>
</section>
<section>
<h2>Summary</h2>
<table>
<thead><tr><th>Implementation</th><th>Scenario</th><th>Round</th><th>Seconds</th><th>Total work</th><th>Total variation</th><th>Relative RMSE</th><th>Correlation</th></tr></thead>
<tbody>
{rows_html}
</tbody>
</table>
</section>
{class_sections}
{bucket_sections}
</body>
</html>
"""
    )


def metric(label, value):
    return f"<div><span>{html.escape(label)}</span><strong>{html.escape(value)}</strong></div>"


def class_table(implementation, scenario, round_index, class_rows):
    rows = [
        row
        for row in class_rows
        if row["implementation"] == implementation
        and row["scenario"] == scenario
        and row["round"] == round_index
    ]
    body = "\n".join(
        f"<tr><td>{html.escape(row['budget_class'])}</td>"
        f"<td>{row['threads']}</td>"
        f"<td>{row['mean_virtual_budget']:.3e}</td>"
        f"<td>{row['expected_share']:.5f}</td>"
        f"<td>{row['actual_share']:.5f}</td>"
        f"<td>{row['actual_over_expected']:.4f}</td></tr>"
        for row in rows
    )
    return f"""<section>
<h2>{html.escape(implementation)} {html.escape(scenario)} Round {round_index} Classes</h2>
<table>
<thead><tr><th>Class</th><th>Threads</th><th>Mean budget</th><th>Expected share</th><th>Actual share</th><th>Actual / expected</th></tr></thead>
<tbody>
{body}
</tbody>
</table>
</section>"""


def bucket_table(implementation, scenario, round_index, bucket_rows):
    rows = [
        row
        for row in bucket_rows
        if row["implementation"] == implementation
        and row["scenario"] == scenario
        and row["round"] == round_index
    ]
    body = "\n".join(
        f"<tr><td>{row['bucket']}</td>"
        f"<td>{row['threads']}</td>"
        f"<td>{row['min_virtual_budget']:.3e}</td>"
        f"<td>{row['max_virtual_budget']:.3e}</td>"
        f"<td>{row['expected_share']:.5f}</td>"
        f"<td>{row['actual_share']:.5f}</td>"
        f"<td>{row['actual_over_expected']:.4f}</td></tr>"
        for row in rows
    )
    return f"""<section>
<h2>{html.escape(implementation)} {html.escape(scenario)} Round {round_index} Budget Deciles</h2>
<table>
<thead><tr><th>Bucket</th><th>Threads</th><th>Min budget</th><th>Max budget</th><th>Expected share</th><th>Actual share</th><th>Actual / expected</th></tr></thead>
<tbody>
{body}
</tbody>
</table>
</section>"""


def print_summary(rows):
    writer = csv.DictWriter(
        sys.stdout,
        fieldnames=[
            "implementation",
            "scenario",
            "round",
            "threads",
            "round_seconds",
            "total_work",
            "total_variation",
            "weighted_relative_rmse",
            "correlation",
        ],
        extrasaction="ignore",
    )
    writer.writeheader()
    writer.writerows(rows)


def main():
    args = parse_args()
    args.repo_root = args.repo_root.resolve()
    args.llvm_binary = args.llvm_binary.resolve()
    output_dir = (
        args.output_dir
        or args.repo_root / "results" / "native-virtual-budget-fairness"
    ).resolve()

    parsed = run_native_hosts(args)
    all_rows, summary_rows, bucket_rows, class_rows = summarize(parsed)
    detail_csv, summary_csv, bucket_csv, class_csv, html_report = write_outputs(
        output_dir, all_rows, summary_rows, bucket_rows, class_rows, args
    )

    print()
    print_summary(summary_rows)
    print()
    print(f"wrote_detail_csv,{detail_csv}")
    print(f"wrote_summary_csv,{summary_csv}")
    print(f"wrote_bucket_csv,{bucket_csv}")
    print(f"wrote_class_csv,{class_csv}")
    print(f"wrote_html_report,{html_report}")

    if args.fail_total_variation_above is not None:
        worst = max(row["total_variation"] for row in summary_rows)
        if worst > args.fail_total_variation_above:
            raise SystemExit(
                f"total variation {worst:.6f} exceeded threshold "
                f"{args.fail_total_variation_above:.6f}"
            )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
