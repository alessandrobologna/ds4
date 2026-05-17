# MTP 4x2x2 Progress

This document tracks the current speculative decoding work on the fixed-shape
4x2x2 MTP tree path. It is intentionally a working log: keep measured results,
dead ends, validation commands, and next decisions here so the thread state is
not the source of truth.

## Scope

- Branch: `codex/mtp-4x2x2-tree-kernel`
- Host for model-backed validation: `studio.local`
- Primary model:
  `$HOME/.ds4/cache/gguf/DeepSeek-V4-Flash-IQ2XXS-w2Q2K-AProjQ8-SExpQ8-OutQ8-chat-v2-imatrix.gguf`
- MTP model:
  `$HOME/.ds4/cache/gguf/DeepSeek-V4-Flash-MTP-Q4K-Q8_0-F32.gguf`
- Historical tree shape under investigation: root top-k plus `4x2x2`
  child/grandchild tree.
- Current async runahead prototype shape: top-1 MTP frontier chaining only. It
  must not be compared as a root-top-k `4x2x2` verifier implementation until
  root recovery is explicitly wired into the async path.
- Default bounded throughput workload: `-n 64`; use `-n 96` or `-n 128` only
  when a candidate looks promising.
- Prompt classes:
  - Counting: `Count from 1 to 200, separated by spaces.`
  - Explanation: `Explain why speculative decoding can improve language model throughput in two concise paragraphs.`
  - Code completion:

```c
Complete this C function:

#include <stdbool.h>
#include <stdint.h>

bool parse_u32(const char *s, uint32_t *out) {
```

## Current Decision Snapshot

Run date: 2026-05-14

The live experimental path is now `DS4_MTP_SCHED2=1`. The older
`DS4_MTP_RUNAHEAD_*` sections below are retained as historical evidence only;
they are no longer the current implementation contract. `DS4_MTP_SPEC_DISABLE=1`
remains the MTP-loaded-disabled baseline.

Sched2 implements the greenfield speculative schedule for `N=2`:

- target validates only the current frontier transition;
- MTP drafts the next suffix concurrently on its async lane;
- if the verified token fails, the suffix is discarded and the target token is
  used as the restart point;
- if it succeeds, the accepted prefix is committed and the concurrently drafted
  suffix is promoted into the next cycle.

Validation and instrumentation now use:

- `DS4_MTP_SCHED2=1` to enable the path;
- `DS4_MTP_SCHED2_TIMING=1` for one parseable cycle line per sched2 cycle;
- `DS4_MTP_SCHED2_VALIDATE=1` for serial target logits/top-id validation;
- `DS4_MTP_SCHED2_VALIDATE_LOG=1` for verbose validation rows.

Decision:

- Correctness gate: pass. On `studio.local`, deterministic stdout matched the
  serial target baseline for all three standard prompts. The validation smoke
  reported `max_delta=0` and `mismatches=0` for accepted rows inspected.
- Overlap gate: fail. The gate required `>= 2 ms` overlap and `>= 15%` of the
  shorter GPU span on at least two of the three prompts. `N=2` had `0` passing
  cycles on all three prompts, with average overlap about `1.51 ms`.
- Throughput gate: fail. Sched2 is slower than both serial target and
  MTP-loaded-disabled baselines on the decision host.
- Therefore, the single-chain sched2 schedule stops at `N=2`. Its result is a
  no-go for useful target/MTP overlap in that shape, not a no-go for every
  possible future verifier. The follow-up branch-k experiment below changes the
  MTP work shape by drafting alternate future paths.

Studio matrix:

| Prompt | Serial target | MTP-loaded-disabled | Sched2 `N=2` | Stdout | Overlap pass cycles | Avg overlap | Max overlap |
|---|---:|---:|---:|---|---:|---:|---:|
| Counting | 37.20 t/s | 36.24 t/s | 33.64 t/s | match | 0/47 | 1.511 ms | 1.622 ms |
| Explanation | 36.74 t/s | 36.16 t/s | 33.90 t/s | match | 0/41 | 1.511 ms | 1.621 ms |
| Code completion | 37.13 t/s | 36.25 t/s | 33.99 t/s | match | 0/48 | 1.507 ms | 1.567 ms |

Raw output directory on `studio.local`:
`/tmp/ds4-mtp-matrix/sched2_20260514_082935`.

## Branch-K Sched2 Experiment

Run date: 2026-05-14

Hypothesis:

- The single-chain MTP async lane was too small to fill the target verifier
  window. Make MTP work harder by drafting a tiny alternate-path tree: from the
  current accepted frontier token, MTP produces top-k candidate roots for the
  next token and one child for each root.
- The target still verifies only the current architectural frontier. After the
  target decode reveals the exact next-token top id, sched2 promotes only the
  branch whose root equals that target id. All other branch work is discarded.

Implementation:

- New env flag: `DS4_MTP_SCHED2_BRANCH_K=N`, clamped to `2..8` for the branch
  experiment.
- Per-branch async scratch now stores branch-local raw cache and hidden state,
  so the selected branch can be promoted without rerunning the MTP child.
- The MTP async command does:
  1. one root MTP step with top-k output;
  2. one child MTP step for each root candidate;
  3. a host-side branch select after target verification completes.

Local smoke:

- Count prompt, `-n 16`, `DS4_MTP_SCHED2_VALIDATE=1`,
  `DS4_MTP_SCHED2_BRANCH_K=2`, `--mtp-draft 2`: stdout matched serial baseline;
  validation rows reported `max_delta=0`, `mismatches=0`.
- Count prompt, `-n 32`, no validation, branch-k matched serial stdout for both
  `--mtp-draft 2` and `--mtp-draft 3`.

Studio matrix:

| Prompt | Serial target | MTP-loaded-disabled | Branch-k d2 | Branch-k d3 | Stdout | Branch-k d2 avg overlap | Branch-k d3 avg overlap |
|---|---:|---:|---:|---:|---|---:|---:|
| Counting | 37.12 t/s | 36.44 t/s | 32.15 t/s | 32.79 t/s | match | 2.806 ms | 2.556 ms |
| Explanation | 36.96 t/s | 36.45 t/s | 32.45 t/s | 32.92 t/s | match | 2.775 ms | 2.925 ms |
| Code completion | 36.95 t/s | 35.79 t/s | 32.23 t/s | 32.82 t/s | match | 2.664 ms | 2.466 ms |

Branch selection stats:

| Prompt | d2 cycles | d2 branch cycles | d2 rank0 | d2 rank1 | d3 cycles | d3 branch cycles | d3 rank0 | d3 rank1 |
|---|---:|---:|---:|---:|---:|---:|---:|---:|
| Counting | 50 | 29 | 14 | 6 | 48 | 16 | 16 | 0 |
| Explanation | 40 | 23 | 17 | 0 | 42 | 18 | 15 | 0 |
| Code completion | 49 | 27 | 18 | 3 | 49 | 15 | 14 | 1 |

Decision:

- Branch-k proves that the scheduler can hide a larger MTP workload: average
  studio overlap rose from about `1.51 ms` for single-chain sched2 to
  `2.46-2.93 ms` for branch-k.
- The alternate branch is sometimes useful: rank-1 branch recovery happened on
  counting and code prompts.
- Throughput still fails badly: both branch-k variants are around `32-33 t/s`,
  below serial target and below the MTP-loaded-disabled baseline.
- Do not promote branch-k as implemented. It is valuable evidence that overlap
  can be increased by adding MTP work, but the extra work/pressure costs more
  than the recovered branch value in this executor shape.

Raw output directory on `studio.local`:
`/tmp/ds4-mtp-matrix/sched2_branchk_20260514_085614`.

## Prior Runahead State

This section preserves the pre-sched2 runahead findings that led to the
greenfield scheduler work. That earlier direction shifted from the root/top-k
`4x2x2` verifier probe to MTP branch-prediction/runahead. The tree probes remain
as evidence for why the old verifier direction was not promotable yet; the
async runahead work was a prototype, not a valid architecture rejection of the
intended algorithm. It had an env-gated async MTP lane with isolated Metal
command and scratch state plus device-side token chaining, but the prior
measurement still paid one full target decode per accepted token. It was also
top-1 only at the root; the root-top-k recovery used by the non-runahead
verifier was not ported into that async scheduler.

Measured and removed or avoided:

- C-composed 4x2x2 verifier hot paths: slower than the existing path.
- `DS4_MTP_BRANCH_CHILD2_VERIFY` hot paths: slower than the existing path.
- Branch-attention batch experiment: verifier-grade drift and slower runtime.

Prior probe additions:

- MTP draft decode now has a dedicated one-row async scratch arena
  (`ds4_mtp_async_scratch`). The MTP block/output/top-k path binds that arena
  while drafting so target decode scratch and MTP draft scratch are no longer
  the same Metal tensors.
- `DS4_MTP_RUNAHEAD_ENABLE=1` enables a real async MTP runahead lane: while the
  target path verifies token `k`, the MTP lane drafts future tokens in a
  separate command queue. The latest version chains MTP tokens on the GPU by
  feeding the previous top-k result directly into the next MTP embedding and
  token-aware router path, so there is no CPU readback between child and
  grandchild draft steps.
- `DS4_MTP_RUNAHEAD_VERIFY_N_EXACT=1` enables the first fixed-shape exact
  multi-token target verifier for the async runahead chain. It verifies up to
  the current `--mtp-draft N` frontier with the exact one-token target decode
  kernels in layer-major order, then commits the accepted prefix. On partial
  accept it restores the verifier snapshot and replays only the accepted prefix,
  so this is a correctness/cost artifact rather than the final fast kernel.
- `DS4_MTP_VERIFY_DECODE_N_VALIDATE=1` adds a verifier validation pass: replay
  the same suffix with serial target decode, compare every verified logits row
  and top token, and report `max_delta` plus mismatch count.
- `DS4_MTP_VERIFY_DECODE_N_BATCH_HEAD=1` keeps the exact decode-layer/cache
  verifier but batches the output head across verified rows. This is the
  narrowest faster candidate so far: it is top-token equivalent in validation,
  but not bit-identical to row-wise output head.
- `DS4_MTP_SPEC_DISABLE=1` now disables regular MTP drafting inside
  `ds4_session_eval_internal`; previously it only bypassed the speculative CLI
  call site, so it was not a valid MTP-off baseline when `--mtp-draft > 1`.
- The sequential `DS4_MTP_PREDICT_CHAIN_*` measurement path was removed. It was
  exact, but it verified target tokens sequentially and was not the speculative
  runahead algorithm under consideration.
- `DS4_MTP_TREE_4X2X2_CHILD_ROW_VERIFY` now handles
  `ratio4_emit_boundary` in probe mode by serially restoring the target root
  state, capturing both child hidden rows, and running the output head in a
  batch.
- `DS4_MTP_TREE_4X2X2_HC_DIFF=1` adds probe-only final-HC comparisons for the
  noemit row-batch verifier. This separates output-head drift from drift inside
  the target verifier layer stack.
- The rejected `DS4_MTP_TREE_4X2X2_RATIO4_BOUNDARY_BATCH` probe path has been
  removed from code. Its measurement is retained below as negative evidence.

Experiment hygiene:

- Removed from live code, retained only as documented negative evidence:
  `DS4_MTP_PREDICT_CHAIN_*` and
  `DS4_MTP_TREE_4X2X2_RATIO4_BOUNDARY_BATCH`.
- Previously measured dead hot-path branches such as
  `DS4_MTP_BRANCH_CHILD2_VERIFY` and the branch-attention batch experiment are
  not live command-path flags in `ds4.c`.
- Remaining non-promoted verifier work is explicitly env-gated:
  `DS4_MTP_RUNAHEAD_VERIFY_N_EXACT=1`,
  `DS4_MTP_VERIFY_DECODE_N_VALIDATE=1`, and
  `DS4_MTP_VERIFY_DECODE_N_BATCH_HEAD=1`.
- The async path remains top-1 root only; `4x2x2` recovery is deferred rather
  than silently wired into a verifier that cannot beat the linear chain.

## Latest Throughput Matrix

Run date: 2026-05-13

Command shape:

```sh
./ds4 --metal -m "$MODEL" --mtp "$MTP" --mtp-draft 3 \
  --ctx 1024 --nothink -sys "" --temp 0 -n 64 \
  --prompt-file "$PROMPT"
```

For the baseline variant, omit `--mtp "$MTP" --mtp-draft 3`. For the tree
variant, add `DS4_MTP_TREE_4X2X2_ENABLE=1 DS4_MTP_TIMING=1`.

| Prompt | Baseline | Default MTP | Tree/root-topk |
|---|---:|---:|---:|
| Counting | 37.05 t/s | 40.40 t/s | 40.21 t/s |
| Explanation | 36.93 t/s | 35.94 t/s | 36.02 t/s |
| Code completion | 36.76 t/s | 35.44 t/s | 35.51 t/s |

Interpretation:

- Counting still benefits from default MTP, and the tree path is slightly
  behind default MTP after cleanup.
- Explanation and code completion remain below baseline with either MTP path.
- The tree path has no production-throughput argument without a verifier kernel
  that is both faster and target-exact.

## Post-Fix Async Runahead Timing Matrix

Run date: 2026-05-13

Host: `studio.local`, rsynced worktree
`/Users/studio/git/.worktrees/antirez/ds4/mtp-4x2x2-tree-kernel-384a`.

This matrix was run after fixing both comparison defects:

- `DS4_MTP_SPEC_DISABLE=1` is now a real all-MTP-off baseline switch even when
  the MTP model is loaded.
- GPU token chaining now passes the chained device token through token-aware
  router selection instead of only through embedding.

Command shape:

```sh
./ds4 --metal -m "$MODEL" --ctx 1024 --nothink -sys "" \
  --temp 0 -n 64 --prompt-file "$PROMPT"

DS4_MTP_SPEC_DISABLE=1 \
./ds4 --metal -m "$MODEL" --mtp "$MTP" --mtp-draft 4 \
  --ctx 1024 --nothink -sys "" --temp 0 -n 64 \
  --prompt-file "$PROMPT"

DS4_MTP_RUNAHEAD_ENABLE=1 DS4_MTP_GOVERNOR_DISABLE=1 DS4_MTP_TIMING=1 \
./ds4 --metal -m "$MODEL" --mtp "$MTP" --mtp-draft 4 \
  --ctx 1024 --nothink -sys "" --temp 0 -n 64 \
  --prompt-file "$PROMPT"
```

For the default MTP comparator, run the same MTP command without the env vars.
That comparator is not cleanly exact for every prompt, so use it only as
background evidence.

| Prompt | Baseline | MTP loaded, disabled | Default MTP draft4 | Runahead4 | Output |
|---|---:|---:|---:|---:|---|
| Counting | 37.41 t/s | 36.79 t/s | 25.45 t/s | 33.55 t/s | disabled/default/runahead match |
| Explanation | 37.19 t/s | 36.75 t/s | 35.77 t/s | 33.56 t/s | disabled/runahead match; default differs |
| Code completion | 37.36 t/s | 36.44 t/s | 34.42 t/s | 33.72 t/s | disabled/default/runahead match |

Runahead timing counters:

| Prompt | Cycles | Committed | Avg commit | Max commit | Starts | Appended | Discarded | Start | Target | Stall | Total | ms/token |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| Counting | 16 | 48 | 3.00 | 3 | 48 | 79 | 47 | 4.363 ms | 1443.425 ms | 0.146 ms | 1448.095 ms | 30.169 |
| Explanation | 16 | 37 | 2.31 | 4 | 37 | 69 | 48 | 3.775 ms | 1113.488 ms | 0.108 ms | 1117.486 ms | 30.202 |
| Code completion | 17 | 45 | 2.65 | 5 | 44 | 76 | 47 | 4.264 ms | 1338.776 ms | 0.152 ms | 1343.346 ms | 29.852 |

Interpretation:

- The fixed MTP-disable path is now a valid comparator: it matches baseline
  stdout and lands near baseline throughput while paying only the cost of
  having the MTP model loaded.
- The runahead4 path is exact on all three prompts after the token-router fix,
  but it remains slower than baseline.
- MTP queue startup and explicit wait/stall are tiny; target decode dominates
  runtime at about `30 ms` per committed token.
- The current algorithm still starts one async extension for nearly every
  committed target token and discards a large amount of queued work after
  mismatches. It prepares future MTP frontier state, but it does not yet reduce
  target verification cost enough to win.
- Default MTP draft4 is not a reliable architecture comparator here:
  explanation output diverged from baseline, while counting matched but was
  unexpectedly slow. A focused `DS4_MTP_TIMING=1` count run showed repeated
  micro-verifier cycles with `drafted=4 committed=3`, `verify=~70 ms`, and
  `replay=~58 ms`, explaining the `~25 t/s` result.

## Runahead Draft-Depth Sweep

Run date: 2026-05-13

Host: `studio.local`, rsynced worktree
`/Users/studio/git/.worktrees/antirez/ds4/mtp-4x2x2-tree-kernel-384a`.

Command shape:

```sh
DS4_MTP_RUNAHEAD_ENABLE=1 DS4_MTP_GOVERNOR_DISABLE=1 DS4_MTP_TIMING=1 \
./ds4 --metal -m "$MODEL" --mtp "$MTP" --mtp-draft "$DRAFT" \
  --ctx 1024 --nothink -sys "" --temp 0 -n 64 \
  --prompt-file "$PROMPT"
```

Each run was compared byte-for-byte with a fresh no-MTP baseline for the same
prompt. All draft depths matched baseline output.

Throughput:

| Draft cap | Count | Explain | Code | Average |
|---:|---:|---:|---:|---:|
| Baseline | 37.05 t/s | 37.06 t/s | 36.86 t/s | 36.99 t/s |
| 1 | 36.87 t/s | 36.93 t/s | 37.09 t/s | 36.96 t/s |
| 2 | 34.18 t/s | 34.01 t/s | 34.31 t/s | 34.17 t/s |
| 3 | 33.70 t/s | 33.61 t/s | 33.75 t/s | 33.69 t/s |
| 4 | 33.18 t/s | 32.95 t/s | 33.44 t/s | 33.19 t/s |
| 5 | 32.78 t/s | 32.75 t/s | 30.52 t/s | 32.02 t/s |
| 6 | 32.52 t/s | 32.34 t/s | 32.20 t/s | 32.35 t/s |
| 7 | 31.97 t/s | 32.08 t/s | 32.49 t/s | 32.18 t/s |
| 8 | 31.63 t/s | 31.48 t/s | 31.99 t/s | 31.70 t/s |

Selected runahead timing counters:

| Draft cap | Count target/ms-token | Explain target/ms-token | Code target/ms-token | Discarded queued tokens |
|---:|---:|---:|---:|---:|
| 2 | `29.36` | `29.14` | `28.98` | `136/136/152` |
| 4 | `30.52` | `30.67` | `30.09` | `407/408/444` |
| 8 | `32.52` | `33.33` | `32.15` | `945/948/1058` |

Interpretation:

- `--mtp-draft 1` does not enter the speculative runahead scheduler in the CLI;
  it is effectively an MTP-loaded, no-speculation run and stays at baseline.
- Deeper runahead queues are exact, but they do not improve throughput. Average
  throughput drops from `34.17 t/s` at depth 2 to `31.70 t/s` at depth 8.
- Target time per committed token rises with draft depth, while explicit MTP
  stall stays tiny. This supports the same-GPU contention hypothesis: the CPU
  wait is hidden, but the target queue slows while the draft queue runs.
- The discarded queued-token count grows quickly with depth, so larger queues
  mostly create extra draft work that gets thrown away after the first mismatch.

## Metal Command Timeline Check

Run date: 2026-05-13

Host: `studio.local`, rsynced worktree
`/Users/studio/git/.worktrees/antirez/ds4/mtp-4x2x2-tree-kernel-384a`.

Implementation:

- Added env-gated Metal command-buffer timeline logging behind
  `DS4_METAL_COMMAND_TIMELINE=1`.
- The logger labels target and MTP command buffers and prints
  `kernelStartTime`, `kernelEndTime`, `GPUStartTime`, and `GPUEndTime` after
  completion. The intent is to distinguish CPU-side async submission from real
  GPU overlap.

Command:

```sh
DS4_METAL_COMMAND_TIMELINE=1 \
DS4_MTP_RUNAHEAD_ENABLE=1 DS4_MTP_GOVERNOR_DISABLE=1 DS4_MTP_TIMING=1 \
./ds4 --metal -m "$MODEL" --mtp "$MTP" --mtp-draft 4 \
  --ctx 1024 --nothink -sys "" --temp 0 -n 24 \
  --prompt-file /tmp/ds4-mtp-matrix/count.txt
```

Observed:

- Timeline events: `74` command buffers, `18` MTP and `56` target.
- MTP buffers with target GPU overlap: `1/18`, and the one overlap was only a
  boundary-scale overlap of about `0.005 ms`.
- MTP buffers serialized before the next target buffer: `17/18`.
- Typical MTP-to-target gap was `0.001-0.004 ms`, for example:
  `ds4-mtp-18 gpu=961146.349645875-961146.354498375`, then
  `ds4-target-19 gpu=961146.354499042-961146.356737375`.

Interpretation:

- The code does not perform an explicit CPU wait before target validation, but
  the GPU timeline shows Metal effectively serializing the MTP and target
  command buffers in this run.
- Previous tiny `stall_ms` values only prove that the MTP buffer had completed
  by the time the CPU waited for it. They do not prove real GPU overlap.
- The draft-depth sweep should therefore be read mostly as added serialized
  MTP work plus discarded queued work, not as successful target/draft overlap
  with mild contention.

External sanity check:

- A ChatGPT Pro pass agreed with the main conclusion: separate compute queues on
  one Apple GPU may serialize, and Metal does not expose a public API to force
  cross-queue compute overlap, partition GPU cores, or set ordinary compute
  queue priority.
- The only new actionable direction it highlighted is
  `MTLDispatchTypeConcurrent` via
  `computeCommandEncoderWithDispatchType:`. The local macOS SDK documents
  `MTLDispatchTypeConcurrent` as allowing dispatches inside a compute encoder
  to execute in parallel with each other; unsupported devices treat it like a
  normal compute encoder.
- This is not a drop-in change for the existing graph encoder, because most
  target decode kernels depend on previous kernels in the same encoder. A safe
  experiment needs either a small independent-dispatch microbench first, or a
  narrow combined MTP/target pass where independence and explicit barriers are
  clear.
- Events and fences are synchronization tools, not overlap enablers. They may
  move waits onto the GPU timeline but cannot remove a true data dependency.
- Re-check false resource dependencies before concluding the scheduler alone is
  responsible: Metal hazard tracking is resource-granularity, so separate
  offsets inside one `MTLBuffer` may still serialize otherwise independent work.

Perplexity Deep Research cross-check:

- A Perplexity Deep Research pass independently reached the same practical
  conclusion: two compute queues on one Apple GPU should not be treated as a
  guaranteed parallel execution lane, and the best Metal-side overlap
  experiment is a single command buffer using a
  `MTLDispatchTypeConcurrent` compute encoder.
- Treat its AGX/Asahi scheduler model as explanatory background, not an Apple
  API contract. The parts verified locally in the macOS SDK are the relevant
  public surfaces: `MTLDispatchTypeConcurrent`,
  `computeCommandEncoderWithDispatchType:`, command-buffer event waits/signals,
  compute encoder fences, and per-resource/heap `hazardTrackingMode`.
- The report adds two useful diagnostics before we make a final drop decision:
  audit whether MTP and target work still share tracked Metal resources at a
  granularity that can force conservative ordering, and use Metal System Trace
  on `studio.local` to see whether decode is ALU-limited or memory-limited.
- If decode is memory-limited, real MTP/target overlap is unlikely to help even
  if we unlock it, because both paths would contend for the same unified memory
  bandwidth. In that case the promotable path must come from fewer target
  decodes or a target-exact multi-token verifier, not GPU queue concurrency.

Gemini Pro share cross-check:

- Gemini independently gave the same practical recommendation set: Metal does
  not expose public GPU core partitioning, ordinary compute queue priority is
  not a hardware lane assignment, events/fences only add synchronization, and
  the strongest overlap experiment is a single command buffer with
  `MTLDispatchTypeConcurrent`.
- It also emphasized the same hardware reality check: use Metal System Trace
  and inspect GPU occupancy and memory bandwidth before investing in a
  concurrent-encoder rewrite.
- Its stronger framing that command buffers are generally atomic/serialized is
  not sufficient as a design conclusion after our microbench: synthetic
  independent command buffers on separate queues did overlap on both local and
  `studio.local`. The better conclusion is narrower: the real DS4 target/MTP
  graph did not overlap in the measured run, and even synthetic overlap can
  produce serial-shaped wall time when both kernels contend for the same GPU
  resources.

## Metal Concurrency Microbench

Run date: 2026-05-13

Implementation:

- Added `./ds4_test --metal-concurrency`, backed by a synthetic
  `kernel_concurrency_spin` Metal kernel.
- The probe compares one dispatch, two dispatches in a normal serial compute
  encoder, two dispatches in one `MTLDispatchTypeConcurrent` compute encoder,
  and two independent command buffers committed to separate queues.
- Defaults are intentionally long enough to make GPU timeline readings stable:
  `4096` groups, `256` threads, `16384` loop iterations. Override with
  `DS4_METAL_CONCURRENCY_PROBE_GROUPS`,
  `DS4_METAL_CONCURRENCY_PROBE_THREADS`, and
  `DS4_METAL_CONCURRENCY_PROBE_ITERS`.
- The probe now also varies resource layout: private disjoint buffers, shared
  default disjoint buffers, shared default same-buffer offsets, forced tracked
  same-buffer offsets, and untracked same-buffer offsets. It can repeat each
  workload as a stream of serial dispatches with
  `DS4_METAL_CONCURRENCY_PROBE_DISPATCHES`.

Studio result:

```sh
DS4_METAL_COMMAND_TIMELINE=1 ./ds4_test --metal-concurrency
```

On `studio.local` / Apple M3 Ultra:

| Shape | GPU time | Interpretation |
|---|---:|---|
| Single dispatch | `7.391 ms` | Baseline synthetic kernel cost. |
| Serial encoder, two dispatches | `14.731 ms` | Almost exactly `2x`. |
| Concurrent encoder, two dispatches | `14.462 ms` | No material win over serial for this full-occupancy kernel. |
| Separate queues, two command buffers | span `14.879 ms`, overlap `14.290 ms` | Real GPU-time overlap exists, but each dispatch stretches to about `2x`, so wall time is still serial-shaped. |

Local result on Apple M5 Max was qualitatively the same with the stable default
workload: serial pair `24.113 ms`, concurrent pair `24.221 ms`, and separate
queues overlapping for part of the run while still spanning `25.042 ms`.

Resource-layout result:

```sh
DS4_METAL_CONCURRENCY_PROBE_DISPATCHES=8 ./ds4_test --metal-concurrency
```

On `studio.local` / Apple M3 Ultra:

| Variant | Single stream | Serial pair | Concurrent encoder pair | Separate queues | Interpretation |
|---|---:|---:|---:|---:|---|
| Private disjoint buffers | `59.432 ms` | `118.825 ms` | `115.502 ms` | span `115.670 ms`, overlap `113.007 ms` | Overlap exists, but contention keeps span near `2x`. |
| Shared default disjoint buffers | `59.441 ms` | `118.929 ms` | `115.460 ms` | span `115.272 ms`, overlap `108.324 ms` | Shared read-only source does not force serialization by itself. |
| Shared default same buffer, disjoint offsets | `59.366 ms` | `118.841 ms` | `115.505 ms` | span `118.892 ms`, overlap `0.000 ms` | Same `MTLBuffer` identity serializes separate queues. |
| Shared tracked same buffer, disjoint offsets | `59.396 ms` | `118.855 ms` | `115.445 ms` | span `118.869 ms`, overlap `0.000 ms` | Forced tracking behaves like default. |
| Shared untracked same buffer, disjoint offsets | `59.416 ms` | `118.915 ms` | `115.465 ms` | span `115.456 ms`, overlap `114.342 ms` | Untracked resources remove that same-buffer serialization in the synthetic case. |

Interpretation:

- Metal compute overlap is not globally impossible on Apple GPUs. The synthetic
  separate-queue case overlapped heavily on `studio.local`.
- Overlap alone is not a useful speedup guarantee. When two full-occupancy
  kernels overlap, they contend and the total span remains roughly equal to the
  two-dispatch serial cost.
- Resource identity matters. Two command buffers writing disjoint offsets of
  the same default/tracked `MTLBuffer` serialized across queues even though
  disjoint buffers overlapped. The untracked same-buffer variant overlapped on
  `studio.local`, which makes false resource sharing a credible DS4 diagnostic.
- The earlier DS4 timeline result is therefore more specific: the real MTP and
  target decode command buffers did not overlap in that graph shape. Possible
  causes are false resource dependencies, large dispatch granularity,
  bandwidth/occupancy saturation, or scheduler choices for the actual kernel
  mix.
- Do not rewrite the full graph encoder around `MTLDispatchTypeConcurrent`
  based on the microbench. The next useful checks are a real DS4 resource
  identity audit and Metal System Trace before attempting a narrow combined
  encoder or untracked-resource prototype.

Resource-hazard audit snapshot:

- The async path now has separate command queues, separate scratch structs
  (`g_default_scratch` and `g_mtp_scratch`), and separate transient buffer
  arrays (`g_default_transient_buffers` and `g_mtp_transient_buffers`).
- `ds4_gpu_begin_mtp_async_commands()` switches both active scratch and active
  transient ownership to the MTP lane; `ds4_gpu_end_mtp_async_commands()` and
  waits restore the default target lane.
- The async MTP chain also uses its own persistent tensor set in
  `ds4_mtp_async_scratch`, including `raw_cache`, `state_hc`, `next_hc`, logits,
  router/output tensors, and per-layer work tensors.
- Remaining shared resources are expected: model mmap view buffers and pipeline
  states. The model views are read-only inputs to the kernels, but they can
  still be the practical memory-bandwidth bottleneck.
- All regular tensors and scratch buffers currently use default Metal hazard
  tracking via `MTLResourceStorageModeShared`.
- The synthetic same-buffer result means the next audit should prove whether
  any real target/MTP lane buffers still share one `MTLBuffer` base at command
  buffer submission time. If they do, separate allocations are the safer first
  fix; `MTLResourceHazardTrackingModeUntracked` should be considered only for
  buffers whose cross-queue dependencies are explicitly owned by the scheduler.

## DS4 Buffer Identity Audit

Run date: 2026-05-13

Implementation:

- Added `DS4_METAL_BUFFER_AUDIT=1` to record the `MTLBuffer` identities touched
  by each target or MTP command buffer at submission time. The audit tracks
  graph tensors, tensor copies, scratch buffers, and transient buffers. Model
  mmap view buffers are not counted in this audit because they are expected
  read-only inputs and would hide the mutable-buffer signal.
- Added `DS4_METAL_BUFFER_AUDIT_DETAILS=1` for shared-buffer labels, length,
  storage mode, and hazard-tracking mode. The MTP async scratch tensors are now
  labeled as `mtp_async.*` so shared-buffer reports identify the actual arena.
- The audit stores retained `MTLBuffer` objects, not raw pointer-only values, so
  details remain valid even after transient-array ownership changes.

Command shapes:

```sh
DS4_METAL_BUFFER_AUDIT=1 \
DS4_MTP_RUNAHEAD_ENABLE=1 DS4_MTP_GOVERNOR_DISABLE=1 DS4_MTP_TIMING=1 \
./ds4 --metal -m "$MODEL" --mtp "$MTP" --mtp-draft 4 \
  --ctx 1024 --nothink -sys "" --temp 0 -n 24 \
  --prompt-file /tmp/ds4-mtp-matrix/count.txt

DS4_METAL_COMMAND_TIMELINE=1 DS4_METAL_BUFFER_AUDIT=1 \
DS4_MTP_RUNAHEAD_ENABLE=1 DS4_MTP_GOVERNOR_DISABLE=1 DS4_MTP_TIMING=1 \
./ds4 --metal -m "$MODEL" --mtp "$MTP" --mtp-draft 4 \
  --ctx 1024 --nothink -sys "" --temp 0 -n 12 \
  --prompt-file /tmp/ds4-mtp-matrix/count.txt
```

Observed on `studio.local`:

- `-n 24` audit: `56` target audit records; `51` had `shared=0`; `5` had
  `shared=44`. The `shared=0` records covered all `target-flush` records and
  the large target decode records. Throughput was `33.20 t/s`.
- Labeled `-n 12` detail pass: `29` target audit records; `27` had `shared=0`;
  `2` had `shared=44`. The shared buffers were the MTP async scratch arena, for
  example `mtp_async.kv_raw`, `mtp_async.ffn_norm`, `mtp_async.output_norm`,
  `mtp_async.router_selected`, and `mtp_async.logits`.
- The nonzero `shared=44` cases were the small `target-end` MTP-draft command
  buffers that run on the target queue after the previous async MTP work has
  already been waited/promoted. They are not the target verifier/decode command
  buffers that we expected to overlap with the MTP queue.
- The combined timeline/audit pass still showed serialization even when the
  following target command buffers had `shared=0`; for example
  `ds4-mtp-6 gpu=971731.660653000-971731.665560000`, then
  `ds4-target-7 gpu=971731.665560875-971731.667845500`.

Interpretation:

- False same-buffer identity among the tracked mutable target-verifier buffers
  is not the reason the real DS4 MTP command buffers serialize before target
  decode in this run.
- The remaining shared-buffer report is real but expected: the synchronous
  target-queue MTP draft reuses the MTP async scratch arena after the async lane
  has completed. It should not be read as evidence of intended target/MTP
  overlap.
- The next concurrency question is therefore scheduler/resource saturation, not
  an obvious mutable-buffer alias. Metal System Trace is the next useful source
  of evidence before attempting untracked resources or a combined encoder.

## Metal System Trace Capture

Run date: 2026-05-13

Trace commands:

```sh
xctrace record --quiet --no-prompt --template "Metal System Trace" \
  --output /tmp/ds4-mtp-matrix/count_metal_system_n12.trace \
  --target-stdout /tmp/ds4-mtp-matrix/count_metal_system_n12.out \
  --env DS4_MTP_RUNAHEAD_ENABLE=1 \
  --env DS4_MTP_GOVERNOR_DISABLE=1 \
  --env DS4_MTP_TIMING=1 \
  --env DS4_METAL_COMMAND_TIMELINE=1 \
  --launch -- ./ds4 --metal -m "$MODEL" --mtp "$MTP" --mtp-draft 4 \
    --ctx 1024 --nothink -sys "" --temp 0 -n 12 \
    --prompt-file /tmp/ds4-mtp-matrix/count.txt

xctrace record --quiet --no-prompt --template "Metal System Trace" \
  --instrument "Metal GPU Counters" \
  --output /tmp/ds4-mtp-matrix/count_metal_counters_n8.trace \
  --target-stdout /tmp/ds4-mtp-matrix/count_metal_counters_n8.out \
  --env DS4_MTP_RUNAHEAD_ENABLE=1 \
  --env DS4_MTP_GOVERNOR_DISABLE=1 \
  --env DS4_MTP_TIMING=1 \
  --env DS4_METAL_COMMAND_TIMELINE=1 \
  --launch -- ./ds4 --metal -m "$MODEL" --mtp "$MTP" --mtp-draft 4 \
    --ctx 1024 --nothink -sys "" --temp 0 -n 8 \
    --prompt-file /tmp/ds4-mtp-matrix/count.txt
```

Captured artifacts on `studio.local`:

- `/tmp/ds4-mtp-matrix/count_metal_system_n12.trace`, `30M`, completed with
  exit code `0`. The TOC records `Metal System Trace`, duration `2.999707 s`,
  target process `ds4`, pid `73117`, exit code `0`.
- `/tmp/ds4-mtp-matrix/count_metal_counters_n8.trace`, `24M`, completed with
  exit code `0`, but Instruments reported
  `Selected counter profile is not supported on target device`.
- The counter trace TOC showed `Metal GPU Counters` with
  `Counter Set: Performance Limiters` and `Shader Timeline: Enabled`, but the
  exported `gpu-counter-info`, `gpu-counter-value`,
  `metal-gpu-counter-profile`, and `metal-shader-profiler-intervals` tables
  contained schema only. This run did not yield usable occupancy or memory
  bandwidth counters on the M3 Ultra through the CLI path.

Observed scheduling:

- The target work is not blocked on the host waiting for the MTP command buffer
  to finish. In the counter trace, `ds4-mtp-6` was committed at
  `00:02.367.062`; `ds4-target-7` was committed at `00:02.367.482`; and
  `ds4-mtp-6` did not complete until `00:02.372.832`.
- Despite that in-flight submission overlap, the GPU compute work remained
  essentially back-to-back. The `DS4_METAL_COMMAND_TIMELINE` pairs from the
  `-n 12` trace included:

| Pair | GPU handoff gap |
|---|---:|
| `ds4-mtp-6` -> `ds4-target-7` | `0.001875 ms` |
| `ds4-mtp-9` -> `ds4-target-10` | `0.000625 ms` |
| `ds4-mtp-12` -> `ds4-target-13` | `0.000750 ms` |
| `ds4-mtp-18` -> `ds4-target-19` | `0.000875 ms` |
| `ds4-mtp-30` -> `ds4-target-31` | `-0.003417 ms` |

- The explicit-counter `-n 8` rerun showed the same shape:
  `0.000500/0.000708/0.000542/-0.004375/0.000833/0.000750 ms` for the observed
  MTP-to-target handoffs.
- `metal-gpu-intervals` independently showed the same pattern: MTP blit/compute
  intervals and the following target compute intervals are adjacent, with at
  most a few microseconds of timestamp noise, not a material overlap window.

Interpretation:

- The implementation is submitting target work while MTP work is still in
  flight. The failure to overlap is therefore not explained by a CPU-side wait
  for draft generation to complete.
- The trace strengthens the current hypothesis: the real target/MTP kernel mix
  is being serialized by GPU scheduling or resource pressure even when the
  tracked mutable buffers do not alias.
- The CLI trace did not answer the hardware-headroom question because usable
  occupancy and bandwidth counters were not emitted. A future counter attempt
  would need a manually configured Instruments capture or a different supported
  counter profile on the target machine.
- This evidence does not justify a broad untracked-resource conversion or a
  full graph-encoder rewrite. The next architecture-level progress still needs
  a target-exact verifier path that can accept more than one token without one
  full serial target decode per accepted token.

## Exact Runahead Multi-Token Verifier

Run date: 2026-05-13

Implementation:

- Added `metal_graph_verify_decode_exact()`: a fixed-shape verifier for
  `N <= 16` draft tokens that keeps the normal target decode kernels and cache
  update order, but encodes the suffix layer-by-layer in one command stream.
- Added `DS4_MTP_RUNAHEAD_VERIFY_N_EXACT=1` to make the async runahead path
  fill the MTP frontier, verify the current draft chain with that exact target
  verifier, and commit the accepted prefix.
- Added `DS4_MTP_VERIFY_DECODE_N_VALIDATE=1` to restore the target snapshot,
  serially replay the same verified suffix, and compare every verified logits
  row/top token against the exact verifier output.
- Added `DS4_MTP_VERIFY_DECODE_N_BATCH_HEAD=1` to test a faster output stage:
  decode layers and cache updates remain exact and row-ordered, but the final
  output head is run with the batched verifier head.
- The partial-accept path is deliberately conservative: after a mismatch it
  restores the target snapshot and replays the accepted prefix with normal
  serial target decode. This keeps state exact but is expected to be slow.

Validation command shape:

```sh
DS4_MTP_RUNAHEAD_ENABLE=1 \
DS4_MTP_RUNAHEAD_VERIFY_N_EXACT=1 \
DS4_MTP_VERIFY_DECODE_N_VALIDATE=1 \
DS4_MTP_GOVERNOR_DISABLE=1 \
DS4_MTP_TIMING=1 \
./ds4 --metal -m "$MODEL" --mtp "$MTP" --mtp-draft "$N" \
  --ctx 1024 --nothink -sys "" --temp 0 --seed 1 -n 16 \
  -p "Count from 1 to 20."
```

Validation sweep on `studio.local`:

| Draft cap | Output vs baseline | Verified rows | Committed rows | Max logit delta | Top mismatches | Throughput with validation |
|---:|---|---|---|---:|---:|---:|
| 2 | Match | `2:5` | `2:5` | `0` | `0` | `21.10 t/s` |
| 3 | Match | `3:4` | `3:4` | `0` | `0` | `19.73 t/s` |
| 4 | Match | `4:3` | `3:1,4:2` | `0` | `0` | `17.17 t/s` |
| 5 | Match | `3:1,5:2` | `3:1,5:2` | `0` | `0` | `19.06 t/s` |
| 6 | Match | `3:1,6:2` | `3:1,4:1,6:1` | `0` | `0` | `14.96 t/s` |
| 7 | Match | `3:1,7:2` | `3:2,7:1` | `0` | `0` | `13.82 t/s` |
| 8 | Match | `6:1,8:1` | `4:1,8:1` | `0` | `0` | `15.39 t/s` |

Timing command shape:

```sh
DS4_MTP_RUNAHEAD_ENABLE=1 \
DS4_MTP_RUNAHEAD_VERIFY_N_EXACT=1 \
DS4_MTP_GOVERNOR_DISABLE=1 \
DS4_MTP_TIMING=1 \
./ds4 --metal -m "$MODEL" --mtp "$MTP" --mtp-draft "$N" \
  --ctx 1024 --nothink -sys "" --temp 0 --seed 1 -n 32 \
  -p "Count from 1 to 20."
```

Timing sweep on `studio.local`; ordinary serial target baseline without MTP was
`37.58 t/s`:

| Draft cap | Output vs baseline | Throughput | Verified rows | Committed rows | Avg verify | Avg replay |
|---:|---|---:|---|---|---:|---:|
| 2 | Match | `33.41 t/s` | `2:10` | `2:10` | `57.371 ms` | `0.000 ms` |
| 3 | Match | `33.39 t/s` | `3:8` | `3:8` | `86.706 ms` | `0.000 ms` |
| 4 | Match | `26.82 t/s` | `2:1,4:6` | `2:1,3:2,4:4` | `107.344 ms` | `24.012 ms` |
| 5 | Match | `18.98 t/s` | `5:7` | `2:3,3:2,5:2` | `143.614 ms` | `47.089 ms` |
| 6 | Match | `17.88 t/s` | `5:1,6:5` | `3:3,4:1,5:1,6:1` | `167.198 ms` | `82.160 ms` |
| 7 | Match | `16.71 t/s` | `5:1,7:5` | `2:1,3:2,4:1,5:1,7:1` | `191.068 ms` | `77.632 ms` |
| 8 | Match | `15.62 t/s` | `2:1,5:1,8:5` | `2:4,4:2,8:1` | `192.989 ms` | `54.918 ms` |

Interpretation:

- The verifier contract is now real and target-exact for the exercised
  positions: every validation row had `max_delta=0`, `mismatches=0`, and
  baseline-matching stdout.
- Reusing exact one-token decode kernels in a layer-major suffix stream is not
  a promotable throughput path. Even the all-accepted `N=2` and `N=3` cases
  landed at about `33.4 t/s` versus the `37.58 t/s` baseline, and deeper
  fixed shapes are much slower once partial-accept replay appears.
- This is still useful architecture evidence: the control/state contract for a
  multi-token target verifier is implementable, but a speed win requires a
  faster fixed-shape target verifier primitive than the current decode-kernel
  composition.

Batch-output-head validation:

```sh
DS4_MTP_RUNAHEAD_ENABLE=1 \
DS4_MTP_RUNAHEAD_VERIFY_N_EXACT=1 \
DS4_MTP_VERIFY_DECODE_N_BATCH_HEAD=1 \
DS4_MTP_VERIFY_DECODE_N_VALIDATE=1 \
DS4_MTP_GOVERNOR_DISABLE=1 \
DS4_MTP_TIMING=1 \
./ds4 --metal -m "$MODEL" --mtp "$MTP" --mtp-draft "$N" \
  --ctx 1024 --nothink -sys "" --temp 0 --seed 1 -n 16 \
  -p "Count from 1 to 20."
```

| Draft cap | Output vs baseline | Verified rows | Committed rows | Max logit delta | Top mismatches | Throughput with validation |
|---:|---|---|---|---:|---:|---:|
| 2 | Match | `2:5` | `2:5` | `1.52588e-05` | `0` | `21.32 t/s` |
| 3 | Match | `3:4` | `3:4` | `1.52588e-05` | `0` | `19.82 t/s` |
| 4 | Match | `4:3` | `3:1,4:2` | `1.52588e-05` | `0` | `17.61 t/s` |
| 5 | Match | `3:1,5:2` | `3:1,5:2` | `1.52588e-05` | `0` | `19.10 t/s` |
| 6 | Match | `3:1,6:2` | `3:1,4:1,6:1` | `1.52588e-05` | `0` | `15.32 t/s` |
| 7 | Match | `3:1,7:2` | `3:2,7:1` | `1.52588e-05` | `0` | `13.91 t/s` |
| 8 | Match | `6:1,8:1` | `4:1,8:1` | `1.52588e-05` | `0` | `15.51 t/s` |

Batch-output-head timing sweep on `studio.local`; ordinary serial target
baseline without MTP was `37.61 t/s`:

| Draft cap | Output vs baseline | Throughput | Verified rows | Committed rows | Avg verify | Avg replay |
|---:|---|---:|---|---|---:|---:|
| 2 | Match | `33.90 t/s` | `2:10` | `2:10` | `56.184 ms` | `0.000 ms` |
| 3 | Match | `33.82 t/s` | `3:8` | `3:8` | `85.033 ms` | `0.000 ms` |
| 4 | Match | `27.24 t/s` | `2:1,4:6` | `2:1,3:2,4:4` | `104.960 ms` | `23.709 ms` |
| 5 | Match | `19.23 t/s` | `5:7` | `2:3,3:2,5:2` | `140.314 ms` | `47.132 ms` |
| 6 | Match | `18.14 t/s` | `5:1,6:5` | `3:3,4:1,5:1,6:1` | `163.108 ms` | `82.056 ms` |
| 7 | Match | `17.00 t/s` | `5:1,7:5` | `2:1,3:2,4:1,5:1,7:1` | `186.194 ms` | `77.313 ms` |
| 8 | Match | `15.88 t/s` | `2:1,5:1,8:5` | `2:4,4:2,8:1` | `188.429 ms` | `54.813 ms` |

Best-candidate three-prompt confirmation, `--mtp-draft 2`,
`DS4_MTP_VERIFY_DECODE_N_BATCH_HEAD=1`, `-n 64`:

| Prompt | Baseline | Batch-head exact runahead | Output | Counters |
|---|---:|---:|---|---|
| Counting | `36.89 t/s` | `34.01 t/s` | Match | `verified=2:21`, `committed=2:21`, avg verify `56.350 ms`, replay `0.000 ms` |
| Explanation | `36.76 t/s` | `27.87 t/s` | Match | `verified=2:21`, `committed=1:7,2:14`, avg verify `56.755 ms`, replay `9.173 ms` |
| Code completion | `37.01 t/s` | `30.91 t/s` | Match | `verified=2:21`, `committed=1:3,2:18`, avg verify `57.009 ms`, replay `3.819 ms` |

Batch-head interpretation:

- Batched output head is top-token equivalent in this sweep, but not
  bit-identical: max row-logit delta was `1.52588e-05`.
- The speedup over row-wise output head is too small to change the architecture
  decision. The best case, `N=2`, improved from `33.41 t/s` to `33.90 t/s` on
  the short counting sweep, still below the `37.61 t/s` baseline.
- The three-prompt confirmation lost on every prompt. Explanation and code
  completion suffered partial accepts and paid replay cost.
- Current verdict: keep the exact row-head path as the correctness reference,
  keep the batch-head path as a near-exact diagnostic candidate, and do not
  promote either as the target verifier.

## Latest Child-Row Probe Evidence

Run date: 2026-05-13

Probe command shape:

```sh
DS4_MTP_TREE_4X2X2_PROBE=1 \
DS4_MTP_TREE_4X2X2_PROBE_STEPS=5 \
DS4_MTP_TREE_4X2X2_ROOT_ROW_VERIFY=1 \
DS4_MTP_TREE_4X2X2_CHILD_ROW_VERIFY=1 \
DS4_MTP_TREE_4X2X2_BATCH_HEAD=1 \
DS4_MTP_TREE_4X2X2_HC_DIFF=1 \
DS4_MTP_TIMING=1 \
./ds4 --metal -m "$MODEL" --mtp "$MTP" --mtp-draft 3 \
  --ctx 1024 --nothink -sys "" --temp 0 -n 40 \
  --prompt-file /tmp/ds4-mtp-matrix/code.txt
```

Observed on the exact code-completion prompt:

| Case | Time | Top mismatches | Drift evidence |
|---|---:|---:|---|
| Serial branch/root rows | 105-124 ms for 4 rows | 0 | Logit max deltas stayed near `1e-5`. |
| Noemit root-row batch | 69-73 ms for 4 rows | 0 | Final-HC max delta `0.75-4.98`; logit max delta `0.36-2.74`. |
| Serial child row at `ratio4_emit_boundary` | 63.76-64.07 ms for 2 rows | 0 | Logit max delta `~1e-5`; exact enough but slow. |
| Noemit non-boundary child-row batch | 101-105 ms with HC diagnostics enabled | 0 | Final-HC max delta `0.29-2.06`; logit max delta `0.13-0.80`. |
| Full serial child scratch | 224-250 ms for 8 rows | 0 | Logit max deltas stayed near `1e-5`. |
| Removed `ratio4_emit_boundary_batch` | 50.44-51.52 ms for 2 rows | 0 | Faster than serial boundary, but max logit delta was `0.096-0.104`. |

Interpretation:

- The current noemit row-batch verifier is not merely suffering output-head
  drift. Its final HC already diverges from the serial target path.
- The only exact row path measured so far is serial token decode plus batched
  output head. That path is too expensive for a hot verifier.
- A promotable implementation needs a new fixed-shape decode primitive that
  preserves the serial target math and cache/reduction order while avoiding
  per-candidate serial restore.

## Validation Log

Latest validation, 2026-05-13:

| Command | Host | Result |
|---|---|---|
| `make ds4_test ds4` | local | Pass |
| `git diff --check` | local | Pass |
| `./ds4_test --metal-kernels` | local | Pass |
| `make ds4_test ds4` | `studio.local` | Pass |
| `./ds4_test --metal-kernels` | `studio.local` | Pass |
| Baseline vs `--mtp "$MTP" --mtp-draft 2`, `-p "Count from 1 to 20." -n 32 --temp 0 --seed 1 --nothink` | `studio.local` | stdout matched byte-for-byte; baseline `37.23 t/s`, MTP `36.30 t/s`; governor disabled speculation after 4 cycles. |
| Baseline vs `DS4_MTP_RUNAHEAD_ENABLE=1 --mtp "$MTP" --mtp-draft 2`, `-p "Count from 1 to 20." -n 32 --temp 0 --seed 1 --nothink` | `studio.local` | stdout matched byte-for-byte; baseline `37.53 t/s`, runahead `34.37 t/s`; MTP wait was hidden behind target decode. |
| Three-prompt `DS4_MTP_RUNAHEAD_ENABLE=1` matrix, `-n 64 --temp 0 --ctx 1024 --nothink -sys ""` | `studio.local` | stdout matched fresh baseline for all prompts; see Async Runahead1 Prototype. |
| Baseline vs `DS4_MTP_RUNAHEAD_ENABLE=1 --mtp "$MTP" --mtp-draft 4`, `-p "Count from 1 to 20." -n 48 --temp 0 --seed 1 --nothink` | `studio.local` | stdout matched byte-for-byte; baseline `36.94 t/s`, runahead-chain `33.20 t/s`; max committed depth observed was 5 tokens/call. |
| Three-prompt `DS4_MTP_RUNAHEAD_ENABLE=1 --mtp-draft 4` chain matrix, `-n 64 --temp 0 --ctx 1024 --nothink -sys ""` | `studio.local` | stdout matched fresh baseline for all prompts; see Async Runahead Chain Prototype. |
| `make ds4_test ds4` after MTP-disable and token-router fixes | local | Pass |
| Baseline vs `DS4_MTP_SPEC_DISABLE=1 --mtp "$MTP" --mtp-draft 4`, `-p "Count from 1 to 20." -n 48 --temp 0 --seed 1 --nothink` after fix | `studio.local` | stdout matched byte-for-byte; baseline `37.19 t/s`, MTP-loaded disabled path `36.28 t/s`. |
| Baseline vs `DS4_MTP_RUNAHEAD_ENABLE=1 --mtp "$MTP" --mtp-draft 4`, same smoke after token-router fix | `studio.local` | stdout matched byte-for-byte; runahead-chain `33.56 t/s`. |
| Post-fix three-prompt timing matrix, baseline vs MTP-loaded-disabled vs default MTP draft4 vs `DS4_MTP_RUNAHEAD_ENABLE=1 --mtp-draft 4`, `-n 64 --temp 0 --ctx 1024 --nothink -sys ""` | `studio.local` | Disabled path matched baseline for all prompts; runahead matched baseline for all prompts; runahead landed at `33.55/33.56/33.72 t/s` vs baseline `37.41/37.19/37.36 t/s`; see Post-Fix Async Runahead Timing Matrix. |
| Runahead draft-depth sweep, `--mtp-draft 1..8`, three prompts, `-n 64 --temp 0 --ctx 1024 --nothink -sys ""` | `studio.local` | All outputs matched baseline. Average throughput was `36.96/34.17/33.69/33.19/32.02/32.35/32.18/31.70 t/s` for draft caps `1..8`; see Runahead Draft-Depth Sweep. |
| `DS4_METAL_COMMAND_TIMELINE=1` timeline check, count prompt, `--mtp-draft 4 -n 24` | `studio.local` | 17/18 MTP command buffers serialized before the next target command buffer; the only apparent overlap was about `0.005 ms`. |
| `DS4_METAL_COMMAND_TIMELINE=1 ./ds4_test --metal-concurrency` | local | Pass; synthetic separate queues overlapped, but total span stayed serial-shaped (`single 11.997 ms`, serial pair `24.113 ms`, concurrent pair `24.221 ms`, separate span `25.042 ms`). |
| `DS4_METAL_COMMAND_TIMELINE=1 ./ds4_test --metal-concurrency` | `studio.local` | Pass; synthetic separate queues overlapped (`14.290 ms` overlap), but total span stayed serial-shaped (`single 7.391 ms`, serial pair `14.731 ms`, concurrent pair `14.462 ms`, separate span `14.879 ms`). |
| `DS4_METAL_CONCURRENCY_PROBE_DISPATCHES=8 ./ds4_test --metal-concurrency` | local | Pass; same-buffer default/tracked layouts serialized separate queues, while disjoint and untracked same-buffer layouts overlapped but remained serial-shaped in span. |
| `DS4_METAL_CONCURRENCY_PROBE_DISPATCHES=8 ./ds4_test --metal-concurrency` | `studio.local` | Pass; default/tracked same-buffer layouts had `0.000 ms` overlap, while untracked same-buffer layout restored `114.342 ms` overlap over a `115.456 ms` span. |
| `DS4_METAL_BUFFER_AUDIT=1 DS4_MTP_RUNAHEAD_ENABLE=1 --mtp-draft 4`, count prompt, `-n 24` | `studio.local` | Pass; `56` target audit records, `51` with `shared=0`; the `5` nonzero records were small target-queue MTP draft buffers, not the large target verifier/decode buffers. Generation `33.20 t/s`. |
| `DS4_METAL_BUFFER_AUDIT_DETAILS=1 DS4_MTP_RUNAHEAD_ENABLE=1 --mtp-draft 4`, count prompt, `-n 12` | `studio.local` | Pass; nonzero shared buffers were labeled `mtp_async.*` scratch tensors such as `mtp_async.kv_raw`, `mtp_async.ffn_norm`, and `mtp_async.logits`. |
| `DS4_METAL_COMMAND_TIMELINE=1 DS4_METAL_BUFFER_AUDIT=1 DS4_MTP_RUNAHEAD_ENABLE=1 --mtp-draft 4`, count prompt, `-n 12` | `studio.local` | Pass; MTP still serialized before the next target command even when that target command reported `shared=0`, e.g. `ds4-mtp-6` ended at `971731.665560000` and `ds4-target-7` started at `971731.665560875`. |
| `xctrace record --template "Metal System Trace"` with `DS4_METAL_COMMAND_TIMELINE=1 DS4_MTP_RUNAHEAD_ENABLE=1 --mtp-draft 4`, count prompt, `-n 12` | `studio.local` | Pass; trace saved at `/tmp/ds4-mtp-matrix/count_metal_system_n12.trace`; target work was submitted while MTP was still in flight, but GPU execution remained back-to-back with handoff gaps around `0.0006-0.0019 ms` plus one `-0.0034 ms` timestamp-noise case. |
| `xctrace record --template "Metal System Trace" --instrument "Metal GPU Counters"` with the same env, count prompt, `-n 8` | `studio.local` | Pass with Instruments warning `Selected counter profile is not supported on target device`; TOC showed `Performance Limiters` and shader timeline enabled, but exported counter/shader tables had schema only, so no usable occupancy or memory-bandwidth counters were produced. |
| `make ds4_test ds4` after adding the exact runahead verifier | local | Pass |
| `./ds4_test --metal-kernels` after adding the exact runahead verifier | local | Pass |
| `make ds4_test ds4` after adding the exact runahead verifier | `studio.local` | Pass |
| Baseline vs `DS4_MTP_RUNAHEAD_VERIFY_N_EXACT=1 DS4_MTP_VERIFY_DECODE_N_VALIDATE=1 --mtp-draft 4`, count prompt, `-n 24 --seed 1` | `studio.local` | stdout matched baseline; validation reported `max_delta=0`, `mismatches=0`; baseline `38.27 t/s`, validation run `17.51 t/s`. |
| Exact runahead verifier validation sweep, `--mtp-draft 2..8`, count prompt, `-n 16 --seed 1` | `studio.local` | stdout matched baseline for every draft cap; all validation rows reported `max_delta=0`, `mismatches=0`. |
| Exact runahead verifier timing sweep, `--mtp-draft 2..8`, count prompt, `-n 32 --seed 1` | `studio.local` | stdout matched baseline for every draft cap; throughput was `33.41/33.39/26.82/18.98/17.88/16.71/15.62 t/s` for draft caps `2..8` versus baseline `37.58 t/s`. |
| `make ds4_test ds4 && ./ds4_test --metal-kernels` after adding the batch-output-head verifier variant | `studio.local` | Pass |
| `git diff --check` after exact/batch verifier docs and code | local | Pass |
| `./ds4_test` | local | Failed long-context output expectations; remaining suites continued and passed. |

The model-backed probe and matrix were run from a non-git rsync copy on
`studio.local`, so `git diff --check` remains a local validation step.

## Decision Log

- Do not promote the serial `ratio4_emit_boundary` verifier path. It proves the
  boundary state reconstruction is correct, but the timing is not competitive.
- Do not promote `DS4_MTP_TREE_4X2X2_RATIO4_BOUNDARY_BATCH` either. It narrows
  the boundary cost to about `50-52 ms` and keeps top tokens stable in the
  probe, but the child-row logits drift by about `0.1`, which is too large for
  a target verifier continuation row.
- Do not promote the current noemit row-batch verifier. The latest HC-diff
  probe shows the drift enters before the output head, in the row-batch layer
  stack itself.
- Do not keep the sequential predicted-chain prototype as a generation path.
  It is removed from code because it is a measurement simulation, not the
  desired branch-prediction scheduler.
- Do not promote the current `DS4_MTP_RUNAHEAD_ENABLE=1` mode as a throughput
  path yet. It proves exact async MTP scratch/queue isolation and hides MTP wait,
  but it accepts only one target token per cycle and is slower than baseline.
- Treat the previous deeper async runahead-chain throughput result as a partial
  prototype result, not an architecture-level rejection. It still paid one full
  target decode for every accepted token, `DS4_MTP_SPEC_DISABLE` did not disable
  all MTP drafting, and GPU token chaining did not yet feed token-aware router
  selection.
- Do not compare the async runahead-chain path to the root-top-k `4x2x2` tree
  path. The async path currently gates on the top-1 MTP root matching the target
  token; it does not reseed from a recovered rank>0 root candidate.
- Do not spend the next iteration splitting target/MTP mutable tensors based on
  the current evidence. The real submission-time audit found `shared=0` for the
  target verifier/decode command buffers that follow MTP work; the nonzero
  shared records are the synchronous target-queue MTP draft reusing the
  `mtp_async.*` scratch arena after MTP completion.
- Do not classify the current runahead serialization as a CPU-side wait for
  draft generation. Metal System Trace shows target command buffers being
  submitted before the preceding MTP command buffer completes. The serialization
  is happening at GPU execution time.
- Do not use the current CLI Metal GPU Counters trace as an occupancy or memory
  bandwidth source. It recorded no usable counter rows on the M3 Ultra despite
  enabling the `Metal GPU Counters` instrument.
- Do not make queue overlap or GPU partitioning the primary promotion path
  without new evidence of material hardware headroom. The measured real DS4
  handoffs are microsecond-scale adjacency, not useful overlap.
- Do not promote the current exact runahead verifier as a throughput path. It
  proves the multi-token verifier state contract and serial-logit equivalence,
  but exact decode-kernel composition is still slower than baseline even at
  `N=2..3` and degrades further with deeper fixed shapes.
- Treat the exact runahead verifier as the new correctness reference for any
  faster fixed-shape verifier kernel. A future candidate must match its
  row-by-row logits/top tokens before it gets a throughput matrix.
- Do not promote the batch-output-head variant either. It is top-token
  equivalent in the current validation sweep, but has `1.52588e-05` row-logit
  drift and still loses the three-prompt `N=2` confirmation against baseline.
- Explicitly defer reconnecting root-top-k `4x2x2` recovery into async runahead.
  Tree recovery can only help after there is a target verifier that wins on a
  simple linear draft chain. The current verifier bottleneck would make a
  wider tree pay more target verification cost without a promotion path.
- Remove and keep removed the rejected boundary-batch branch. It no longer has
  a plausible promotion path under the current implementation.
- Keep future experiments fixed-shape and narrow until there is a measured win.
- Avoid reintroducing the removed C-composed verifier and branch-attention
  experiments unless a new kernel primitive changes the cost model.

## Removed Predicted-Chain Probe

Run date: 2026-05-13

The removed sequential probe used target decode and MTP draft in series to
estimate an ideal overlap ceiling. It was intentionally not the final
algorithm: it still verified one target token at a time and only reported what
could have been hidden if the MTP side had run concurrently.

Three-prompt `-n 64` result before removal:

| Prompt | Baseline | Default MTP | Tree/root-topk | Predict-chain |
|---|---:|---:|---:|---:|
| Counting | 37.05 t/s | 40.40 t/s | 40.21 t/s | 33.98 t/s |
| Explanation | 36.93 t/s | 35.94 t/s | 36.02 t/s | 33.76 t/s |
| Code completion | 36.76 t/s | 35.44 t/s | 35.51 t/s | 34.00 t/s |

Output comparison:

- `count`, `explain`, and `code` predictor-chain outputs matched the baseline
  target outputs byte-for-byte.

Timing evidence from depth-8 chains:

- MTP draft time: about `15.4-16.0 ms` per 8-token chain.
- Exact target decode time: about `216-225 ms` per 8-token chain.
- The ideal async overlap ceiling is therefore only about `15-16 ms` per
  8-token chain.
- Predictor hits varied by prompt and chain, commonly `2-6` hits out of `8`;
  the control flow correctly corrected misses, but corrections do not remove
  the target decode cost.

Decision:

- Removed from code so future measurements cannot accidentally use it as a
  stand-in for the real runahead scheduler.
- The numbers remain useful negative evidence: pure overlap of the existing
  sequential MTP draft slice is not enough by itself, because exact target
  decode remains dominant.

## Async Scratch Isolation

Run date: 2026-05-13

Implementation:

- Added `ds4_mtp_async_scratch`, allocated only for MTP-enabled graphs.
- Duplicated the one-row decode and MTP output-head scratch touched by
  `metal_graph_encode_decode_layer()` and `metal_graph_encode_output_head_mtp()`
  for the MTP path: HC mix/split views, attention intermediates, FFN/router/MoE
  intermediates, output-head tensors, and a 16-entry top-k buffer.
- Routed `metal_graph_eval_mtp_draft_from_hc()` through a scoped scratch bind.
  MTP frontend tensors (`mtp_embed`, `mtp_hproj_hc`, `mtp_input_hc`, etc.) were
  already separate and remain unchanged.
- Allocated scratch `ffn_out` eagerly so debug/steering paths cannot allocate a
  lazy tensor into the target graph while the MTP scratch is bound.

Validation:

- Local: `git diff --check`, `make ds4_test ds4`, and
  `./ds4_test --metal-kernels` passed.
- `studio.local`: `make ds4_test ds4` and `./ds4_test --metal-kernels` passed.
- `studio.local` deterministic generation matched baseline stdout byte-for-byte
  for `Count from 1 to 20.`, `-n 32`, `--temp 0`, `--seed 1`, `--nothink`.

Decision:

- Keep this as the first real prerequisite for branch-prediction/runahead. It
  does not claim a speedup yet; it removes the scratch alias that prevented MTP
  drafting from becoming a separate GPU lane.

## Async Runahead1 Prototype

Run date: 2026-05-13

Implementation:

- Added a second Metal command queue and independent internal scratch context
  for MTP work. Default target decode and MTP runahead now select separate
  scratch arenas and transient buffer lists while encoding commands.
- Added async command helpers for beginning, committing, waiting, and querying
  MTP work.
- Added an env-gated depth-1 scheduler behind `DS4_MTP_RUNAHEAD_ENABLE=1`.
  The scheduler starts one MTP child draft before target decode, waits only
  after target decode, promotes the MTP raw-cache/HC frontier when the target
  token matches, and rewinds the promoted frontier on a later miss.
- The mode intentionally verifies only the first target token in each cycle.
  It does not run target verification for `k+1` after a miss at `k`.

Smoke result:

- Command: baseline vs `DS4_MTP_RUNAHEAD_ENABLE=1 --mtp "$MTP" --mtp-draft 2`,
  `-p "Count from 1 to 20." -n 32 --temp 0 --seed 1 --nothink`.
- Output: stdout matched byte-for-byte.
- Throughput: baseline `37.53 t/s`, runahead `34.37 t/s`.
- Timing: MTP start was about `0.05-0.20 ms`, target decode about
  `27-31 ms`, and MTP wait about `0.001-0.015 ms`.

Three-prompt `-n 64` matrix:

Command shape:

```sh
DS4_MTP_RUNAHEAD_ENABLE=1 DS4_MTP_GOVERNOR_DISABLE=1 DS4_MTP_TIMING=1 \
./ds4 --metal -m "$MODEL" --mtp "$MTP" --mtp-draft 2 \
  --ctx 1024 --nothink -sys "" --temp 0 -n 64 \
  --prompt-file "$PROMPT"
```

For the baseline variant, omit `--mtp "$MTP" --mtp-draft 2` and the env vars.

| Prompt | Baseline | Runahead1 | Output | Runahead timing |
|---|---:|---:|---|---|
| Counting | 36.97 t/s | 34.47 t/s | Match | target avg `28.95 ms`, wait max `0.005 ms` |
| Explanation | 36.87 t/s | 34.20 t/s | Match | target avg `28.89 ms`, wait max `0.004 ms` |
| Code completion | 37.01 t/s | 34.07 t/s | Match | target avg `28.99 ms`, wait max `0.003 ms` |

Interpretation:

- The separate MTP GPU lane is functionally exact for the tested deterministic
  prompts.
- The MTP wait is effectively hidden behind target decode, so the scratch/queue
  architecture is viable.
- The current mode is slower because it gives up the existing multi-token accept
  path and emits one target token per loop. This is an architecture prerequisite,
  not a promotable throughput result.

Decision:

- Keep the async queue/scratch architecture.
- Do not promote depth-1 runahead as the shipping speculative path.
- The next iteration should combine hidden MTP runahead with multi-token accept
  and deeper MTP token chaining, then re-run the same three-prompt matrix before
  considering a larger confirmation run.

## Async Runahead Chain Prototype

Run date: 2026-05-13

Implementation:

- Extended the async MTP lane from depth-1 to a queued runahead frontier.
- Chained MTP tokens on the GPU: each MTP top-1 token is written to the async
  `comp_selected` buffer, then a tensor view of that token feeds the next MTP
  embedding step in the same command buffer.
- Rejoined multi-token acceptance without target over-verification. The target
  path decodes exactly the next candidate token, compares the resulting logits
  to the next queued MTP token, and stops immediately at the first mismatch.
- Added per-cycle timing and depth counters in the `runahead-chain` timing log:
  committed depth, queued depth, appended depth, discarded depth, target time,
  and stall time waiting for the MTP queue.

Corrections after review:

- The initial GPU token chaining was incomplete: chained rows fed the device
  token to embedding but still passed scalar `-1` to token-aware router
  selection. This has been fixed by passing a token tensor through the decode
  layer when one is available.
- The initial MTP-off comparison was invalid: `DS4_MTP_SPEC_DISABLE=1` bypassed
  the speculative wrapper but did not stop regular MTP drafting inside
  `ds4_session_eval_internal`. This has been fixed.
- The async chain remains top-1 at the root. It does not use the existing
  root-top-k recovery logic from the non-runahead speculative path.
- The async extension path still clones the full MTP raw-cache frontier before
  each extension. That is a known performance defect, not a property of the
  intended algorithm.
- The measured slowdown below is therefore retained only as evidence about the
  partial prototype, not as a rejection of the intended algorithm.

Smoke result:

- Command: baseline vs `DS4_MTP_RUNAHEAD_ENABLE=1 --mtp "$MTP" --mtp-draft 4`,
  `-p "Count from 1 to 20." -n 48 --temp 0 --seed 1 --nothink`.
- Output: stdout matched byte-for-byte.
- Throughput: baseline `36.94 t/s`, runahead-chain `33.20 t/s`.
- Timing: committed depth reached 5 tokens/call, MTP stall stayed below
  `0.015 ms`, and target decode remained the dominant cost.

Three-prompt `-n 64` matrix:

Command shape:

```sh
DS4_MTP_RUNAHEAD_ENABLE=1 DS4_MTP_GOVERNOR_DISABLE=1 DS4_MTP_TIMING=1 \
./ds4 --metal -m "$MODEL" --mtp "$MTP" --mtp-draft 4 \
  --ctx 1024 --nothink -sys "" --temp 0 -n 64 \
  --prompt-file "$PROMPT"
```

For the baseline variant, omit `--mtp "$MTP" --mtp-draft 4` and the env vars.

| Prompt | Baseline | Runahead-chain | Output | Chain counters |
|---|---:|---:|---|---|
| Counting | 36.90 t/s | 33.18 t/s | Match | 16 cycles, 48 committed, avg commit `3.00`, discarded `47`, max stall `0.012 ms` |
| Explanation | 37.12 t/s | 33.03 t/s | Match | 16 cycles, 37 committed, avg commit `2.31`, discarded `48`, max stall `0.010 ms` |
| Code completion | 36.85 t/s | 33.09 t/s | Match | 17 cycles, 45 committed, avg commit `2.65`, discarded `47`, max stall `0.015 ms` |

Interpretation:

- The partial implementation satisfied one proposed constraint: it did not run
  target verification for `k+1` after a mismatch at `k`.
- The MTP queue rarely stalled the CPU-visible wait point, but the target lane
  still slowed because the second GPU queue shares the same device.
- The exact target lane still has to decode every accepted token, and that cost
  dominates. The runahead chain therefore lands near `33 t/s`, below the
  `~37 t/s` baseline and below the earlier default-MTP win on counting.
- This does not prove the intended algorithm is bad. It proves this prototype
  was not yet the intended algorithm because it lacked a path that commits extra
  tokens without a full serial target decode for each accepted token.
- No larger bounded confirmation run was started because the small matrix was
  already invalid as an architecture-level rejection.

Decision:

- Do not use this matrix to reject async runahead as an architecture.
- Keep the implementation as an env-gated diagnostic/reference path while
  fixing the remaining semantic and measurement gaps.
- The next production plan must either add a target-exact multi-row verifier
  that avoids one full target decode per accepted token, or explicitly measure a
  narrower algorithm that only claims MTP frontier preparation benefits.
- Before any new matrix is used for a promote/drop decision, remove the
  per-extension full-frontier clone or prove it is not on the hot path being
  measured.

## Corrected Next Plan

Status: superseded by the 2026-05-14 corrected parallel-pipeline validation
below. The plan was:

1. Preserve the async MTP queue/scratch and GPU token-chaining code behind
   `DS4_MTP_RUNAHEAD_ENABLE=1`, with token tensors feeding every token-dependent
   MTP decode stage.
2. Keep `DS4_MTP_SPEC_DISABLE=1` as a true all-MTP-off baseline switch.
3. Keep the real DS4 `MTLBuffer` identity audit available as a guardrail, but
   do not treat mutable target/MTP aliasing as the current blocker: the latest
   audit found `shared=0` for the target verifier/decode buffers that follow
   MTP work. The only nonzero shared case is the synchronous MTP draft on the
   target queue reusing `mtp_async.*` scratch after the async lane completes.
4. Treat the current Metal System Trace result as a scheduling result, not a
   hardware-counter result: target command buffers are submitted while MTP is
   still in flight, but real GPU compute execution stays back-to-back. The CLI
   counter attempt did not produce usable occupancy or memory-bandwidth rows.
5. Consider `MTLResourceHazardTrackingModeUntracked` only for a narrow,
   explicitly-owned experiment. The current audit does not justify broad
   untracked conversion as a false-aliasing fix.
6. Prototype a combined MTP/target encoder or resource-untracked experiment
   only if a future trace or narrow direct experiment shows material overlap
   headroom. Do not convert the full graph encoder based on the synthetic
   microbench alone.
7. Use the exact runahead verifier as the reference contract for positions
   `1..N`: it matches serial target logits/top tokens, but its current
   decode-kernel composition is too slow to promote.
8. Either wire root-top-k recovery into async runahead or keep the async path
   explicitly labeled as top-1-only.
9. Remove the full MTP raw-cache clone from every async extension by keeping the
   async frontier resident across cycles or copying only the delta rows/state.
10. Drop the current decode-composed fixed-shape verifier as a promotable
    target verifier. It should remain env-gated as the reference oracle for any
    future fused verifier primitive.
11. If this line of work continues, the next implementation plan is not another
    scheduler or tree change. It is a genuinely fused target verifier primitive
    for `N=2` first, validated against `metal_graph_verify_decode_exact()` and
    required to beat the serial baseline before any `N>2` or `4x2x2` recovery
    work resumes.
12. Ask for an independent review before merging or promoting any of the
    env-gated verifier experiments.

## Corrected Parallel Pipeline Validation

Run date: 2026-05-14

Implementation:

- Fixed the exact runahead schedule so async MTP suffix generation is no longer
  joined before target verification.
- The exact path now starts async MTP extension for future tokens, snapshots the
  target frontier, verifies only the currently available frontier, and waits for
  / promotes the async suffix only after the verified frontier fully survives.
- `verify_n == 1` is now allowed. This covers the steady-state pipeline where
  the target decodes the current target-selected token while MTP drafts the next
  token.
- Added timing split for `start_async`, `snapshot`, `verify`, `wait_async`,
  `promote`, `validate`, and `replay`.
- Added `DS4_METAL_BUFFER_AUDIT_MODEL_VIEWS=1` to include read-only model-view
  `MTLBuffer`s in the buffer audit.
- Added `DS4_METAL_GRAPH_CONCURRENT_ENCODERS=1` as a deliberately broad
  real-graph `MTLDispatchTypeConcurrent` probe.

Validation and build:

| Check | Host | Result |
|---|---|---|
| `git diff --check` | local | Pass |
| `make ds4_test ds4` | local | Pass |
| `./ds4_test --metal-kernels` | local M5 Max | Pass |
| `make ds4_test ds4 && ./ds4_test --metal-kernels` | `studio.local` M3 Ultra | Pass |
| Count prompt, `--mtp-draft 2`, `DS4_MTP_RUNAHEAD_VERIFY_N_EXACT=1 DS4_MTP_VERIFY_DECODE_N_VALIDATE=1`, `-n 16 --seed 1` | `studio.local` | stdout matched serial; validation rows reported `max_delta=0`, `mismatches=0`. |

Key corrected-pipeline observation:

- The serialization bug found by review was real: the previous exact path
  started async MTP and immediately joined it before target verification.
- After the fix, `wait_async` is essentially hidden (`~0.001-0.006 ms` per
  cycle in the count runs).
- This proves the CPU-side pipeline order is now correct, but it does not prove
  useful heavy GPU overlap. In timeline captures, MTP finishes during the small
  snapshot/embed/setup commands or hands off back-to-back to the heavy target
  verifier.

Three-prompt `-n 64` matrix, `--mtp-draft 2`:

| Prompt | Pure serial target | MTP-loaded disabled | Corrected exact runahead d2 | Output |
|---|---:|---:|---:|---|
| Counting | `37.06 t/s` | `36.39 t/s` | `33.84 t/s` | Match |
| Explanation | `37.06 t/s` | `36.31 t/s` | `33.68 t/s` | Match |
| Code | `36.96 t/s` | `36.37 t/s` | `33.64 t/s` | Match |

Draft-depth sweep, count prompt, corrected exact runahead, `-n 64`:

| `--mtp-draft` | Throughput | Output | Notes |
|---:|---:|---|---|
| 2 | `33.82 t/s` | Match | Steady state `verified=1`, `appended=1`, wait hidden. |
| 3 | `33.57 t/s` | Match | Often `verified=2`, but no win over d2. |
| 4 | `19.98 t/s` | Match | Discarded suffix work dominates. |
| 5 | `17.56 t/s` | Match | Worse. |
| 6 | `15.64 t/s` | Match | Worse. |
| 7 | `14.01 t/s` | Match | Worse. |
| 8 | `12.83 t/s` | Match | Worse. |

Draft-3 confirmation on the standard prompts:

| Prompt | Corrected exact runahead d3 | Output | Notes |
|---|---:|---|---|
| Counting | `33.54 t/s` | Match | Comparable to d2, still below baseline. |
| Explanation | `25.78 t/s` | Match | Discards return; not viable. |
| Code | `29.92 t/s` | Match | Discards return; not viable. |

Batch-output-head diagnostic:

- Count prompt only, corrected exact runahead:
  - `--mtp-draft 2`: `33.52 t/s`, stdout match.
  - `--mtp-draft 3`: `33.80 t/s`, stdout match.
- This does not change the architecture decision; it remains below both serial
  baselines.

GPU timeline evidence:

- Corrected d2 count timeline:
  - `ds4-mtp-6` GPU interval
    `[1007153.629689292, 1007153.631385167]`.
  - Heavy target verifier `ds4-target-9` started at
    `1007153.632015875`.
  - The async MTP wait was hidden, but the heavy verifier started after MTP had
    already finished.
- Corrected d4 count timeline:
  - `ds4-mtp-6` ended at `1007178.453029792`; heavy
    `ds4-target-9` started at `1007178.453030375`.
  - Later, `ds4-mtp-28` ended at `1007178.652091375`; heavy
    `ds4-target-31` started at `1007178.652087625`, a timestamp-noise overlap
    of about `0.004 ms`.
- Interpretation: the corrected schedule removes the CPU early-join bug, but
  the real DS4 heavy target/MTP work still does not show useful overlap. The
  useful hidden portion is small MTP work during setup, not a target-verifier
  throughput win.

Resource audit with model views:

- With `DS4_METAL_BUFFER_AUDIT_MODEL_VIEWS=1`, heavy target verifier/output
  buffers report `shared=1` against the MTP lane.
- `DS4_METAL_BUFFER_AUDIT_DETAILS=1` identifies the shared buffer as
  `ds4_model_view_0`, a read-only model mmap view:
  `len=86714793984 storage=0 hazard=2`.
- Mutable target/MTP scratch separation remains clean for the large verifier
  buffers; the shared model-view evidence points to common weight bandwidth,
  not mutable state aliasing.

Real-graph `MTLDispatchTypeConcurrent` probe:

```sh
DS4_METAL_GRAPH_CONCURRENT_ENCODERS=1 \
DS4_MTP_RUNAHEAD_ENABLE=1 \
DS4_MTP_GOVERNOR_DISABLE=1 \
DS4_MTP_TIMING=1 \
DS4_MTP_RUNAHEAD_VERIFY_N_EXACT=1 \
DS4_MTP_VERIFY_DECODE_N_VALIDATE=1 \
./ds4 --metal -m "$MODEL" --mtp "$MTP" --mtp-draft 2 \
  --ctx 1024 --nothink -sys "" --temp 0 --seed 1 -n 16 \
  --prompt-file /tmp/ds4-mtp-matrix/count.txt
```

Result:

- Failed validation / generation on `studio.local`.
- First two rows showed faster per-row verify (`~16.4 ms` instead of
  `~28-31 ms`), but the third validation row reported
  `exact_top=0 gpu_top=65536 serial_top=0`, then the verifier failed.
- Decision: broad concurrent graph encoders are not correctness-safe for this
  graph. Any future concurrent-encoder work would need a narrow, dependency-
  annotated kernel/pipeline rewrite rather than flipping the graph encoder.

Decision:

- Drop the corrected exact runahead pipeline as a promotable algorithm.
- It is now correctly scheduled and correctness-valid, but it cannot beat the
  serial target baseline or even the MTP-loaded-disabled baseline on the
  standard prompt matrix.
- Deeper draft depths make the result worse because discarded suffix work
  dominates.
- The Metal scheduling question is answered for the current DS4 graph shape:
  useful draft/target heavy-compute overlap was not observed even after fixing
  the early join; broad `MTLDispatchTypeConcurrent` is unsafe.
- This is not a proof that parallel MTP/target scheduling is impossible. It is
  a proof that the current implicit-order graph cannot safely be made parallel
  with a broad encoder switch. The next architecture track is a greenfield,
  dependency-aware Metal graph scheduler where kernel dependencies, scratch
  ownership, model views, and concurrency regions are explicit.

## Greenfield Metal Scheduler 2

Scope:

- Treat this as greenfield scheduler work, not a compatibility-preserving
  refactor of the existing graph executor.
- The first milestone was a scheduler contract: every compute node declares
  resource reads, writes, lane, and explicit dependencies; the scheduler emits
  concurrent encoder waves only for ready nodes with no read/write or
  write/write conflict.
- The first algorithm milestone was not throughput. It was a GPU timestamp proof
  that target verification and MTP suffix drafting can overlap usefully under
  the explicit scheduler contract.

Implemented:

- Added typed sched2 resource declarations for tensor slices, read-only model
  views, scratch arenas, command outputs, and host readbacks.
- Added sched2 node lanes: `target` and `mtp`.
- Replaced the bitmask-only probe with typed reads/writes, explicit deps,
  conflict splitting, and cycle/deadlock rejection.
- Kept the synthetic spin kernels only in `--metal-sched2` as the unit harness.
- Routed the real speculative path through `DS4_MTP_SCHED2=1`.
- Removed the unsafe broad `DS4_METAL_GRAPH_CONCURRENT_ENCODERS` path from live
  code; the prior broad switch failed validation and is preserved only as
  negative evidence.
- Added command-span accumulation for target and MTP command buffers so sched2
  timing reports GPU start/end overlap rather than CPU wait hiding.
- Forced sched2 verification to `N=2` first: exactly one frontier transition is
  verified while the MTP lane drafts the next suffix.

Local validation:

```sh
make ds4_test ds4
./ds4_test --metal-sched2
./ds4_test --metal-kernels
git diff --check
```

Result:

- `make ds4_test ds4`: pass.
- `./ds4_test --metal-sched2`: pass on Apple M5 Max.
  - `nodes=4`
  - `waves=2`
  - `concurrent_waves=1`
  - wave 0: `independent-a,independent-b,chain-producer`
  - wave 1: `chain-consumer`
- `./ds4_test --metal-sched2`: pass on `studio.local` Apple M3 Ultra with the
  same two-wave plan.
- `./ds4_test --metal-kernels`: pass.
- `git diff --check`: pass.
- Local M5 smoke, `-n 16`, stdout matched the serial baseline. Active cycles
  showed target/MTP GPU overlap around `5.2-12.8 ms`, but local throughput was
  below baseline and the local host is not the decision machine.

Studio validation and timing:

- `DS4_MTP_SCHED2_VALIDATE=1` count-prompt smoke reported `max_delta=0` and
  `mismatches=0` for accepted rows inspected.
- Three-prompt `-n 64` matrix matched serial stdout for all prompts.
- Average overlap on `studio.local` was about `1.51 ms`, with max overlap about
  `1.62 ms`.
- No prompt met the overlap gate of `>= 2 ms` and `>= 15%` of the shorter GPU
  span on useful cycles.

Decision:

- Do not promote sched2 as a throughput path.
- Do not run `N=3..8` for this schedule because `N=2` failed the overlap gate.
- Keep sched2 as a scheduler/instrumentation harness and negative architecture
  result. A future attempt should first change verifier cost or fuse verifier
  work; deeper suffix drafting on top of this separate target verifier is not
  expected to recover the gap.

## Experiment Entry Template

```md
### YYYY-MM-DD - short experiment name

- Branch:
- Files touched:
- Hypothesis:
- Command:
- Prompt set:
- Result:
- Validation:
- Decision:
```
