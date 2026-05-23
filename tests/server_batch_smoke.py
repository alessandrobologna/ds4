#!/usr/bin/env python3
"""Server-level checks for the experimental public batch API.

The harness deliberately stays on the Python standard library so it can run
from a plain checkout after `make ds4-server`.  These checks exercise the
public batch API through server flags and logs, without reaching into private
graph or tensor internals.
"""

from __future__ import annotations

import argparse
import concurrent.futures
import contextlib
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


class PortInUseError(CheckError):
    pass


def find_free_port() -> int:
    avoid = {8000, 8010}
    extra = os.environ.get("DS4_SERVER_BATCH_AVOID_PORTS", "")
    for item in extra.split(","):
        item = item.strip()
        if item.isdigit():
            avoid.add(int(item))
    for _ in range(128):
        with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as sock:
            sock.bind((HOST, 0))
            port = int(sock.getsockname()[1])
            if port not in avoid:
                return port
    raise CheckError("could not find a free port")


def resolve_model(path: str | None) -> str:
    model = path or os.environ.get("DS4_TEST_MODEL") or "ds4flash.gguf"
    if not Path(model).is_file():
        raise CheckError(
            f"model not found: {model} (pass --model or set DS4_TEST_MODEL)"
        )
    return model


def server_failed_with_address_in_use(lines: list[str]) -> bool:
    return any(
        "failed to listen" in line
        and ("Address already in use" in line or "EADDRINUSE" in line)
        for line in lines
    )


class Server:
    def __init__(
        self,
        argv: list[str],
        port: int,
        env: dict[str, str] | None = None,
        cwd: str | None = None,
    ):
        self.argv = argv
        self.port = port
        self.env = env
        self.cwd = cwd
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
            env=self.env,
            cwd=self.cwd,
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
                if server_failed_with_address_in_use(self.lines):
                    raise PortInUseError("\n".join(self.lines[-40:]))
                raise CheckError(
                    "server exited before listening:\n" + "\n".join(self.lines[-40:])
                )
            if any("listening on http://" in line for line in self.lines):
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


@contextlib.contextmanager
def server_with_retries(
    make_argv,
    env: dict[str, str] | None = None,
    attempts: int = 8,
    cwd: str | None = None,
):
    last_error: PortInUseError | None = None
    for attempt in range(1, attempts + 1):
        port = find_free_port()
        server = Server(make_argv(port), port, env=env, cwd=cwd)
        try:
            server.__enter__()
        except PortInUseError as exc:
            last_error = exc
            server.stop()
            if attempt == attempts:
                break
            time.sleep(0.05 * attempt)
            continue
        try:
            yield server, port
        finally:
            server.__exit__(None, None, None)
        return
    raise last_error or CheckError("could not start server without a port collision")


def server_argv(
    args: argparse.Namespace,
    port: int,
    max_slots: int,
    kv_dir: str | None = None,
    backend: str | None = None,
    tokens: int | None = None,
) -> list[str]:
    if backend is None:
        backend = args.backend
    if tokens is None:
        tokens = args.tokens
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
        "--max-slots",
        str(max_slots),
        "--batch-wait-us",
        str(args.batch_wait_us),
        "--batch-backend",
        backend,
    ]
    if getattr(args, "experimental_batched_prefill", False):
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


def completion_body(prompt: str, max_tokens: int) -> dict:
    return {
        "model": "deepseek-chat",
        "prompt": prompt,
        "max_tokens": max_tokens,
        "temperature": 0,
        "top_p": 1,
        "stream": False,
    }


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


def post_json(port: int, path: str, body: dict, timeout: float) -> bytes:
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


def run_completion(port: int, prompt: str, max_tokens: int, timeout: float) -> dict:
    start = time.monotonic()
    data = post_json(
        port,
        "/v1/completions",
        completion_body(prompt, max_tokens),
        timeout=timeout,
    )
    obj = json.loads(data.decode("utf-8"))
    choices = obj.get("choices") or []
    usage = obj.get("usage") or {}
    completion_tokens = int(usage.get("completion_tokens") or 0)
    if not choices or completion_tokens <= 0:
        raise CheckError(f"completion did not return generated tokens: {obj}")
    return {
        "elapsed": time.monotonic() - start,
        "text": str(choices[0].get("text") or ""),
        "prompt_tokens": int(usage.get("prompt_tokens") or 0),
        "completion_tokens": completion_tokens,
    }


def run_chat(port: int, prompt: str, max_tokens: int, timeout: float) -> dict:
    start = time.monotonic()
    data = post_json(
        port,
        "/v1/chat/completions",
        chat_body(prompt, max_tokens, False),
        timeout=timeout,
    )
    obj = json.loads(data.decode("utf-8"))
    usage = obj.get("usage") or {}
    completion_tokens = int(usage.get("completion_tokens") or 0)
    if completion_tokens <= 0:
        raise CheckError(f"chat completion did not return generated tokens: {obj}")
    return {
        "elapsed": time.monotonic() - start,
        "prompt_tokens": int(usage.get("prompt_tokens") or 0),
        "completion_tokens": completion_tokens,
    }


def stream_chat(port: int, prompt: str, max_tokens: int, timeout: float) -> dict:
    conn = http.client.HTTPConnection(HOST, port, timeout=timeout)
    payload = json.dumps(chat_body(prompt, max_tokens, True)).encode("utf-8")
    events = 0
    completion_tokens = 0
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
            raw = line[len(b"data: ") :].strip()
            if raw == b"[DONE]":
                done = True
                break
            if not raw:
                continue
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
    if events <= 0 or not done:
        raise CheckError(
            f"stream did not complete cleanly: events={events} done={done}"
        )
    return {"events": events, "completion_tokens": completion_tokens}


def cancel_stream_after_first_event(port: int, timeout: float) -> None:
    conn = http.client.HTTPConnection(HOST, port, timeout=timeout)
    payload = json.dumps(
        chat_body("Start a short answer, then keep going briefly.", 8, True)
    ).encode("utf-8")
    conn.request(
        "POST",
        "/v1/chat/completions",
        payload,
        {"Content-Type": "application/json", "Content-Length": str(len(payload))},
    )
    resp = conn.getresponse()
    if resp.status != 200:
        data = resp.read()
        conn.close()
        raise CheckError(f"HTTP {resp.status}: {data[:500]!r}")
    deadline = time.monotonic() + timeout
    saw_event = False
    while time.monotonic() < deadline:
        line = resp.readline()
        if line.startswith(b"data: ") and line.strip() != b"data: [DONE]":
            saw_event = True
            break
        if not line:
            break
    conn.close()
    if not saw_event:
        raise CheckError("stream cancellation probe did not receive an event")


def max_decode_batch(lines: list[str]) -> int:
    best = 0
    for line in lines:
        match = re.search(r"\bbatch=(\d+)\b", line)
        if match:
            best = max(best, int(match.group(1)))
    return best


def max_prefill_batch(lines: list[str]) -> int:
    best = 0
    for line in lines:
        if "prefill=" not in line:
            continue
        match = re.search(r"\bbatch=(\d+)\b", line)
        if match:
            best = max(best, int(match.group(1)))
    return best


def count_logs(lines: list[str], needle: str) -> int:
    return sum(1 for line in lines if needle in line)


def resolve_futures(futures, server: Server, label: str) -> list[dict]:
    results = []
    try:
        for future in futures:
            results.append(future.result())
    except Exception as exc:
        raise CheckError(
            f"{label} request failed: {exc}\n" + "\n".join(server.lines[-100:])
        ) from exc
    return results


def run_parity(args: argparse.Namespace) -> None:
    args.model = resolve_model(args.model)
    prompt = "Deterministically complete this tiny phrase: local batch parity"
    results: dict[int, dict] = {}
    for max_slots in (1, 2):
        def make_argv(port: int, slots=max_slots) -> list[str]:
            return server_argv(args, port, slots)

        with server_with_retries(make_argv) as (_server, port):
            results[max_slots] = run_completion(
                port, prompt, args.tokens, args.request_timeout
            )
    if results[1]["text"] != results[2]["text"]:
        raise CheckError(f"--max-slots parity mismatch: {results}")
    print(
        "server-batch-parity: OK "
        f"text={results[1]['text']!r} tokens={results[1]['completion_tokens']}"
    )


def run_smoke(args: argparse.Namespace) -> None:
    args.model = resolve_model(args.model)

    def make_argv(port: int) -> list[str]:
        return server_argv(args, port, 2)

    with server_with_retries(make_argv) as (server, port):
        prompts = [
            "Say hi in one word:",
            "Name one color:",
        ]
        with concurrent.futures.ThreadPoolExecutor(max_workers=2) as pool:
            futures = [
                pool.submit(run_completion, port, prompt, 2, args.request_timeout)
                for prompt in prompts
            ]
            results = [future.result() for future in futures]
        misses_after_concurrent = [
            line for line in server.lines if "live kv cache miss" in line
        ]
        stream = stream_chat(port, "Reply with a two word greeting.", 2, args.request_timeout)
        expected_backend = re.escape(args.backend)
        if not server.wait_for_log(rf"experimental batch backend={expected_backend} slots=2", timeout=2):
            raise CheckError(f"server did not log {args.backend} batch startup")
        if misses_after_concurrent:
            raise CheckError(
                "independent concurrent requests overwrote a live slot:\n"
                + "\n".join(misses_after_concurrent[-20:])
            )
        max_batch = max_decode_batch(server.lines)
        if max_batch < 2:
            raise CheckError(
                "concurrent requests did not coalesce into a multi-slot decode step:\n"
                + "\n".join(server.lines[-80:])
            )
        print(
            "server-batch-smoke: OK "
            f"texts={[r['text'] for r in results]} stream_events={stream['events']} "
            f"max_batch={max_batch}"
        )


def run_cancel(args: argparse.Namespace) -> None:
    args.model = resolve_model(args.model)

    def make_argv(port: int) -> list[str]:
        return server_argv(args, port, 2)

    with server_with_retries(make_argv) as (_server, port):
        cancel_stream_after_first_event(port, args.request_timeout)
        result = run_completion(
            port,
            "After a cancelled stream, answer with one token:",
            1,
            args.request_timeout,
        )
        print(
            "server-batch-cancel: OK "
            f"text={result['text']!r} tokens={result['completion_tokens']}"
        )


def run_disk_cache(args: argparse.Namespace) -> None:
    args.model = resolve_model(args.model)
    prompts = [
        (
            "Disk cache alpha prompt. " * 16
            + "Answer with one deterministic token for alpha."
        ),
        (
            "Disk cache beta prompt. " * 16
            + "Answer with one deterministic token for beta."
        ),
    ]

    with tempfile.TemporaryDirectory(prefix="ds4-batch-kv-smoke-") as tmp:
        kv_dir = str(Path(tmp) / "kv")

        def make_argv(port: int) -> list[str]:
            return server_argv(args, port, 2, kv_dir=kv_dir)

        with server_with_retries(make_argv) as (server, port):
            for prompt in prompts:
                run_completion(port, prompt, 1, args.request_timeout)
            if not server.wait_for_log(r"kv cache stored", timeout=20):
                raise CheckError("warmup requests did not store disk KV")
            stores = sum(1 for line in server.lines if "kv cache stored" in line)
            if stores < 2:
                raise CheckError(f"expected two disk KV stores, saw {stores}")

        with server_with_retries(make_argv) as (server, port):
            with concurrent.futures.ThreadPoolExecutor(max_workers=2) as pool:
                futures = [
                    pool.submit(run_completion, port, prompt, 1, args.request_timeout)
                    for prompt in prompts
                ]
                results = [future.result() for future in futures]
            hits = sum(1 for line in server.lines if "kv cache hit" in line)
            if hits < 2:
                raise CheckError(
                    f"expected two disk KV hits in multi-slot mode, saw {hits}\n"
                    + "\n".join(server.lines[-80:])
                )
            print(
                "server-batch-disk-cache: OK "
                f"hits={hits} texts={[r['text'] for r in results]}"
            )


def make_prefill_prompt(seed: str) -> str:
    return (
        f"{seed}: "
        + "This prompt is deliberately long enough for the experimental "
          "batched prefill scheduler to have visible segment work. "
        * 6
        + "Answer with one compact word."
    )


def make_prefill_body(repeats: int) -> str:
    return "\n".join(f"{i:04d}. {PREFILL_UNIT}" for i in range(repeats))


def run_batched_prefill(args: argparse.Namespace) -> None:
    args.model = resolve_model(args.model)
    old_experimental = args.experimental_batched_prefill
    args.experimental_batched_prefill = True
    env = os.environ.copy()
    env.setdefault("DS4_BATCH_PREFILL_FANOUT_MIN_TOKENS", "16")
    env.setdefault("DS4_BATCH_PREFILL_STEP_LIMIT_TOKENS", "256")
    env.setdefault("DS4_BATCH_PREFILL_WAIT_US", "100000")
    env["DS4_BATCH_SESSION_SEGMENTED_PREFILL"] = "0"
    try:
        def make_argv(port: int) -> list[str]:
            return server_argv(args, port, 2)

        segment_prompts = [
            make_prefill_prompt("prefill alpha"),
            make_prefill_prompt("prefill beta"),
        ]
        with server_with_retries(make_argv, env=env) as (server, port):
            with concurrent.futures.ThreadPoolExecutor(max_workers=2) as pool:
                futures = [
                    pool.submit(run_completion, port, prompt, 1, args.request_timeout)
                    for prompt in segment_prompts
                ]
                results = resolve_futures(futures, server, "segmented prefill")
            segment_logs = count_logs(server.lines, "prefill=segment")
            if segment_logs > 0:
                raise CheckError(
                    "session-slots default unexpectedly used segmented prefill:\n"
                    + "\n".join(server.lines[-80:])
                )
            if any(r["completion_tokens"] <= 0 for r in results):
                raise CheckError(f"default prefill requests did not complete: {results}")

        segmented_env = env.copy()
        segmented_env["DS4_BATCH_SESSION_SEGMENTED_PREFILL"] = "1"
        with server_with_retries(make_argv, env=segmented_env) as (server, port):
            with concurrent.futures.ThreadPoolExecutor(max_workers=2) as pool:
                futures = [
                    pool.submit(run_completion, port, prompt, 1, args.request_timeout)
                    for prompt in segment_prompts
                ]
                results = resolve_futures(futures, server, "segmented prefill")
            segment_logs = count_logs(server.lines, "prefill=segment")
            if segment_logs <= 0:
                raise CheckError(
                    "opt-in session-slots batched prefill did not use segment API:\n"
                    + "\n".join(server.lines[-80:])
                )
            if any(r["completion_tokens"] <= 0 for r in results):
                raise CheckError(f"opt-in batched prefill requests did not complete: {results}")

        def make_shared_argv(port: int) -> list[str]:
            return server_argv(args, port, 2, backend="shared-decode")

        with server_with_retries(make_shared_argv, env=env) as (server, port):
            previous_segments = 0
            for label in ("first", "reused"):
                prompts = [
                    make_prefill_prompt(f"shared {label} gamma"),
                    make_prefill_prompt(f"shared {label} delta"),
                ]
                with concurrent.futures.ThreadPoolExecutor(max_workers=2) as pool:
                    futures = [
                        pool.submit(run_completion, port, prompt, 1, args.request_timeout)
                        for prompt in prompts
                    ]
                    shared_results = resolve_futures(futures, server, f"{label} shared prefill")
                shared_segments = count_logs(server.lines, "prefill=segment")
                if shared_segments <= previous_segments:
                    raise CheckError(
                        f"{label} shared-decode batched prefill did not use segment API:\n"
                        + "\n".join(server.lines[-80:])
                    )
                if any(r["completion_tokens"] <= 0 for r in shared_results):
                    raise CheckError(f"{label} shared-decode prefill requests did not complete")
                previous_segments = shared_segments

        with server_with_retries(make_argv, env=env) as (server, port):
            shared = make_prefill_prompt("shared prefill fanout")
            fanout_prompts = [
                shared + f"\nUnique fanout suffix for client {i}: keep this request distinct."
                for i in range(2)
            ]
            with concurrent.futures.ThreadPoolExecutor(max_workers=2) as pool:
                futures = [
                    pool.submit(run_completion, port, prompt, 1, args.request_timeout)
                    for prompt in fanout_prompts
                ]
                fanout_results = resolve_futures(futures, server, "fanout prefill")
            fanout_logs = count_logs(server.lines, "prefill=fanout")
            if fanout_logs <= 0:
                raise CheckError(
                    "experimental batched prefill did not use shared-prefix fanout:\n"
                    + "\n".join(server.lines[-80:])
                )
            if any(r["completion_tokens"] <= 0 for r in fanout_results):
                raise CheckError(f"fanout prefill requests did not complete: {fanout_results}")

        with tempfile.TemporaryDirectory(prefix="ds4-batch-prefill-kv-") as tmp:
            kv_dir = str(Path(tmp) / "kv")

            def make_kv_argv(port: int) -> list[str]:
                return server_argv(args, port, 2, kv_dir=kv_dir)

            with server_with_retries(make_kv_argv, env=env) as (server, port):
                prefixes = [
                    "Cached prefill alpha. " * 12 + "Answer with one compact word.",
                    "Cached prefill beta. " * 12 + "Answer with one compact word.",
                ]
                for prompt in prefixes:
                    run_completion(port, prompt, 1, args.request_timeout)
                if not server.wait_for_log(r"kv cache stored", timeout=20):
                    raise CheckError("prefill disk-cache warmup did not store KV")
                stores = count_logs(server.lines, "kv cache stored")
                if stores < 2:
                    raise CheckError(f"expected at least two prefill KV stores, saw {stores}")

                hits_before = count_logs(server.lines, "kv cache hit")
                with concurrent.futures.ThreadPoolExecutor(max_workers=2) as pool:
                    futures = [
                        pool.submit(run_completion, port, prompt, 1, args.request_timeout)
                        for prompt in prefixes
                    ]
                    cached_results = resolve_futures(futures, server, "cached prefill")
                kv_hits = count_logs(server.lines, "kv cache hit") - hits_before
                if kv_hits < 2:
                    raise CheckError(
                        f"expected two prefill KV hits, saw {kv_hits}\n"
                        + "\n".join(server.lines[-80:])
                    )
                if any(r["completion_tokens"] <= 0 for r in cached_results):
                    raise CheckError("cached prefill requests did not complete")
            print(
                "server-batch-prefill: OK "
                f"fanout={fanout_logs} segments={segment_logs} "
                f"kv_hits={kv_hits} "
                f"texts={[r['text'] for r in fanout_results + results]}"
            )
    finally:
        args.experimental_batched_prefill = old_experimental


def benchmark_workload(args: argparse.Namespace, workload: str) -> tuple[str, int]:
    if workload == "decode":
        return BENCH_PROMPT, args.max_tokens
    if workload == "prefill":
        return make_prefill_body(args.prefill_repeats), args.prefill_max_tokens
    if workload == "mixed":
        return (
            make_prefill_body(args.prefill_repeats)
            + "\n\n"
            + BENCH_PROMPT,
            args.max_tokens,
        )
    raise CheckError(f"unknown benchmark workload: {workload}")


def benchmark_prompts(args: argparse.Namespace, workload: str, clients: int) -> tuple[list[str], int]:
    prompt, max_tokens = benchmark_workload(args, workload)
    prompts = [prompt for _ in range(clients)]
    if workload in {"prefill", "mixed"} and args.prefill_unique_prefix:
        prompts = [
            f"Unique request prefix {i:02d}: this request diverges before the long body.\n{prompt}"
            for i in range(clients)
        ]
    if workload in {"prefill", "mixed"} and args.prefill_unique_suffix:
        prompts = [
            prompt + f"\nUnique request suffix {i:02d}: answer for client {i} only after the shared context."
            for i in range(clients)
        ]
    return prompts, max_tokens


def benchmark_once(
    args: argparse.Namespace, workload: str, label: str, clients: int, trial: int
) -> dict:
    if label == "serialized":
        max_slots = 1
        backend = args.backend
    else:
        max_slots = max(clients, args.batch_slots_min)
        backend = label
    prompts, max_tokens = benchmark_prompts(args, workload, clients)
    old_experimental = args.experimental_batched_prefill
    if workload in {"prefill", "mixed"}:
        args.experimental_batched_prefill = True
    env = os.environ.copy()
    env.setdefault("DS4_BATCH_PREFILL_FANOUT_MIN_TOKENS", "16")
    env.setdefault("DS4_BATCH_PREFILL_STEP_LIMIT_TOKENS", "256")
    env.setdefault("DS4_BATCH_PREFILL_WAIT_US", "100000")
    try:
        def make_argv(port: int) -> list[str]:
            return server_argv(
                args,
                port,
                max_slots=max_slots,
                backend=backend,
                tokens=max_tokens,
            )

        with server_with_retries(make_argv, env=env) as (server, port):
            started = time.monotonic()
            with concurrent.futures.ThreadPoolExecutor(max_workers=clients) as pool:
                futures = {
                    pool.submit(run_chat, port, prompts[i], max_tokens, args.request_timeout): i
                    for i in range(clients)
                }
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
                    for future in futures:
                        future.cancel()
                    raise CheckError(
                        f"benchmark timed out workload={workload} label={label} "
                        f"clients={clients} completed={len(results)}/{clients}\n"
                        + "\n".join(server.lines[-80:])
                    ) from exc
            wall = time.monotonic() - started
            prompt_tokens = sum(int(r["prompt_tokens"]) for r in results)
            completion_tokens = sum(int(r["completion_tokens"]) for r in results)
            prompt_tps = prompt_tokens / wall if wall > 0 else 0.0
            completion_tps = completion_tokens / wall if wall > 0 else 0.0
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
                "prompt_tps": prompt_tps,
                "completion_tps": completion_tps,
                "reqs_per_sec": clients / wall if wall > 0 else 0.0,
                "metric_name": metric_name,
                "metric": metric,
                "max_batch": max_decode_batch(server.lines),
                "max_prefill_batch": max_prefill_batch(server.lines),
                "prefill_fanout": count_logs(server.lines, "prefill=fanout"),
                "prefill_chunk": count_logs(server.lines, "prefill=chunk"),
                "prefill_segment": count_logs(server.lines, "prefill=segment"),
            }
    finally:
        args.experimental_batched_prefill = old_experimental


def median(values: list[float]) -> float:
    ordered = sorted(values)
    n = len(ordered)
    if n == 0:
        return 0.0
    mid = n // 2
    if n % 2:
        return ordered[mid]
    return 0.5 * (ordered[mid - 1] + ordered[mid])


def run_benchmark(args: argparse.Namespace) -> None:
    args.model = resolve_model(args.model)
    rows: list[dict] = []
    workloads = ["decode", "prefill"] if args.workload == "both" else [args.workload]
    labels = [v.strip() for v in args.labels.split(",") if v.strip()]
    for workload in workloads:
        for clients in args.clients:
            for label in labels:
                for trial in range(1, args.trials + 1):
                    row = benchmark_once(args, workload, label, clients, trial)
                    if (
                        args.expect_prefill_fanout and
                        workload in {"prefill", "mixed"} and
                        label == "shared-decode" and
                        clients > 1 and
                        row["prefill_fanout"] <= 0
                    ):
                        raise CheckError(
                            "expected shared-decode prefill fanout logs for "
                            f"clients={clients} trial={trial}, saw none"
                        )
                    if (
                        args.expect_prefill_batch and
                        workload in {"prefill", "mixed"} and
                        label == "shared-decode" and
                        clients > 1 and
                        row["max_prefill_batch"] < 2
                    ):
                        raise CheckError(
                            "expected shared-decode prefill batching for "
                            f"clients={clients} trial={trial}, max_prefill_batch={row['max_prefill_batch']}"
                        )
                    if (
                        args.expect_prefill_chunk and
                        workload in {"prefill", "mixed"} and
                        label == "shared-decode" and
                        clients > 1 and
                        row["prefill_chunk"] + row["prefill_segment"] <= 0
                    ):
                        raise CheckError(
                            "expected shared-decode row prefill chunk/segment logs for "
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
                        f"prefill_chunk={row['prefill_chunk']} "
                        f"prefill_segment={row['prefill_segment']}",
                        flush=True,
                    )

    print("server-batch-benchmark: medians")
    summary: dict[tuple[str, str, int], float] = {}
    for workload in workloads:
        metric_name = "prompt_tps" if workload == "prefill" else "completion_tps"
        for clients in args.clients:
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
    parser.add_argument(
        "mode",
        choices=[
            "all",
            "smoke",
            "parity",
            "cancel",
            "disk-cache",
            "batched-prefill",
            "prefill-smoke",
            "benchmark",
        ],
    )
    parser.add_argument("--server", default="./ds4-server")
    parser.add_argument("--model")
    parser.add_argument("--ctx", type=int, default=256)
    parser.add_argument("--tokens", type=int, default=2)
    parser.add_argument("--backend", default="session-slots")
    parser.add_argument("--batch-wait-us", type=int, default=500)
    parser.add_argument("--experimental-batched-prefill", action="store_true")
    parser.add_argument("--request-timeout", type=float, default=180.0)
    parser.add_argument("--workload", choices=["decode", "prefill", "mixed", "both"], default="decode")
    parser.add_argument("--max-tokens", type=int, default=96)
    parser.add_argument("--prefill-repeats", type=int, default=80)
    parser.add_argument("--prefill-max-tokens", type=int, default=1)
    parser.add_argument("--batch-slots-min", type=int, default=2)
    parser.add_argument("--clients", default="1,2,4")
    parser.add_argument("--labels", default="serialized,session-slots,shared-decode")
    parser.add_argument("--trials", type=int, default=3)
    parser.add_argument("--progress", action="store_true")
    parser.add_argument("--prefill-unique-suffix", action="store_true")
    parser.add_argument("--prefill-unique-prefix", action="store_true")
    parser.add_argument("--expect-prefill-fanout", action="store_true")
    parser.add_argument("--expect-prefill-batch", action="store_true")
    parser.add_argument("--expect-prefill-chunk", action="store_true")
    args = parser.parse_args(argv)
    if args.trials < 1:
        parser.error("--trials must be >= 1")
    if args.prefill_repeats < 1:
        parser.error("--prefill-repeats must be >= 1")
    if args.prefill_max_tokens < 1:
        parser.error("--prefill-max-tokens must be >= 1")
    if args.batch_slots_min < 2:
        parser.error("--batch-slots-min must be >= 2")
    clients: list[int] = []
    for item in str(args.clients).split(","):
        item = item.strip()
        if not item:
            parser.error("--clients must be a comma-separated list of integers >= 1")
        try:
            value = int(item)
        except ValueError:
            parser.error("--clients must be a comma-separated list of integers >= 1")
        if value < 1:
            parser.error("--clients must be a comma-separated list of integers >= 1")
        clients.append(value)
    args.clients = clients
    valid_labels = {"serialized", "session-slots", "shared-decode"}
    labels = [v.strip() for v in args.labels.split(",") if v.strip()]
    if not labels or any(v not in valid_labels for v in labels):
        parser.error("--labels must contain serialized, session-slots, and/or shared-decode")
    return args


def main(argv: list[str]) -> int:
    args = parse_args(argv)
    try:
        if args.mode in {"all", "parity"}:
            run_parity(args)
        if args.mode in {"all", "smoke"}:
            run_smoke(args)
        if args.mode in {"all", "cancel"}:
            run_cancel(args)
        if args.mode in {"all", "disk-cache"}:
            run_disk_cache(args)
        if args.mode in {"all", "batched-prefill", "prefill-smoke"}:
            run_batched_prefill(args)
        if args.mode == "benchmark":
            run_benchmark(args)
    except CheckError as exc:
        print(f"server-batch-smoke: FAIL: {exc}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
