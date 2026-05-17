# MTP Parallel Pipeline Progress

This document tracks the next DS4 speculative decoding architecture track:
exact multi-token target validation plus concurrent MTP future drafting.

The previous working log, `MTP_4X2X2_PROGRESS.md`, remains the historical record
for root/top-k verifier probes, async runahead, sched2, and branch-k negative
evidence. This file starts from the newer cost-model conclusion:

- scheduling overlap is possible;
- single-token validation cannot beat serial target generation by itself;
- the speed lever must come from committing multiple target-validated tokens per
  target verifier pass, while MTP drafts useful future work in parallel.

## Goal

Fully validate whether DS4 can use a parallel speculative pipeline to beat the
serial target baseline:

1. Validate a primary MTP chain with an exact multi-token target verifier.
2. Concurrently draft future work with the MTP model while target validation is
   running.
3. Promote only candidate work that matches the target-validated architectural
   stream.
4. Benchmark enough schedule variants to reach a promote/drop architecture
   decision, not just a single probe result.

## Current Invariants

- The target model remains the oracle. Sched2 may never commit a token that the
  serial target path would not emit.
- MTP can draft ahead, branch, and waste work. It cannot define the output
  stream.
- Exact validation must compare accepted rows against serial target logits/top
  ids in validation mode.
- GPU overlap evidence must come from command-buffer GPU timestamps, not CPU
  wait time.
- Throughput promotion requires beating both:
  - pure serial target baseline;
  - MTP-loaded-disabled baseline, `DS4_MTP_SPEC_DISABLE=1`.

## Candidate Schedules

### A. Depth-First Chain Validation

Draft:

```text
A: a1 -> a2 -> a3 -> ... -> aN
```

Target validates the whole chain in one exact pass and commits the longest
accepted prefix.

Purpose:

- Establish the core speed lever: more than one accepted token per target
  verifier pass.

Gate:

- Must match serial stdout and validation logits/top ids.
- Must show target verifier cost per accepted token below serial one-token
  target decode.

### B. Depth Validation With Continuation Runahead

Cycle:

```text
target validates: A[1..N]
MTP drafts:       A[N+1..N+M]
```

If target accepts enough of `A`, promote the concurrently drafted continuation.
If target rejects early, discard it and restart from the accepted target token.

Purpose:

- Combine the real speed lever, depth validation, with the scheduling overlap
  that sched2 already proved is possible.

Initial matrix:

- `N=2..8`
- `M=1`, plus selected `M=2`
- `--mtp-draft` large enough to hold `N+M`

### C. Alternate-Root Chain Runahead

Cycle:

```text
target validates: A[1..N]
MTP drafts:       B[1..N], C[1..N], ...
```

If `A` fails at the root and the target root equals `b1`, promote chain `B` as
the next candidate. Otherwise discard alternate chains.

Purpose:

- Recover from top-1 MTP root misses.
- Only worthwhile if root-rank data shows enough rank-1/rank-2 recoveries.

Initial matrix:

- `branch_k=2,4`
- `N=2,3`

### D. Hybrid Continuation Plus Alternate Branches

Cycle:

```text
target validates: A[1..N]
MTP drafts:       A[N+1..N+M]
                  B[1..K]
                  C[1..K]
```

Promotion policy:

- If `A` accepts deeply, prefer continuation `A+`.
- If `A` fails near root and target root matches an alternate branch, promote
  that branch.
- Otherwise discard all speculative work and restart from the target token.

Purpose:

- Test the full branch-predictor style pipeline once the simpler schedules have
  bounded costs.

Initial matrix:

- `N=3,4`
- `M=1,2`
- `branch_k=2`
- alternate depth `K=2`

## Measurements

Every run should report:

- deterministic stdout match against serial baseline;
- validation `max_delta` and top-id mismatches for accepted verifier rows;
- accepted tokens per target verifier pass;
- target verifier GPU span;
- MTP draft GPU span;
- overlap and gap;
- discarded MTP work;
- branch rank selected, if any;
- throughput versus serial target and MTP-loaded-disabled baseline.

## Standard Prompts

Use the same three-prompt `-n 64` matrix as the previous log:

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

## Studio Matrix Template

```sh
MODEL="/Users/studio/.ds4/cache/gguf/DeepSeek-V4-Flash-IQ2XXS-w2Q2K-AProjQ8-SExpQ8-OutQ8-chat-v2-imatrix.gguf"
MTP="/Users/studio/git/antirez/ds4/gguf/DeepSeek-V4-Flash-MTP-Q4K-Q8_0-F32.gguf"

./ds4 --metal -m "$MODEL" \
  --ctx 1024 --nothink -sys "" --temp 0 -n 64 \
  --prompt-file "$PROMPT"

DS4_MTP_SPEC_DISABLE=1 ./ds4 --metal -m "$MODEL" --mtp "$MTP" \
  --mtp-draft "$DRAFT" \
  --ctx 1024 --nothink -sys "" --temp 0 -n 64 \
  --prompt-file "$PROMPT"

DS4_MTP_SCHED2=1 DS4_MTP_SCHED2_TIMING=1 DS4_MTP_SCHED2_VALIDATE=1 \
  DS4_MTP_SCHED2_VERIFY_N="$N" DS4_MTP_SCHED2_CONT_M="$M" \
  ./ds4 --metal -m "$MODEL" --mtp "$MTP" \
  --mtp-draft "$DRAFT" \
  --ctx 1024 --nothink -sys "" --temp 0 -n 64 \
  --prompt-file "$PROMPT"
```

## Implementation Notes

### 2026-05-14 Depth Continuation Controls

Added sched2 controls for schedule B:

- `DS4_MTP_SCHED2_VERIFY_N=N`: maximum exact target rows to validate from the
  current MTP chain.
- `DS4_MTP_SCHED2_CONT_M=M`: continuation tokens to let MTP draft while the
  target validates the current chain.

The timing line now reports `verify_limit`, `cont_m`, `accepted_per_verify`,
`target_gpu`, `mtp_gpu`, `overlap`, `gap`, replay cost, and discard counts.
`target_gpu` is captured immediately after `metal_graph_verify_decode_exact`,
so replay/validation work does not inflate the verifier overlap proof.

### 2026-05-14 Root-Branch Controls

Added sched2 controls for schedule C:

- `DS4_MTP_SCHED2_ROOT_BRANCH=1`: draft alternate current-root chains from the
  MTP top-k roots while target verifies the current root.
- `DS4_MTP_SCHED2_BRANCH_K=K`: number of root candidates to draft.
- `DS4_MTP_SCHED2_BRANCH_DEPTH=D`: total chain depth per root, including the
  root token. The promoted suffix length is `D-1` because the target-verified
  root is already committed.

The timing line reports `root_branch=1`, `branch_depth`, `branch_rank`, and
`appended` suffix tokens. `branch_rank=-1` means the target root was not inside
the drafted top-k roots, so all branch work was discarded.

## Promotion Criteria

Promote only if all are true:

- stdout matches serial target for the standard prompt matrix;
- validation rows match serial target top ids, with documented `max_delta`;
- throughput beats both serial target and MTP-loaded-disabled baseline
  reproducibly;
- overlap evidence shows MTP work is hidden rather than stretching target work
  enough to erase the win;
- the implementation has a clear fallback path and no old experimental env flag
  dependency.

## Decision Log

### 2026-05-14 Schedule B: Depth Validation With Continuation Runahead

Artifact directory:

- `/tmp/ds4-mtp-matrix/sched2_depthcont_20260514_093133` on `studio.local`

Local and studio checks:

- `make ds4_test ds4`: pass locally and on `studio.local`
- `./ds4_test --metal-sched2`: pass locally and on `studio.local`
- `./ds4_test --metal-kernels`: pass locally and on `studio.local`
- `git diff --check`: pass locally

Correctness:

- All schedule-B outputs matched serial baseline stdout for the three standard
  prompts.
- Validation smoke: `count`, `-n 16`, `N=2`, `M=1`, `rows=12`,
  `mismatches=0`, `max_delta=0`.
- Cross-prompt validation sweep:
  `/tmp/ds4-mtp-matrix/sched2_validation_20260514_094820`, `B_v2m1`,
  `count/explain/code`, stdout match, `rows=12/13/12`, `mismatches=0`,
  `max_delta=0`.

Baselines:

| prompt | serial target | MTP-loaded-disabled |
| --- | ---: | ---: |
| count | 37.00 t/s | 36.45 t/s |
| explain | 36.94 t/s | 36.31 t/s |
| code | 37.08 t/s | 35.80 t/s |

Selected schedule-B results:

| prompt | variant | t/s | avg committed | avg accepted/verified | avg target GPU | avg MTP GPU | avg overlap |
| --- | --- | ---: | ---: | ---: | ---: | ---: | ---: |
| count | N=2 M=1 | 33.53 | 1.500 | 1.000 | 44.637 ms | 2.412 ms | 2.299 ms |
| count | N=2 M=2 | 33.16 | 1.500 | 1.000 | 45.248 ms | 3.186 ms | 3.070 ms |
| count | N=3 M=1 | 20.03 | 1.500 | 0.844 | 58.235 ms | 3.192 ms | 3.081 ms |
| explain | N=2 M=1 | 25.85 | 1.171 | 0.857 | 42.989 ms | 2.543 ms | 2.432 ms |
| explain | N=2 M=2 | 26.15 | 1.182 | 0.864 | 43.915 ms | 3.487 ms | 3.363 ms |
| explain | N=3 M=1 | 21.65 | 1.242 | 0.798 | 54.946 ms | 3.541 ms | 3.418 ms |
| code | N=2 M=1 | 29.90 | 1.324 | 0.946 | 42.418 ms | 2.542 ms | 2.430 ms |
| code | N=2 M=2 | 25.46 | 1.297 | 0.865 | 46.873 ms | 3.288 ms | 3.171 ms |
| code | N=3 M=1 | 25.33 | 1.531 | 0.896 | 54.920 ms | 3.454 ms | 3.343 ms |

Depth sweep with `M=1`:

| prompt | N=4 | N=5 | N=6 | N=7 | N=8 |
| --- | ---: | ---: | ---: | ---: | ---: |
| count | 17.55 t/s | 15.65 t/s | 13.99 t/s | 12.85 t/s | 11.91 t/s |
| explain | 19.19 t/s | 16.18 t/s | 14.74 t/s | 13.52 t/s | 12.50 t/s |
| code | 21.52 t/s | 18.11 t/s | 15.94 t/s | 14.65 t/s | 13.63 t/s |

Overlap gate:

- `N=2 M=1` clears the GPU overlap threshold on all prompts:
  `2.299 ms`, `2.432 ms`, and `2.430 ms` average overlap.
- Increasing `M` and `N` increases measured overlap, but the extra overlapped
  work is not useful enough to offset verifier/replay/discard cost.

Decision for schedule B:

- **Do not promote.**
- Keep as correctness and scheduling evidence: exact target verification plus
  concurrent MTP continuation is real and deterministic, but it loses to both
  baselines on every standard prompt.
- Main failure mode: exact verifier cost grows with `N`, while committed tokens
  per verifier pass stays low (`~1.17..1.75` across prompts), and deeper chains
  amplify replay/discard cost instead of amortizing target work.

Overall architecture decision remains open pending alternate-root and hybrid
schedule tests.

### 2026-05-14 Schedule C: Alternate-Root Chain Runahead

Artifact directory:

- `/tmp/ds4-mtp-matrix/sched2_rootbranch_20260514_094247` on `studio.local`

Checks:

- `make ds4_test ds4`: pass locally and on `studio.local`
- `./ds4_test --metal-sched2`: pass locally and on `studio.local`
- `./ds4_test --metal-kernels`: pass locally and on `studio.local`
- `git diff --check`: pass locally

Correctness:

- All schedule-C outputs matched serial baseline stdout for the three standard
  prompts.
- Validation smoke: `count`, `-n 16`, `branch_k=2`, `branch_depth=2`,
  `rows=7`, `mismatches=0`, `max_delta=0`.
- Cross-prompt validation sweep:
  `/tmp/ds4-mtp-matrix/sched2_validation_20260514_094820`, `C_k2d2`,
  `count/explain/code`, stdout match, `rows=7/7/7`, `mismatches=0`,
  `max_delta=0`.

Baselines:

| prompt | serial target | MTP-loaded-disabled |
| --- | ---: | ---: |
| count | 36.85 t/s | 36.40 t/s |
| explain | 36.84 t/s | 36.28 t/s |
| code | 37.07 t/s | 36.23 t/s |

Schedule-C throughput:

| prompt | k=2 depth=2 | k=4 depth=2 | k=2 depth=3 | k=4 depth=3 |
| --- | ---: | ---: | ---: | ---: |
| count | 33.03 t/s | 31.43 t/s | 31.21 t/s | 28.31 t/s |
| explain | 32.87 t/s | 31.30 t/s | 31.41 t/s | 28.36 t/s |
| code | 32.87 t/s | 31.11 t/s | 31.20 t/s | 28.49 t/s |

Branch-rank evidence:

| prompt | variant | cycles | misses | selected ranks |
| --- | --- | ---: | ---: | --- |
| count | k=2 depth=2 | 31 | 23 | rank1: 8 |
| count | k=4 depth=2 | 31 | 17 | rank1: 11, rank2: 3 |
| explain | k=2 depth=2 | 29 | 26 | rank1: 3 |
| explain | k=4 depth=2 | 30 | 26 | rank0: 1, rank1: 3 |
| code | k=2 depth=2 | 31 | 25 | rank0: 2, rank1: 4 |
| code | k=4 depth=2 | 32 | 20 | rank0: 2, rank1: 7, rank2: 3 |

Overlap:

- Every root-branch variant clears the overlap threshold on every prompt.
- Average overlap scales with drafted branch work:
  about `3.2 ms` for `k=2 depth=2`, `6.5 ms` for `k=4 depth=2` or
  `k=2 depth=3`, and `12..13 ms` for `k=4 depth=3`.

Decision for schedule C:

- **Do not promote.**
- The mechanism is correct and genuinely parallel, but the target root is often
  not inside the drafted branch set. The additional branch work lowers
  throughput on every prompt even when overlap is strong.
- Main failure mode: branch work grows with `branch_k * depth`, while promoted
  suffix tokens remain sparse (`appended_avg` stayed below `1.0` for all runs).

Overall architecture decision remains open pending hybrid schedule tests.

### 2026-05-14 Schedule D: Adaptive Root-Branch Plus Continuation

Artifact directory:

- `/tmp/ds4-mtp-matrix/sched2_hybrid_20260514_094546` on `studio.local`

Tested policy:

- `DS4_MTP_SCHED2_ROOT_BRANCH=1`
- `DS4_MTP_SCHED2_VERIFY_N=2`
- `DS4_MTP_SCHED2_CONT_M=1`
- branch variants: `k=2 depth=2`, `k=4 depth=2`, `k=2 depth=3`

In this DS4 call shape, the target root token is already known before the
sched2 verifier path is entered. Therefore the practical hybrid policy is
adaptive rather than simultaneous:

- if only the current root is queued, run schedule C root-branch drafting;
- if a deeper accepted chain survives into the next cycle, run schedule B
  continuation drafting.

Correctness:

- All schedule-D outputs matched serial baseline stdout for the three standard
  prompts.
- Validation smoke: `count`, `-n 16`, `k=2 depth=2`, `N=2 M=1`, `rows=7`,
  `mismatches=0`, `max_delta=0`.
- Cross-prompt validation sweep:
  `/tmp/ds4-mtp-matrix/sched2_validation_20260514_094820`, `D_hk2d2`,
  `count/explain/code`, stdout match, `rows=7/7/7`, `mismatches=0`,
  `max_delta=0`.

Baselines:

| prompt | serial target | MTP-loaded-disabled |
| --- | ---: | ---: |
| count | 37.07 t/s | 36.23 t/s |
| explain | 36.97 t/s | 36.44 t/s |
| code | 37.11 t/s | 36.44 t/s |

Schedule-D throughput:

| prompt | k=2 depth=2 | k=4 depth=2 | k=2 depth=3 |
| --- | ---: | ---: | ---: |
| count | 32.88 t/s | 31.43 t/s | 31.44 t/s |
| explain | 33.00 t/s | 31.22 t/s | 31.41 t/s |
| code | 32.91 t/s | 31.33 t/s | 31.43 t/s |

Cycle shape:

| prompt | variant | root cycles | continuation cycles | selected ranks / misses |
| --- | --- | ---: | ---: | --- |
| count | k=2 depth=2 | 31 | 0 | rank1: 8, miss: 23 |
| count | k=4 depth=2 | 31 | 0 | rank1: 11, rank2: 3, miss: 17 |
| count | k=2 depth=3 | 31 | 0 | rank1: 8, miss: 23 |
| explain | k=2 depth=2 | 29 | 0 | rank1: 3, miss: 26 |
| explain | k=4 depth=2 | 30 | 0 | rank0: 1, rank1: 3, miss: 26 |
| explain | k=2 depth=3 | 29 | 0 | rank1: 3, miss: 26 |
| code | k=2 depth=2 | 31 | 0 | rank0: 2, rank1: 4, miss: 25 |
| code | k=4 depth=2 | 31 | 1 | rank0: 2, rank1: 7, rank2: 3, miss: 20 |
| code | k=2 depth=3 | 31 | 0 | rank0: 2, rank1: 4, miss: 25 |

Decision for schedule D:

- **Do not promote.**
- The adaptive hybrid is correct but does not materially combine schedule B and
  schedule C. Nearly all cycles remain root-branch cycles because promoted branch
  suffixes rarely match the next target token deeply enough to create a
  continuation-validation cycle.
- This closes the current sched2 variant set unless we change the verifier
  contract or MTP quality source. We have real GPU overlap, but not enough
  useful accepted speculative work to beat baseline.

## Final Architecture Decision

Decision:

- **Drop promotion for the current sched2 speculative pipeline variants.**
- Keep the sched2 infrastructure and timing hooks as useful evidence and as a
  future experimental substrate.

What was proven:

- Sched2 can produce real target/MTP GPU overlap with correct resource
  isolation.
- Exact target verification is deterministic against serial stdout and serial
  logits/top ids for representative schedule-B/C/D validation sweeps.
- The tested parallel algorithms do not beat serial target decode or the
  MTP-loaded-disabled baseline on the standard prompts.

Why promotion drops:

- Schedule B validates multiple target rows while drafting continuation, but
  verifier cost scales with `N` and accepted tokens per verifier pass stay too
  low.
- Schedule C drafts alternate root chains, but the target root often misses the
  drafted top-k set, so most branch work is discarded.
- Schedule D does not become a meaningful simultaneous hybrid in the current
  DS4 call shape. The target root is known before sched2 runs, and promoted
  branch suffixes rarely survive into deeper continuation cycles.

Next viable directions:

- Improve MTP quality or use a stronger drafter so accepted speculative work is
  dense enough to amortize target verification.
- Change the verifier contract to make multi-token target verification cheaper
  than repeated target decode.
- Keep sched2 for future graph-scheduling experiments, but do not route default
  generation through these variants.

## 2026-05-14 Sched2 Production-Policy Pass

After comparing the sched2 path with the older `mtp-performance-50` policy
notes, the sched2 hot path had two policy/runtime issues that made its
throughput numbers too pessimistic:

- The governor counted every `N=1` sched2 cycle as `0` committed drafts, even
  when the available MTP frontier matched the next target-selected token. For
  runahead, that frontier hit is the useful speculative work.
- The default `N=1` sched2 verifier still used the generic verifier wrapper and
  took a frontier snapshot, even though `N=1` is exactly ordinary target decode
  of the current token.

Implementation changes:

- Added sched2-specific target-margin gating:
  `DS4_MTP_SCHED2_TARGET_MARGIN_SKIP` defaults to `5.0`, and
  `DS4_MTP_SCHED2_TARGET_MARGIN_DISABLE=1` disables it.
- The normal MTP draft gate now uses the sched2 margin default when
  `DS4_MTP_SCHED2=1`, so future suffix work is not constantly seeded from
  low-confidence target states.
- `N=1`, non-validation sched2 now skips frontier snapshot/restore and routes
  the target side through `metal_graph_eval_token_raw_swa()`, the same target
  decode primitive used by the serial path.
- The sched2 governor now records a matched available frontier as useful
  speculative work, plus any additional accepted verifier rows.

Validation:

- Local: `make ds4_test ds4`, `./ds4_test --metal-sched2`,
  `./ds4_test --metal-kernels`, and `git diff --check` passed.
- `studio.local`: the same build and Metal checks passed on Apple M3 Ultra.
- Validation smoke:
  `/tmp/ds4-mtp-matrix/sched2_policy_20260514_143224`,
  `count_sched2_validate_after_policy.*`, `DS4_MTP_SCHED2_VALIDATE=1`,
  `-n 16`, `validate_rows=12`, `bad_rows=0`.

Performance artifact:

- `/tmp/ds4-mtp-matrix/sched2_policy_20260514_143224`

Three-prompt `-n 64 --temp 0 --ctx 1024 --nothink -sys ""` matrix:

| prompt | serial target | MTP-disabled | sched2 d2 before direct decode | sched2 d2 production | sched2 d2 no margin | sched2 d4 production |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| count | 36.92 | 36.24 | 34.07 | 34.39 | 34.11 | 33.27 |
| explain | 36.96 | 36.13 | 35.05 | 35.24 | 34.05 | 34.16 |
| code | 37.09 | 36.18 | 34.12 | 34.24 | 34.08 | 33.41 |

All sched2 production rows matched serial stdout byte-for-byte. The
non-sched2 default MTP d2 lane in this branch did not match serial stdout in
this matrix, so it is not used as an exact comparison point here. The
`DS4_MTP_STRICT=1` current-branch control matched serial stdout but measured
`32.93/35.82/35.62 t/s` for `count/explain/code`.

Timing smoke after the direct-target-decode change:

- `count_sched2_d2_direct_timing.*`, `-n 24`
- `rows=18`, `snapshot_zero=18`
- average target GPU span `27.273 ms`
- average MTP GPU span `1.538 ms`
- average target/MTP overlap `0.000 ms`

Decision:

- **Keep the production-policy cleanup.**
- **Do not promote sched2 d2/d4 as a throughput path.** The cleaned-up path is
  more honest and slightly faster, but the GPU timeline still shows the MTP
  work executing back-to-back with target decode rather than overlapping.
  Without useful target/MTP overlap, sched2 remains below both serial target and
  the MTP-loaded-disabled baseline.
