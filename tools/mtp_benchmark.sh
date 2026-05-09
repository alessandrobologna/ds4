#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
MODEL="/Users/alessandro/git/antirez/ds4/ds4flash.gguf"
MTP="/Users/alessandro/git/antirez/ds4/gguf/DeepSeek-V4-Flash-MTP-Q4K-Q8_0-F32.gguf"
RUNS=5
TOKENS=64
DRAFT=6
MARGIN=0
PROMPT="Explain Redis streams in one paragraph."
CSV=""

usage() {
    cat <<EOF
Usage: tools/mtp_benchmark.sh [options]

Options:
  --model FILE      Base GGUF model path. Default: $MODEL
  --mtp FILE        MTP GGUF path. Default: $MTP
  --runs N          Interleaved runs per mode. Default: $RUNS
  --tokens N        Generation token budget. Default: $TOKENS
  --draft N         MTP draft depth. Default: $DRAFT
  --margin F        MTP margin passed to MTP modes. Default: $MARGIN
  --prompt TEXT     Prompt text.
  --csv FILE        CSV output path. Default: temporary file.
  -h, --help        Show this help.
EOF
}

while [ "$#" -gt 0 ]; do
    case "$1" in
        --model) MODEL="$2"; shift 2 ;;
        --mtp) MTP="$2"; shift 2 ;;
        --runs) RUNS="$2"; shift 2 ;;
        --tokens) TOKENS="$2"; shift 2 ;;
        --draft) DRAFT="$2"; shift 2 ;;
        --margin) MARGIN="$2"; shift 2 ;;
        --prompt) PROMPT="$2"; shift 2 ;;
        --csv) CSV="$2"; shift 2 ;;
        -h|--help) usage; exit 0 ;;
        *) echo "unknown option: $1" >&2; usage >&2; exit 2 ;;
    esac
done

if [ ! -x "$ROOT/ds4" ]; then
    echo "ds4 binary not found; run: make ds4" >&2
    exit 2
fi
if [ ! -f "$MODEL" ]; then
    echo "model not found: $MODEL" >&2
    exit 2
fi
if [ ! -f "$MTP" ]; then
    echo "mtp model not found: $MTP" >&2
    exit 2
fi

TMP_PARENT="${TMPDIR:-/tmp}"
WORKDIR="$(mktemp -d "$TMP_PARENT/ds4-mtp-bench.XXXXXX")"
trap 'rm -rf "$WORKDIR"' EXIT
if [ -z "$CSV" ]; then
    CSV="$TMP_PARENT/ds4-mtp-benchmark-$(date +%Y%m%d%H%M%S)-$$.csv"
fi

printf 'mode,run,tps,bytes,sha256,rc\n' > "$CSV"

run_mode() {
    local mode="$1"
    local run="$2"
    local out="$WORKDIR/${mode}.${run}.out"
    local err="$WORKDIR/${mode}.${run}.err"
    local -a cmd=("$ROOT/ds4" -m "$MODEL" --temp 0 --nothink -n "$TOKENS" -p "$PROMPT")
    local -a envcmd=()

    case "$mode" in
        baseline)
            ;;
        disabled)
            envcmd=(env DS4_MTP_SPEC_DISABLE=1)
            cmd+=("--mtp" "$MTP" "--mtp-draft" "$DRAFT" "--mtp-margin" "$MARGIN")
            ;;
        exact)
            cmd+=("--mtp" "$MTP" "--mtp-draft" "$DRAFT" "--mtp-margin" "$MARGIN")
            ;;
        speed)
            cmd+=("--mtp" "$MTP" "--mtp-draft" "$DRAFT" "--mtp-margin" "$MARGIN" "--mtp-speed")
            ;;
        *)
            echo "unknown mode: $mode" >&2
            exit 2
            ;;
    esac

    set +e
    if [ "${#envcmd[@]}" -gt 0 ]; then
        "${envcmd[@]}" "${cmd[@]}" >"$out" 2>"$err"
    else
        "${cmd[@]}" >"$out" 2>"$err"
    fi
    local rc=$?
    set -e

    local tps
    tps="$(awk '/generation:/ { for (i = 1; i <= NF; i++) if ($i == "generation:") print $(i + 1) }' "$err" | tail -n 1)"
    if [ -z "$tps" ]; then tps=0; fi
    local bytes sha
    bytes="$(wc -c < "$out" | tr -d ' ')"
    sha="$(shasum -a 256 "$out" | awk '{print $1}')"
    printf '%s,%s,%s,%s,%s,%s\n' "$mode" "$run" "$tps" "$bytes" "$sha" "$rc" >> "$CSV"
    printf 'mode=%s run=%s tps=%s bytes=%s rc=%s\n' "$mode" "$run" "$tps" "$bytes" "$rc" >&2
}

median_for_mode() {
    local mode="$1"
    awk -F, -v mode="$mode" '$1 == mode { print $3 }' "$CSV" |
        sort -n |
        awk '{ v[NR] = $1 } END {
            if (NR == 0) { print "0"; exit }
            if (NR % 2) print v[(NR + 1) / 2];
            else print (v[NR / 2] + v[NR / 2 + 1]) / 2;
        }'
}

unique_hashes_for_mode() {
    local mode="$1"
    awk -F, -v mode="$mode" '$1 == mode { print $5 }' "$CSV" | sort -u | wc -l | tr -d ' '
}

for run in $(seq 1 "$RUNS"); do
    for mode in baseline disabled exact speed; do
        run_mode "$mode" "$run"
    done
done

base_med="$(median_for_mode baseline)"
disabled_med="$(median_for_mode disabled)"
exact_med="$(median_for_mode exact)"
speed_med="$(median_for_mode speed)"
speed_ratio="$(awk -v s="$speed_med" -v b="$base_med" 'BEGIN { if (b == 0) print "0"; else printf "%.3f", s / b }')"

cat <<EOF
csv=$CSV
baseline_median_tps=$base_med hashes=$(unique_hashes_for_mode baseline)
disabled_median_tps=$disabled_med hashes=$(unique_hashes_for_mode disabled)
exact_median_tps=$exact_med hashes=$(unique_hashes_for_mode exact)
speed_median_tps=$speed_med hashes=$(unique_hashes_for_mode speed)
speed_vs_baseline=$speed_ratio
EOF
