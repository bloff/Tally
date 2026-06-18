#!/usr/bin/env python3
"""Run the budget-walk minithread experiment and plot its scaling curve."""

import argparse
import csv
import os
import subprocess
import sys
from pathlib import Path

os.environ.setdefault("MPLCONFIGDIR", "/tmp/tally-matplotlib")

import matplotlib.pyplot as plt


REPO_ROOT = Path(__file__).resolve().parents[1]


def parse_args():
    parser = argparse.ArgumentParser(
        description="Run bin/budget_walk and plot measured work against ideal linear scaling."
    )
    parser.add_argument("--metacycles", type=int, default=10000)
    parser.add_argument("--base-budget", type=int, default=100)
    parser.add_argument("--budget-step", type=int, default=100)
    parser.add_argument("--output-dir", type=Path, default=REPO_ROOT / "results")
    parser.add_argument("--tag", default="", help="append a suffix to the generated CSV and PNG filenames")
    parser.add_argument("--binary", type=Path, default=REPO_ROOT / "bin" / "budget_walk")
    parser.add_argument("--build", action="store_true", help="build the project before running")
    parser.add_argument("--build-dir", type=Path, default=Path("/tmp/tally-build-current"))
    parser.add_argument("--show", action="store_true", help="open an interactive matplotlib window")
    return parser.parse_args()


def build_project(build_dir):
    subprocess.run(["cmake", "-S", str(REPO_ROOT), "-B", str(build_dir)], check=True)
    subprocess.run(["cmake", "--build", str(build_dir), "-j", str(os.cpu_count() or 1)], check=True)


def run_experiment(binary, metacycles, base_budget, budget_step):
    if not binary.exists():
        raise FileNotFoundError(f"{binary} does not exist; run with --build first")

    completed = subprocess.run(
        [str(binary), str(metacycles), str(base_budget), str(budget_step)],
        cwd=REPO_ROOT,
        check=True,
        text=True,
        stdout=subprocess.PIPE,
    )

    lines = completed.stdout.splitlines()
    try:
        header_index = next(i for i, line in enumerate(lines) if line.startswith("thread,"))
    except StopIteration as exc:
        raise ValueError("budget_walk output did not contain a CSV header") from exc

    return list(csv.DictReader(lines[header_index:]))


def write_csv(rows, csv_path):
    csv_path.parent.mkdir(parents=True, exist_ok=True)
    with csv_path.open("w", newline="") as csv_file:
        writer = csv.DictWriter(csv_file, fieldnames=rows[0].keys())
        writer.writeheader()
        writer.writerows(rows)


def plot(rows, png_path):
    budgets = [int(row["budget_per_metacycle"]) for row in rows]
    vertices = [int(row["vertices_walked"]) for row in rows]
    linearity = [float(row["linearity_ratio"]) for row in rows]

    baseline_budget = budgets[0]
    baseline_vertices = vertices[0]
    ideal = [baseline_vertices * budget / baseline_budget for budget in budgets]

    plt.style.use("seaborn-v0_8-whitegrid")
    fig, (ax1, ax2) = plt.subplots(
        2,
        1,
        figsize=(9, 7),
        height_ratios=[2.2, 1],
        sharex=True,
        constrained_layout=True,
    )
    fig.suptitle("Budget vs. Random-Walk Work Across 10 Minithreads", fontsize=15, weight="bold")

    ax1.plot(
        budgets,
        vertices,
        marker="o",
        linewidth=2.5,
        color="#1f77b4",
        label="Measured vertices walked",
    )
    ax1.plot(
        budgets,
        ideal,
        linestyle="--",
        linewidth=2,
        color="#555555",
        label="Ideal linear scaling from thread 0",
    )
    ax1.set_ylabel("Vertices walked")
    ax1.legend(loc="upper left")
    ax1.annotate(
        f"{vertices[-1]:,} vertices at budget {budgets[-1]:,}",
        xy=(budgets[-1], vertices[-1]),
        xytext=(-150, -26),
        textcoords="offset points",
        arrowprops={"arrowstyle": "->", "color": "#555555"},
        fontsize=9,
    )

    ax2.axhline(1.0, linestyle="--", linewidth=1.5, color="#777777")
    ax2.plot(budgets, linearity, marker="s", linewidth=2.2, color="#d62728")
    ax2.set_xlabel("Budget per metacycle")
    ax2.set_ylabel("Linearity ratio")
    ax2.set_ylim(0.9, 1.02)
    ax2.text(
        budgets[0],
        0.905,
        "1.0 means work increased exactly in proportion to budget",
        fontsize=9,
        color="#444444",
    )

    for ax in (ax1, ax2):
        ax.spines["top"].set_visible(False)
        ax.spines["right"].set_visible(False)

    png_path.parent.mkdir(parents=True, exist_ok=True)
    fig.savefig(png_path, dpi=180)
    return fig


def main():
    args = parse_args()
    if args.build:
        build_project(args.build_dir)

    rows = run_experiment(args.binary, args.metacycles, args.base_budget, args.budget_step)
    if not rows:
        print("budget_walk produced no CSV rows", file=sys.stderr)
        return 1

    args.output_dir.mkdir(parents=True, exist_ok=True)
    suffix = f"_{args.tag}" if args.tag else ""
    csv_path = args.output_dir / f"budget_walk_scaling{suffix}.csv"
    png_path = args.output_dir / f"budget_walk_scaling{suffix}.png"

    write_csv(rows, csv_path)
    fig = plot(rows, png_path)

    top = rows[-1]
    print(f"Wrote {csv_path}")
    print(f"Wrote {png_path}")
    print(
        "Top budget result: "
        f"{top['vertices_walked']} vertices walked at budget {top['budget_per_metacycle']} "
        f"(linearity ratio {float(top['linearity_ratio']):.3f})"
    )

    if args.show:
        plt.show()
    else:
        plt.close(fig)

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
