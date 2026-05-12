#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
CACHE_ROOT="${DS4_GGUF_CACHE:-$HOME/.ds4/cache}"
DEFAULT_MODEL_NAME="DeepSeek-V4-Flash-Q4KExperts-F16HC-F16Compressor-F16Indexer-Q8Attn-Q8Shared-Q8Out-chat-v2.gguf"
DEFAULT_MTP_NAME="DeepSeek-V4-Flash-MTP-Q4K-Q8_0-F32.gguf"

find_default_gguf() {
    local name="$1"
    shift
    local path
    for path in "$@"; do
        if [ -f "$path" ]; then
            printf '%s\n' "$path"
            return 0
        fi
    done
    printf '%s/%s\n' "$CACHE_ROOT" "$name"
}

MODEL="$(find_default_gguf "$DEFAULT_MODEL_NAME" \
    "$CACHE_ROOT/$DEFAULT_MODEL_NAME" \
    "$CACHE_ROOT/gguf/$DEFAULT_MODEL_NAME" \
    "$ROOT/ds4flash.gguf")"
MTP="$(find_default_gguf "$DEFAULT_MTP_NAME" \
    "$CACHE_ROOT/$DEFAULT_MTP_NAME" \
    "$CACHE_ROOT/gguf/$DEFAULT_MTP_NAME" \
    "$ROOT/gguf/$DEFAULT_MTP_NAME")"
RUNS=5
TOKENS=64
DRAFT=6
MARGIN=0
PROMPT="Explain Redis streams in one paragraph."
CSV=""
INCLUDE_RESIDENT=0
INCLUDE_SESSION=0
INCLUDE_DECODE2=0
INCLUDE_TARGET_PAIR=0
INCLUDE_NO_TARGET_PAIR=0
INCLUDE_FAST_ATTN=0
INCLUDE_NO_FAST_ATTN=0
FAST_ATTN_LAYERS="${DS4_MTP_FAST_ATTN_LAYERS:-14,16-42}"
INCLUDE_SPEED=1

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
  --include-resident
                   Add a resident no-spec lane: MTP opened/mapped with draft=1.
  --include-session
                   Add a session no-spec lane: MTP opened/mapped, draft=N, no speculation.
  --include-decode2
                   Add the sequential-shape exact N=2 verifier lane.
  --include-target-pair
                   Add the target-first pair-prefetch verifier lane.
  --include-no-target-pair
                   Add the exact verifier lane with target-first pair disabled.
  --include-fast-attn
                   Add exact verifier with diagnostic fast batch attention layers.
                   Layer list defaults to $FAST_ATTN_LAYERS; override with DS4_MTP_FAST_ATTN_LAYERS.
  --include-no-fast-attn
                   Add exact verifier with fast batch attention disabled.
  --no-speed       Skip the approximate --mtp-speed lane.
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
        --include-resident) INCLUDE_RESIDENT=1; shift ;;
        --include-session) INCLUDE_SESSION=1; shift ;;
        --include-decode2) INCLUDE_DECODE2=1; shift ;;
        --include-target-pair) INCLUDE_TARGET_PAIR=1; shift ;;
        --include-no-target-pair) INCLUDE_NO_TARGET_PAIR=1; shift ;;
        --include-fast-attn) INCLUDE_FAST_ATTN=1; shift ;;
        --include-no-fast-attn) INCLUDE_NO_FAST_ATTN=1; shift ;;
        --no-speed) INCLUDE_SPEED=0; shift ;;
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
        resident)
            cmd+=("--mtp" "$MTP" "--mtp-draft" "1" "--mtp-margin" "$MARGIN")
            ;;
        session)
            envcmd=(env DS4_MTP_NO_SPECULATE=1)
            cmd+=("--mtp" "$MTP" "--mtp-draft" "$DRAFT" "--mtp-margin" "$MARGIN")
            ;;
        exact)
            cmd+=("--mtp" "$MTP" "--mtp-draft" "$DRAFT" "--mtp-margin" "$MARGIN")
            ;;
        decode2)
            envcmd=(env DS4_MTP_DECODE2_EXACT=1)
            cmd+=("--mtp" "$MTP" "--mtp-draft" "$DRAFT" "--mtp-margin" "$MARGIN")
            ;;
        target_pair)
            envcmd=(env DS4_MTP_EXACT_TARGET_FIRST_PAIR=1)
            cmd+=("--mtp" "$MTP" "--mtp-draft" "$DRAFT" "--mtp-margin" "$MARGIN")
            ;;
        no_target_pair)
            envcmd=(env DS4_MTP_NO_EXACT_TARGET_FIRST_PAIR=1)
            cmd+=("--mtp" "$MTP" "--mtp-draft" "$DRAFT" "--mtp-margin" "$MARGIN")
            ;;
        fast_attn)
            envcmd=(env DS4_METAL_ENABLE_EXACT_BATCH_ATTENTION=1 DS4_METAL_EXACT_BATCH_ATTENTION_LAYERS="$FAST_ATTN_LAYERS")
            cmd+=("--mtp" "$MTP" "--mtp-draft" "$DRAFT" "--mtp-margin" "$MARGIN")
            ;;
        no_fast_attn)
            envcmd=(env DS4_METAL_DISABLE_EXACT_BATCH_ATTENTION=1)
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

first_hash_for_mode() {
    local mode="$1"
    awk -F, -v mode="$mode" '$1 == mode { print $5; exit }' "$CSV"
}

ratio_to_baseline() {
    local value="$1"
    local baseline="$2"
    awk -v v="$value" -v b="$baseline" 'BEGIN { if (b == 0) print "0"; else printf "%.3f", v / b }'
}

hash_matches_baseline() {
    local mode="$1"
    local baseline_hash="$2"
    awk -F, -v mode="$mode" -v baseline_hash="$baseline_hash" '
        $1 == mode {
            seen = 1
            if ($5 != baseline_hash) mismatch = 1
        }
        END {
            if (!seen || mismatch) print "0";
            else print "1";
        }' "$CSV"
}

MODES=(baseline disabled)
if [ "$INCLUDE_RESIDENT" -ne 0 ]; then
    MODES+=(resident)
fi
if [ "$INCLUDE_SESSION" -ne 0 ]; then
    MODES+=(session)
fi
MODES+=(exact)
if [ "$INCLUDE_DECODE2" -ne 0 ]; then
    MODES+=(decode2)
fi
if [ "$INCLUDE_TARGET_PAIR" -ne 0 ]; then
    MODES+=(target_pair)
fi
if [ "$INCLUDE_NO_TARGET_PAIR" -ne 0 ]; then
    MODES+=(no_target_pair)
fi
if [ "$INCLUDE_FAST_ATTN" -ne 0 ]; then
    MODES+=(fast_attn)
fi
if [ "$INCLUDE_NO_FAST_ATTN" -ne 0 ]; then
    MODES+=(no_fast_attn)
fi
if [ "$INCLUDE_SPEED" -ne 0 ]; then
    MODES+=(speed)
fi
for run in $(seq 1 "$RUNS"); do
    offset=$(( (run - 1) % ${#MODES[@]} ))
    for i in "${!MODES[@]}"; do
        mode="${MODES[$(( (i + offset) % ${#MODES[@]} ))]}"
        run_mode "$mode" "$run"
    done
done

base_med="$(median_for_mode baseline)"
disabled_med="$(median_for_mode disabled)"
resident_med=""
if [ "$INCLUDE_RESIDENT" -ne 0 ]; then
    resident_med="$(median_for_mode resident)"
fi
session_med=""
if [ "$INCLUDE_SESSION" -ne 0 ]; then
    session_med="$(median_for_mode session)"
fi
exact_med="$(median_for_mode exact)"
decode2_med=""
if [ "$INCLUDE_DECODE2" -ne 0 ]; then
    decode2_med="$(median_for_mode decode2)"
fi
target_pair_med=""
if [ "$INCLUDE_TARGET_PAIR" -ne 0 ]; then
    target_pair_med="$(median_for_mode target_pair)"
fi
no_target_pair_med=""
if [ "$INCLUDE_NO_TARGET_PAIR" -ne 0 ]; then
    no_target_pair_med="$(median_for_mode no_target_pair)"
fi
fast_attn_med=""
if [ "$INCLUDE_FAST_ATTN" -ne 0 ]; then
    fast_attn_med="$(median_for_mode fast_attn)"
fi
no_fast_attn_med=""
if [ "$INCLUDE_NO_FAST_ATTN" -ne 0 ]; then
    no_fast_attn_med="$(median_for_mode no_fast_attn)"
fi
speed_med=""
if [ "$INCLUDE_SPEED" -ne 0 ]; then
    speed_med="$(median_for_mode speed)"
fi
baseline_hash="$(first_hash_for_mode baseline)"
disabled_ratio="$(ratio_to_baseline "$disabled_med" "$base_med")"
resident_ratio=""
if [ "$INCLUDE_RESIDENT" -ne 0 ]; then
    resident_ratio="$(ratio_to_baseline "$resident_med" "$base_med")"
fi
session_ratio=""
if [ "$INCLUDE_SESSION" -ne 0 ]; then
    session_ratio="$(ratio_to_baseline "$session_med" "$base_med")"
fi
exact_ratio="$(ratio_to_baseline "$exact_med" "$base_med")"
decode2_ratio=""
if [ "$INCLUDE_DECODE2" -ne 0 ]; then
    decode2_ratio="$(ratio_to_baseline "$decode2_med" "$base_med")"
fi
target_pair_ratio=""
if [ "$INCLUDE_TARGET_PAIR" -ne 0 ]; then
    target_pair_ratio="$(ratio_to_baseline "$target_pair_med" "$base_med")"
fi
no_target_pair_ratio=""
if [ "$INCLUDE_NO_TARGET_PAIR" -ne 0 ]; then
    no_target_pair_ratio="$(ratio_to_baseline "$no_target_pair_med" "$base_med")"
fi
fast_attn_ratio=""
if [ "$INCLUDE_FAST_ATTN" -ne 0 ]; then
    fast_attn_ratio="$(ratio_to_baseline "$fast_attn_med" "$base_med")"
fi
no_fast_attn_ratio=""
if [ "$INCLUDE_NO_FAST_ATTN" -ne 0 ]; then
    no_fast_attn_ratio="$(ratio_to_baseline "$no_fast_attn_med" "$base_med")"
fi
speed_ratio=""
if [ "$INCLUDE_SPEED" -ne 0 ]; then
    speed_ratio="$(ratio_to_baseline "$speed_med" "$base_med")"
fi

cat <<EOF
csv=$CSV
baseline_median_tps=$base_med hashes=$(unique_hashes_for_mode baseline)
disabled_median_tps=$disabled_med hashes=$(unique_hashes_for_mode disabled)
disabled_vs_baseline=$disabled_ratio hash_matches_baseline=$(hash_matches_baseline disabled "$baseline_hash")
$(if [ "$INCLUDE_RESIDENT" -ne 0 ]; then
    printf 'resident_median_tps=%s hashes=%s\nresident_vs_baseline=%s hash_matches_baseline=%s\n' \
        "$resident_med" \
        "$(unique_hashes_for_mode resident)" \
        "$resident_ratio" \
        "$(hash_matches_baseline resident "$baseline_hash")"
fi)
$(if [ "$INCLUDE_SESSION" -ne 0 ]; then
    printf 'session_median_tps=%s hashes=%s\nsession_vs_baseline=%s hash_matches_baseline=%s\n' \
        "$session_med" \
        "$(unique_hashes_for_mode session)" \
        "$session_ratio" \
        "$(hash_matches_baseline session "$baseline_hash")"
fi)
exact_median_tps=$exact_med hashes=$(unique_hashes_for_mode exact)
exact_vs_baseline=$exact_ratio hash_matches_baseline=$(hash_matches_baseline exact "$baseline_hash")
$(if [ "$INCLUDE_DECODE2" -ne 0 ]; then
    printf 'decode2_median_tps=%s hashes=%s\ndecode2_vs_baseline=%s hash_matches_baseline=%s\n' \
        "$decode2_med" \
        "$(unique_hashes_for_mode decode2)" \
        "$decode2_ratio" \
        "$(hash_matches_baseline decode2 "$baseline_hash")"
fi)
$(if [ "$INCLUDE_TARGET_PAIR" -ne 0 ]; then
    printf 'target_pair_median_tps=%s hashes=%s\ntarget_pair_vs_baseline=%s hash_matches_baseline=%s\n' \
        "$target_pair_med" \
        "$(unique_hashes_for_mode target_pair)" \
        "$target_pair_ratio" \
        "$(hash_matches_baseline target_pair "$baseline_hash")"
fi)
$(if [ "$INCLUDE_NO_TARGET_PAIR" -ne 0 ]; then
    printf 'no_target_pair_median_tps=%s hashes=%s\nno_target_pair_vs_baseline=%s hash_matches_baseline=%s\n' \
        "$no_target_pair_med" \
        "$(unique_hashes_for_mode no_target_pair)" \
        "$no_target_pair_ratio" \
        "$(hash_matches_baseline no_target_pair "$baseline_hash")"
fi)
$(if [ "$INCLUDE_FAST_ATTN" -ne 0 ]; then
    printf 'fast_attn_layers=%s\nfast_attn_median_tps=%s hashes=%s\nfast_attn_vs_baseline=%s hash_matches_baseline=%s\n' \
        "$FAST_ATTN_LAYERS" \
        "$fast_attn_med" \
        "$(unique_hashes_for_mode fast_attn)" \
        "$fast_attn_ratio" \
        "$(hash_matches_baseline fast_attn "$baseline_hash")"
fi)
$(if [ "$INCLUDE_NO_FAST_ATTN" -ne 0 ]; then
    printf 'no_fast_attn_median_tps=%s hashes=%s\nno_fast_attn_vs_baseline=%s hash_matches_baseline=%s\n' \
        "$no_fast_attn_med" \
        "$(unique_hashes_for_mode no_fast_attn)" \
        "$no_fast_attn_ratio" \
        "$(hash_matches_baseline no_fast_attn "$baseline_hash")"
fi)
$(if [ "$INCLUDE_SPEED" -ne 0 ]; then
    printf 'speed_median_tps=%s hashes=%s\nspeed_vs_baseline=%s\n' \
        "$speed_med" \
        "$(unique_hashes_for_mode speed)" \
        "$speed_ratio"
fi)
EOF
