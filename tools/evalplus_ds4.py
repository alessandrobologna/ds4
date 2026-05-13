#!/usr/bin/env python3
"""Generate EvalPlus samples with a DS4 OpenAI-compatible server.

This script intentionally stays small: EvalPlus owns the datasets and the
execution-based scoring, while DS4 only needs to produce one JSONL sample per
task. Start ds4-server separately, then run this against /v1/chat/completions.
"""

from __future__ import annotations

import argparse
import json
import re
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
            "Install with: python -m pip install evalplus"
        ) from exc
    raise SystemExit(f"unsupported dataset: {dataset}")


def strip_markdown_code(text: str) -> str:
    fence = re.search(r"```(?:python|py)?\s*\n(.*?)```", text, re.DOTALL | re.IGNORECASE)
    if fence:
        text = fence.group(1)
    text = text.strip()
    prefixes = (
        "Here is the completed code:",
        "Here is the code:",
        "Sure, here is the code:",
    )
    for prefix in prefixes:
        if text.startswith(prefix):
            text = text[len(prefix):].strip()
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
            raw = resp.read()
            return json.loads(raw.decode("utf-8", errors="replace"))
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
) -> tuple[str, float]:
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
    return content, elapsed


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


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--dataset", choices=("humaneval", "mbpp"), default="humaneval")
    parser.add_argument("--base-url", default="http://127.0.0.1:8000/v1")
    parser.add_argument("--model", default="deepseek-chat")
    parser.add_argument("--api", choices=("chat", "completions"), default="chat")
    parser.add_argument("--output", required=True)
    parser.add_argument("--metadata-output")
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
    out_path.parent.mkdir(parents=True, exist_ok=True)
    meta_path.parent.mkdir(parents=True, exist_ok=True)

    total_elapsed = 0.0
    with out_path.open("w", encoding="utf-8") as samples, meta_path.open("w", encoding="utf-8") as meta:
        for index, task_id in enumerate(task_ids, 1):
            problem = problems[task_id]
            raw, elapsed = generate_one(
                args.base_url,
                args.model,
                problem["prompt"],
                args.api,
                args.max_tokens,
                args.stop,
                args.timeout,
            )
            total_elapsed += elapsed
            solution = completion_to_solution(problem["prompt"], raw)
            samples.write(json.dumps({"task_id": task_id, "solution": solution}) + "\n")
            samples.flush()
            meta.write(
                json.dumps(
                    {
                        "task_id": task_id,
                        "api": args.api,
                        "elapsed_sec": elapsed,
                        "raw_chars": len(raw),
                        "solution_chars": len(solution),
                    }
                )
                + "\n"
            )
            meta.flush()
            print(
                f"{index}/{len(task_ids)} {task_id} elapsed={elapsed:.2f}s "
                f"solution_chars={len(solution)}",
                file=sys.stderr,
                flush=True,
            )

    print(
        f"wrote {len(task_ids)} samples to {out_path} in {total_elapsed:.2f}s",
        file=sys.stderr,
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
