#!/usr/bin/env python3
"""Summarize DS4 EvalPlus metadata by mode."""

from __future__ import annotations

import argparse
import json
import statistics
import sys
from pathlib import Path
from typing import Any


def load_rows(paths: list[Path]) -> list[dict[str, Any]]:
    rows: list[dict[str, Any]] = []
    for path in paths:
        with path.open("r", encoding="utf-8") as fp:
            for lineno, line in enumerate(fp, 1):
                if not line.strip():
                    continue
                try:
                    row = json.loads(line)
                except json.JSONDecodeError as exc:
                    raise SystemExit(f"{path}:{lineno}: invalid JSON: {exc}") from exc
                row.setdefault("mode", path.stem.replace(".meta", ""))
                rows.append(row)
    return rows


def summarize(rows: list[dict[str, Any]]) -> list[dict[str, Any]]:
    by_mode: dict[str, list[dict[str, Any]]] = {}
    for row in rows:
        by_mode.setdefault(str(row.get("mode") or "unspecified"), []).append(row)

    summaries: list[dict[str, Any]] = []
    for mode, mode_rows in sorted(by_mode.items()):
        elapsed = [
            float(row["elapsed_sec"]) for row in mode_rows if row.get("elapsed_sec") is not None
        ]
        tokens = [
            int(row["completion_tokens"])
            for row in mode_rows
            if row.get("completion_tokens") is not None
        ]
        tps = [
            float(row["completion_tps"])
            for row in mode_rows
            if row.get("completion_tps") is not None
        ]
        total_elapsed = sum(elapsed)
        total_tokens = sum(tokens) if tokens else None
        summaries.append(
            {
                "mode": mode,
                "tasks": len(mode_rows),
                "total_completion_tokens": total_tokens,
                "total_elapsed_sec": total_elapsed,
                "aggregate_completion_tps": (
                    total_tokens / total_elapsed
                    if total_tokens is not None and total_elapsed > 0.0
                    else None
                ),
                "mean_completion_tps": statistics.fmean(tps) if tps else None,
                "median_completion_tps": statistics.median(tps) if tps else None,
            }
        )
    return summaries


def fmt(value: Any) -> str:
    if value is None:
        return "-"
    if isinstance(value, float):
        return f"{value:.2f}"
    return str(value)


def print_table(summaries: list[dict[str, Any]]) -> None:
    columns = [
        ("mode", "mode"),
        ("tasks", "tasks"),
        ("total_completion_tokens", "tokens"),
        ("total_elapsed_sec", "elapsed_s"),
        ("aggregate_completion_tps", "agg_tps"),
        ("mean_completion_tps", "mean_tps"),
        ("median_completion_tps", "median_tps"),
    ]
    widths = [
        max(len(title), *(len(fmt(row[key])) for row in summaries))
        for key, title in columns
    ]
    print("  ".join(title.ljust(width) for width, (_key, title) in zip(widths, columns)))
    print("  ".join("-" * width for width in widths))
    for row in summaries:
        print(
            "  ".join(
                fmt(row[key]).ljust(width)
                for width, (key, _title) in zip(widths, columns)
            )
        )


def print_csv(summaries: list[dict[str, Any]]) -> None:
    columns = [
        "mode",
        "tasks",
        "total_completion_tokens",
        "total_elapsed_sec",
        "aggregate_completion_tps",
        "mean_completion_tps",
        "median_completion_tps",
    ]
    print(",".join(columns))
    for row in summaries:
        print(",".join(fmt(row[key]) for key in columns))


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("metadata", nargs="+", type=Path)
    parser.add_argument("--format", choices=("table", "csv", "json"), default="table")
    args = parser.parse_args()

    summaries = summarize(load_rows(args.metadata))
    if args.format == "json":
        json.dump(summaries, sys.stdout, indent=2, sort_keys=True)
        print()
    elif args.format == "csv":
        print_csv(summaries)
    else:
        print_table(summaries)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
