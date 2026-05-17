#!/usr/bin/env python3
"""Run a small GSM8K slice against a DS4 OpenAI-compatible server."""

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


NUMBER_RE = re.compile(r"-?\d+(?:,\d{3})*(?:\.\d+)?")


def read_text(path_or_url: str) -> str:
    if path_or_url.startswith(("http://", "https://")):
        with urllib.request.urlopen(path_or_url, timeout=120) as resp:
            return resp.read().decode("utf-8", errors="replace")
    return Path(path_or_url).read_text(encoding="utf-8")


def load_rows(path_or_url: str, limit: int) -> list[dict[str, Any]]:
    rows = []
    for line in read_text(path_or_url).splitlines():
        if not line.strip():
            continue
        rows.append(json.loads(line))
        if limit and len(rows) >= limit:
            break
    return rows


def extract_gold(answer: str) -> str:
    if "####" in answer:
        answer = answer.rsplit("####", 1)[1]
    return normalize_answer(answer)


def normalize_answer(text: str) -> str:
    text = text.replace("$", "").replace(",", "").strip()
    numbers = NUMBER_RE.findall(text)
    if not numbers:
        return ""
    value = numbers[-1].replace(",", "")
    if "." in value:
        value = value.rstrip("0").rstrip(".")
    return value


def extract_prediction(text: str) -> str:
    if "####" in text:
        return normalize_answer(text.rsplit("####", 1)[1])
    return normalize_answer(text)


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
    question: str,
    max_tokens: int,
    timeout: float,
) -> tuple[str, float, dict[str, Any]]:
    payload = {
        "model": model,
        "messages": [
            {
                "role": "system",
                "content": (
                    "Solve the grade-school math problem. Show concise reasoning, "
                    "and end with the final answer on its own line as #### <answer>."
                ),
            },
            {"role": "user", "content": question},
        ],
        "temperature": 0,
        "top_p": 1,
        "max_tokens": max_tokens,
        "think": False,
        "stream": False,
    }
    start = time.perf_counter()
    response = post_json(base_url.rstrip("/") + "/chat/completions", payload, timeout)
    elapsed = time.perf_counter() - start
    try:
        content = response["choices"][0]["message"]["content"]
    except (KeyError, IndexError, TypeError) as exc:
        raise RuntimeError(f"unexpected response shape: {response}") from exc
    usage = response.get("usage")
    return content, elapsed, usage if isinstance(usage, dict) else {}


def int_or_none(value: Any) -> int | None:
    return value if isinstance(value, int) else None


def summarize(rows: list[dict[str, Any]], mode: str) -> dict[str, Any]:
    elapsed = [float(row["elapsed_sec"]) for row in rows]
    tokens = [
        int(row["completion_tokens"])
        for row in rows
        if row.get("completion_tokens") is not None
    ]
    tps = [
        float(row["completion_tps"])
        for row in rows
        if row.get("completion_tps") is not None
    ]
    total_elapsed = sum(elapsed)
    total_tokens = sum(tokens) if tokens else None
    failed = [row["idx"] for row in rows if not row["correct"]]
    return {
        "mode": mode,
        "tasks": len(rows),
        "accuracy": (sum(1 for row in rows if row["correct"]) / len(rows)) if rows else None,
        "correct": sum(1 for row in rows if row["correct"]),
        "failed": failed,
        "total_elapsed_sec": total_elapsed,
        "total_completion_tokens": total_tokens,
        "aggregate_tps": (
            total_tokens / total_elapsed
            if total_tokens is not None and total_elapsed > 0.0
            else None
        ),
        "mean_tps": statistics.fmean(tps) if tps else None,
        "median_tps": statistics.median(tps) if tps else None,
    }


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--dataset", required=True)
    parser.add_argument("--base-url", default="http://127.0.0.1:8000/v1")
    parser.add_argument("--model", default="deepseek-chat")
    parser.add_argument("--mode", default="unspecified")
    parser.add_argument("--output", required=True)
    parser.add_argument("--summary-output", required=True)
    parser.add_argument("--limit", type=int, default=20)
    parser.add_argument("--max-tokens", type=int, default=512)
    parser.add_argument("--timeout", type=float, default=600.0)
    args = parser.parse_args()

    problems = load_rows(args.dataset, args.limit)
    out_path = Path(args.output)
    summary_path = Path(args.summary_output)
    out_path.parent.mkdir(parents=True, exist_ok=True)
    summary_path.parent.mkdir(parents=True, exist_ok=True)

    rows: list[dict[str, Any]] = []
    with out_path.open("w", encoding="utf-8") as out:
        for idx, item in enumerate(problems):
            raw, elapsed, usage = generate_one(
                args.base_url,
                args.model,
                item["question"],
                args.max_tokens,
                args.timeout,
            )
            completion_tokens = int_or_none(usage.get("completion_tokens"))
            completion_tps = (
                completion_tokens / elapsed
                if completion_tokens is not None and elapsed > 0.0
                else None
            )
            gold = extract_gold(item["answer"])
            pred = extract_prediction(raw)
            row = {
                "idx": idx,
                "mode": args.mode,
                "question": item["question"],
                "gold": gold,
                "pred": pred,
                "correct": pred == gold,
                "elapsed_sec": elapsed,
                "completion_tokens": completion_tokens,
                "completion_tps": completion_tps,
                "raw": raw,
            }
            rows.append(row)
            out.write(json.dumps(row, ensure_ascii=True))
            out.write("\n")
            out.flush()
            tps_text = f" completion_tps={completion_tps:.2f}" if completion_tps else ""
            print(
                f"{args.mode} {idx + 1}/{len(problems)} correct={row['correct']}"
                f" pred={pred or '<none>'} gold={gold}{tps_text}",
                flush=True,
            )

    summary = summarize(rows, args.mode)
    summary_path.write_text(
        json.dumps(summary, indent=2, sort_keys=True) + "\n",
        encoding="utf-8",
    )
    print(json.dumps(summary, indent=2, sort_keys=True))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
