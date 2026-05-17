#!/usr/bin/env bash
set -euo pipefail

usage() {
    cat <<'USAGE'
usage: tools/mtp_native_depth_sweep.sh --model MODEL --mtp MTP [options]

Runs the standard native-MTP depth selector: serial target baseline plus
DS4_MTP_NATIVE depths K=2..5 on count/explain/code prompt files. This is the
required smoke after verifier-economics changes such as batch-KV, compressor
projection, output-head, command-layout, or small-M kernel rewrites.

Options:
  --model PATH          Target GGUF. Defaults to DS4_BENCH_MODEL or ./ds4flash.gguf.
  --mtp PATH            MTP GGUF. Defaults to DS4_BENCH_MTP_MODEL.
  --prompt-dir DIR      Directory containing count.txt, explain.txt, code.txt.
                        Default: /tmp/ds4-mtp-matrix.
  --prompts "NAMES"     Space-separated prompt names. Default: "count explain code".
  --depths "DEPTHS"     Space-separated depths. Default: "2 3 4 5".
  --out-dir DIR         Artifact directory. Default: /tmp/ds4-native-depth-sweep-<timestamp>.
  --ctx N               Context size. Default: 1024.
  -n N                  Generated tokens. Default: 64.
  --native-env "ENV"    Extra/override native env. Default:
                        DS4_MTP_NATIVE=1 DS4_MTP_NATIVE_TIMING=1
                        DS4_MTP_NATIVE_VERIFY_OPT=smallm
  -h, --help            Show this help.

Example:
  tools/mtp_native_depth_sweep.sh \
    --model "$MODEL" --mtp "$MTP" \
    --prompt-dir /tmp/ds4-mtp-matrix

After the winner is selected, run representative quality/TPS slices for that
depth, for example with tools/mtp_quality_gate.sh and a GSM8K slice.
USAGE
}

timestamp() {
    date +%Y%m%d_%H%M%S
}

parse_generation_tps() {
    python3 - "$1" <<'PY'
import re
import sys

text = open(sys.argv[1], encoding="utf-8", errors="replace").read()
matches = re.findall(r"generation:\s*([0-9]+(?:\.[0-9]+)?)\s*t/s", text)
print(matches[-1] if matches else "")
PY
}

parse_native_metrics() {
    python3 - "$1" <<'PY'
import re
import statistics
import sys

text = open(sys.argv[1], encoding="utf-8", errors="replace").read()
cycle_lines = [line for line in text.splitlines() if "ds4: mtp native" in line and " cycle " in line]

def nums(pattern):
    out = []
    for line in cycle_lines:
        m = re.search(pattern, line)
        if m:
            out.append(float(m.group(1)))
    return out

def ints(pattern):
    return [int(v) for v in nums(pattern)]

accepted = ints(r"\baccepted=([0-9]+)")
discarded = ints(r"\bdiscarded=([0-9]+)")
decode = nums(r"\bverifier_decode_gpu=([0-9]+(?:\.[0-9]+)?) ms")
total = nums(r"\bverifier_total_gpu=([0-9]+(?:\.[0-9]+)?) ms")
mismatches = ints(r"\bmismatches=([0-9]+)")
topid = ints(r"\btopid_frontier=([0-9]+)")

def avg(values):
    return f"{statistics.fmean(values):.3f}" if values else ""

topid_summary = ""
if topid:
    topid_summary = f"{sum(1 for v in topid if v)}/{len(topid)}"

print("\t".join([
    str(len(cycle_lines)),
    avg(accepted),
    avg(discarded),
    avg(decode),
    avg(total),
    str(sum(mismatches)) if mismatches else "",
    topid_summary,
]))
PY
}

rank_depths() {
    python3 - "$1" <<'PY'
import csv
import statistics
import sys

path = sys.argv[1]
rows = list(csv.DictReader(open(path, encoding="utf-8"), delimiter="\t"))
by_depth = {}
for row in rows:
    if row["mode"] != "native" or row["stdout"] != "match" or not row["tps"]:
        continue
    by_depth.setdefault(row["depth"], []).append(float(row["tps"]))

print("depth\tmean_tps\tprompt_count")
for depth, values in sorted(by_depth.items(), key=lambda item: int(item[0])):
    print(f"{depth}\t{statistics.fmean(values):.3f}\t{len(values)}")

if by_depth:
    winner, values = max(by_depth.items(), key=lambda item: statistics.fmean(item[1]))
    print(f"winner\t{winner}\t{statistics.fmean(values):.3f}")
PY
}

MODEL="${DS4_BENCH_MODEL:-./ds4flash.gguf}"
MTP="${DS4_BENCH_MTP_MODEL:-}"
PROMPT_DIR="/tmp/ds4-mtp-matrix"
PROMPTS="count explain code"
DEPTHS="2 3 4 5"
OUT_DIR=""
CTX=1024
N_GEN=64
NATIVE_ENV="DS4_MTP_NATIVE=1 DS4_MTP_NATIVE_TIMING=1 DS4_MTP_NATIVE_VERIFY_OPT=smallm"

while [ "$#" -gt 0 ]; do
    case "$1" in
        --model) MODEL="$2"; shift 2 ;;
        --mtp) MTP="$2"; shift 2 ;;
        --prompt-dir) PROMPT_DIR="$2"; shift 2 ;;
        --prompts) PROMPTS="$2"; shift 2 ;;
        --depths) DEPTHS="$2"; shift 2 ;;
        --out-dir) OUT_DIR="$2"; shift 2 ;;
        --ctx) CTX="$2"; shift 2 ;;
        -n) N_GEN="$2"; shift 2 ;;
        --native-env) NATIVE_ENV="$2"; shift 2 ;;
        -h|--help) usage; exit 0 ;;
        *) echo "unknown option: $1" >&2; usage >&2; exit 2 ;;
    esac
done

if [ ! -x ./ds4 ]; then
    echo "missing ./ds4; build it first with: make ds4" >&2
    exit 1
fi
if [ ! -f "$MODEL" ]; then
    echo "model not found: $MODEL" >&2
    exit 1
fi
if [ -z "$MTP" ] || [ ! -f "$MTP" ]; then
    echo "mtp model not found: ${MTP:-<empty>}" >&2
    exit 1
fi

if [ -z "$OUT_DIR" ]; then
    OUT_DIR="/tmp/ds4-native-depth-sweep-$(timestamp)"
fi
mkdir -p "$OUT_DIR"

SUMMARY="$OUT_DIR/summary.tsv"
RANKING="$OUT_DIR/depth-ranking.tsv"
printf "prompt\tmode\tdepth\ttps\tstdout\tcycles\tavg_accepted\tavg_discarded\tavg_verifier_decode_gpu_ms\tavg_verifier_total_gpu_ms\tmismatches\ttopid_frontier\tartifact\n" > "$SUMMARY"

echo "artifact_dir=$OUT_DIR"
echo "model=$MODEL" > "$OUT_DIR/metadata.txt"
echo "mtp=$MTP" >> "$OUT_DIR/metadata.txt"
echo "prompt_dir=$PROMPT_DIR" >> "$OUT_DIR/metadata.txt"
echo "prompts=$PROMPTS" >> "$OUT_DIR/metadata.txt"
echo "depths=$DEPTHS" >> "$OUT_DIR/metadata.txt"
echo "ctx=$CTX" >> "$OUT_DIR/metadata.txt"
echo "n=$N_GEN" >> "$OUT_DIR/metadata.txt"
echo "native_env=$NATIVE_ENV" >> "$OUT_DIR/metadata.txt"

for prompt in $PROMPTS; do
    prompt_file="$PROMPT_DIR/$prompt.txt"
    if [ ! -f "$prompt_file" ]; then
        echo "prompt not found: $prompt_file" >&2
        exit 1
    fi

    serial_base="$OUT_DIR/$prompt.serial"
    echo "running prompt=$prompt mode=serial"
    ./ds4 --metal -m "$MODEL" \
        --ctx "$CTX" --nothink -sys "" --temp 0 -n "$N_GEN" \
        --prompt-file "$prompt_file" \
        > "$serial_base.out" 2> "$serial_base.err"
    serial_tps="$(parse_generation_tps "$serial_base.err")"
    printf "%s\tserial\t-\t%s\toracle\t\t\t\t\t\t\t\t%s\n" \
        "$prompt" "$serial_tps" "$serial_base" >> "$SUMMARY"

    for depth in $DEPTHS; do
        native_base="$OUT_DIR/$prompt.k$depth.native"
        echo "running prompt=$prompt mode=native depth=$depth"
        # shellcheck disable=SC2086
        env $NATIVE_ENV ./ds4 --metal -m "$MODEL" --mtp "$MTP" --mtp-draft "$depth" \
            --ctx "$CTX" --nothink -sys "" --temp 0 -n "$N_GEN" \
            --prompt-file "$prompt_file" \
            > "$native_base.out" 2> "$native_base.err"
        native_tps="$(parse_generation_tps "$native_base.err")"
        if cmp -s "$serial_base.out" "$native_base.out"; then
            stdout="match"
            : > "$native_base.diff"
        else
            stdout="diff"
            diff -u "$serial_base.out" "$native_base.out" > "$native_base.diff" || true
        fi
        metrics="$(parse_native_metrics "$native_base.err")"
        printf "%s\tnative\t%s\t%s\t%s\t%s\t%s\n" \
            "$prompt" "$depth" "$native_tps" "$stdout" "$metrics" "$native_base" >> "$SUMMARY"
    done
done

rank_depths "$SUMMARY" | tee "$RANKING"
echo "summary=$SUMMARY"
echo "ranking=$RANKING"
