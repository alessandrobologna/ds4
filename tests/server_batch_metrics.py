#!/usr/bin/env python3
"""Capture ds4-server batch efficiency metrics across max-slot sizes.

This is a measurement harness, not a correctness test.  It intentionally keeps
performance assertions out of the smoke suite because local GPU residency,
thermal state, and prompt shape can move the numbers around.
"""

from __future__ import annotations

import argparse
import collections
import concurrent.futures
import http.client
import json
import math
import re
import subprocess
import sys
import threading
import time
from pathlib import Path
from typing import Any

from server_batch_smoke import (
    HOST,
    CheckError,
    PREFILL_UNIT,
    post_json,
    resolve_model,
    server_argv,
    server_with_retries,
)


PROMPTS = [
    "continuous batching",
    "GPU prefill",
    "decode scheduling",
    "slot reuse",
    "queue wait metrics",
    "token streaming",
    "batch efficiency",
    "request isolation",
]


def percentile(values: list[float], pct: float) -> float:
    if not values:
        return 0.0
    xs = sorted(values)
    pos = (len(xs) - 1) * pct
    lo = int(math.floor(pos))
    hi = int(math.ceil(pos))
    if lo == hi:
        return xs[lo]
    return xs[lo] + (xs[hi] - xs[lo]) * (pos - lo)


def parse_slots(raw: str) -> list[int]:
    slots: list[int] = []
    for item in raw.split(","):
        item = item.strip()
        if not item:
            continue
        try:
            value = int(item)
        except ValueError as exc:
            raise CheckError(f"invalid slot count {item!r}") from exc
        if value <= 0:
            raise CheckError(f"slot counts must be positive, got {value}")
        if value not in slots:
            slots.append(value)
    if not slots:
        raise CheckError("at least one slot count is required")
    return slots


def make_prefill_body(repeats: int) -> str:
    return "\n".join(f"{i:04d}. {PREFILL_UNIT}" for i in range(repeats))


def make_prompt(args: argparse.Namespace, index: int) -> str:
    topic = PROMPTS[index % len(PROMPTS)]
    unique = (
        f"Request {index:03d} has a unique opening about {topic}. "
        "Do not reuse another request's wording."
    )
    decode = (
        f"Request marker {index}. Write a continuous deterministic paragraph "
        f"about {topic}. Avoid lists, avoid a conclusion, do not summarize, "
        "and keep adding concrete details until the token budget ends."
    )
    if args.prompt_mode == "short":
        return f"{unique} Answer in one compact sentence."
    if args.prompt_mode == "mixed":
        return f"{unique}\n\n{make_prefill_body(args.prefill_repeats)}\n\n{decode}"
    return decode


def metrics_chat_body(prompt: str, max_tokens: int, stream: bool) -> dict[str, Any]:
    return {
        "model": "deepseek-v4-flash",
        "messages": [{"role": "user", "content": prompt}],
        "max_tokens": max_tokens,
        "temperature": 0,
        "stream": stream,
        "thinking": {"type": "disabled"},
    }


def measure_stream_chat(
    port: int,
    prompt: str,
    max_tokens: int,
    timeout: float,
) -> dict[str, Any]:
    conn = http.client.HTTPConnection(HOST, port, timeout=timeout)
    payload = json.dumps(metrics_chat_body(prompt, max_tokens, True)).encode("utf-8")
    started = time.perf_counter()
    first_event: float | None = None
    first_content: float | None = None
    events = 0
    content_events = 0
    completion_tokens = 0
    prompt_tokens = 0
    text_chars = 0
    done = False
    try:
        conn.request(
            "POST",
            "/v1/chat/completions",
            payload,
            {"Content-Type": "application/json", "Content-Length": str(len(payload))},
        )
        resp = conn.getresponse()
        if resp.status != 200:
            raise CheckError(f"HTTP {resp.status}: {resp.read()[:500]!r}")
        while True:
            line = resp.readline()
            if not line:
                break
            if not line.startswith(b"data: "):
                continue
            now = time.perf_counter()
            raw = line[len(b"data: ") :].strip()
            if raw == b"[DONE]":
                done = True
                break
            if not raw:
                continue
            if first_event is None:
                first_event = now
            events += 1
            try:
                obj = json.loads(raw.decode("utf-8", "replace"))
            except json.JSONDecodeError:
                continue
            usage = obj.get("usage") or {}
            prompt_tokens = max(prompt_tokens, int(usage.get("prompt_tokens") or 0))
            completion_tokens = max(
                completion_tokens, int(usage.get("completion_tokens") or 0)
            )
            for choice in obj.get("choices") or []:
                delta = choice.get("delta") or {}
                content = delta.get("content") or ""
                if content:
                    if first_content is None:
                        first_content = now
                    content_events += 1
                    text_chars += len(content)
    finally:
        conn.close()
    ended = time.perf_counter()
    if not done:
        raise CheckError("stream request did not receive [DONE]")
    if completion_tokens <= 0:
        completion_tokens = max(1, content_events)
    return {
        "latency_s": ended - started,
        "ttfe_s": (first_event - started) if first_event is not None else None,
        "ttft_s": (first_content - started) if first_content is not None else None,
        "events": events,
        "content_events": content_events,
        "text_chars": text_chars,
        "prompt_tokens": prompt_tokens,
        "completion_tokens": completion_tokens,
    }


def measure_chat(
    port: int,
    prompt: str,
    max_tokens: int,
    timeout: float,
) -> dict[str, Any]:
    started = time.perf_counter()
    data = post_json(
        port,
        "/v1/chat/completions",
        metrics_chat_body(prompt, max_tokens, False),
        timeout=timeout,
    )
    ended = time.perf_counter()
    obj = json.loads(data.decode("utf-8"))
    usage = obj.get("usage") or {}
    completion_tokens = int(usage.get("completion_tokens") or 0)
    if completion_tokens <= 0:
        raise CheckError(f"chat completion did not return generated tokens: {obj}")
    return {
        "latency_s": ended - started,
        "ttfe_s": None,
        "ttft_s": None,
        "events": 0,
        "content_events": 0,
        "text_chars": 0,
        "prompt_tokens": int(usage.get("prompt_tokens") or 0),
        "completion_tokens": completion_tokens,
    }


def summarize_results(
    max_slots: int,
    concurrency: int,
    request_count: int,
    wall_s: float,
    results: list[dict[str, Any]],
) -> dict[str, Any]:
    latencies = [float(r["latency_s"]) for r in results]
    ttfe = [float(r["ttfe_s"]) for r in results if r["ttfe_s"] is not None]
    ttft = [float(r["ttft_s"]) for r in results if r["ttft_s"] is not None]
    prompt_tokens = sum(int(r["prompt_tokens"]) for r in results)
    completion_tokens = sum(int(r["completion_tokens"]) for r in results)
    total_tokens = prompt_tokens + completion_tokens
    return {
        "max_slots": max_slots,
        "concurrency": concurrency,
        "requests": request_count,
        "wall_s": wall_s,
        "request_per_s": request_count / wall_s if wall_s > 0 else 0.0,
        "prompt_tokens": prompt_tokens,
        "completion_tokens": completion_tokens,
        "total_tokens": total_tokens,
        "completion_tok_per_s": completion_tokens / wall_s if wall_s > 0 else 0.0,
        "total_tok_per_s": total_tokens / wall_s if wall_s > 0 else 0.0,
        "latency_p50_ms": percentile(latencies, 0.50) * 1000.0,
        "latency_p95_ms": percentile(latencies, 0.95) * 1000.0,
        "ttfe_p50_ms": percentile(ttfe, 0.50) * 1000.0 if ttfe else None,
        "ttft_p50_ms": percentile(ttft, 0.50) * 1000.0 if ttft else None,
    }


def field_values(lines: list[str], name: str) -> list[float]:
    rx = re.compile(rf"\b{re.escape(name)}=([0-9]+(?:\.[0-9]+)?)")
    values: list[float] = []
    for line in lines:
        match = rx.search(line)
        if match:
            values.append(float(match.group(1)))
    return values


def batch_hist(lines: list[str], marker: str) -> dict[str, int]:
    hist: collections.Counter[int] = collections.Counter()
    for line in lines:
        if marker not in line:
            continue
        match = re.search(r"\bbatch=(\d+)\b", line)
        if match:
            hist[int(match.group(1))] += 1
    return {str(k): hist[k] for k in sorted(hist)}


def max_batch_for(lines: list[str], marker: str) -> int:
    hist = batch_hist(lines, marker)
    return max((int(k) for k in hist), default=0)


def slot_reuse_count(lines: list[str]) -> int:
    starts: collections.Counter[int] = collections.Counter()
    for line in lines:
        if "prompt start" not in line:
            continue
        match = re.search(r"\bbatch slot=(\d+)\b", line)
        if match:
            starts[int(match.group(1))] += 1
    return sum(max(0, count - 1) for count in starts.values())


def summarize_server_metrics(lines: list[str], rss_samples_kib: list[int]) -> dict[str, Any]:
    prefill_step_ms = [
        float(match.group(1))
        for line in lines
        if "prefill=" in line
        for match in [re.search(r"\bstep_ms=([0-9]+(?:\.[0-9]+)?)", line)]
        if match
    ]
    decode_step_ms = [
        float(match.group(1))
        for line in lines
        if "decode=" in line
        for match in [re.search(r"\bstep_ms=([0-9]+(?:\.[0-9]+)?)", line)]
        if match
    ]
    queue_wait = field_values(lines, "queue_wait_ms")
    slot_wait = field_values(lines, "slot_wait_ms")
    request_prefill = field_values(lines, "prefill_ms")
    request_decode = field_values(lines, "decode_ms")
    peak_rss_kib = max(rss_samples_kib) if rss_samples_kib else 0
    avg_rss_kib = sum(rss_samples_kib) / len(rss_samples_kib) if rss_samples_kib else 0.0
    return {
        "server_queue_wait_p50_ms": percentile(queue_wait, 0.50),
        "server_queue_wait_p95_ms": percentile(queue_wait, 0.95),
        "server_slot_wait_p50_ms": percentile(slot_wait, 0.50),
        "server_slot_wait_p95_ms": percentile(slot_wait, 0.95),
        "server_request_prefill_p50_ms": percentile(request_prefill, 0.50),
        "server_request_prefill_p95_ms": percentile(request_prefill, 0.95),
        "server_request_decode_p50_ms": percentile(request_decode, 0.50),
        "server_request_decode_p95_ms": percentile(request_decode, 0.95),
        "server_prefill_step_count": len(prefill_step_ms),
        "server_prefill_step_total_ms": sum(prefill_step_ms),
        "server_prefill_step_p50_ms": percentile(prefill_step_ms, 0.50),
        "server_decode_step_count": len(decode_step_ms),
        "server_decode_step_total_ms": sum(decode_step_ms),
        "server_decode_step_p50_ms": percentile(decode_step_ms, 0.50),
        "decode_batch_hist": batch_hist(lines, "decode="),
        "prefill_batch_hist": batch_hist(lines, "prefill="),
        "observed_max_batch": max_batch_for(lines, "decode="),
        "observed_max_prefill_batch": max_batch_for(lines, "prefill="),
        "slot_reuse": slot_reuse_count(lines),
        "rss_peak_mib": peak_rss_kib / 1024.0,
        "rss_avg_mib": avg_rss_kib / 1024.0,
    }


class RssSampler:
    def __init__(self, pid: int, interval_s: float = 0.5):
        self.pid = pid
        self.interval_s = interval_s
        self.samples: list[int] = []
        self._stop = threading.Event()
        self._thread = threading.Thread(target=self._run, daemon=True)

    def __enter__(self) -> "RssSampler":
        self._thread.start()
        return self

    def __exit__(self, exc_type, exc, tb) -> None:
        self._stop.set()
        self._thread.join(timeout=2.0)

    def _run(self) -> None:
        while not self._stop.is_set():
            try:
                out = subprocess.check_output(
                    ["ps", "-o", "rss=", "-p", str(self.pid)],
                    text=True,
                    stderr=subprocess.DEVNULL,
                ).strip()
                if out:
                    self.samples.append(int(out.splitlines()[0].strip()))
            except (OSError, subprocess.SubprocessError, ValueError):
                pass
            self._stop.wait(self.interval_s)


def run_one_slot_size(args: argparse.Namespace, max_slots: int) -> dict[str, Any]:
    concurrency = args.concurrency if args.concurrency is not None else max_slots
    request_count = args.requests or max(concurrency * 2, 8)
    if concurrency <= 0:
        raise CheckError("concurrency must be positive")
    if request_count <= 0:
        raise CheckError("request count must be positive")

    def make_argv(port: int) -> list[str]:
        argv = server_argv(args, port, max_slots)
        if args.server_chdir:
            argv += ["--chdir", args.server_chdir]
        return argv

    with server_with_retries(make_argv, cwd=args.server_cwd) as (server, port):
        assert server.proc is not None
        with RssSampler(server.proc.pid) as rss:
            for i in range(args.warmup_requests):
                measure_chat(
                    port,
                    make_prompt(args, 1000 + i),
                    args.tokens,
                    args.request_timeout,
                )

            started = time.perf_counter()
            with concurrent.futures.ThreadPoolExecutor(max_workers=concurrency) as pool:
                futures = [
                    pool.submit(
                        measure_stream_chat if args.stream else measure_chat,
                        port,
                        make_prompt(args, i),
                        args.tokens,
                        args.request_timeout,
                    )
                    for i in range(request_count)
                ]
                results = [future.result() for future in concurrent.futures.as_completed(futures)]
            wall_s = time.perf_counter() - started
        server_metrics = summarize_server_metrics(server.lines, rss.samples)
        if max_slots == 1 and server_metrics["observed_max_batch"] == 0:
            server_metrics["observed_max_batch"] = 1
        if args.log_dir:
            log_dir = Path(args.log_dir)
            log_dir.mkdir(parents=True, exist_ok=True)
            (log_dir / f"slots-{max_slots}.server.log").write_text(
                "\n".join(server.lines) + "\n",
                encoding="utf-8",
            )

    row = summarize_results(max_slots, concurrency, request_count, wall_s, results)
    row.update(server_metrics)
    return row


def add_efficiency(rows: list[dict[str, Any]]) -> None:
    baseline = next((row for row in rows if row["max_slots"] == 1), rows[0])
    baseline_tps = float(baseline["completion_tok_per_s"])
    for row in rows:
        tps = float(row["completion_tok_per_s"])
        speedup = tps / baseline_tps if baseline_tps > 0 else 0.0
        row["speedup_vs_baseline"] = speedup
        row["slot_efficiency_vs_baseline"] = (
            speedup / int(row["max_slots"]) if row["max_slots"] else 0.0
        )


def fmt_ms(value: Any) -> str:
    if value is None:
        return "-"
    return f"{float(value):.1f}"


def print_table(rows: list[dict[str, Any]]) -> None:
    headers = [
        "slots",
        "maxbat",
        "conc",
        "req",
        "wall_s",
        "req/s",
        "out_tok/s",
        "tot_tok/s",
        "p50_ms",
        "p95_ms",
        "ttft_ms",
        "srv_pre_ms",
        "srv_dec_ms",
        "rss_mib",
        "speedup",
        "eff/slot",
    ]
    print(" ".join(f"{h:>10}" for h in headers))
    for row in rows:
        values = [
            f"{row['max_slots']:10d}",
            f"{int(row.get('observed_max_batch') or 0):10d}",
            f"{row['concurrency']:10d}",
            f"{row['requests']:10d}",
            f"{row['wall_s']:10.3f}",
            f"{row['request_per_s']:10.2f}",
            f"{row['completion_tok_per_s']:10.2f}",
            f"{row['total_tok_per_s']:10.2f}",
            f"{row['latency_p50_ms']:10.1f}",
            f"{row['latency_p95_ms']:10.1f}",
            f"{fmt_ms(row['ttft_p50_ms']):>10}",
            f"{row.get('server_request_prefill_p50_ms', 0.0):10.1f}",
            f"{row.get('server_request_decode_p50_ms', 0.0):10.1f}",
            f"{row.get('rss_peak_mib', 0.0):10.1f}",
            f"{row['speedup_vs_baseline']:10.2f}",
            f"{row['slot_efficiency_vs_baseline']:10.2f}",
        ]
        print(" ".join(values))


def parse_args(argv: list[str]) -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--server", default="./ds4-server")
    parser.add_argument(
        "--server-chdir",
        help="Optional --chdir passed to ds4-server before model/runtime asset loading.",
    )
    parser.add_argument(
        "--server-cwd",
        help="Optional process working directory for ds4-server.",
    )
    parser.add_argument("--model")
    parser.add_argument("--ctx", type=int, default=512)
    parser.add_argument("--tokens", type=int, default=16)
    parser.add_argument("--backend", default="session-slots")
    parser.add_argument("--batch-wait-us", type=int, default=500)
    parser.add_argument("--experimental-batched-prefill", action="store_true")
    parser.add_argument("--request-timeout", type=float, default=240.0)
    parser.add_argument(
        "--slots",
        default="1,2,4",
        help="Comma-separated --max-slots values to measure. Default: 1,2,4",
    )
    parser.add_argument(
        "--concurrency",
        type=int,
        help="Concurrent client requests. Default: largest value from --slots",
    )
    parser.add_argument(
        "--requests",
        type=int,
        help="Total requests per slot-size run. Default: max(concurrency*2, 8)",
    )
    parser.add_argument("--warmup-requests", type=int, default=1)
    parser.add_argument(
        "--prompt-mode",
        choices=["long-decode", "mixed", "short"],
        default="long-decode",
        help="Prompt shape. long-decode uses different prompts that try to exhaust --tokens.",
    )
    parser.add_argument(
        "--prefill-repeats",
        type=int,
        default=20,
        help="Repeated body rows for --prompt-mode mixed. Default: 20",
    )
    parser.add_argument(
        "--no-stream",
        action="store_false",
        dest="stream",
        help="Use non-streaming chat requests; TTFT columns will be empty.",
    )
    parser.add_argument(
        "--json-out",
        help="Optional path for machine-readable metrics JSON.",
    )
    parser.add_argument(
        "--log-dir",
        help="Optional directory for raw per-slot server logs.",
    )
    parser.set_defaults(stream=True)
    args = parser.parse_args(argv)
    args.slots = parse_slots(args.slots)
    if args.prefill_repeats < 1:
        parser.error("--prefill-repeats must be >= 1")
    return args


def main(argv: list[str]) -> int:
    args = parse_args(argv)
    try:
        args.model = resolve_model(args.model)
        rows = [run_one_slot_size(args, slots) for slots in args.slots]
        add_efficiency(rows)
        print_table(rows)
        if args.json_out:
            payload = {
                "config": {
                    "model": args.model,
                    "backend": args.backend,
                    "slots": args.slots,
                    "concurrency": args.concurrency or max(args.slots),
                    "requests": args.requests
                    or max((args.concurrency or max(args.slots)) * 2, 8),
                    "tokens": args.tokens,
                    "stream": args.stream,
                    "batch_wait_us": args.batch_wait_us,
                    "prompt_mode": args.prompt_mode,
                    "prefill_repeats": args.prefill_repeats,
                    "experimental_batched_prefill": args.experimental_batched_prefill,
                },
                "results": rows,
            }
            Path(args.json_out).write_text(
                json.dumps(payload, indent=2, sort_keys=True) + "\n",
                encoding="utf-8",
            )
            print(f"wrote metrics JSON to {args.json_out}")
    except CheckError as exc:
        print(f"server-batch-metrics: FAIL: {exc}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
