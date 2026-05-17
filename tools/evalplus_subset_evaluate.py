#!/usr/bin/env python3
"""Evaluate only the EvalPlus tasks present in a sample JSONL file."""

from __future__ import annotations

import argparse
import json
import multiprocessing
import statistics
import sys
from collections import Counter, defaultdict
from concurrent.futures import ProcessPoolExecutor, as_completed
from pathlib import Path
from typing import Any


def load_evalplus(dataset: str, mini: bool, noextreme: bool, version: str):
    try:
        from evalplus.data import (
            get_human_eval_plus,
            get_human_eval_plus_hash,
            get_mbpp_plus,
            get_mbpp_plus_hash,
            load_solutions,
        )
        from evalplus.eval import PASS, estimate_pass_at_k
        from evalplus.eval._special_oracle import MBPP_OUTPUT_NOT_NONE_TASKS
        from evalplus.evaluate import check_correctness, get_groundtruth
    except ImportError as exc:
        raise SystemExit(
            "EvalPlus is not installed in this Python environment. "
            "Run this command through uv, for example: "
            "uv run --with evalplus python tools/evalplus_subset_evaluate.py ..."
        ) from exc

    if dataset == "humaneval":
        problems = get_human_eval_plus(mini=mini, noextreme=noextreme, version=version)
        dataset_hash = get_human_eval_plus_hash(
            mini=mini, noextreme=noextreme, version=version
        )
        output_not_none_tasks: list[str] = []
    elif dataset == "mbpp":
        problems = get_mbpp_plus(mini=mini, noextreme=noextreme, version=version)
        dataset_hash = get_mbpp_plus_hash(mini=mini, noextreme=noextreme, version=version)
        output_not_none_tasks = MBPP_OUTPUT_NOT_NONE_TASKS
    else:
        raise SystemExit(f"unsupported dataset: {dataset}")

    expected_output = get_groundtruth(problems, dataset_hash, output_not_none_tasks)
    return {
        "PASS": PASS,
        "check_correctness": check_correctness,
        "estimate_pass_at_k": estimate_pass_at_k,
        "expected_output": expected_output,
        "load_solutions": load_solutions,
        "problems": problems,
    }


def parse_task_ids(raw: str | None) -> set[str] | None:
    if not raw:
        return None
    return {item.strip() for item in raw.split(",") if item.strip()}


def collect_samples(
    load_solutions,
    samples_path: str,
    task_ids: set[str] | None,
    limit: int | None,
) -> list[dict[str, Any]]:
    rows: list[dict[str, Any]] = []
    for sample in load_solutions(samples_path):
        if task_ids is not None and sample["task_id"] not in task_ids:
            continue
        rows.append(sample)
        if limit is not None and len(rows) >= limit:
            break
    if not rows:
        raise SystemExit("no samples selected")
    return rows


def evaluate_subset(args: argparse.Namespace) -> dict[str, Any]:
    ep = load_evalplus(args.dataset, args.mini, args.noextreme, args.version)
    task_filter = parse_task_ids(args.task_ids)
    samples = collect_samples(ep["load_solutions"], str(args.samples), task_filter, args.limit)
    n_workers = args.parallel or max(1, multiprocessing.cpu_count() // 2)

    futures = []
    completion_id: Counter[str] = Counter()
    task_results: dict[str, list[dict[str, Any]]] = defaultdict(list)
    with ProcessPoolExecutor(max_workers=n_workers) as executor:
        for sample in samples:
            task_id = sample["task_id"]
            problem = ep["problems"].get(task_id)
            if problem is None:
                print(f"warning: skipping unknown task {task_id}", file=sys.stderr)
                continue
            solution = sample.get("solution") or problem["prompt"] + sample["completion"]
            futures.append(
                executor.submit(
                    ep["check_correctness"],
                    args.dataset,
                    completion_id[task_id],
                    problem,
                    solution,
                    ep["expected_output"][task_id],
                    args.base_only,
                    not args.test_details,
                    sample.get("_identifier"),
                    args.min_time_limit,
                    args.gt_time_limit_factor,
                )
            )
            completion_id[task_id] += 1

        if not futures:
            raise SystemExit("no valid samples selected")

        for future in as_completed(futures):
            result = future.result()
            task_results[result["task_id"]].append(result)

    for results in task_results.values():
        results.sort(key=lambda item: item["completion_id"])

    total = []
    base_correct = []
    plus_correct = []
    failed_base: list[str] = []
    failed_plus: list[str] = []
    pass_status = ep["PASS"]
    for task_id, results in sorted(task_results.items()):
        total.append(len(results))
        bc = sum(result["base"][0] == pass_status for result in results)
        base_correct.append(bc)
        if bc == 0:
            failed_base.append(task_id)
        if not args.base_only:
            pc = sum(
                result["base"][0] == result["plus"][0] == pass_status
                for result in results
            )
            plus_correct.append(pc)
            if pc == 0:
                failed_plus.append(task_id)

    estimate_pass_at_k = ep["estimate_pass_at_k"]
    base_pass_at_1 = statistics.fmean(estimate_pass_at_k(total, base_correct, 1))
    plus_pass_at_1 = None
    if plus_correct:
        plus_pass_at_1 = statistics.fmean(estimate_pass_at_k(total, plus_correct, 1))

    return {
        "dataset": args.dataset,
        "samples": len(samples),
        "tasks": len(task_results),
        "base_pass_at_1": base_pass_at_1,
        "plus_pass_at_1": plus_pass_at_1,
        "failed_base": failed_base,
        "failed_plus": failed_plus if not args.base_only else None,
    }


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("dataset", choices=("humaneval", "mbpp"))
    parser.add_argument("--samples", required=True, type=Path)
    parser.add_argument("--parallel", type=int)
    parser.add_argument("--base-only", action="store_true")
    parser.add_argument("--test-details", action="store_true")
    parser.add_argument("--mini", action="store_true")
    parser.add_argument("--noextreme", action="store_true")
    parser.add_argument("--version", default="default")
    parser.add_argument("--limit", type=int)
    parser.add_argument("--task-ids")
    parser.add_argument("--output", type=Path)
    parser.add_argument("--min-time-limit", type=float, default=1.0)
    parser.add_argument("--gt-time-limit-factor", type=float, default=4.0)
    args = parser.parse_args()

    result = evaluate_subset(args)
    print(f"{result['dataset']} subset")
    print(f"tasks:\t{result['tasks']}")
    print(f"base pass@1:\t{result['base_pass_at_1']:.3f}")
    if result["plus_pass_at_1"] is not None:
        print(f"plus pass@1:\t{result['plus_pass_at_1']:.3f}")
    if result["failed_base"]:
        print("failed base:\t" + ",".join(result["failed_base"]))
    if result["failed_plus"]:
        print("failed plus:\t" + ",".join(result["failed_plus"]))

    if args.output:
        args.output.parent.mkdir(parents=True, exist_ok=True)
        args.output.write_text(json.dumps(result, indent=2, sort_keys=True) + "\n", encoding="utf-8")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
