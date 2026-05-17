#!/usr/bin/env bash
set -euo pipefail

usage() {
    cat <<'USAGE'
usage: tools/mtp_quality_gate.sh --mode NAME [options] -- [ds4-server MTP args...]

Starts ds4-server, generates a small EvalPlus sample set, runs syntax and
subset HumanEval+/MBPP+ checks, and writes a per-mode artifact directory.

Options:
  --mode NAME              Label for output files and metadata.
  --out-dir DIR            Artifact directory. Default: /tmp/ds4-mtp-quality-<timestamp>
  --limit N                Number of tasks to generate. Default: 10
  --task-ids IDS           Comma-separated EvalPlus task IDs. Overrides --limit.
  --dataset NAME           humaneval or mbpp. Default: humaneval
  --api NAME               chat or completions. Default: chat
  --port N                 Server port. Default: 8120
  --max-tokens N           Max generation tokens. Default: 1024
  --server-env "A=1 B=2"   Extra environment for ds4-server.
  --base-model PATH        Base GGUF. Default: DS4_QUALITY_BASE_MODEL or ./ds4flash.gguf
  --mtp-model PATH         MTP GGUF. Default: DS4_QUALITY_MTP_MODEL

Examples:
  tools/mtp_quality_gate.sh --mode serial --limit 20

  tools/mtp_quality_gate.sh --mode block-k16-lagged --limit 20 \
    --server-env "DS4_MTP_BLOCK_VERIFY=1 DS4_MTP_BLOCK_CHUNK_VERIFY=1 DS4_MTP_BLOCK_LAGGED_CACHE=1" \
    -- --mtp "{MTP}" --mtp-draft 16
USAGE
}

timestamp() {
    date +%Y%m%d%H%M%S
}

wait_port() {
    local port="$1"
    python3 - "$port" <<'PY'
import socket
import sys
import time

port = int(sys.argv[1])
deadline = time.time() + 120
last = None
while time.time() < deadline:
    try:
        with socket.create_connection(("127.0.0.1", port), timeout=0.5):
            sys.exit(0)
    except OSError as exc:
        last = exc
        time.sleep(0.5)
print(f"port {port} did not open: {last}", file=sys.stderr)
sys.exit(1)
PY
}

syntax_check() {
    local samples="$1"
    local out="$2"
    python3 - "$samples" "$out" <<'PY'
import ast
import json
import sys

samples_path, out_path = sys.argv[1], sys.argv[2]
ok = 0
total = 0
failed = []
with open(samples_path, encoding="utf-8") as fp:
    for line in fp:
        row = json.loads(line)
        total += 1
        try:
            ast.parse(row["solution"])
            ok += 1
        except SyntaxError:
            failed.append(row["task_id"])
result = {"syntax_ok": ok, "syntax_total": total, "failed_syntax": failed}
with open(out_path, "w", encoding="utf-8") as fp:
    json.dump(result, fp, indent=2, sort_keys=True)
    fp.write("\n")
print(f"syntax_ok {ok}/{total}")
PY
}

python_evalplus() {
    if python3 - "$@" <<'PY' >/dev/null 2>&1
import evalplus  # noqa: F401
PY
    then
        python3 "$@"
    else
        uv run --with evalplus python "$@"
    fi
}

MODE=""
OUT_DIR=""
LIMIT=10
TASK_IDS=""
DATASET="humaneval"
API="chat"
PORT=8120
MAX_TOKENS=1024
SERVER_ENV=""
BASE_MODEL="${DS4_QUALITY_BASE_MODEL:-$PWD/ds4flash.gguf}"
MTP_MODEL="${DS4_QUALITY_MTP_MODEL:-$HOME/.ds4/cache/gguf/DeepSeek-V4-Flash-MTP-Q4K-Q8_0-F32.gguf}"

while [ "$#" -gt 0 ]; do
    case "$1" in
        --mode) MODE="$2"; shift 2 ;;
        --out-dir) OUT_DIR="$2"; shift 2 ;;
        --limit) LIMIT="$2"; shift 2 ;;
        --task-ids) TASK_IDS="$2"; shift 2 ;;
        --dataset) DATASET="$2"; shift 2 ;;
        --api) API="$2"; shift 2 ;;
        --port) PORT="$2"; shift 2 ;;
        --max-tokens) MAX_TOKENS="$2"; shift 2 ;;
        --server-env) SERVER_ENV="$2"; shift 2 ;;
        --base-model) BASE_MODEL="$2"; shift 2 ;;
        --mtp-model) MTP_MODEL="$2"; shift 2 ;;
        -h|--help) usage; exit 0 ;;
        --) shift; break ;;
        *) echo "unknown option: $1" >&2; usage >&2; exit 2 ;;
    esac
done

if [ -z "$MODE" ]; then
    echo "--mode is required" >&2
    usage >&2
    exit 2
fi
if [ ! -x ./ds4-server ]; then
    echo "missing ./ds4-server; build it first with: make ds4-server" >&2
    exit 1
fi
if [ ! -f "$BASE_MODEL" ]; then
    echo "base model not found: $BASE_MODEL" >&2
    exit 1
fi

if [ -z "$OUT_DIR" ]; then
    OUT_DIR="/tmp/ds4-mtp-quality-$(timestamp)"
fi
mkdir -p "$OUT_DIR"

SERVER_PID=""
cleanup() {
    if [ -n "${SERVER_PID:-}" ] && kill -0 "$SERVER_PID" 2>/dev/null; then
        kill "$SERVER_PID" 2>/dev/null || true
        wait "$SERVER_PID" 2>/dev/null || true
    fi
}
trap cleanup EXIT

SERVER_ARGS=()
while [ "$#" -gt 0 ]; do
    SERVER_ARGS+=("$1")
    shift
done
for i in "${!SERVER_ARGS[@]}"; do
    if [ "${SERVER_ARGS[$i]}" = "{MTP}" ]; then
        SERVER_ARGS[$i]="$MTP_MODEL"
    fi
done

echo "artifact_dir=$OUT_DIR"
echo "mode=$MODE"
echo "server_env=$SERVER_ENV"
printf '%s\n' "$SERVER_ENV" > "$OUT_DIR/$MODE.server-env.txt"
if [ "${#SERVER_ARGS[@]}" -gt 0 ]; then
    printf '%s\n' "${SERVER_ARGS[@]}" > "$OUT_DIR/$MODE.server-args.txt"
else
    : > "$OUT_DIR/$MODE.server-args.txt"
fi

if [ -n "$SERVER_ENV" ]; then
    if [ "${#SERVER_ARGS[@]}" -gt 0 ]; then
        # shellcheck disable=SC2086
        env $SERVER_ENV ./ds4-server -m "$BASE_MODEL" "${SERVER_ARGS[@]}" \
            --ctx 32768 --port "$PORT" -n "$MAX_TOKENS" \
            >"$OUT_DIR/$MODE.server.out" 2>"$OUT_DIR/$MODE.server.err" &
    else
        # shellcheck disable=SC2086
        env $SERVER_ENV ./ds4-server -m "$BASE_MODEL" \
            --ctx 32768 --port "$PORT" -n "$MAX_TOKENS" \
            >"$OUT_DIR/$MODE.server.out" 2>"$OUT_DIR/$MODE.server.err" &
    fi
else
    if [ "${#SERVER_ARGS[@]}" -gt 0 ]; then
        ./ds4-server -m "$BASE_MODEL" "${SERVER_ARGS[@]}" \
            --ctx 32768 --port "$PORT" -n "$MAX_TOKENS" \
            >"$OUT_DIR/$MODE.server.out" 2>"$OUT_DIR/$MODE.server.err" &
    else
        ./ds4-server -m "$BASE_MODEL" \
            --ctx 32768 --port "$PORT" -n "$MAX_TOKENS" \
            >"$OUT_DIR/$MODE.server.out" 2>"$OUT_DIR/$MODE.server.err" &
    fi
fi
SERVER_PID=$!
wait_port "$PORT"

SAMPLES="$OUT_DIR/$MODE.jsonl"
META="$OUT_DIR/$MODE.meta.jsonl"
SUMMARY="$OUT_DIR/$MODE.summary.json"
SYNTAX="$OUT_DIR/$MODE.syntax.json"
SUBSET="$OUT_DIR/$MODE.subset.json"

GEN_ARGS=(
    tools/evalplus_ds4.py
    --dataset "$DATASET"
    --api "$API"
    --mode "$MODE"
    --base-url "http://127.0.0.1:$PORT/v1"
    --output "$SAMPLES"
    --metadata-output "$META"
    --summary-output "$SUMMARY"
    --max-tokens "$MAX_TOKENS"
)
if [ -n "$TASK_IDS" ]; then
    GEN_ARGS+=(--task-ids "$TASK_IDS")
else
    GEN_ARGS+=(--limit "$LIMIT")
fi

python_evalplus "${GEN_ARGS[@]}" 2>"$OUT_DIR/$MODE.generator.err"

syntax_check "$SAMPLES" "$SYNTAX" | tee "$OUT_DIR/$MODE.syntax.txt"

EVALPLUS_MAX_MEMORY_BYTES=-1 python_evalplus tools/evalplus_subset_evaluate.py "$DATASET" \
    --samples "$SAMPLES" \
    --mini \
    --parallel 8 \
    --output "$SUBSET" \
    >"$OUT_DIR/$MODE.subset.txt" 2>"$OUT_DIR/$MODE.subset.err"
cat "$OUT_DIR/$MODE.subset.txt"

tools/evalplus_tps_summary.py "$META" | tee "$OUT_DIR/$MODE.tps.txt"
echo "artifact_dir=$OUT_DIR"
