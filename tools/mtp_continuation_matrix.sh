#!/usr/bin/env bash
set -euo pipefail

usage() {
    cat <<'USAGE'
usage: tools/mtp_continuation_matrix.sh --model MODEL --mtp MTP [options]

Runs the native K=4 continuation comparison matrix for count/explain/code:
serial, native K4, lazy continuation baseline, fused-input continuation,
fixed-chain continuation, state-only continuation, and state-reuse continuation.

Options:
  --model PATH          Target GGUF.
  --mtp PATH            MTP GGUF.
  --prompt-dir DIR      Directory with count.txt, explain.txt, code.txt.
                        Default: /tmp/ds4-mtp-matrix.
  --out-dir DIR         Artifact directory.
  --prompts "NAMES"     Space-separated prompts. Default: "count explain code".
  -n N                  Generated tokens. Default: 512.
  --validate            Enable DS4_MTP_NATIVE_VALIDATE=1.
  --no-timing           Disable DS4_MTP_NATIVE_TIMING=1.
  --stage-profile       Enable DS4_MTP_NATIVE_CONT_STAGE_PROFILE=1.
  --mtp-top1-only       Enable DS4_MTP_NATIVE_MTP_TOP1_ONLY=1.
  -h, --help            Show this help.
USAGE
}

timestamp() {
    date +%Y%m%d_%H%M%S
}

parse_tps() {
    python3 - "$1" <<'PY'
import re
import sys

text = open(sys.argv[1], encoding="utf-8", errors="replace").read()
values = re.findall(r"generation:\s*([0-9]+(?:\.[0-9]+)?)\s*t/s", text)
print(values[-1] if values else "")
PY
}

parse_native_metrics() {
    python3 - "$1" <<'PY'
import re
import statistics
import sys

text = open(sys.argv[1], encoding="utf-8", errors="replace").read()
lines = [line for line in text.splitlines()
         if "ds4: mtp native" in line and " cycle " in line]

def nums(name):
    out = []
    pattern = rf"\b{name}=(-?[0-9]+(?:\.[0-9]+)?)"
    for line in lines:
        match = re.search(pattern, line)
        if match:
            out.append(float(match.group(1)))
    return out

def ints(name):
    return [int(v) for v in nums(name)]

def avg(values):
    return f"{statistics.fmean(values):.3f}" if values else ""

accepted = nums("accepted")
cont_gpu = nums("sched2_cont_mtp_gpu")
started = ints("sched2_cont_started")
finished = ints("sched2_cont_finished")
stored = ints("native_cont_stored")
used = ints("native_cont_used")
dropped = ints("native_cont_dropped")
mismatches = ints("mismatches")

print("\t".join([
    str(len(lines)),
    avg(accepted),
    avg(cont_gpu),
    str(sum(started)),
    str(sum(finished)),
    str(sum(stored)),
    str(sum(used)),
    str(sum(dropped)),
    str(sum(mismatches)) if mismatches else "",
]))
PY
}

MODEL=""
MTP=""
PROMPT_DIR="/tmp/ds4-mtp-matrix"
OUT_DIR=""
PROMPTS="count explain code"
N_GEN=512
VALIDATE=0
TIMING=1
STAGE_PROFILE=0
MTP_TOP1_ONLY=0

while [ "$#" -gt 0 ]; do
    case "$1" in
        --model) MODEL="$2"; shift 2 ;;
        --mtp) MTP="$2"; shift 2 ;;
        --prompt-dir) PROMPT_DIR="$2"; shift 2 ;;
        --out-dir) OUT_DIR="$2"; shift 2 ;;
        --prompts) PROMPTS="$2"; shift 2 ;;
        -n) N_GEN="$2"; shift 2 ;;
        --validate) VALIDATE=1; shift ;;
        --no-timing) TIMING=0; shift ;;
        --stage-profile) STAGE_PROFILE=1; shift ;;
        --mtp-top1-only) MTP_TOP1_ONLY=1; shift ;;
        -h|--help) usage; exit 0 ;;
        *) echo "unknown option: $1" >&2; usage >&2; exit 2 ;;
    esac
done

if [ -z "$MODEL" ] || [ ! -f "$MODEL" ]; then
    echo "model not found: ${MODEL:-<empty>}" >&2
    exit 1
fi
if [ -z "$MTP" ] || [ ! -f "$MTP" ]; then
    echo "mtp model not found: ${MTP:-<empty>}" >&2
    exit 1
fi
if [ ! -x ./ds4 ]; then
    echo "missing ./ds4; build it first with make ds4" >&2
    exit 1
fi
if [ -z "$OUT_DIR" ]; then
    OUT_DIR="/tmp/ds4-native-continuation-matrix-$(timestamp)"
fi
mkdir -p "$OUT_DIR"

SUMMARY="$OUT_DIR/summary.tsv"
printf "prompt\tmode\ttps\tstdout\tcycles\tavg_accepted\tavg_cont_mtp_gpu_ms\tcont_started\tcont_finished\tcont_stored\tcont_used\tcont_dropped\tmismatches\tartifact\n" > "$SUMMARY"

run_serial() {
    local prompt="$1"
    local base="$OUT_DIR/$prompt.serial"
    ./ds4 --metal -m "$MODEL" \
        --ctx 1024 --nothink -sys "" --temp 0 -n "$N_GEN" \
        --prompt-file "$PROMPT_DIR/$prompt.txt" \
        > "$base.out" 2> "$base.err"
    local tps
    tps="$(parse_tps "$base.err")"
    printf "%s\tserial\t%s\toracle\t\t\t\t\t\t\t\t\t\t%s\n" \
        "$prompt" "$tps" "$base" >> "$SUMMARY"
}

run_native_mode() {
    local prompt="$1"
    local mode="$2"
    local cont_m="$3"
    local fused_input="$4"
    local state_only="${5:-0}"
    local reuse_state="${6:-0}"
    local fused_chain="${7:-0}"
    local base="$OUT_DIR/$prompt.$mode"
    local envs=(
        DS4_MTP_NATIVE=1
        DS4_MTP_NATIVE_VERIFY_OPT=smallm
    )
    if [ "$TIMING" = 1 ]; then
        envs+=(DS4_MTP_NATIVE_TIMING=1)
    fi
    if [ "$VALIDATE" = 1 ]; then
        envs+=(DS4_MTP_NATIVE_VALIDATE=1)
    fi
    if [ "$STAGE_PROFILE" = 1 ]; then
        envs+=(DS4_MTP_NATIVE_CONT_STAGE_PROFILE=1)
    fi
    if [ "$MTP_TOP1_ONLY" = 1 ]; then
        envs+=(DS4_MTP_NATIVE_MTP_TOP1_ONLY=1)
    fi
    if [ "$cont_m" != "0" ]; then
        envs+=(
            DS4_MTP_NATIVE_TARGET_FIRST_CONT=1
            "DS4_MTP_NATIVE_SCHED2_CONT_M=$cont_m"
            DS4_MTP_NATIVE_CONT_SKIP_FINAL_TOP=1
            DS4_MTP_NATIVE_CONT_LAZY_TAIL=1
        )
    fi
    if [ "$fused_input" = "1" ]; then
        envs+=(DS4_MTP_NATIVE_CONT_FUSED_INPUT=1)
    fi
    if [ "$state_only" = "1" ]; then
        envs+=(DS4_MTP_NATIVE_CONT_STATE_ONLY=1)
    fi
    if [ "$reuse_state" = "1" ]; then
        envs+=(DS4_MTP_NATIVE_CONT_REUSE_STATE_DRAFT=1)
    fi
    if [ "$fused_chain" = "1" ]; then
        envs+=(DS4_MTP_NATIVE_CONT_FUSED_CHAIN=1)
    fi

    env "${envs[@]}" ./ds4 --metal -m "$MODEL" --mtp "$MTP" --mtp-draft 4 \
        --ctx 1024 --nothink -sys "" --temp 0 -n "$N_GEN" \
        --prompt-file "$PROMPT_DIR/$prompt.txt" \
        > "$base.out" 2> "$base.err"

    local stdout
    if cmp -s "$OUT_DIR/$prompt.serial.out" "$base.out"; then
        stdout="match"
        : > "$base.diff"
    else
        stdout="diff"
        diff -u "$OUT_DIR/$prompt.serial.out" "$base.out" > "$base.diff" || true
    fi

    local tps metrics
    tps="$(parse_tps "$base.err")"
    metrics="$(parse_native_metrics "$base.err")"
    printf "%s\t%s\t%s\t%s\t%s\t%s\n" \
        "$prompt" "$mode" "$tps" "$stdout" "$metrics" "$base" >> "$SUMMARY"
}

for prompt in $PROMPTS; do
    prompt_file="$PROMPT_DIR/$prompt.txt"
    if [ ! -f "$prompt_file" ]; then
        echo "prompt not found: $prompt_file" >&2
        exit 1
    fi

    echo "running prompt=$prompt mode=serial"
    run_serial "$prompt"
    echo "running prompt=$prompt mode=native_k4"
    run_native_mode "$prompt" native_k4 0 0
    echo "running prompt=$prompt mode=cont_m2_base"
    run_native_mode "$prompt" cont_m2_base 2 0
    echo "running prompt=$prompt mode=cont_m2_fused_input"
    run_native_mode "$prompt" cont_m2_fused_input 2 1
    echo "running prompt=$prompt mode=cont_m2_fused_chain"
    run_native_mode "$prompt" cont_m2_fused_chain 2 1 0 0 1
    echo "running prompt=$prompt mode=cont_m2_state_only"
    run_native_mode "$prompt" cont_m2_state_only 2 0 1
    echo "running prompt=$prompt mode=cont_m2_state_only_fused_input"
    run_native_mode "$prompt" cont_m2_state_only_fused_input 2 1 1
    echo "running prompt=$prompt mode=cont_m2_state_reuse"
    run_native_mode "$prompt" cont_m2_state_reuse 2 0 1 1
    echo "running prompt=$prompt mode=cont_m2_state_reuse_fused_input"
    run_native_mode "$prompt" cont_m2_state_reuse_fused_input 2 1 1 1
    echo "running prompt=$prompt mode=cont_m3_base"
    run_native_mode "$prompt" cont_m3_base 3 0
    echo "running prompt=$prompt mode=cont_m3_fused_input"
    run_native_mode "$prompt" cont_m3_fused_input 3 1
    echo "running prompt=$prompt mode=cont_m3_fused_chain"
    run_native_mode "$prompt" cont_m3_fused_chain 3 1 0 0 1
    echo "running prompt=$prompt mode=cont_m3_state_only"
    run_native_mode "$prompt" cont_m3_state_only 3 0 1
    echo "running prompt=$prompt mode=cont_m3_state_only_fused_input"
    run_native_mode "$prompt" cont_m3_state_only_fused_input 3 1 1
    echo "running prompt=$prompt mode=cont_m3_state_reuse"
    run_native_mode "$prompt" cont_m3_state_reuse 3 0 1 1
    echo "running prompt=$prompt mode=cont_m3_state_reuse_fused_input"
    run_native_mode "$prompt" cont_m3_state_reuse_fused_input 3 1 1 1
done

echo "summary=$SUMMARY"
cat "$SUMMARY"
