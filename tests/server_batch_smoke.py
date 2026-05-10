#!/usr/bin/env python3
"""Server-level smoke and benchmark checks for DS4 batch mode.

This script intentionally uses only the Python standard library so it can run
on a plain checkout after `make ds4-server`.
"""

from __future__ import annotations

import argparse
import concurrent.futures
import http.client
import json
import os
import re
import socket
import subprocess
import sys
import tempfile
import threading
import time
from pathlib import Path


HOST = "127.0.0.1"
SMOKE_PROMPT = (
    "This prompt is intentionally cacheable for DS4 disk KV smoke testing. "
    "Keep the prefix stable so two later requests can load the same checkpoint "
    "into separate slots and stream concurrently."
)
BENCH_PROMPT = (
    "Write a compact deterministic paragraph about continuous batching in an "
    "inference server. Avoid lists and keep going until the token budget ends."
)
PREFILL_UNIT = (
    "This is a stable prefill benchmark segment for measuring prompt ingestion "
    "without disk cache reuse. It contains enough ordinary prose to exercise "
    "tokenization, prompt rendering, and model prefill work in the server."
)


class CheckError(RuntimeError):
    pass


def find_free_port() -> int:
    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as sock:
        sock.bind((HOST, 0))
        return int(sock.getsockname()[1])


def resolve_model(path: str | None) -> str:
    model = path or os.environ.get("DS4_TEST_MODEL") or "ds4flash.gguf"
    if not Path(model).is_file():
        raise CheckError(
            f"model not found: {model} (pass --model or set DS4_TEST_MODEL)"
        )
    return model


class Server:
    def __init__(self, argv: list[str], port: int):
        self.argv = argv
        self.port = port
        self.proc: subprocess.Popen[str] | None = None
        self.lines: list[str] = []
        self._reader: threading.Thread | None = None

    def __enter__(self) -> "Server":
        self.proc = subprocess.Popen(
            self.argv,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            text=True,
            bufsize=1,
        )
        self._reader = threading.Thread(target=self._read_log, daemon=True)
        self._reader.start()
        self.wait_ready()
        return self

    def __exit__(self, exc_type, exc, tb) -> None:
        self.stop()

    def _read_log(self) -> None:
        assert self.proc is not None
        assert self.proc.stdout is not None
        for line in self.proc.stdout:
            self.lines.append(line.rstrip("\n"))

    def wait_ready(self, timeout: float = 30.0) -> None:
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            if self.proc and self.proc.poll() is not None:
                raise CheckError(
                    "server exited before listening:\n" + "\n".join(self.lines[-40:])
                )
            if any("listening on http://" in line for line in self.lines):
                return
            with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as sock:
                sock.settimeout(0.1)
                if sock.connect_ex((HOST, self.port)) == 0:
                    return
            time.sleep(0.05)
        raise CheckError("server did not become ready:\n" + "\n".join(self.lines[-40:]))

    def wait_for_log(self, pattern: str, timeout: float = 15.0) -> bool:
        rx = re.compile(pattern)
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            if any(rx.search(line) for line in self.lines):
                return True
            if self.proc and self.proc.poll() is not None:
                return False
            time.sleep(0.05)
        return False

    def stop(self) -> None:
        if not self.proc:
            return
        if self.proc.poll() is None:
            self.proc.terminate()
            try:
                self.proc.wait(timeout=20)
            except subprocess.TimeoutExpired:
                self.proc.kill()
                self.proc.wait(timeout=10)
        if self._reader:
            self._reader.join(timeout=2)


def chat_body(prompt: str, max_tokens: int, stream: bool) -> dict:
    body = {
        "model": "deepseek-chat",
        "messages": [{"role": "user", "content": prompt}],
        "max_tokens": max_tokens,
        "temperature": 0,
        "top_p": 1,
        "stream": stream,
    }
    if stream:
        body["stream_options"] = {"include_usage": True}
    return body


def post_json(port: int, path: str, body: dict, timeout: float = 180.0) -> bytes:
    conn = http.client.HTTPConnection(HOST, port, timeout=timeout)
    payload = json.dumps(body).encode("utf-8")
    try:
        conn.request(
            "POST",
            path,
            payload,
            {"Content-Type": "application/json", "Content-Length": str(len(payload))},
        )
        resp = conn.getresponse()
        data = resp.read()
        if resp.status != 200:
            raise CheckError(f"HTTP {resp.status}: {data[:500]!r}")
        return data
    finally:
        conn.close()


def run_chat(port: int, prompt: str, max_tokens: int, timeout: float) -> dict:
    start = time.monotonic()
    data = post_json(
        port,
        "/v1/chat/completions",
        chat_body(prompt, max_tokens, False),
        timeout=timeout,
    )
    elapsed = time.monotonic() - start
    obj = json.loads(data.decode("utf-8"))
    usage = obj.get("usage") or {}
    prompt_tokens = int(usage.get("prompt_tokens") or 0)
    completion_tokens = int(usage.get("completion_tokens") or 0)
    if completion_tokens <= 0:
        raise CheckError(f"missing completion token usage: {obj}")
    return {
        "elapsed": elapsed,
        "prompt_tokens": prompt_tokens,
        "completion_tokens": completion_tokens,
    }


def stream_chat(port: int, prompt: str, max_tokens: int) -> dict:
    conn = http.client.HTTPConnection(HOST, port, timeout=180)
    payload = json.dumps(chat_body(prompt, max_tokens, True)).encode("utf-8")
    first_event = None
    last_event = None
    events = 0
    completion_tokens = 0
    done = False
    start = time.monotonic()
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
            raw = line[len(b"data: ") :].strip()
            if raw == b"[DONE]":
                done = True
                break
            if not raw:
                continue
            now = time.monotonic()
            if first_event is None:
                first_event = now
            last_event = now
            events += 1
            try:
                obj = json.loads(raw.decode("utf-8"))
            except json.JSONDecodeError:
                continue
            usage = obj.get("usage") or {}
            completion_tokens = max(
                completion_tokens, int(usage.get("completion_tokens") or 0)
            )
    finally:
        conn.close()
    return {
        "elapsed": time.monotonic() - start,
        "events": events,
        "completion_tokens": completion_tokens,
        "first_event": first_event,
        "last_event": last_event,
        "done": done,
    }


def server_argv(
    args: argparse.Namespace,
    port: int,
    max_slots: int,
    backend: str | None,
    tokens: int,
    kv_dir: str | None = None,
) -> list[str]:
    argv = [
        args.server,
        "--model",
        args.model,
        "--ctx",
        str(args.ctx),
        "--tokens",
        str(tokens),
        "--host",
        HOST,
        "--port",
        str(port),
    ]
    if max_slots > 1:
        argv += [
            "--max-slots",
            str(max_slots),
            "--batch-wait-us",
            str(args.batch_wait_us),
        ]
        if backend:
            argv += ["--batch-backend", backend]
        if args.experimental_batched_prefill:
            argv += ["--experimental-batched-prefill"]
    if kv_dir:
        argv += [
            "--kv-disk-dir",
            kv_dir,
            "--kv-disk-space-mb",
            "256",
            "--kv-cache-min-tokens",
            "8",
            "--kv-cache-cold-max-tokens",
            "1000",
            "--kv-cache-continued-interval-tokens",
            "0",
            "--kv-cache-boundary-trim-tokens",
            "0",
            "--kv-cache-boundary-align-tokens",
            "0",
        ]
    return argv


def max_logged_batch(lines: list[str]) -> int:
    best = 0
    for line in lines:
        if "batch backend=" not in line:
            continue
        match = re.search(r" batch=(\d+)", line)
        if match:
                best = max(best, int(match.group(1)))
    return best


def max_logged_prefill_batch(lines: list[str]) -> int:
    best = 0
    for line in lines:
        if "batch backend=" not in line or "prefill=" not in line:
            continue
        match = re.search(r" batch=(\d+)", line)
        if match:
            best = max(best, int(match.group(1)))
    return best


def count_logs(lines: list[str], needle: str) -> int:
    return sum(1 for line in lines if needle in line)


def run_smoke(args: argparse.Namespace) -> None:
    args.model = resolve_model(args.model)
    with tempfile.TemporaryDirectory(prefix="ds4-batch-smoke-") as tmp:
        port = find_free_port()
        kv_dir = str(Path(tmp) / "kv")
        argv = server_argv(
            args,
            port,
            max_slots=2,
            backend=args.backend,
            tokens=args.smoke_tokens,
            kv_dir=kv_dir,
        )
        with Server(argv, port) as server:
            run_chat(port, SMOKE_PROMPT, 4, args.request_timeout)
            if not server.wait_for_log(r"kv cache stored", timeout=20):
                raise CheckError("warmup request did not store disk KV")
            with concurrent.futures.ThreadPoolExecutor(max_workers=2) as pool:
                futures = [
                    pool.submit(stream_chat, port, SMOKE_PROMPT, args.smoke_tokens)
                    for _ in range(2)
                ]
                results = [f.result() for f in futures]

            max_batch = max_logged_batch(server.lines)
            kv_hits = count_logs(server.lines, "kv cache hit")
            kv_stores = count_logs(server.lines, "kv cache stored")
            if any(r["events"] <= 0 or not r["done"] for r in results):
                raise CheckError(f"streaming requests did not complete: {results}")
            firsts = [r["first_event"] for r in results if r["first_event"] is not None]
            lasts = [r["last_event"] for r in results if r["last_event"] is not None]
            if len(firsts) != 2 or len(lasts) != 2 or max(firsts) >= min(lasts):
                raise CheckError(f"streams did not overlap: {results}")
            if max_batch < 2:
                raise CheckError("server logs did not show a decode batch of 2")
            if kv_hits < 2:
                raise CheckError(f"expected at least two disk KV hits, saw {kv_hits}")
            print(
                "server-batch-smoke: OK "
                f"backend={args.backend} events={[r['events'] for r in results]} "
                f"max_batch={max_batch} kv_hits={kv_hits} kv_stores={kv_stores}"
            )


def make_prefill_prompt(repeats: int) -> str:
    return "\n".join(f"{i:04d}. {PREFILL_UNIT}" for i in range(repeats))


def benchmark_workload(args: argparse.Namespace, workload: str) -> tuple[str, int]:
    if workload == "decode":
        return BENCH_PROMPT, args.max_tokens
    if workload == "prefill":
        return make_prefill_prompt(args.prefill_repeats), args.prefill_max_tokens
    raise CheckError(f"unknown benchmark workload: {workload}")


def benchmark_prompts(args: argparse.Namespace, workload: str, clients: int) -> tuple[list[str], int]:
    prompt, max_tokens = benchmark_workload(args, workload)
    prompts = [prompt for _ in range(clients)]
    if workload == "prefill" and args.prefill_unique_prefix:
        prompts = [
            f"Unique request prefix {i:02d}: this request starts differently before the long body.\n{prompt}"
            for i in range(clients)
        ]
    if workload == "prefill" and args.prefill_unique_suffix:
        prompts = [
            prompt +
            f"\nUnique request suffix {i:02d}: answer for client {i} only after the shared context."
            for i in range(clients)
        ]
    return prompts, max_tokens


def benchmark_once(
    args: argparse.Namespace, workload: str, label: str, clients: int, trial: int
) -> dict:
    port = find_free_port()
    if label == "serialized":
        max_slots = 1
        backend = None
    else:
        max_slots = max(clients, args.batch_slots_min)
        backend = label
    prompts, max_tokens = benchmark_prompts(args, workload, clients)
    argv = server_argv(
        args,
        port,
        max_slots=max_slots,
        backend=backend,
        tokens=max_tokens,
    )
    with Server(argv, port) as server:
        started = time.monotonic()
        pool = concurrent.futures.ThreadPoolExecutor(max_workers=clients)
        futures = {
            pool.submit(run_chat, port, prompts[i], max_tokens, args.request_timeout): i
            for i in range(clients)
        }
        try:
            results = []
            try:
                for future in concurrent.futures.as_completed(
                    futures, timeout=args.request_timeout
                ):
                    idx = futures[future]
                    result = future.result()
                    results.append(result)
                    if args.progress:
                        print(
                            "server-batch-benchmark: "
                            f"completed request={idx} workload={workload} "
                            f"label={label} clients={clients} "
                            f"elapsed={result['elapsed']:.3f}s",
                            flush=True,
                        )
            except concurrent.futures.TimeoutError as exc:
                logs = "\n".join(server.lines[-80:])
                raise CheckError(
                    f"benchmark timed out workload={workload} label={label} "
                    f"clients={clients} completed={len(results)}/{clients}\n{logs}"
                ) from exc
            except Exception as exc:
                logs = "\n".join(server.lines[-80:])
                raise CheckError(
                    f"benchmark request failed workload={workload} "
                    f"label={label} clients={clients}: {exc}\n{logs}"
                ) from exc
        finally:
            pool.shutdown(wait=False, cancel_futures=True)
        wall = time.monotonic() - started
        prompt_tokens = sum(int(r["prompt_tokens"]) for r in results)
        completion_tokens = sum(int(r["completion_tokens"]) for r in results)
        completion_tps = completion_tokens / wall if wall > 0 else 0.0
        prompt_tps = prompt_tokens / wall if wall > 0 else 0.0
        reqs_per_sec = clients / wall if wall > 0 else 0.0
        metric_name = "prompt_tps" if workload == "prefill" else "completion_tps"
        metric = prompt_tps if workload == "prefill" else completion_tps
        return {
            "workload": workload,
            "label": label,
            "clients": clients,
            "trial": trial,
            "slots": max_slots,
            "prompt_tokens": prompt_tokens,
            "completion_tokens": completion_tokens,
            "wall": wall,
            "completion_tps": completion_tps,
            "prompt_tps": prompt_tps,
            "reqs_per_sec": reqs_per_sec,
            "metric_name": metric_name,
            "metric": metric,
            "max_batch": max_logged_batch(server.lines),
            "max_prefill_batch": max_logged_prefill_batch(server.lines),
            "prefill_fanout": count_logs(server.lines, "prefill=fanout"),
            "prefill_chunk": count_logs(server.lines, "prefill=chunk"),
        }


def median(values: list[float]) -> float:
    ordered = sorted(values)
    n = len(ordered)
    mid = n // 2
    if n % 2:
        return ordered[mid]
    return 0.5 * (ordered[mid - 1] + ordered[mid])


def run_benchmark(args: argparse.Namespace) -> None:
    args.model = resolve_model(args.model)
    rows: list[dict] = []
    clients_list = [int(v) for v in args.clients.split(",") if v.strip()]
    workloads = ["decode", "prefill"] if args.workload == "both" else [args.workload]
    labels = [v.strip() for v in args.labels.split(",") if v.strip()]
    for workload in workloads:
        for clients in clients_list:
            for label in labels:
                for trial in range(1, args.trials + 1):
                    row = benchmark_once(args, workload, label, clients, trial)
                    if (
                        args.expect_prefill_fanout
                        and workload == "prefill"
                        and label == "shared-decode"
                        and clients > 1
                        and row["prefill_fanout"] <= 0
                    ):
                        raise CheckError(
                            "expected shared-decode prefill fanout logs for "
                            f"clients={clients} trial={trial}, saw none"
                        )
                    if (
                        args.expect_prefill_batch
                        and workload == "prefill"
                        and label == "shared-decode"
                        and clients > 1
                        and row["max_prefill_batch"] < 2
                    ):
                        raise CheckError(
                            "expected shared-decode prefill batching for "
                            f"clients={clients} trial={trial}, max_prefill_batch={row['max_prefill_batch']}"
                        )
                    if (
                        args.expect_prefill_chunk
                        and workload == "prefill"
                        and label == "shared-decode"
                        and clients > 1
                        and row["prefill_chunk"] <= 0
                    ):
                        raise CheckError(
                            "expected shared-decode row prefill chunk logs for "
                            f"clients={clients} trial={trial}, saw none"
                        )
                    rows.append(row)
                    print(
                        "server-batch-benchmark: "
                        f"{workload} {label} clients={clients} slots={row['slots']} "
                        f"trial={trial} {row['metric_name']}={row['metric']:.2f} "
                        f"prompt_tokens={row['prompt_tokens']} "
                        f"completion_tokens={row['completion_tokens']} "
                        f"wall={row['wall']:.3f}s reqs_per_sec={row['reqs_per_sec']:.3f} "
                        f"max_batch={row['max_batch']} "
                        f"max_prefill_batch={row['max_prefill_batch']} "
                        f"prefill_fanout={row['prefill_fanout']} "
                        f"prefill_chunk={row['prefill_chunk']}",
                        flush=True,
                    )

    print("server-batch-benchmark: medians")
    summary: dict[tuple[str, str, int], float] = {}
    for workload in workloads:
        metric_name = "prompt_tps" if workload == "prefill" else "completion_tps"
        for clients in clients_list:
            for label in labels:
                vals = [
                    r["metric"]
                    for r in rows
                    if r["workload"] == workload and
                       r["label"] == label and
                       r["clients"] == clients
                ]
                summary[(workload, label, clients)] = median(vals)
                print(
                    f"  {workload:7s} {label:13s} clients={clients} "
                    f"median_{metric_name}={summary[(workload, label, clients)]:.2f}"
                )
            if "serialized" in labels and "shared-decode" in labels:
                base = summary[(workload, "serialized", clients)]
                shared = summary[(workload, "shared-decode", clients)]
                gain = ((shared / base) - 1.0) * 100.0 if base > 0 else 0.0
                print(
                    f"  {workload:7s} shared-decode gain vs serialized "
                    f"clients={clients}: {gain:+.1f}%"
                )


def parse_args(argv: list[str]) -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("mode", choices=["smoke", "benchmark"])
    parser.add_argument("--server", default="./ds4-server")
    parser.add_argument("--model", default=None)
    parser.add_argument("--ctx", type=int, default=4096)
    parser.add_argument("--batch-wait-us", type=int, default=500)
    parser.add_argument("--request-timeout", type=float, default=600.0)
    parser.add_argument("--backend", default="shared-decode")
    parser.add_argument("--smoke-tokens", type=int, default=30)
    parser.add_argument("--workload", choices=["decode", "prefill", "both"], default="decode")
    parser.add_argument("--max-tokens", type=int, default=96)
    parser.add_argument("--prefill-repeats", type=int, default=160)
    parser.add_argument("--prefill-max-tokens", type=int, default=1)
    parser.add_argument("--batch-slots-min", type=int, default=2)
    parser.add_argument("--clients", default="1,2,4,8")
    parser.add_argument("--labels", default="serialized,session-slots,shared-decode")
    parser.add_argument("--trials", type=int, default=3)
    parser.add_argument("--progress", action="store_true")
    parser.add_argument("--experimental-batched-prefill", action="store_true")
    parser.add_argument(
        "--prefill-unique-suffix",
        action="store_true",
        help="For prefill benchmarks, keep the long prefix shared but add a unique per-client suffix.",
    )
    parser.add_argument(
        "--prefill-unique-prefix",
        action="store_true",
        help="For prefill benchmarks, make each prompt diverge near the start.",
    )
    parser.add_argument(
        "--expect-prefill-fanout",
        action="store_true",
        help="Fail shared-decode prefill benchmark trials with clients > 1 unless fanout is observed.",
    )
    parser.add_argument(
        "--expect-prefill-batch",
        action="store_true",
        help="Fail shared-decode prefill benchmark trials with clients > 1 unless any prefill batch is observed.",
    )
    parser.add_argument(
        "--expect-prefill-chunk",
        action="store_true",
        help="Fail shared-decode prefill benchmark trials with clients > 1 unless row prefill chunking is observed.",
    )
    args = parser.parse_args(argv)
    if args.mode == "smoke" and args.backend not in {"shared-decode", "session-slots"}:
        parser.error("--backend must be shared-decode or session-slots")
    if args.trials < 1:
        parser.error("--trials must be >= 1")
    if args.prefill_repeats < 1:
        parser.error("--prefill-repeats must be >= 1")
    if args.prefill_max_tokens < 1:
        parser.error("--prefill-max-tokens must be >= 1")
    if args.batch_slots_min < 2:
        parser.error("--batch-slots-min must be >= 2")
    valid_labels = {"serialized", "session-slots", "shared-decode"}
    labels = [v.strip() for v in args.labels.split(",") if v.strip()]
    if not labels or any(v not in valid_labels for v in labels):
        parser.error("--labels must contain serialized, session-slots, and/or shared-decode")
    return args


def main(argv: list[str]) -> int:
    args = parse_args(argv)
    try:
        if args.mode == "smoke":
            run_smoke(args)
        else:
            run_benchmark(args)
    except CheckError as exc:
        print(f"server-batch-{args.mode}: FAIL: {exc}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
