#!/usr/bin/env bash
set -euo pipefail

usage() {
    cat <<'USAGE'
usage: tools/mtp_gsm8k_gate.sh --mode NAME --dataset JSONL [options] -- [ds4-server MTP args...]

Starts ds4-server, runs a GSM8K slice, and writes JSONL plus summary artifacts.

Options:
  --mode NAME              Label for output files and metadata.
  --dataset PATH_OR_URL    GSM8K JSONL path or URL.
  --out-dir DIR            Artifact directory. Default: /tmp/ds4-gsm8k-<timestamp>
  --limit N                Number of tasks to generate. Default: 20
  --port N                 Server port. Default: 8220
  --max-tokens N           Max generation tokens. Default: 512
  --server-env "A=1 B=2"   Extra environment for ds4-server.
  --base-model PATH        Base GGUF. Default: DS4_QUALITY_BASE_MODEL or ./ds4flash.gguf
  --mtp-model PATH         MTP GGUF. Default: DS4_QUALITY_MTP_MODEL

Example:
  tools/mtp_gsm8k_gate.sh --mode native-k4 --dataset /tmp/gsm8k_test.jsonl \
    --server-env "DS4_MTP_NATIVE=1 DS4_MTP_NATIVE_VERIFY_OPT=smallm" \
    -- --mtp "{MTP}" --mtp-draft 4
USAGE
}

timestamp() {
    date +%Y%m%d_%H%M%S
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

MODE=""
DATASET=""
OUT_DIR=""
LIMIT=20
PORT=8220
MAX_TOKENS=512
SERVER_ENV=""
BASE_MODEL="${DS4_QUALITY_BASE_MODEL:-$PWD/ds4flash.gguf}"
MTP_MODEL="${DS4_QUALITY_MTP_MODEL:-$HOME/.ds4/cache/gguf/DeepSeek-V4-Flash-MTP-Q4K-Q8_0-F32.gguf}"

while [ "$#" -gt 0 ]; do
    case "$1" in
        --mode) MODE="$2"; shift 2 ;;
        --dataset) DATASET="$2"; shift 2 ;;
        --out-dir) OUT_DIR="$2"; shift 2 ;;
        --limit) LIMIT="$2"; shift 2 ;;
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

if [ -z "$MODE" ] || [ -z "$DATASET" ]; then
    echo "--mode and --dataset are required" >&2
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
    OUT_DIR="/tmp/ds4-gsm8k-$(timestamp)"
fi
mkdir -p "$OUT_DIR"

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

SERVER_PID=""
cleanup() {
    if [ -n "${SERVER_PID:-}" ] && kill -0 "$SERVER_PID" 2>/dev/null; then
        kill "$SERVER_PID" 2>/dev/null || true
        wait "$SERVER_PID" 2>/dev/null || true
    fi
}
trap cleanup EXIT

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

python3 tools/gsm8k_ds4.py \
    --dataset "$DATASET" \
    --base-url "http://127.0.0.1:$PORT/v1" \
    --mode "$MODE" \
    --output "$OUT_DIR/$MODE.results.jsonl" \
    --summary-output "$OUT_DIR/$MODE.summary.json" \
    --limit "$LIMIT" \
    --max-tokens "$MAX_TOKENS" \
    | tee "$OUT_DIR/$MODE.run.txt"

echo "artifact_dir=$OUT_DIR"
