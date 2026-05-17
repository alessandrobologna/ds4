#!/usr/bin/env python3
"""Generate EvalPlus samples with a DS4 OpenAI-compatible server.

This is intentionally only the sample-generation half of the quality gate:
EvalPlus owns the datasets and execution scoring, while DS4 only needs to
produce one JSONL sample per task plus timing metadata.
"""

from __future__ import annotations

import argparse
import json
import re
import statistics
import sys
import time
import urllib.error
import urllib.request
from pathlib import Path
from typing import Any


def load_problems(dataset: str) -> dict[str, dict[str, Any]]:
    try:
        if dataset == "humaneval":
            from evalplus.data import get_human_eval_plus

            return get_human_eval_plus()
        if dataset == "mbpp":
            from evalplus.data import get_mbpp_plus

            return get_mbpp_plus()
    except ImportError as exc:
        raise SystemExit(
            "EvalPlus is not installed in this Python environment. "
            "Run this command through uv, for example: "
            "uv run --with evalplus python tools/evalplus_ds4.py ..."
        ) from exc
    raise SystemExit(f"unsupported dataset: {dataset}")


def strip_markdown_code(text: str) -> str:
    fence = re.search(r"```(?:python|py)?\s*\n(.*?)```", text, re.DOTALL | re.IGNORECASE)
    if fence:
        text = fence.group(1)
    text = text.strip()
    for prefix in (
        "Here is the completed code:",
        "Here is the code:",
        "Sure, here is the code:",
    ):
        if text.startswith(prefix):
            text = text[len(prefix) :].strip()
    return text.rstrip() + "\n"


def completion_to_solution(prompt: str, raw: str) -> str:
    code = strip_markdown_code(raw)
    if prompt.strip() in code:
        return code
    if re.search(r"^\s*def\s+\w+\s*\(", code, re.MULTILINE):
        return code
    return prompt.rstrip() + "\n" + code


def post_json(url: str, payload: dict[str, Any], timeout: float) -> dict[str, Any]:
    data = json.dumps(payload).encode("utf-8")
    req = urllib.request.Request(
        url,
        data=data,
        headers={"Content-Type": "application/json"},
        method="POST",
    )
    try:
        with urllib.request.urlopen(req, timeout=timeout) as resp:
            return json.loads(resp.read().decode("utf-8", errors="replace"))
    except urllib.error.HTTPError as exc:
        body = exc.read().decode("utf-8", errors="replace")
        raise RuntimeError(f"HTTP {exc.code}: {body}") from exc


def generate_one(
    base_url: str,
    model: str,
    prompt: str,
    api: str,
    max_tokens: int,
    stops: list[str],
    timeout: float,
) -> tuple[str, float, dict[str, Any]]:
    if api == "chat":
        url = base_url.rstrip("/") + "/chat/completions"
        payload = {
            "model": model,
            "messages": [
                {
                    "role": "system",
                    "content": (
                        "You are completing Python programming benchmark tasks. "
                        "Return only valid Python code. Do not use Markdown fences. "
                        "Do not explain the answer."
                    ),
                },
                {
                    "role": "user",
                    "content": (
                        "Complete the following Python function or program. "
                        "Return the full self-contained Python solution.\n\n"
                        + prompt
                    ),
                },
            ],
            "temperature": 0,
            "top_p": 1,
            "max_tokens": max_tokens,
            "think": False,
            "stream": False,
        }
    else:
        url = base_url.rstrip("/") + "/completions"
        payload = {
            "model": model,
            "prompt": prompt,
            "temperature": 0,
            "top_p": 1,
            "max_tokens": max_tokens,
            "think": False,
            "stream": False,
        }
    if stops:
        payload["stop"] = stops
    start = time.perf_counter()
    response = post_json(url, payload, timeout)
    elapsed = time.perf_counter() - start
    try:
        choice = response["choices"][0]
        content = choice["message"]["content"] if api == "chat" else choice["text"]
    except (KeyError, IndexError, TypeError) as exc:
        raise RuntimeError(f"unexpected response shape: {response}") from exc
    usage = response.get("usage")
    return content, elapsed, usage if isinstance(usage, dict) else {}


def parse_task_ids(raw: str | None, all_ids: list[str], limit: int | None) -> list[str]:
    if raw:
        wanted = [item.strip() for item in raw.split(",") if item.strip()]
        missing = [item for item in wanted if item not in all_ids]
        if missing:
            raise SystemExit(f"unknown task id(s): {', '.join(missing)}")
        return wanted
    if limit is None:
        return all_ids
    return all_ids[:limit]


def int_or_none(value: Any) -> int | None:
    return value if isinstance(value, int) else None


def write_summary(path: Path, rows: list[dict[str, Any]], mode: str) -> None:
    elapsed_values = [row["elapsed_sec"] for row in rows]
    completion_tokens = [
        row["completion_tokens"] for row in rows if row.get("completion_tokens") is not None
    ]
    completion_tps = [
        row["completion_tps"] for row in rows if row.get("completion_tps") is not None
    ]
    summary = {
        "mode": mode,
        "tasks": len(rows),
        "total_elapsed_sec": sum(elapsed_values),
        "mean_elapsed_sec": statistics.fmean(elapsed_values) if elapsed_values else None,
        "median_elapsed_sec": statistics.median(elapsed_values) if elapsed_values else None,
        "total_completion_tokens": sum(completion_tokens) if completion_tokens else None,
        "mean_completion_tokens": statistics.fmean(completion_tokens) if completion_tokens else None,
        "median_completion_tokens": statistics.median(completion_tokens)
        if completion_tokens
        else None,
        "mean_completion_tps": statistics.fmean(completion_tps) if completion_tps else None,
        "median_completion_tps": statistics.median(completion_tps) if completion_tps else None,
    }
    path.write_text(json.dumps(summary, indent=2, sort_keys=True) + "\n", encoding="utf-8")


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--dataset", choices=("humaneval", "mbpp"), default="humaneval")
    parser.add_argument("--base-url", default="http://127.0.0.1:8000/v1")
    parser.add_argument("--model", default="deepseek-chat")
    parser.add_argument("--api", choices=("chat", "completions"), default="chat")
    parser.add_argument("--mode", default="unspecified")
    parser.add_argument("--output", required=True)
    parser.add_argument("--metadata-output")
    parser.add_argument("--summary-output")
    parser.add_argument("--limit", type=int, default=5)
    parser.add_argument("--task-ids")
    parser.add_argument("--max-tokens", type=int, default=768)
    parser.add_argument(
        "--stop",
        action="append",
        default=[],
        help="Stop sequence to pass to the server. May be repeated.",
    )
    parser.add_argument("--timeout", type=float, default=600.0)
    args = parser.parse_args()

    problems = load_problems(args.dataset)
    task_ids = parse_task_ids(args.task_ids, list(problems.keys()), args.limit)
    out_path = Path(args.output)
    meta_path = Path(args.metadata_output) if args.metadata_output else out_path.with_suffix(".meta.jsonl")
    summary_path = (
        Path(args.summary_output) if args.summary_output else out_path.with_suffix(".summary.json")
    )
    out_path.parent.mkdir(parents=True, exist_ok=True)
    meta_path.parent.mkdir(parents=True, exist_ok=True)
    summary_path.parent.mkdir(parents=True, exist_ok=True)

    rows: list[dict[str, Any]] = []
    with out_path.open("w", encoding="utf-8") as samples, meta_path.open(
        "w", encoding="utf-8"
    ) as meta:
        for index, task_id in enumerate(task_ids, 1):
            problem = problems[task_id]
            raw, elapsed, usage = generate_one(
                args.base_url,
                args.model,
                problem["prompt"],
                args.api,
                args.max_tokens,
                args.stop,
                args.timeout,
            )
            solution = completion_to_solution(problem["prompt"], raw)
            prompt_tokens = int_or_none(usage.get("prompt_tokens"))
            completion_tokens = int_or_none(usage.get("completion_tokens"))
            total_tokens = int_or_none(usage.get("total_tokens"))
            completion_tps = (
                completion_tokens / elapsed
                if completion_tokens is not None and elapsed > 0.0
                else None
            )
            samples.write(json.dumps({"task_id": task_id, "solution": solution}) + "\n")
            samples.flush()
            row = {
                "task_id": task_id,
                "mode": args.mode,
                "api": args.api,
                "elapsed_sec": elapsed,
                "prompt_tokens": prompt_tokens,
                "completion_tokens": completion_tokens,
                "total_tokens": total_tokens,
                "completion_tps": completion_tps,
                "raw_chars": len(raw),
                "solution_chars": len(solution),
            }
            rows.append(row)
            meta.write(json.dumps(row) + "\n")
            meta.flush()
            tps_text = f" completion_tps={completion_tps:.2f}" if completion_tps else ""
            print(
                f"{index}/{len(task_ids)} {task_id} elapsed={elapsed:.2f}s "
                f"solution_chars={len(solution)}{tps_text}",
                file=sys.stderr,
                flush=True,
            )

    write_summary(summary_path, rows, args.mode)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
