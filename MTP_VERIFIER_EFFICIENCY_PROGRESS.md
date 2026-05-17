# MTP Verifier Efficiency Progress

This document tracks the greenfield DS4 efficient target-verifier track for MTP
speculative decoding.

The previous sched2 pipeline work proved that target and MTP GPU work can
overlap, but the tested schedules did not promote because exact target
verification still cost too much per useful accepted token. This track asks a
different question:

- Can exact target verification become materially cheaper per accepted token
  than repeated serial target decode?

Sched2 remains available as a scheduling substrate, but this file is about
verifier economics.

## Goal

Reach a promote/drop architecture decision for four verifier directions:

1. Fused multi-token target verifier for drafted chunks `a1..aN`.
2. Exact cheaper greedy/temp-0 acceptance tests that avoid full-vocab logits
   when possible.
3. Output-head-focused verifier cost isolation and specialization.
4. Target-state contract that commits accepted prefixes without serial replay.

## Invariants

- Serial target decode remains the oracle and fallback.
- No approximate verifier is promotable. Approximate probes must be labeled as
  non-promotable.
- Accepted verifier rows must match serial target logits/top ids in validation
  mode.
- Every timing claim must separate target decode/layer work, output head/top-k,
  replay/restore/prefix commit, MTP drafting if used, and end-to-end throughput.
- Promotion requires beating both:
  - serial target baseline;
  - MTP-loaded-disabled baseline, `DS4_MTP_SPEC_DISABLE=1`.
- This is Apple Metal only. Do not add CUDA implementation work for this track.

## New Experimental Flags

These flags are verifier-track flags. They are not compatibility aliases for
older experiments.

- `DS4_MTP_VERIFIER_BATCH_HEAD=1`
  Use the exact layer-major verifier with batched output-head rows.
- `DS4_MTP_VERIFIER_TOP_ONLY=1`
  Avoid full-logit host readback for non-final verifier rows when validation is
  disabled. The verifier still computes exact top ids.
- `DS4_MTP_VERIFIER_PREFIX_COMMIT=1`
  Capture exact prefix-1/prefix-2 target state inside the verifier and commit
  accepted short prefixes without serial replay.
- `DS4_MTP_VERIFIER_TIMING=1`
  Emit verifier-stage timing, including decode/layer GPU span, output-head GPU
  span, target total GPU span, restore/replay/prefix cost, and acceptance
  metrics.
- `DS4_MTP_VERIFIER_CHUNK_NATIVE=1`
  Route sched2 verification through the chunk-native suffix verifier instead of
  the per-token exact decode verifier.
- `DS4_MTP_VERIFIER_EXACT_MV_BATCH=1`
  During verifier calls only, use the decode matvec kernels over multiple token
  rows in one dispatch for Q8_0/F16 projections. This preserves the
  single-token reduction shape while changing the verifier execution shape. It
  must not affect ordinary serial target decode, because serial decode remains
  the oracle.
- `DS4_MTP_VERIFIER_DECODE2_PREBATCH=1`
  N=2 decode-native microbatch probe. It batches the pre-attention HC/Q/KV
  setup for two drafted tokens, then runs each token through the exact
  decode-order KV/compressor/attention/FFN tail. This is deliberately narrower
  than the prefill/chunk executor and keeps target cache ordering exact.
- `DS4_MTP_VERIFIER_DECODE2_BATCH_FFN_PRE=1`
  Extends the N=2 decode-native path by batching only the FFN HC-pre/RMSNorm
  setup after both rows have completed decode-order attention, then returns to
  exact row-wise router/MoE/shared-expert/HC-post work.
- `DS4_MTP_VERIFIER_DECODE2_BATCH_SHARED=1`
  Extends the N=2 decode-native path by batching FFN HC-pre/RMSNorm, keeping
  router/routed-MoE row-exact, then batching only the row-independent shared
  expert projections before row-exact HC post.
- `DS4_MTP_VERIFIER_DECODE2_BATCH_ROUTED=1`
  Non-promotable probe that keeps router selection row-exact, then batches the
  routed MoE and shared expert tails for N=2. Validation currently shows logit
  drift and, with the fused tiny routed pair path, top-id drift.
- `DS4_MTP_VERIFIER_DECODE2_BATCH_FFN=1`
  Non-promotable probe that batches the whole FFN/MoE tail after decode-order
  attention. It is useful as evidence, but validation currently shows logit
  drift, so it must not be used as an exact verifier.

## Direction 1: Fused Multi-Token Target Verifier

Current implementation surface:

- `metal_graph_verify_decode_exact()` already runs drafted tokens as one
  layer-major target verifier command stream.
- The new track will test this path with `N=2..8`, with stage timing and
  per-accepted-token cost.

Gate:

- Exact stdout/logit match.
- Target verifier GPU span per accepted token below serial one-token decode.

## Direction 2: Cheaper Exact Acceptance Test

Current implementation surface:

- For greedy/temp-0 acceptance, the verifier needs exact top ids for
  intermediate rows, but only the final committed row needs full logits for
  continuation.
- `DS4_MTP_VERIFIER_TOP_ONLY=1` tests removing full-logit host readback for
  non-final rows. This does not avoid full vocab projection yet; it isolates the
  readback/contract part first.

Gate:

- Exact stdout/logit validation remains clean.
- Full-logit readback reduction materially changes verifier economics.

## Direction 3: Output-Head-Focused Verifier

Current implementation surface:

- `metal_graph_encode_output_head()` is the one-row head.
- `metal_graph_encode_output_head_batch()` is the batched head used for
  speculative rows.
- `DS4_MTP_VERIFIER_BATCH_HEAD=1` plus verifier stage timing isolates layer
  work versus output-head/top-k work.

Gate:

- Batched or specialized output-head work reduces total verifier cost enough to
  improve end-to-end throughput.

## Direction 4: Target-State Contract Without Replay

Current implementation surface:

- Existing prefix capture buffers can preserve exact state after prefix 1 and
  prefix 2.
- `DS4_MTP_VERIFIER_PREFIX_COMMIT=1` will capture those states during the
  exact verifier and use `spec_frontier_commit_prefix1/2()` on partial accepts
  instead of restoring and serially replaying accepted tokens.

Gate:

- Partial accepts commit exactly without serial replay.
- Prefix commit cost is lower than replay and improves end-to-end throughput.

## Standard Matrix

Use:

```sh
--ctx 1024 --nothink -sys "" --temp 0 -n 64
```

Prompts:

- Counting: `Count from 1 to 200, separated by spaces.`
- Explanation: `Explain why speculative decoding can improve language model throughput in two concise paragraphs.`
- Code completion:

```c
Complete this C function:

#include <stdbool.h>
#include <stdint.h>

bool parse_u32(const char *s, uint32_t *out) {
```

## Local Checks

```sh
make ds4_test ds4
./ds4_test --metal-sched2
./ds4_test --metal-kernels
git diff --check
```

## Implementation Completed

The verifier-efficiency track landed as a greenfield layer on the sched2 path
without CUDA work:

- `metal_graph_verify_decode_exact()` now accepts verifier-track switches for
  batched output-head rows, top-id-only readback, prefix capture, and per-stage
  profiling.
- `DS4_MTP_VERIFIER_TIMING=1` emits decode/layer GPU span, output-head GPU
  span, total target-verifier GPU span, replay/prefix cost, and acceptance
  counters in the sched2 timing line.
- `DS4_MTP_VERIFIER_PREFIX_COMMIT=1` captures exact prefix-1 and prefix-2
  target state during verification and commits accepted short prefixes through
  `spec_frontier_commit_prefix1/2()` instead of serial replay.
- `DS4_MTP_VERIFIER_TOP_ONLY=1` skips full-logit host readback when validation
  is disabled. Validation forces full rows back on so accepted rows can still be
  compared against the serial oracle.
- `DS4_MTP_VERIFIER_BATCH_HEAD=1` uses the batched output-head path for
  verifier rows.

## Studio Artifacts

- Standard `-n 64` matrix:
  `/tmp/ds4-mtp-matrix/verifier_eff_20260514_103338`
- Validation matrix:
  `/tmp/ds4-mtp-matrix/verifier_eff_validation_20260514_103957`

Studio build and harness checks passed:

```sh
make ds4_test ds4
./ds4_test --metal-sched2
./ds4_test --metal-kernels
```

Local checks also passed:

```sh
make ds4_test ds4
./ds4_test --metal-sched2
./ds4_test --metal-kernels
git diff --check
```

## Correctness Evidence

The `-n 64` standard matrix compared every sched2 verifier variant against the
serial target stdout for the three prompts. There were no stdout diffs.

The validation matrix ran all row verifier depths `N=2..8` plus the N=2
top-only, batch-head, prefix-commit, batch+prefix, and batch+top variants on all
three prompts with:

```sh
DS4_MTP_SCHED2_VALIDATE=1 DS4_MTP_SCHED2_VALIDATE_LOG=1
```

Aggregate validation:

| Item | Value |
| --- | ---: |
| validated variants | 36 |
| serial-oracle verifier rows | 535 |
| sched2 cycles | 359 |
| stdout diffs | 0 |
| row top/logit mismatches | 0 |
| row max delta | 0 |
| cycle mismatches | 0 |
| cycle max delta | 0 |

## Baselines

Three standard prompts, `-n 64 --temp 0 --ctx 1024 --nothink -sys ""`:

| Prompt | Serial target | MTP-loaded-disabled |
| --- | ---: | ---: |
| count | 37.21 t/s | 36.63 t/s |
| explain | 36.82 t/s | 36.24 t/s |
| code | 37.04 t/s | 36.09 t/s |

Promotion requires beating both columns reproducibly.

## Direction 1 Results: Fused Multi-Token Verifier

The fixed-shape verifier is exact, but not economically useful. As `N` grows,
the verifier GPU span grows roughly linearly while accepted tokens per cycle do
not grow enough to amortize it. Past N=2, partial accepts also add replay cost
unless prefix commit is used.

| N | count t/s | count target GPU | explain t/s | explain target GPU | code t/s | code target GPU |
| ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| 2 | 33.39 | 41.53 ms | 25.92 | 39.56 ms | 29.88 | 39.26 ms |
| 3 | 19.94 | 54.02 ms | 21.99 | 49.37 ms | 25.39 | 50.33 ms |
| 4 | 17.52 | 66.40 ms | 19.25 | 63.99 ms | 21.60 | 62.22 ms |
| 5 | 15.61 | 79.03 ms | 16.17 | 75.70 ms | 18.18 | 75.45 ms |
| 6 | 14.07 | 91.57 ms | 14.74 | 86.85 ms | 15.90 | 87.10 ms |
| 7 | 12.92 | 103.30 ms | 13.49 | 98.15 ms | 14.64 | 98.13 ms |
| 8 | 11.89 | 115.12 ms | 12.47 | 109.10 ms | 13.59 | 108.71 ms |

Decision: **drop as a promotion path** in this DS4 Metal shape. It is useful as
an oracle-quality harness, but chunking known tokens through the target graph
does not make verification cheaper than serial target decode.

## Direction 2 Results: Cheaper Exact Acceptance Test

`DS4_MTP_VERIFIER_TOP_ONLY=1` removes full-logit host readback in non-validation
runs, but it still computes the exact output projection/top id. The measured
effect is within noise:

| Variant | count | explain | code |
| --- | ---: | ---: | ---: |
| row N=2 | 33.39 t/s | 25.92 t/s | 29.88 t/s |
| top-only N=2 | 33.63 t/s | 25.78 t/s | 29.79 t/s |

Decision: **drop the readback-only/top-only contract**. Exact acceptance still
needs an exact top proof, and host readback was not the bottleneck.

## Direction 3 Results: Output-Head-Focused Verifier

Batched output head reduces the head GPU span from roughly `1.42-1.51 ms` to
roughly `0.95 ms`, but target hidden/decode work remains about `37-40 ms` for
N=2.

| Variant | count t/s | explain t/s | code t/s | Typical head GPU |
| --- | ---: | ---: | ---: | ---: |
| row N=2 | 33.39 | 25.92 | 29.88 | 1.42-1.51 ms |
| batch-head N=2 | 34.00 | 26.00 | 29.95 | 0.95 ms |
| batch+top N=2 | 33.94 | 25.85 | 29.97 | 0.95 ms |

Decision: **drop as a standalone promotion path**. The output head is real work,
but it is too small a slice of the verifier to change end-to-end economics.

## Direction 4 Results: Target-State Contract Without Replay

Prefix commit is exact and removes serial replay on partial accepts. It helped
the prompts with partial accepts, but still did not approach either baseline:

| Variant | count t/s | explain t/s | code t/s | Replay | Prefix commit |
| --- | ---: | ---: | ---: | ---: | ---: |
| row N=2 | 33.39 | 25.92 | 29.88 | 0.00/7.77/3.01 ms | 0 ms |
| prefix N=2 | 33.13 | 28.73 | 31.16 | 0 ms | 0.00/0.12/0.04 ms |
| batch+prefix N=2 | 33.57 | 28.85 | 31.48 | 0 ms | 0.00/0.12/0.04 ms |

Decision: **keep as a useful exact contract, but drop as sufficient for
promotion**. It removes a known replay tax, yet the verifier remains slower
than serial target decode because the main cost is target hidden-state work.

## Narrow-Probe Decision Log

This section is now explicitly scoped to the first narrow probes around the
existing exact verifier. It is not the final answer for the broader greenfield
verifier question.

Narrow-probe decision: **drop the existing exact-verifier variant family for
promotion in the current DS4 Metal shape**.

All four directions are correct, but none makes exact target verification
materially cheaper per accepted token than repeated serial target decode:

- fused multi-token verification scales target hidden/decode work with `N`;
- top-only/readback reduction does not address the dominant cost;
- output-head batching saves about half a millisecond per N=2 cycle, not enough
  against a roughly 37-40 ms verifier decode span;
- prefix commit removes replay but cannot overcome the target hidden-state cost.

No bounded larger confirmation run was executed for these narrow probes because
no candidate beat the serial target baseline or the MTP-loaded-disabled baseline
on the standard matrix.

Implementation plan for the greenfield continuation:

1. Do not pursue more schedule-only or output-head-only variants for this target
   verifier.
2. Preserve prefix commit as a correctness/usefulness primitive for any future
   speculative path that already has a cheaper verifier.
3. A promotable path needs a different verifier contract that avoids most target
   hidden-state decode work, not just a different Metal schedule around the
   existing target graph.

## Greenfield Chunk-Native Verifier Track

The next implementation target is an exact chunk-native verifier path. The first
concrete attempt is not another wrapper around `metal_graph_verify_decode_exact`;
it changes the backend projection shape:

- `DS4_MTP_VERIFIER_CHUNK_NATIVE=1` routes sched2 verification through
  `metal_graph_verify_suffix_tops()`, the suffix verifier that runs drafted rows
  as a chunk through the batch layer executor.
- `DS4_MTP_VERIFIER_EXACT_MV_BATCH=1` changes Q8_0/F16 small-N projections in
  `ds4_metal.m` to use the decode matvec kernels over `n_tok` rows in one Metal
  dispatch instead of the faster but non-bit-compatible `mul_mv_ext` kernels.
- `DS4_MTP_VERIFIER_PREFIX_BATCH_LAYERS=<K>` is a hybrid verifier probe: run the
  first `K` target layers through the chunk/batch layer executor, then fall back
  to the exact decode-order verifier for the remaining layers. This tests
  whether any prefix of the existing batch executor is exact enough to reuse as
  the start of a chunk-native verifier.
- The purpose of this first rewrite is to preserve decode reduction order for
  the dominant projection kernels while still giving the verifier a chunk-native
  execution shape. Validation will show which remaining batch kernels still
  need exact small-N rewrites.

### 2026-05-14 Greenfield Verifier Implementation Pass

Local and studio builds passed after adding the exact-MV and prefix-batch-layer
probes.

Validation artifacts:

- `/tmp/ds4-mtp-matrix/chunk_native_exactmv_smoke_20260514_110928`
- `/tmp/ds4-mtp-matrix/chunk_native_drift_sweep_20260514_111327`
- `/tmp/ds4-mtp-matrix/exact_decode_control_20260514_111607`
- `/tmp/ds4-mtp-matrix/prefix_batch_layers_sweep2_20260514_112358`
- `/tmp/ds4-mtp-matrix/prefix_k1_flags_20260514_112438`
- `/tmp/ds4-mtp-matrix/exact_batch_embed_control_20260514_112750`

Results:

| Candidate | Correctness | Timing signal | Decision |
| --- | --- | --- | --- |
| Exact decode verifier + batch output head | `max_delta=0`, stdout match | N=2 average decode GPU about 39.5 ms in the count smoke | Remains oracle/control |
| Chunk-native batch layer + exact-MV projections | stdout/top ids matched the count smoke, but logits drifted up to `max_delta=3.49` | Faster than exact decode for N=2 in the smoke, but not exact | Not promotable as state-producing verifier |
| Fusion/reference flag sweep over chunk-native | Same `max_delta=3.49` across HC/QKV/KV/router/MoE/attention-output toggles | No useful isolation | Drift is structural to the prefill-style batch layer contract, not one optional fusion |
| Prefix batch layers `K=1,2,4,8` then exact decode tail | stdout/top ids matched, but logits drifted (`K=1 max_delta=5.21`, `K=8 max_delta=6.77`) | No decode-GPU improvement versus `K=0` | Existing batch layer cannot seed an exact verifier prefix |
| Exact verifier with fixed-shape batch embedding upload | `max_delta=0`, stdout match | Avg decode GPU remained about 39.8 ms in the count smoke | Safe cleanup; not sufficient economics by itself |

Implementation note: the first version of `DS4_MTP_VERIFIER_PREFIX_BATCH_LAYERS`
restored `batch_cur_hc` but not `batch_next_hc` after the prefix batch swapped
the two buffers. That caused a K=1 crash during the first sweep. The restore bug
is fixed, and the repeat sweep completed.

Current architecture conclusion for this pass: reusing the prefill/chunk layer
executor, even for only the first target layer, is not an exact target verifier.
The exact verifier now seeds the fixed-shape token HC rows in one batch upload,
which removes a setup wart while preserving `max_delta=0`, but it does not
change the hidden-state economics. The next implementation should be a
decode-native microbatch executor: keep the decode cache/compressor/order
contract, but create new small-N kernels or encode groups for specific
independent sub-stages. The first concrete targets are small-N HC pre/norm and
Q/KV projection groups, followed by decode-order attention/cache updates and
then small-N FFN/MoE groups.

### 2026-05-14 Decode-Native N=2 Microbatch Pass

Implemented `DS4_MTP_VERIFIER_DECODE2_PREBATCH=1` as the first decode-native
small-N verifier rewrite:

- Added a layer helper that batches the two-token pre-attention setup
  (HC flatten/mix/split/norm, Q/KV projections, Q/KV RMS, Q head norm, RoPE).
- Kept the rest of each token in strict decode order through KV store,
  compressor/indexer update, attention, FFN/MoE, and HC state update.
- Added a verifier-only Metal scope for `DS4_MTP_VERIFIER_EXACT_MV_BATCH=1`.
  The first implementation leaked this override into normal target decode; the
  code prompt diverged even without MTP. The scoped version now leaves plain
  serial target decode unchanged and keeps validation honest.
- Fixed a sched2 partial-accept ordering hazard by waiting for discarded async
  MTP work before target state restore/replay. This prevents rejected suffix
  work from racing with the state that the next target token will use.

Correctness smoke:

- Artifact: `/tmp/ds4-mtp-matrix/scoped_exactmv_code_20260514_115438`
- Plain serial with `DS4_MTP_VERIFIER_EXACT_MV_BATCH=1`: stdout match.
- `exactmv` and `decode2`: stdout match, `max_delta=0`, mismatches `0`.

Fresh standard matrix after the scoped override fix:

- Artifact: `/tmp/ds4-mtp-matrix/decode2_scoped_matrix_20260514_115534`
- Command shape: `-n 64 --temp 0 --ctx 1024 --nothink -sys ""`,
  `DS4_MTP_SCHED2_VERIFY_N=2`, `DS4_MTP_SCHED2_CONT_M=1`,
  `DS4_MTP_VERIFIER_BATCH_HEAD=1`, validation enabled.

| Prompt | Serial | MTP disabled | exactmv | exactmv decode GPU | decode2 | decode2 decode GPU |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| count | 36.86 | 36.22 | 19.72 | 39.720 ms | 20.02 | 38.475 ms |
| explain | 36.75 | 36.32 | 16.49 | 37.976 ms | 16.64 | 37.060 ms |
| code | 37.02 | 36.27 | 17.69 | 37.727 ms | 17.95 | 36.513 ms |

All non-baseline stdout matched serial target. All validated rows had
`max_delta=0` and mismatches `0`.

Decision for this implementation slice: **correct but not promotable**. The
decode-native pre-attention batching saved about `0.9-1.2 ms` from the N=2
verifier decode GPU span, roughly a 2-3% verifier-layer improvement, but the
end-to-end path remains far below both baselines. The result is useful because
it proves the exact verifier can absorb small decode-native grouping without
drift; it also shows that pre-attention grouping alone is much too small. A
promotable greenfield verifier would need to batch or fuse the expensive
decode-order attention/FFN/MoE tails, or change the verifier contract so those
tails are not paid once per drafted token.

### 2026-05-14 Decode-Native FFN Tail Split

Profiler attribution for a synchronized one-token decode showed the expensive
tail stages are not the already-batched Q/KV setup alone:

- Artifact: `/tmp/ds4-mtp-matrix/profile2.log`
- Largest synchronized stage sums across 43 layers: routed MoE `14.402 ms`,
  attention output `14.261 ms`, Q path `12.493 ms`, attention `12.442 ms`,
  FFN HC-pre `10.340 ms`, router `10.239 ms`, compressor/indexer `10.181 ms`,
  shared gate/up `9.895 ms`, shared down `9.887 ms`, KV path `9.830 ms`.

Implemented two deeper N=2 decode-native probes:

- `DS4_MTP_VERIFIER_DECODE2_BATCH_FFN=1`: run both tokens through decode-order
  attention, then batch the entire FFN/MoE tail.
- `DS4_MTP_VERIFIER_DECODE2_BATCH_FFN_PRE=1`: run both tokens through
  decode-order attention, batch only FFN HC-pre/RMSNorm, then return to exact
  row-wise router/MoE/shared/HC-post.

Correctness and isolation artifacts:

- `/tmp/ds4-mtp-matrix/decode2_batch_ffn_smoke_20260514_120531`
- `/tmp/ds4-mtp-matrix/decode2_batch_ffn_flags_20260514_120614`
- `/tmp/ds4-mtp-matrix/decode2_batch_ffn_pre_smoke_20260514_121110`

The full batch-FFN path is **not exact**. The first count smoke kept stdout/top
ids stable but drifted logits up to `max_delta=0.861403`. The flag sweep across
HC fusion, HC norm fusion, router fusion, routed-pair fusion, and MoE F32
intermediate mode did not change that max delta. In the longer matrix attempt,
the full batch-FFN path drifted up to `max_delta=7.87176` on count and failed
validation on explain. It is a non-promotable approximate probe.

The narrower FFN-pre path is exact on the standard matrix:

- Artifact: `/tmp/ds4-mtp-matrix/decode2_ffn_pre_matrix2_20260514_121330`
- Command shape: `-n 64 --temp 0 --ctx 1024 --nothink -sys ""`,
  `DS4_MTP_SCHED2_VERIFY_N=2`, `DS4_MTP_SCHED2_CONT_M=1`,
  `DS4_MTP_VERIFIER_BATCH_HEAD=1`, validation enabled.

| Prompt | Serial | MTP disabled | prebatch | prebatch decode GPU | ffnpre | ffnpre decode GPU |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| count | 36.74 | 36.61 | 20.05 | 38.305 ms | 20.13 | 37.625 ms |
| explain | 36.89 | 36.51 | 16.65 | 37.157 ms | 16.83 | 35.924 ms |
| code | 37.01 | 36.31 | 17.98 | 36.478 ms | 18.10 | 35.706 ms |

All `ffnpre` stdout matched serial target. All validated rows had `max_delta=0`
and mismatches `0`.

Decision for this slice: **correct but still not promotable**. The FFN-pre
split is the strongest exact verifier rewrite so far and saves another
`0.7-1.2 ms` of N=2 verifier decode GPU over pre-attention batching. It still
lands at only `18-20 t/s`, far below both serial and MTP-loaded-disabled
baselines. Full FFN/MoE batching would be the natural larger win, but the
existing batch FFN/MoE kernels are not exact enough to produce promotable
target state. The next meaningful implementation would need exact small-N
router/MoE/shared-expert kernels, not another wrapper around the current batch
FFN executor.

### 2026-05-14 Decode-Native Shared-Expert Split

Implemented `DS4_MTP_VERIFIER_DECODE2_BATCH_SHARED=1` as the next narrower
split after the full FFN batch drifted:

- Both rows still run decode-order attention.
- FFN HC-pre/RMSNorm is batched as in `DECODE2_BATCH_FFN_PRE`.
- Router and routed MoE stay row-exact.
- The shared expert gate/up/down projections and SwiGLU run over both rows in
  one batch.
- HC post returns to the exact row-wise add/expand path.

Correctness and timing artifacts:

- Smoke: `/tmp/ds4-mtp-matrix/decode2_batch_shared_smoke_20260514_122326`
- Matrix: `/tmp/ds4-mtp-matrix/decode2_batch_shared_matrix_20260514_122453`
- Final post-cleanup smoke:
  `/tmp/ds4-mtp-matrix/decode2_batch_shared_final_smoke_20260514_122746`
- Command shape: `-n 64 --temp 0 --ctx 1024 --nothink -sys ""`,
  `DS4_MTP_SCHED2_VERIFY_N=2`, `DS4_MTP_SCHED2_CONT_M=1`,
  `DS4_MTP_VERIFIER_BATCH_HEAD=1`, validation enabled.

| Prompt | Serial | MTP disabled | ffnpre | ffnpre decode GPU | shared | shared decode GPU |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| count | 36.96 | 36.23 | 20.23 | 37.276 ms | 20.22 | 37.254 ms |
| explain | 36.75 | 36.18 | 16.76 | 36.183 ms | 16.88 | 35.818 ms |
| code | 37.00 | 36.25 | 18.09 | 35.712 ms | 18.11 | 35.500 ms |

All `shared` stdout matched serial target. All validated rows had `max_delta=0`
and mismatches `0`.

Decision for this slice: **correct but not promotable**. Batching the shared
expert independently is exact, unlike full FFN/MoE batching, but it saves only
`0.02-0.36 ms` of N=2 verifier decode GPU over FFN-pre and does not change the
throughput picture. This rules out the shared expert alone as the missing
economics lever. A promotable verifier now needs either an exact small-N routed
MoE/router rewrite, an attention-tail rewrite, or a stronger target-state
contract that avoids paying the verifier tail once per drafted token.

### 2026-05-14 Decode-Native Routed-MoE Batch Probe

Implemented `DS4_MTP_VERIFIER_DECODE2_BATCH_ROUTED=1` to isolate the routed
tail:

- FFN HC-pre/RMSNorm remains batched.
- Router selection and route-weight normalization stay row-exact.
- Routed MoE runs through the tiny batch MoE path.
- Shared expert is batched as in `DECODE2_BATCH_SHARED`; HC post stays
  row-exact.

This probe also tightened the tiny batch routed-MoE direct down-sum path so the
special `sum6` down kernel is used only for the one-token decode shape. The
previous `n_tokens <= 4` condition was not safe for exact verifier use.

Correctness artifacts:

- Fused tiny routed pair+SwiGLU:
  `/tmp/ds4-mtp-matrix/decode2_batch_routed_smoke_20260514_123507`
- Fused pair+SwiGLU disabled:
  `/tmp/ds4-mtp-matrix/decode2_batch_routed_no_pair_swiglu_smoke_20260514_123702`
- Direct down-sum narrowed to one-token:
  `/tmp/ds4-mtp-matrix/decode2_batch_routed_nodirect_smoke_20260514_123622`

Results:

- Default routed batch with fused pair+SwiGLU failed the exact validation path:
  first two-token cycle drifted to `max_delta=27.1089` and changed the serial
  top id on row 1.
- Narrowing direct down-sum to one-token did not fix the fused routed path:
  `max_delta=17.2148` with top-id mismatch.
- Disabling the routed pair+SwiGLU fusion avoided top-id mismatches on the
  count smoke, but still drifted logits up to `max_delta=2.67478`.

Decision for this slice: **not exact, not promotable**. Keeping the router
row-exact is not enough; the existing batched routed-MoE primitives change the
target hidden/logit trajectory for N=2. A promotable routed-tail verifier would
need a genuinely exact small-N routed MoE implementation, not the current batch
MoE executor with different fusion/accumulation behavior.

### 2026-05-14 Decode-Native Attention-Output Batch Probe

Implemented `DS4_MTP_VERIFIER_DECODE2_BATCH_ATTN_OUT=1` to test the remaining
attention-tail lever without changing the causal cache contract:

- Q/KV projection, KV store, compressor/indexer updates, and causal attention
  remain row-exact and decode-ordered.
- The verifier stops each row after producing attention heads.
- The two rows then share one batched inverse RoPE, one batched attention output
  projection, and one batched HC expand/split.
- The path then continues through the already exact FFN-pre split. It can also
  be combined with `DS4_MTP_VERIFIER_DECODE2_BATCH_SHARED=1`.

Correctness artifacts:

- Attention-output smoke:
  `/tmp/ds4-mtp-matrix/decode2_batch_attn_out_smoke_20260514_124659`
- Attention-output plus shared-expert smoke:
  `/tmp/ds4-mtp-matrix/decode2_batch_attn_out_shared_smoke_20260514_124820`
- Full matrix:
  `/tmp/ds4-mtp-matrix/decode2_batch_attn_out_matrix_20260514_124916`

Both smoke runs matched deterministic serial stdout and validated with
`max_delta=0`, mismatches `0`.

Matrix command shape: `-n 64 --temp 0 --ctx 1024 --nothink -sys ""`,
`DS4_MTP_SCHED2=1`, `DS4_MTP_SCHED2_VALIDATE=1`,
`DS4_MTP_VERIFIER_BATCH_HEAD=1`, `DS4_MTP_VERIFIER_TIMING=1`,
`DS4_MTP_GOVERNOR_DISABLE=1`.

| Prompt | Serial | MTP disabled | ffnpre | shared | attn-out | attn-out+shared |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| count | 36.07 | 35.50 | 19.25 | 19.17 | 19.09 | 19.18 |
| explain | 35.92 | 35.44 | 21.35 | 21.40 | 21.22 | 21.06 |
| code | 36.01 | 35.55 | 19.41 | 19.42 | 19.26 | 19.31 |

| Prompt | ffnpre decode GPU | shared decode GPU | attn-out decode GPU | attn-out+shared decode GPU |
| --- | ---: | ---: | ---: | ---: |
| count | 26.894 ms | 27.068 ms | 27.179 ms | 27.035 ms |
| explain | 26.709 ms | 26.613 ms | 26.847 ms | 26.991 ms |
| code | 27.125 ms | 27.084 ms | 27.398 ms | 27.398 ms |

All matrix variants matched serial stdout. All validated rows had
`max_delta=0`, mismatches `0`.

Decision for this slice: **correct but not promotable**. The important finding
is architectural: the one-row decode path already has a fused
attention-output-plus-HC kernel. The batched attention-output probe is exact,
but it replaces that fused one-row work with a batched but less fused sequence,
so it does not reduce verifier decode GPU time. This closes the attention-tail
wrapper path. A stronger attention rewrite would need a new exact small-N fused
attention-output-plus-HC kernel, not reuse of the existing batched prefill
attention-output executor.

### 2026-05-14 Small-N Fused Attention-Output+HC Probe

Implemented `DS4_MTP_VERIFIER_DECODE2_BATCH_ATTN_OUT_FUSED=1` as the direct
follow-up to the wrapper result:

- Added a low-projection-only batched attention-output helper.
- Extended `kernel_dsv4_q8_hc_expand4_q8_0` so the exact one-row
  output-B/HC fusion can run over `n_tokens=2` in the Metal grid z dimension.
- The matvec reduction order remains the same per token/row; the only intended
  shape change is dispatching both verifier rows through one fused kernel.
- Combined smoke with `DS4_MTP_VERIFIER_DECODE2_BATCH_SHARED=1` also passed,
  but did not improve timing.

Artifacts:

- Fused smoke:
  `/tmp/ds4-mtp-matrix/decode2_batch_attn_out_fused_smoke_20260514_125901`
- Fused three-prompt matrix:
  `/tmp/ds4-mtp-matrix/decode2_batch_attn_out_fused_matrix_20260514_125934`
- Fused plus shared smoke:
  `/tmp/ds4-mtp-matrix/decode2_batch_attn_out_fused_shared_smoke_20260514_130014`

All fused runs matched serial stdout and validated with `max_delta=0`,
mismatches `0`.

| Prompt | fused t/s | fused decode GPU | prior best exact t/s | prior best exact decode GPU |
| --- | ---: | ---: | ---: | ---: |
| count | 18.95 | 27.329 ms | 19.25 (`ffnpre`) | 26.894 ms |
| explain | 21.04 | 27.079 ms | 21.40 (`shared`) | 26.613 ms |
| code | 19.37 | 27.214 ms | 19.42 (`shared`) | 27.084 ms |

Decision for this slice: **correct but not promotable**. Even restoring the
fused output-B/HC contract for the two verifier rows does not lower the target
verifier span. The attention tail is not the missing verifier-economics lever in
this DS4 Metal shape.

Current verifier-economics conclusion: the exact implementation slices have now
covered HC-attn prebatch, FFN-pre, shared expert, routed MoE, attention-output
wrapper batching, and a small-N fused attention-output+HC kernel. The exact
slices are not fast enough; the only larger routed-MoE slice is not exact. The
next real implementation fork is either:

- build exact small-N fused kernels for routed MoE and attention-output+HC, or
- change the target-state contract so accepted prefixes commit without replay
  and without paying a repeated serial verifier tail.

### 2026-05-14 Greenfield Block-Verifier Path

Implemented `DS4_MTP_BLOCK_VERIFY=1` as a new speculative block-verifier path:

- Uses existing `--mtp-draft K` as the draft block size, clamped to `K <= 8`.
- Adds `DS4_MTP_BLOCK_TIMING=1` and `DS4_MTP_BLOCK_VALIDATE=1`.
- Leaves normal decode unchanged when the flag is absent.
- Does not touch CUDA.
- Schedules async MTP continuation drafting while the target verifies the
  current block; accepted suffix work is promoted only after full block accept,
  and discarded on partial accept.
- Emits one parseable `ds4: mtp block cycle ...` line with drafted/committed
  counts, async wait/promote cost, discarded work, target/MTP GPU spans, overlap,
  validation max delta, and mismatch counts.

The first implementation tried to use the fast layer-batch verifier
(`metal_graph_verify_suffix_tops`) as the block verifier. It produced matching
top ids in validation, but production stdout diverged because the batch verifier
did not produce a safe continuation state. That path is not promotable. The live
block path now uses `metal_graph_verify_decode_exact()` as the exact transaction
substrate and extends it with block-prefix capture for every accepted prefix
slot. `spec_frontier_commit_block_prefix()` then commits the captured target
state without serial replay of accepted tokens.

Local harness:

- Added `./ds4_test --metal-block-verifier`, a Metal top-id harness for the
  output-head acceptance primitive.

Local checks:

```sh
make ds4_test ds4
./ds4_test --metal-block-verifier
./ds4_test --metal-sched2
./ds4_test --metal-kernels
git diff --check
```

All checks pass.

Smoke correctness:

- `K=2`, `K=3`, and `K=8`, count prompt, `-n 16 --temp 0 --seed 1`,
  `DS4_MTP_BLOCK_VERIFY=1 DS4_MTP_BLOCK_TIMING=1`: stdout matched serial target.
- `K=2` validation smoke: stdout matched serial target, `mismatches=0`,
  max diagnostic logit delta around `1.5e-05`.

Three-prompt production matrix:

- Artifact: `/tmp/ds4-mtp-matrix/block_verify_20260514_172315`
- Command shape: `--ctx 1024 --nothink -sys "" --temp 0 -n 64`
- Block variant: `DS4_MTP_BLOCK_VERIFY=1 DS4_MTP_BLOCK_TIMING=1 --mtp-draft 2`

| Prompt | Serial target | MTP-loaded disabled | Block verifier K=2 | Stdout |
| --- | ---: | ---: | ---: | --- |
| count | 38.28 | 37.86 | 35.99 | match |
| explain | 39.14 | 37.58 | 34.52 | match |
| code | 39.24 | 38.57 | 34.85 | match |

K=2 block timing averages:

| Prompt | avg verify wall | avg target GPU | avg accepted/cycle |
| --- | ---: | ---: | ---: |
| count | 65.232 ms | 65.655 ms | 1.500 |
| explain | 65.625 ms | 66.052 ms | 1.600 |
| code | 68.035 ms | 68.434 ms | 1.250 |

Validation matrix:

| Prompt | validate t/s | stdout | max_delta | max mismatches | avg validate wall |
| --- | ---: | --- | ---: | ---: | ---: |
| count | 31.50 | match | 1.14441e-05 | 0 | 54.471 ms |
| explain | 30.03 | match | 1.14441e-05 | 0 | 53.929 ms |
| code | 30.90 | match | 1.14441e-05 | 0 | 53.046 ms |

Decision for this slice: **correct, not yet promotable**. The greenfield block
transaction now has the right output-exact contract and no serial replay on
accept, but the exact decode-block verifier is still slower than both baselines.
The next implementation work should optimize the exact substrate itself:

- split decode-block timing into hidden-state versus output-head spans in the
  block timing line;
- use the existing top-id-only head path for acceptance while reading only the
  final committed logits row;
- build exact small-N fused decode kernels for the expensive routed-MoE and
  attention/output tail, starting from `K=2`;
- keep the unsafe layer-batch verifier only as negative evidence unless it can
  produce exact promotable target state.

#### Decode2 Optimization Sweep Under Block Commit

After the block transaction was correct, the existing exact `decode2` verifier
variants were re-tested under the new no-replay commit contract. The decode2
prebatch path now captures block-prefix state as well as the legacy prefix1/2
slots, so these variants can be tested without falling out of the block commit
path.

Artifacts:

- Count sweep: `/tmp/ds4-mtp-matrix/block_decode2_sweep_20260514_174220`
- Explain sweep: `/tmp/ds4-mtp-matrix/block_decode2_explain_sweep_20260514_174505`
- Code sweep: `/tmp/ds4-mtp-matrix/block_decode2_code_sweep_20260514_174540`
- Unsafe fused-default matrix:
  `/tmp/ds4-mtp-matrix/block_verify_fused_default_20260514_174350`
- Unsafe prebatch-default matrix:
  `/tmp/ds4-mtp-matrix/block_verify_prebatch_default_20260514_174635`

Short `-n 32 --seed 1` sweeps:

| Prompt | Variant | t/s | stdout | avg verify wall |
| --- | --- | ---: | --- | ---: |
| count | none | 33.52 | match | 65.743 ms |
| count | prebatch | 33.80 | match | 63.309 ms |
| count | ffnpre | 34.01 | match | 62.576 ms |
| count | shared | 34.19 | match | 61.085 ms |
| count | attn_fused | 34.87 | match | 56.827 ms |
| explain | none | 32.27 | match | 66.535 ms |
| explain | prebatch | 32.47 | match | 63.885 ms |
| explain | ffnpre | 31.43 | mismatch | 63.341 ms |
| explain | shared | 33.14 | match | 61.144 ms |
| explain | attn_fused | 31.41 | mismatch | 62.350 ms |
| code | none | 31.96 | match | 66.838 ms |
| code | prebatch | 32.38 | match | 63.342 ms |
| code | shared | 30.43 | match | 66.490 ms |
| code | attn_fused | 33.30 | match | 56.578 ms |

Longer `-n 64` default-promotion checks rejected both automatic optimizations:

- Making `attn_fused` the block default improved count, but explanation stdout
  diverged.
- Making conservative `prebatch` the block default still diverged on the
  explanation prompt at `-n 64`.

Decision: **do not promote any decode2 optimization as the default yet**.
`prebatch`, `shared`, and `attn_fused` remain opt-in probes. The only
three-prompt-safe block default is the plain exact decode-block verifier. The
next optimization must either prove exactness on the full `-n 64` matrix before
promotion or introduce a runtime validation/fallback guard that prevents
drifted verifier state from committing.

#### Block Top1 and Guarded Optimization Probe

Implemented two follow-up block-verifier experiments:

- `DS4_MTP_BLOCK_TOP1=1` uses a new fixed-shape Metal `top1` reduction for
  greedy acceptance instead of the generic top-k argsort path when the block
  verifier is in top-id-only production mode.
- `DS4_MTP_BLOCK_GUARD=1 DS4_MTP_BLOCK_OPT=<mode>` runs a requested decode2
  optimization first as a diagnostic candidate, restores the speculative
  frontier, then runs the exact verifier and commits only the exact verifier
  state. The guard reports whether the candidate top ids/accepted prefix would
  have matched, but it is not a throughput path because it deliberately
  double-runs target verification.

Local checks passed after adding the `top1` kernel and extending the Metal
block-verifier harness to compare top-k and top1 results:

```sh
make ds4_test ds4
./ds4_test --metal-block-verifier
./ds4_test --metal-sched2
./ds4_test --metal-kernels
git diff --check
```

Fresh current-code smoke:

- Artifact: `/tmp/ds4-mtp-matrix/block_try_both_20260514_181942`
- Prompt: explanation standard prompt, `-n 24 --temp 0 --ctx 1024 --nothink -sys ""`
- Block variants used `--mtp-draft 2` and `DS4_MTP_GOVERNOR_DISABLE=1`.

| Variant | t/s | stdout vs serial | avg verify wall | Notes |
| --- | ---: | --- | ---: | --- |
| serial target | 36.09 | oracle | n/a | baseline |
| block verifier | 27.40 | match | 63.625 ms | exact block commit |
| block + top1 | 26.62 | match | 66.094 ms | top1 kernel correct, no timing win |
| guarded `attn_fused` | 7.10 | match | 293.996 ms | `guard_rejected=0/9`, exact state committed |

Decision: **do not promote either follow-up yet**. The top1 kernel is correct
and useful as a smaller acceptance primitive, but the output-head/top-k slice
is not the current bottleneck. The guard is useful evidence for candidate
decode2 modes, but by construction it cannot improve throughput until a
candidate is proven safe enough to run without the exact fallback.

#### Block Decode2 Exact-Matvec Scope Fix

The first block `attn_fused` judgment was too pessimistic. The older sched2
verifier wrapped decode2 variants in `ds4_gpu_set_verifier_exact_mv_batch(1)`,
which forces the small-N Q8/F16 matvec path to keep decode-style reductions.
The new `DS4_MTP_BLOCK_VERIFY=1` path did not set that scope, so
`DS4_MTP_BLOCK_OPT=prebatch|shared|attn_fused` was using faster batched matmul
reductions inside a path being judged for exactness.

Fix:

- Block verifier now enables the exact small-N matvec scope whenever
  `DS4_MTP_VERIFIER_EXACT_MV_BATCH=1` is set or `DS4_MTP_BLOCK_OPT` selects a
  decode2 candidate.
- Guarded candidates and standalone block candidates both run under the same
  exact reduction contract.

Failure before the fix:

- Artifact: `/tmp/ds4-mtp-matrix/attn_fused_validate_20260514_193118`
- `DS4_MTP_BLOCK_OPT=attn_fused`, explanation prompt, `-n 64`,
  validation enabled.
- Stdout diverged, verifier row top ids still matched, but
  `max_delta=2.48571`. This means immediate greedy acceptance was not enough:
  logit/state drift later changed normal target sampling.

Correctness after the fix:

- Single failure-case validation:
  `/tmp/ds4-mtp-matrix/attn_fused_block_exactmv_scope_20260514_193518`
- Result: explanation `-n 64`, `stdout=match`, `max_delta=0`,
  mismatches `0`, avg verify `55.818 ms`.

Three-prompt production matrix:

- Artifact: `/tmp/ds4-mtp-matrix/block_attn_fused_fixed_matrix_20260514_193552`
- Command shape: `--ctx 1024 --nothink -sys "" --temp 0 -n 64`,
  block variants use `--mtp-draft 2`.

| Prompt | Serial | MTP disabled | Plain block | Block `attn_fused` | Stdout | Plain verify | Fused verify |
| --- | ---: | ---: | ---: | ---: | --- | ---: | ---: |
| count | 39.24 | 38.07 | 24.49 | 26.88 | match | 65.352 ms | 57.470 ms |
| explain | 37.63 | 37.84 | 27.05 | 29.77 | match | 63.272 ms | 54.907 ms |
| code | 38.68 | 35.82 | 26.49 | 30.69 | match | 68.861 ms | 57.409 ms |

Three-prompt validation matrix:

- Artifact:
  `/tmp/ds4-mtp-matrix/block_attn_fused_fixed_validate_20260514_193652`

| Prompt | validate t/s | stdout | cycles | avg verify | max_delta | mismatches |
| --- | ---: | --- | ---: | ---: | ---: | ---: |
| count | 16.28 | match | 32 | 55.025 ms | 0 | 0 |
| explain | 18.17 | match | 26 | 55.559 ms | 0 | 0 |
| code | 18.34 | match | 26 | 57.849 ms | 0 | 0 |

Decision: **keep block `attn_fused` as the current best exact block-verifier
slice, but do not promote it as the final MTP path yet**. The fix turns
`attn_fused` from unsafe negative evidence into a correct improvement over the
plain block verifier, saving roughly `8-11 ms` per N=2 verifier call. It is
still below serial target and MTP-disabled baselines, so the next implementation
work should build on this exact slice and attack the remaining verifier cost,
especially routed MoE and target hidden-state tail work.

#### Exact N=2 Routed-MoE Slice

Implemented a verifier-only routed-MoE slice behind
`DS4_MTP_BLOCK_ROUTED=1`. This composes with
`DS4_MTP_BLOCK_OPT=attn_fused` and is intentionally scoped to the block
verifier, not the normal decode path.

Fixes and implementation details:

- Fixed the fused routed gate/up+SwiGLU kernels for N>1. The IQ2_XXS and Q4_K
  pair-SwiGLU kernels were writing `mid` and reading route weights using only
  the expert slot. For block verification they must index by
  `token * selected_experts + slot`. This explains the previous routed drift:
  one-token decode was safe, but N=2 reused token-0 routing data for token-1
  activation rows.
- Added `ds4_gpu_set_moe_batch_direct_sum6()` as a scoped Metal switch.
  `DS4_MTP_BLOCK_ROUTED=1` enables the N=2 direct six-expert down-sum only
  inside the exact block-verifier scope, preserving the normal batched MoE
  behavior outside this experiment.
- `DS4_MTP_BLOCK_ROUTED=1` forces the decode2 routed path through the block
  verifier and keeps the exact small-N matvec scope active.
- CUDA was not touched.

Local smoke:

- Artifact: `/tmp/ds4-mtp-matrix/routed_fixed_probe_20260514_200146`
- Explanation prompt, `-n 32`, validation enabled.
- `attn_fused`: stdout match, `max_delta=0`, mismatches `0`.
- `attn_fused+routed`: stdout match, `max_delta=0`, mismatches `0`.

Studio matrix:

- Host: `studio.local`, Apple M3 Ultra.
- Artifact: `/tmp/ds4-mtp-matrix/block_routed_studio_20260514_201125`
- Command shape: `--ctx 1024 --nothink -sys "" --temp 0 -n 64`,
  block variants use `--mtp-draft 2` and `DS4_MTP_GOVERNOR_DISABLE=1`.

Production matrix:

| Prompt | Serial | MTP disabled | Plain block | `attn_fused` | `attn_fused+routed` | Stdout | Plain verify | Fused verify | Routed verify |
| --- | ---: | ---: | ---: | ---: | ---: | --- | ---: | ---: | ---: |
| count | 36.87 | 36.22 | 24.01 | 26.23 | 26.63 | match | 44.361 ms | 39.742 ms | 38.863 ms |
| explain | 36.86 | 36.32 | 26.96 | 29.14 | 29.55 | match | 45.074 ms | 40.240 ms | 39.304 ms |
| code | 36.71 | 36.42 | 26.69 | 28.92 | 29.48 | match | 44.414 ms | 39.901 ms | 38.836 ms |

Validation matrix:

| Prompt | `attn_fused` validate t/s | `attn_fused+routed` validate t/s | Stdout | Fused verify | Routed verify | Routed max_delta | Routed mismatches |
| --- | ---: | ---: | --- | ---: | ---: | ---: | ---: |
| count | 15.21 | 15.41 | match | 39.961 ms | 38.893 ms | 0 | 0 |
| explain | 18.03 | 18.25 | match | 40.489 ms | 39.380 ms | 0 | 0 |
| code | 17.29 | 17.53 | match | 39.861 ms | 38.773 ms | 0 | 0 |

Checks:

```sh
make ds4_test ds4
./ds4_test --metal-block-verifier
./ds4_test --metal-sched2
./ds4_test --metal-kernels
git diff --check
```

The same Metal harnesses also passed on `studio.local`.

Decision: **promote `DS4_MTP_BLOCK_ROUTED=1` as the best current exact
experimental N=2 block-verifier slice, but do not promote it to the default
decode path**. It is a real improvement over fixed `attn_fused`, saving about
`0.9-1.1 ms` per verifier call and improving throughput by roughly
`0.4-0.6 t/s` on the standard prompts. It still loses badly to both serial
target and the MTP-loaded-disabled baseline, so routed MoE alone is not enough
to make the block-verifier architecture viable. The next work should target
larger remaining costs in the target hidden-state tail and acceptance economics
rather than continuing to shave only the routed expert slice.

#### DFlash-Inspired K=16 Block Smoke

After reviewing `z-lab/dflash`, we tested the analogous large-block idea for
the DS4 MTP path: let the MTP drafter propose a much larger autoregressive
suffix, then verify the whole block with the exact target verifier. The DFlash
MLX implementation verifies `[last_committed, draft_1, ..., draft_K]` in one
target call, accepts the longest matching prefix, and emits one target
correction token while the target cache intentionally lags by that correction.
DS4 does not yet have that lagging-cache/bonus-token state contract, so this
smoke keeps the current exact DS4 transaction contract and only unlocks larger
block verification.

Implementation:

- Raised `DS4_MTP_BLOCK_MAX_K` from `8` to `16`.
- This lets `DS4_MTP_BLOCK_VERIFY=1 --mtp-draft 16` exercise the existing
  exact block verifier instead of truncating at `K=8`.
- The exact verifier already had an internal `16`-row limit and `spec_logits`
  was already allocated for `16` rows; the change expands the captured
  block-prefix state slots to match.
- CUDA was not touched.

Local artifact:

- `/tmp/ds4-k16-block-20260515_081434`
- Model: local `ds4flash.gguf` symlink on Apple M5 Max.
- MTP: `/Users/alessandro/git/antirez/ds4/gguf/DeepSeek-V4-Flash-MTP-Q4K-Q8_0-F32.gguf`
- Prompt: `Count from 1 to 200, separated by spaces.`
- Command shape: `--ctx 1024 --nothink -sys "" --temp 0`.

Correctness:

- `-n 24`, `DS4_MTP_BLOCK_VALIDATE=1`, stdout matched serial target.
- Diagnostic validation reported `mismatches=0`.
- `max_delta` was around `0.017-0.019` on some K=16 rows but did not change
  accepted top ids; later small rows were at `1.52588e-05`.
- `-n 64` production stdout also matched serial target.

Timing:

| Run | t/s | cycles | avg drafted | avg committed | avg discarded | avg draft | avg verify | avg target GPU | avg overlap |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| Serial target, `-n 64` | 37.67 | - | - | - | - | - | - | - | - |
| K=16 block, `-n 64` | 8.86 | 16 | 14.25 | 3.00 | 12.00 | 28.595 ms | 393.135 ms | 393.543 ms | 1.352 ms |

Interpretation:

- The autoregressive MTP drafter is fast enough to make `K=16` plausible as a
  drafter experiment: about `31 ms` for a full 16-token cycle on the count
  prompt.
- The current exact target verifier is not a DFlash-style cheap block forward.
  It still scales almost linearly with the number of verified rows: roughly
  `430-480 ms` for full K=16 cycles, with only `3` committed tokens.
- Larger K therefore makes the present verifier path worse, not better. The
  useful lesson from DFlash is not merely "try K=16"; it is the missing state
  contract and verifier economics:
  - allow a pending bonus/correction token whose target state is materialized by
    the next block verifier input, or otherwise commit it without serial replay;
  - build a genuinely cheaper target block forward instead of replaying
    decode-exact rows under one wrapper.

Checks:

```sh
make ds4_test ds4
./ds4_test --metal-block-verifier
./ds4_test --metal-sched2
./ds4_test --metal-kernels
git diff --check
```

#### MLX-Style Chunk Verifier Smoke

Implemented a separate experimental verifier switch:
`DS4_MTP_BLOCK_CHUNK_VERIFY=1`. With this flag, the block verifier bypasses
the repeated decode-exact verifier and routes the target check through
`metal_graph_verify_suffix_tops()`, which performs one native target chunk
forward over the drafted suffix and returns shifted row top ids. The block
transaction still captures every accepted-prefix state slot so DS4 can commit
accepted tokens without serial replay.

This is closer to the DFlash/MLX verifier shape than the previous K=16 smoke,
but it is not yet the full DFlash state contract. DFlash can emit a target
correction token while the target cache intentionally lags by one token and
materializes that token in the next block input. DS4 still requires every
emitted token to be materialized in the current session state, so this first
smoke keeps the stricter DS4 transaction contract.

Implementation:

- Added `DS4_MTP_BLOCK_CHUNK_VERIFY=1`.
- Disabled the N=2 routed and guard verifier variants under this flag so the
  chunk path is measured directly.
- Kept CUDA out of scope.

Local artifact:

- `/tmp/ds4-mlxstyle-chunk-20260515_082203`
- Model: local `ds4flash.gguf` symlink on Apple M5 Max.
- MTP: `/Users/alessandro/git/antirez/ds4/gguf/DeepSeek-V4-Flash-MTP-Q4K-Q8_0-F32.gguf`
- Prompt: `Count from 1 to 200, separated by spaces.`
- Command shape: `--ctx 1024 --nothink -sys "" --temp 0`.

Correctness:

- Short `-n 24` run with `DS4_MTP_BLOCK_VALIDATE=1` matched serial stdout.
- The accepted-row top ids matched (`mismatches=0`).
- Diagnostic full-logit deltas were large on the chunk path
  (`max_delta` up to about `2.68`), but they did not change accepted top ids in
  this smoke. This remains output-exact evidence, not strict logit-identity
  evidence.
- Production `-n 64` stdout matched serial target on the count prompt.

Timing:

| Run | t/s | cycles | avg drafted | avg committed | avg discarded | avg draft | avg verify | avg target GPU | avg overlap |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| Serial target, `-n 64` | 35.96 | - | - | - | - | - | - | - | - |
| K=16 exact block, `-n 64` | 8.86 | 16 | 14.25 | 3.00 | 12.00 | 28.595 ms | 393.135 ms | 393.543 ms | 1.352 ms |
| K=16 chunk block, `-n 64` | 14.97 | 16 | 14.25 | 3.00 | 12.00 | 28.542 ms | 208.571 ms | 208.986 ms | 1.386 ms |

Interpretation:

- The chunk verifier is a real improvement over the repeated exact verifier:
  it cuts K=16 target verification by about `47%`.
- It is still not economically viable because the drafter only commits about
  `3` tokens per 16-token block on this prompt. That makes the target verifier
  cost roughly `69.5 ms` per committed speculative token before counting draft
  and commit overhead, while serial target generation is about `27.8 ms` per
  token in the same local run.
- The useful next implementation target, if we continue this direction, is not
  another wrapper around the current transaction. It is either:
  - a DFlash-style pending correction / lagging-cache contract for DS4 output,
    so the target verifier can emit the mismatch token without immediate serial
    replay; or
  - a drafter that can raise accepted prefix length materially above `3/16`.

Current decision: **keep `DS4_MTP_BLOCK_CHUNK_VERIFY=1` as evidence and a
debug/prototype path, but do not promote it**. It proves that the single target
chunk forward exists and is faster than repeated exact row verification, but it
does not yet beat serial target generation under the current DS4 MTP acceptance
rate.

#### Lagged-Cache / Pending-Correction Contract

Implemented `DS4_MTP_BLOCK_LAGGED_CACHE=1`. This adds the missing DFlash-style
runtime contract in a conservative form:

- A block verifier may emit one target bonus/correction token after the accepted
  draft prefix.
- That bonus token is returned to the caller for output, but it is not appended
  to the materialized target checkpoint yet.
- The session records it as a pending token.
- On the next speculative step, DS4 materializes that pending token without
  returning it again, then immediately continues into the block verifier for the
  next target frontier.

The first implementation deliberately discards promoted MTP suffix work whenever
a pending correction is created. That keeps correctness simple: any suffix
drafted from the old MTP chain is reused only after a later implementation can
prove it was drafted from the same correction token.

Important DS4-specific finding:

- The lagged contract is implementable, but it does not remove the need to
  materialize the correction token before drafting the next DS4 MTP suffix.
- DS4's autoregressive MTP layer needs the target hidden state of the
  correction token to draft after it. DFlash's diffusion drafter can propose a
  larger block from a different contract; DS4 cannot safely draft after an
  unmaterialized correction with the current MTP model state.
- Therefore this implementation improves the output contract and verifies the
  state-machine shape, but it does not by itself make target work cheaper.

Code locations:

- Pending state: `ds4_session.mtp_lagged_pending_valid` and
  `mtp_lagged_pending_token`.
- Env flag: `DS4_MTP_BLOCK_LAGGED_CACHE=1`.
- Emission point: block verifier emits `lagged_bonus` after committing the
  accepted prefix.
- Materialization point: the next speculative step consumes the pending token
  without returning it again, then continues to the next block.

Local correctness smoke:

- Artifact: `/tmp/ds4-lagged-exact-local-20260515_085814`
- Prompt: standard code prompt, `-n 32`.
- Flags:
  `DS4_MTP_BLOCK_VERIFY=1 DS4_MTP_BLOCK_LAGGED_CACHE=1 DS4_MTP_BLOCK_VALIDATE=1 DS4_MTP_BLOCK_TIMING=1 DS4_MTP_GOVERNOR_DISABLE=1`
- Stdout matched serial.
- Accepted-row validation reported `mismatches=0`.
- This validates the lagged contract on the exact verifier.

Studio exact-verifier matrix:

- Artifact: `/tmp/ds4-lagged-exact-studio-20260515_085903`
- Host: `studio.local`, Apple M3 Ultra.
- Flags:
  `DS4_MTP_BLOCK_VERIFY=1 DS4_MTP_BLOCK_LAGGED_CACHE=1 DS4_MTP_BLOCK_TIMING=1 DS4_MTP_GOVERNOR_DISABLE=1`
- Command shape: `--ctx 1024 --nothink -sys "" --temp 0 -n 64 --mtp-draft 16`.

| Prompt | Serial target | Exact lagged | Stdout | Cycles | Avg committed | Avg accepted | Avg verify | Bonus cycles | Avg pending materialize |
| --- | ---: | ---: | --- | ---: | ---: | ---: | ---: | ---: | ---: |
| count | 35.65 t/s | 8.49 t/s | match | 16 | 3.000 | 3.938 | 414.392 ms | 15 | 29.617 ms |
| explain | 35.45 t/s | 7.31 t/s | match | 19 | 2.368 | 3.316 | 404.203 ms | 18 | 29.890 ms |
| code | 35.35 t/s | 8.38 t/s | match | 16 | 2.938 | 3.938 | 419.847 ms | 16 | 29.940 ms |

Studio chunk-verifier matrix with the same lagged contract:

- Artifact: `/tmp/ds4-lagged-studio-20260515_085526`
- Flags:
  `DS4_MTP_BLOCK_VERIFY=1 DS4_MTP_BLOCK_CHUNK_VERIFY=1 DS4_MTP_BLOCK_LAGGED_CACHE=1 DS4_MTP_BLOCK_TIMING=1 DS4_MTP_GOVERNOR_DISABLE=1`

| Prompt | Serial target | Chunk block | Chunk + lagged | Chunk stdout | Lagged stdout | Chunk committed | Lagged accepted | Chunk verify | Lagged verify | Pending materialize |
| --- | ---: | ---: | ---: | --- | --- | ---: | ---: | ---: | ---: | ---: |
| count | 35.46 t/s | 13.57 t/s | 13.54 t/s | match | match | 3.000 | 3.938 | 237.727 ms | 238.254 ms | 29.789 ms |
| explain | 35.70 t/s | 10.74 t/s | 10.74 t/s | match | match | 2.048 | 3.000 | 228.147 ms | 228.119 ms | 30.215 ms |
| code | 35.62 t/s | 11.75 t/s | 11.74 t/s | DIFF | DIFF | 2.368 | 3.316 | 229.455 ms | 229.480 ms | 30.451 ms |

Interpretation:

- The lagged-cache contract itself works: exact lagged mode matches serial
  stdout across the three standard prompts.
- The conservative contract raises emitted tokens per block by about one token
  on most cycles, but then pays about `30 ms` to materialize that pending token
  before the next DS4 MTP draft can be produced.
- With the exact verifier, target verification is still roughly `400-420 ms`
  per block, so the path is much slower than serial.
- With the chunk verifier, target verification drops to roughly `228-238 ms`,
  but the code prompt proves the chunk row-top path is not output-exact yet.
- Current decision: **keep `DS4_MTP_BLOCK_LAGGED_CACHE=1` as a correct
  experimental runtime contract, but do not promote either exact-lagged or
  chunk-lagged**. The next useful work is either fixing chunk-verifier row-top
  exactness for all prompts, or changing the drafter/state contract so DS4 can
  draft after a correction without first paying a separate target materialize.

#### Approximate-Quality Gate Pivot

The exact-output requirement is still the right gate for a true speculative
decoder: if DS4 claims to preserve serial target greedy output, stdout/top-id
must match serial. However, DFlash-style project framing is also useful for a
different question: can this become a faster approximate decoder whose quality
is still acceptable on task benchmarks?

That is a separate promotion contract. Under this approximate contract:

- Exact stdout match becomes a diagnostic, not the pass/fail criterion.
- The quality gate must measure task quality and throughput together.
- A speedup that degrades task quality is not promotable.
- A non-exact candidate may be worth keeping if it passes a bounded quality
  suite while beating serial and MTP-disabled baselines.

Added repo tooling for that track:

- `tools/mtp_quality_gate.sh` starts `ds4-server`, generates EvalPlus samples,
  runs syntax checks, runs a subset HumanEval+/MBPP+ evaluator, and summarizes
  completion TPS.
- `tools/evalplus_ds4.py` generates DS4 samples against the OpenAI-compatible
  server API.
- `tools/evalplus_subset_evaluate.py` scores only the tasks present in the
  generated sample JSONL, avoiding the stock EvalPlus all-tasks requirement for
  quick iteration.
- `tools/evalplus_tps_summary.py` summarizes per-task metadata into aggregate
  throughput.

This mirrors the practical shape of the DFlash benchmark suite: dataset-backed
quality and throughput rather than byte-for-byte transcript comparison. It also
matches earlier DS4 MTP quick-gate practice: sample generation, syntax counting,
subset EvalPlus scoring, and TPS summary.

Example serial baseline:

```sh
make ds4-server
tools/mtp_quality_gate.sh \
  --mode serial \
  --dataset humaneval \
  --task-ids HumanEval/0,HumanEval/8,HumanEval/16,HumanEval/24,HumanEval/32
```

Example approximate chunk/lagged candidate:

```sh
DS4_QUALITY_MTP_MODEL=/path/to/DeepSeek-V4-Flash-MTP-Q4K-Q8_0-F32.gguf \
tools/mtp_quality_gate.sh \
  --mode block-k16-chunk-lagged \
  --dataset humaneval \
  --task-ids HumanEval/0,HumanEval/8,HumanEval/16,HumanEval/24,HumanEval/32 \
  --server-env "DS4_MTP_BLOCK_VERIFY=1 DS4_MTP_BLOCK_CHUNK_VERIFY=1 DS4_MTP_BLOCK_LAGGED_CACHE=1 DS4_MTP_GOVERNOR_DISABLE=1" \
  -- --mtp "{MTP}" --mtp-draft 16
```

Initial promotion rule for approximate mode:

- Run the same task IDs for serial, MTP-loaded-disabled, exact current MTP, and
  the approximate candidate.
- Require no syntax regression.
- Require equal or better subset pass@1 versus the serial baseline on the
  selected quick gate before looking at speed.
- If the quick gate passes, expand to a larger HumanEval+/MBPP+ slice before
  considering promotion.

Current decision: **reframe chunk-lagged as an approximate-quality candidate,
not as exact speculative decoding**. The code prompt divergence from the
standard three-prompt matrix is no longer an automatic disqualifier for this
track, but it must be paid for with benchmark quality evidence.

Studio quality-gate smoke:

- Artifact: `/tmp/ds4-mtp-quality-studio-20260515_091216`
- Host: `studio.local`.
- Dataset: HumanEval+ mini scoring through EvalPlus.
- Task slice:
  `HumanEval/0,HumanEval/8,HumanEval/16,HumanEval/24,HumanEval/32`.
- Max tokens: `1024`.

| Mode | Syntax | Plus pass@1 | Failed task | Tokens | Aggregate TPS | Exact vs serial |
| --- | ---: | ---: | --- | ---: | ---: | --- |
| `serial` | 5/5 | 0.800 | `HumanEval/32` | 738 | 28.19 | baseline |
| `mtp-loaded-disabled` | 5/5 | 0.800 | `HumanEval/32` | 738 | 29.18 | exact |
| `exact-mtp-d2` | 5/5 | 0.800 | `HumanEval/32` | 738 | 29.05 | exact |
| `block-k16-chunk-lagged` | 5/5 | 0.800 | `HumanEval/32` | 551 | 11.32 | differs |

Chunk/lagged exactness diagnostics against serial:

| Task | Exact output | Sequence similarity | Serial chars | Chunk chars |
| --- | --- | ---: | ---: | ---: |
| `HumanEval/0` | yes | 1.000 | 247 | 247 |
| `HumanEval/8` | no | 0.844 | 286 | 254 |
| `HumanEval/16` | yes | 1.000 | 296 | 296 |
| `HumanEval/24` | yes | 1.000 | 128 | 128 |
| `HumanEval/32` | no | 0.574 | 1401 | 905 |

Interpretation:

- The quality gate works and is useful: it allowed a non-exact candidate to be
  assessed on task success instead of immediately rejected by stdout drift.
- On this small slice, `block-k16-chunk-lagged` preserved the same pass@1 and
  syntax score as serial despite non-exact text on two tasks.
- It is still not promotable because it is much slower than serial and exact
  MTP on the same quality gate (`11.32` aggregate TPS versus `28-29`).
- The best current baseline remains normal exact `--mtp-draft 2`, not the
  approximate chunk/lagged verifier.

#### Aggressive K=16 Approximate Chunk Track

Implemented `DS4_MTP_APPROX_CHUNK=1`, an intentionally aggressive path that
does not pretend to be exact speculative decoding:

- MTP supplies a K-token block, using `--mtp-draft K`.
- The target graph is teacher-forced once over the emitted block to keep target
  KV/frontiers and next-token logits coherent with the emitted text.
- No serial replay/materialization is performed on the full-block path.
- `DS4_MTP_APPROX_MIN_PREFIX=0` selects the pure MTP-primary fast path.
- The default guarded path uses target-root anchoring plus
  `DS4_MTP_APPROX_TARGET_TOPK=4`, only committing MTP tokens while each next
  token is inside the target row's top-k set.
- `DS4_MTP_APPROX_TARGET_TOPK=1` is the quality-control point: it reduces to
  exact target-top acceptance through the new no-replay block transaction.

This is the non-conservative experiment that was missing from the earlier
work. It deliberately tries to buy speed with approximate acceptance, then lets
the EvalPlus quality gate decide.

Implementation notes:

- New helper `metal_graph_apply_suffix_last_logits()` runs a target batch over
  the committed approximate suffix and computes only the last-row logits, rather
  than full logits/top-k for every row.
- `metal_graph_verify_suffix_topk()` extends the previous top-1 verifier to
  return target top-k IDs per shifted row for approximate acceptance guards.
- The approximate runtime keeps a one-token MTP preview when the whole block is
  committed, so the next chunk can start from MTP state without a serial target
  materialization.

Count-prompt smoke on `studio.local`:

- Artifact: `/tmp/ds4-approx-smoke-20260515`
- Command shape:
  `--mtp-draft 16 --ctx 1024 --nothink -sys "" --temp 0 -n 64`

| Mode | Speed | Output quality |
| --- | ---: | --- |
| Pure MTP-primary, `DS4_MTP_APPROX_MIN_PREFIX=0` | 59.17 t/s | Bad: derailed into repeated/non-English tokens |
| Target-root prefix guard | 53.53 t/s | Bad: still derailed |
| Target top-4 guarded default | 21.70 t/s | Bad: count sequence corrupted |

HumanEval+ 5-task quality slice on `studio.local`:

- Artifact: `/tmp/ds4-mtp-quality-approx-20260515_094021`
- Task slice:
  `HumanEval/0,HumanEval/8,HumanEval/16,HumanEval/24,HumanEval/32`
- Max tokens: `1024`

| Mode | Syntax | Plus pass@1 | Tokens | Aggregate TPS | Decision |
| --- | ---: | ---: | ---: | ---: | --- |
| `approx-k16-fast` (`MIN_PREFIX=0`) | 0/5 | 0.000 | 1212 | 50.49 | Fast, unusable |
| `approx-k16-prefix4` | 0/5 | 0.000 | 2140 | 13.43 | Unusable and slow |
| `approx-k16-top4` | 1/5 | 0.000 | 2299 | 16.52 | Unusable |
| `approx-k16-top2` | 2/5 | 0.200 | 2231 | 22.18 | Better but below baseline quality |
| `approx-k16-top1` | 4/5 | 0.800 | 559 | 11.87 | Quality-safe but too slow |

Reference from the previous quality-gate smoke:

| Mode | Syntax | Plus pass@1 | Tokens | Aggregate TPS |
| --- | ---: | ---: | ---: | ---: |
| `serial` | 5/5 | 0.800 | 738 | 28.19 |
| `mtp-loaded-disabled` | 5/5 | 0.800 | 738 | 29.18 |
| `exact-mtp-d2` | 5/5 | 0.800 | 738 | 29.05 |

Interpretation:

- The speed lever exists. Pure K=16 MTP-primary generation can exceed serial
  target throughput substantially.
- The current DS4 MTP model is not good enough as an unchecked sampler. It
  collapses syntax and task quality immediately.
- Target top-k plausibility is not a sufficient quality guard: top-2 and top-4
  still fail HumanEval quality.
- Top-1 restores quality, but then the target block transaction is slower than
  the existing exact MTP and serial baselines.

Current decision for this slice: **do not promote approximate K=16 acceptance
yet**. The aggressive implementation finally maps the speed/quality frontier:
the fast corner is real but low quality, and the quality-safe corner is slow.
The next speed work should not be another looser acceptance rule; it should
target the cost of the top-1/no-replay target block transaction, especially
avoiding full per-row output-head/top-k work when only shifted top-1 acceptance
and last-row logits are needed.

#### Target Raw-KV Graft Probe

Question tested: is K=16 quality low because the MTP drafter has only its own
speculative raw cache, rather than access to target-prefix KV/cache history?

Implementation:

- Added `DS4_MTP_TARGET_KV_GRAFT=1`.
- Added `DS4_MTP_TARGET_KV_GRAFT_LAYER=N`, default `1`.
- The probe copies an uncompressed target `layer_raw_cache[N]` into
  `mtp_raw_cache` before the root MTP draft, then sets `mtp_n_raw` to the
  target-prefix raw span before the current token.
- This creates a hybrid cache for the MTP chain:
  target-prefix raw rows plus MTP-owned speculative suffix rows.
- The probe is deliberately limited to uncompressed target layers `0` and `1`.
  Compressed/indexer layer grafting would require a separate cache-space
  translation and is not part of this first falsification pass.

Validation:

- Local:
  - `make ds4_test ds4`
  - `./ds4_test --metal-kernels`
  - `./ds4_test --metal-block-verifier`
  - `./ds4_test --metal-sched2`
  - `git diff --check`
- Studio artifact: `/tmp/ds4-mtp-quality-kvgraft-20260515_210328`
- Task slice:
  `HumanEval/0,HumanEval/8,HumanEval/16,HumanEval/24,HumanEval/32`
- Max tokens: `1024`

| Mode | Syntax | Plus pass@1 | Tokens | Aggregate TPS | Avg committed | Avg exact prefix | Decision |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | --- |
| Previous `approx-k16-fast` | 0/5 | 0.000 | 1212 | 50.49 | n/a | n/a | Fast, unusable |
| `fast-graft-l1` | 0/5 | 0.000 | 605 | 40.59 | 15.82 | 0.45 | No improvement |
| `fast-graft-l0` | 2/5 | 0.000 | 1216 | 50.38 | 15.56 | 0.99 | Syntax better, still fails |
| Previous `approx-k16-top2` | 2/5 | 0.200 | 2231 | 22.18 | n/a | n/a | Below baseline quality |
| `top2-graft-l1` | 1/5 | 0.200 | 959 | 24.58 | 6.99 | 1.75 | No quality lift |
| `top2-graft-l0` | 2/5 | 0.400 | 443 | 15.50 | 2.93 | 1.89 | Some signal, not promotable |
| Previous `approx-k16-top4` | 1/5 | 0.000 | 2299 | 16.52 | n/a | n/a | Unusable |
| `top4-graft-l1` | 2/5 | 0.000 | 229 | 12.94 | 3.51 | 1.53 | No quality lift |
| `top4-graft-l0` | 0/5 | 0.000 | 2329 | 19.91 | 4.97 | 1.72 | Unusable |
| Previous `approx-k16-top1` | 4/5 | 0.800 | 559 | 11.87 | n/a | n/a | Quality-safe, slow |
| `top1-graft-l1` | 5/5 | 0.800 | 747 | 10.00 | 1.76 | 1.76 | Quality-safe, slower |

Reference baselines remain:

| Mode | Syntax | Plus pass@1 | Tokens | Aggregate TPS |
| --- | ---: | ---: | ---: | ---: |
| `serial` | 5/5 | 0.800 | 738 | 28.19 |
| `mtp-loaded-disabled` | 5/5 | 0.800 | 738 | 29.18 |
| `exact-mtp-d2` | 5/5 | 0.800 | 738 | 29.05 |

Interpretation:

- Giving MTP a copied target raw-cache prefix does **not** make unguarded K=16
  usable. Both layer 0 and layer 1 grafts still fail pass@1.
- Layer choice matters. Layer 0 grafting improved the top-2 guarded pass@1 from
  `0.200` to `0.400` on this tiny slice, so cache context is not irrelevant.
- The improvement is not enough: `top2-graft-l0` is still below serial quality
  and far below serial throughput (`15.50` versus `28.19` TPS).
- Layer 1 grafting often hurts or adds overhead without a quality lift, which
  suggests target raw rows are not automatically compatible with the MTP block's
  learned cache space.

Decision for this probe: **do not promote target raw-KV grafting**. The result
weakens the simple explanation that MTP fails only because it lacks target-prefix
cache access. There is a small layer-0 signal worth remembering, but a useful
solution would need a trained/cache-compatible block drafter or a much cheaper
exact target verifier, not just copying target raw rows into the current MTP
cache.
