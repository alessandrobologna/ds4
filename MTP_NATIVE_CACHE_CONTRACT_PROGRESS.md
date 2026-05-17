# DS4 Native MTP Cache-Contract Progress

This document tracks the greenfield native-MTP cache-contract rewrite. It is separate from the earlier verifier-efficiency and sched2 notes so the current line of work stays focused on the MTPLX/Gemma-style hypothesis: native MTP quality depends first on a faithful cache/state contract, then on small-block verifier economics.

## Depth Selection Rule

Any change that materially affects per-row verifier economics invalidates the
current native depth choice. Examples include batch-KV, batched compressor
projection, top-id/frontier work, command-buffer layout changes, and small-M
verifier kernel rewrites.

After such a change, re-sweep `--mtp-draft 2`, `3`, `4`, and `5` on the same
three-prompt smoke matrix first. Use that run only to choose the next candidate
depth. Then run representative HumanEval/EvalPlus and GSM slices for the
winner before making any performance or promotion claim. A `-n 64` smoke can
say "this works" and "this depth should be evaluated next"; it cannot decide
promotion by itself.

## 2026-05-15 Initial Native Path

Implemented `DS4_MTP_NATIVE=1` as a new experimental path.

Key behavior:

- Native mode disables the old `ds4_session_eval()` post-token MTP pre-draft path. Prompt/user-token evaluation no longer leaves speculative MTP rows conditioned on unaccepted guesses.
- Native mode owns the generation cycle:
  - accepts the current serial target `first_token`;
  - processes accepted/drafted tokens through MTP with an explicit ledger;
  - verifies the drafted suffix with the exact target verifier;
  - commits accepted target prefix state from verifier-produced state, not serial target replay;
  - commits only the corresponding accepted MTP raw rows and hidden state.
- New flags:
  - `DS4_MTP_NATIVE=1`
  - `DS4_MTP_NATIVE_TIMING=1`
  - `DS4_MTP_NATIVE_VALIDATE=1`
  - `DS4_MTP_NATIVE_CACHE_MODE=owned|reset|target_prefix_read`
  - `DS4_MTP_NATIVE_VERIFY_OPT=exact|attn_fused|attn_fused_routed|smallm`
- `--mtp-draft K` is clamped to native depths `K=2..5`.

Cache modes:

- `owned`: persistent native MTP raw-cache history. This is the promotable candidate.
- `reset`: diagnostic A/B mode. The MTP raw frontier is reset at the start of every native cycle.
- `target_prefix_read`: explicitly rejected for the current tensor contract. The current DS4 MTP block owns its attention cache and has no external target-prefix KV input. The old raw target-KV graft copied target rows into the wrong cache and is preserved only as failed evidence, not as this mode.

Instrumentation:

- Native timing line:
  - cache mode
  - depth
  - drafted tokens
  - processed MTP rows
  - committed tokens
  - first suffix draft/top comparison
  - preview token
  - discarded work
  - target serial step time
  - MTP draft time
  - target verifier wall/GPU decode and head times
  - validation time
  - target commit time
  - MTP ledger commit time
  - MTP raw base/kept/discarded rows
  - validation max delta and mismatch count
- First timing use prints a tensor-contract audit with target/MTP tensor quantization types and the target-prefix-read rejection reason.
- Native timing includes `verify_opt`.
- The default native verifier is now `exact`. Earlier optimized defaults were
  moved behind explicit `DS4_MTP_NATIVE_VERIFY_OPT` values after HumanEval/0
  exposed a routed-MoE verifier regression.

Local harness:

- Added `./ds4_test --metal-mtp-cache-contract`.
- The harness currently validates pure ledger invariants:
  - owned vs reset raw-frontier start;
  - full/partial keep row calculation;
  - clamp behavior against raw window/capacity;
  - preview rows are never counted as committed unless the accepted prefix covers them.
- The target verifier uses block-prefix capture slots, so accepted suffix prefixes can be committed from verifier-produced compressed state. Raw cache rows are produced in-place by the verifier at their final token positions; rejected future rows remain invisible and are overwritten before they can be attended.

Validated locally:

```sh
make ds4_test ds4
make ds4-server
./ds4_test --metal-mtp-cache-contract
./ds4_test --metal-kernels
./ds4_test --metal-sched2
./ds4_test --metal-block-verifier
git diff --check
```

Current status:

- Code compiles, including `ds4`, `ds4_test`, and `ds4-server`.
- Ledger, Metal kernels, sched2, and block-verifier harnesses pass locally.
- Local M5 Max model-backed smoke passes exact stdout for `count`, `K=3`, `owned`, with validate mode reporting `mismatches=0` and `max_delta` around `1e-5`.
- Studio model-backed optimization, approximate-acceptance, and state-contract matrices are recorded below from the isolated `studio.local` worktree.

Local smoke artifacts:

- `/tmp/ds4-native-smoke-20260515-230117`: serial vs native `K=3`, `DS4_MTP_NATIVE_VALIDATE=1`, `-n 16`, stdout match.
- `/tmp/ds4-native-prod-smoke-20260515-230202`: serial vs native `K=3`, production timing, `-n 64`, stdout match.
- `/tmp/ds4-native-count-sweep-20260515-230237`: count prompt sweep, `-n 64`, owned/reset `K=2..5`.

Count prompt sweep:

| variant | t/s | stdout | avg accepted | avg discarded | avg target | avg draft | avg verify |
| --- | ---: | --- | ---: | ---: | ---: | ---: | ---: |
| serial target | 35.70 | oracle | - | - | - | - | - |
| MTP-loaded-disabled | 35.34 | match | - | - | - | - | - |
| current upstream-style MTP `K=3` | 39.66 | match | - | - | - | - | - |
| native owned `K=2` | 31.72 | match | 2.000 | 0.000 | 27.018 ms | 4.690 ms | 29.929 ms |
| native owned `K=3` | 34.24 | match | 2.909 | 0.091 | 25.368 ms | 6.663 ms | 51.631 ms |
| native owned `K=4` | 32.70 | match | 4.000 | 0.000 | 25.655 ms | 9.372 ms | 85.745 ms |
| native owned `K=5` | 28.16 | match | 4.000 | 0.938 | 25.047 ms | 10.812 ms | 104.847 ms |
| native reset `K=2` | 33.90 | match | 2.000 | 0.000 | 25.225 ms | 4.394 ms | 28.033 ms |
| native reset `K=3` | 33.44 | match | 2.560 | 0.400 | 25.942 ms | 6.595 ms | 42.832 ms |
| native reset `K=4` | 34.28 | match | 4.000 | 0.000 | 25.288 ms | 8.765 ms | 81.243 ms |
| native reset `K=5` | 27.12 | match | 4.000 | 0.938 | 26.192 ms | 11.017 ms | 108.837 ms |

Initial interpretation:

- The cache-contract path is now correct enough to run and accept multiple tokens without serial replay.
- On the simple count prompt, persistent `owned` cache does not materially beat `reset`; in fact the best reset case is slightly faster. This weakens the MTPLX-style persistence hypothesis for this current DS4 MTP tensor contract, but it is only one prompt.
- The current upstream-style MTP path remains faster on this prompt. The native path's immediate bottleneck is verifier economics: `K=3` accepts almost three tokens per cycle, but still pays one serial target step plus about two target-token-equivalent verifier rows.

### 2026-05-16 First Native Performance Pass

Implemented the first native verifier optimization pass:

- Added `DS4_MTP_NATIVE_VERIFY_OPT`.
- Defaulted native `K=3` suffix verification to `attn_fused_routed`, because `K=3` verifies a 2-row suffix and can reuse the best exact decode2 verifier slice from the block-verifier track.
- Kept `exact` as an explicit control mode.
- Did not run local model/perf tests after this change; use the isolated `studio.local` worktree for the next matrix.

Expected measurement:

- Compare `DS4_MTP_NATIVE_VERIFY_OPT=exact` versus default `attn_fused_routed` for native owned/reset `K=3`.
- The metric to watch first is native timing `verify=` / `verifier_decode_gpu=`.
- Promotion still needs quality/TPS, not byte-exact output alone, for approximate candidates.

Studio artifact:

- `/tmp/ds4-native-opt-studio-20260516_010340`

Studio `K=3`, `owned`, `-n 64` matrix:

| Prompt | Serial | MTP-disabled | Current MTP | Native exact | Native optimized | Stdout notes |
| --- | ---: | ---: | ---: | ---: | ---: | --- |
| count | 37.21 | 36.38 | 37.54 | 31.75 | 30.81 | all match |
| explain | 37.25 | 36.82 | 36.33 | 28.99 | 32.05 | current MTP differs; native rows match |
| code | 37.18 | 36.49 | 36.86 | 30.94 | 34.33 | all match |

Native verifier timing:

| Prompt | Variant | cycles | avg accepted | avg discarded | avg target | avg draft | avg verify | avg verifier decode GPU |
| --- | --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| count | exact | 22 | 2.909 | 0.091 | 27.416 ms | 6.482 ms | 56.194 ms | 51.830 ms |
| count | optimized | 22 | 2.909 | 0.091 | 28.142 ms | 6.570 ms | 48.916 ms | 41.885 ms |
| explain | exact | 28 | 2.286 | 0.714 | 27.279 ms | 6.466 ms | 43.817 ms | 40.349 ms |
| explain | optimized | 28 | 2.286 | 0.714 | 27.198 ms | 6.334 ms | 36.544 ms | 32.912 ms |
| code | exact | 24 | 2.667 | 0.333 | 27.201 ms | 6.448 ms | 51.103 ms | 47.065 ms |
| code | optimized | 24 | 2.667 | 0.333 | 27.144 ms | 6.410 ms | 42.712 ms | 38.486 ms |

Validation smoke, optimized native `K=3`, `owned`, `-n 16`:

| Prompt | stdout | cycles | max_delta | mismatches | avg verify | avg validation |
| --- | --- | ---: | ---: | ---: | ---: | ---: |
| count | match | 6 | 0 | 0 | 40.554 ms | 44.885 ms |
| explain | match | 6 | 0 | 0 | 49.955 ms | 55.325 ms |
| code | match | 6 | 0 | 0 | 41.187 ms | 45.420 ms |

Overall code-inefficiency interpretation:

- The optimization is real at the verifier-stage level, cutting native `K=3` verifier wall time by about `7-8 ms` and decode-GPU time by about `7-10 ms` on all three prompts.
- End-to-end TPS improves substantially on explain/code versus the plain native verifier, but count regresses slightly due to non-verifier variance/overhead. The native exact path remains below serial and below the best current MTP row.
- This first performance pass confirms that the native path has optimization headroom, but the remaining gap is still the target-root serial step plus verifier economics. The next high-leverage work should either remove the target-root serial step from the native cycle, or move to quality-gated approximate native acceptance where stdout equality is diagnostic rather than the promotion criterion.

### 2026-05-16 Native Top-Only Production Verification

Implemented the next native production-path optimization:

- `DS4_MTP_NATIVE_VALIDATE=1` keeps the old full-logit verifier readback so row-level `max_delta` and top-id validation stay diagnostic.
- Production native verification now passes `top_only=1` to `metal_graph_verify_decode_exact()` and does not allocate/read back `suffix_n * vocab` verifier logits.
- Acceptance still uses exact target top IDs from the verifier. When a suffix prefix is committed, the runtime reads back only the final committed verifier row into `s->logits` for the next generation step.
- The final-row readback happens before restoring/committing the target frontier, while `spec_logits` still belongs to the verifier output. A first attempt that read this row after restore passed `count` but diverged on `explain`/`code`.
- Native timing lines now include `top_only=0|1` and `logits_read=... ms`.
- Added experimental `DS4_MTP_NATIVE_COMMIT_OPT=capture|optimistic_full`.
- `capture` is the existing exact transaction path: capture verifier-produced state for every suffix row, restore the frontier, then commit the accepted prefix from captured state.
- `optimistic_full` is a production-only exact prototype: skip per-row verifier state capture, keep the verifier-produced target state on full accept, and restore plus replay the accepted suffix row on partial accept. This targets the common `K=3` full-accept case where capture/commit copies may cost more than they save.
- `auto` starts conservatively with capture, switches to optimistic full-accept only after full accepts, and locks back to capture if a partial accept appears while already in capture mode.
- Timing lines now include `commit_opt`, `capture_rows`, `kept_verifier_state`, and `replay`.

Expected effect:

- Top-only should reduce host readback and memory traffic for native production cycles, especially at `K=3..5`.
- Optimistic full-accept should reduce exact verifier decode GPU time when capture copies are a meaningful part of the decode span.
- It does not change verifier math or accepted-token semantics.
- It is not expected to solve the larger target-root serial step by itself.

Studio artifact:

- `/tmp/ds4-native-toponly-fix-studio-20260516_011759`
- `/tmp/ds4-native-depth-sweep-studio-20260516_012249`

Correctness:

- Current-binary serial and native top-only stdout match for `count`, `explain`, and `code`.
- `DS4_MTP_NATIVE_VALIDATE=1` short runs match current serial stdout for all three prompts and report `max_delta=0`, `mismatches=0`.
- Note: the older `/tmp/ds4-native-opt-studio-20260516_010340` serial text is no longer a valid byte oracle after the later synced code; always compare against a serial run produced by the same binary.

Current Studio `K=2..5`, `owned`, top-only production sweep:

| Prompt | K | t/s | stdout | avg accepted | avg discarded | avg target | avg draft | avg verify | avg verifier decode GPU |
| --- | ---: | ---: | --- | ---: | ---: | ---: | ---: | ---: | ---: |
| count | 2 | 31.39 | match | 2.000 | 0.000 | 26.789 ms | 4.397 ms | 30.907 ms | 27.895 ms |
| count | 3 | 35.16 | match | 3.000 | 0.000 | 27.325 ms | 6.536 ms | 49.715 ms | 44.825 ms |
| count | 4 | 31.76 | match | 4.000 | 0.000 | 26.620 ms | 9.253 ms | 88.388 ms | 81.703 ms |
| count | 5 | 25.93 | match | 4.000 | 0.938 | 26.832 ms | 10.744 ms | 115.073 ms | 106.581 ms |
| explain | 2 | 31.43 | match | 1.750 | 0.250 | 27.546 ms | 4.349 ms | 22.585 ms | 20.332 ms |
| explain | 3 | 31.38 | match | 2.423 | 0.577 | 27.309 ms | 6.506 ms | 42.107 ms | 37.980 ms |
| explain | 4 | 24.32 | match | 2.520 | 1.480 | 27.644 ms | 8.649 ms | 66.550 ms | 61.522 ms |
| explain | 5 | 19.03 | match | 2.625 | 2.292 | 27.340 ms | 10.622 ms | 99.534 ms | 92.043 ms |
| code | 2 | 31.34 | match | 1.882 | 0.118 | 27.666 ms | 4.364 ms | 26.513 ms | 23.791 ms |
| code | 3 | 33.77 | match | 2.739 | 0.261 | 27.464 ms | 6.566 ms | 45.659 ms | 41.214 ms |
| code | 4 | 27.46 | match | 3.316 | 0.684 | 27.241 ms | 8.723 ms | 83.680 ms | 77.334 ms |
| code | 5 | 23.72 | match | 3.368 | 1.579 | 26.811 ms | 10.650 ms | 103.012 ms | 95.434 ms |

Current interpretation:

- Top-only production readback is correct, but full-vocab host readback was not the bottleneck; `logits_read` is only about `0.02-0.03 ms` per native cycle.
- `K=3` remains the best exact native depth for count/code. `K=2` is competitive on explain because acceptance falls enough that the cheaper verifier wins.
- `K=4/5` should be dropped for the exact native verifier unless the multi-row target verifier is rewritten; their verifier spans dominate the cycle.
- The remaining exact-native bottleneck is target economics: every native cycle pays one serial target root step plus a target verifier span. The next real optimization must cut verifier decode time or change the state contract, not merely reduce readback.

### 2026-05-16 Exact Commit Optimization Result

Studio artifact:

- `/tmp/ds4-native-commit-auto-lock-studio-20260516_013742`

Same-binary baseline matrix, `K=3`, `owned`, `attn_fused_routed`, `-n 64`:

| Prompt | Serial | MTP-disabled | Current MTP | Native capture | Native optimistic_full | Native auto |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| count | 36.89 | 36.22 | 35.96 | 35.04 | 36.51 | 36.52 |
| explain | 36.80 | 36.60 | 35.30 | 31.35 | 29.70 | 31.33 |
| code | 36.75 | 36.25 | 36.06 | 33.95 | 34.10 | 33.85 |

Correctness:

- Serial, MTP-disabled, native capture, native optimistic_full, and native auto matched stdout on all three prompts.
- Current MTP differed from current serial on `explain` and `code`, consistent with the earlier decision that quality-gated modes need task/eval judgment rather than byte equality.

Exact-path result:

- `optimistic_full` proves that per-row verifier capture has real cost: on `count`, verifier decode GPU dropped from `45.103 ms` to `42.939 ms`, commit dropped from `0.394 ms` to `0.016 ms`, and throughput improved from `35.04` to `36.51 t/s`.
- The same optimization is not generally promotable because partial accepts pay replay. On `explain`, optimistic replay averaged `7.365 ms/cycle` and throughput fell to `29.70 t/s`.
- Conservative `auto` avoids replay on `explain` and keeps the count gain, but it still does not beat serial or MTP-disabled on the full matrix.
- Direction 1 status: useful exact-path micro-optimization, not enough for promotion. Keep `DS4_MTP_NATIVE_COMMIT_OPT=optimistic_full|auto` as evidence/prototype while moving to quality-gated approximate acceptance and first-depth contract work.

### 2026-05-16 Quality-Gated Top-K Acceptance Result

Implemented `DS4_MTP_NATIVE_APPROX_TOPK=N` for the quality-gated approximate native track.

Implementation notes:

- `N=1` preserves exact temp-0 top-id acceptance.
- For `N>1`, the first suffix token can be accepted if it is in the current target top-N from `s->logits`; later suffix rows can be accepted if the draft token is in that verifier row's exact target top-N.
- The verifier now requests `top_k` target IDs instead of only top-1 when this mode is enabled.
- Timing lines include `approx_topk`, `approx_rows`, and `first_suffix_rank`.
- This is explicitly non-exact output unless the generated stdout happens to match; promotion requires quality evidence, not byte equality.

Current-binary Studio artifact:

- `/tmp/ds4-native-approx-topk-current-studio-20260516_015336`
- Earlier quality artifact: `/tmp/ds4-native-approx-quality-studio-20260516_014320`

Three-prompt `K=3`, `owned`, `attn_fused_routed`, `auto` matrix:

| Prompt | Mode | generation t/s | stdout | avg accepted | verifier decode GPU | approx rows |
| --- | --- | ---: | --- | ---: | ---: | ---: |
| count | serial | 36.83 | oracle | - | - | - |
| count | current MTP | 36.35 | match | - | - | - |
| count | native top-k1 | 36.44 | match | 3.000 | 43.053 ms | 0 |
| count | native top-k2 | 36.54 | match | 3.000 | 42.829 ms | 0 |
| count | native top-k4 | 36.27 | match | 3.000 | 43.287 ms | 0 |
| count | native top-k8 | 36.33 | match | 3.000 | 43.288 ms | 0 |
| explain | serial | 36.78 | oracle | - | - | - |
| explain | current MTP | 35.18 | diff | - | - | - |
| explain | native top-k1 | 31.34 | match | 2.423 | 38.221 ms | 0 |
| explain | native top-k2 | 31.57 | diff | 2.520 | 39.368 ms | 7 |
| explain | native top-k4 | 34.40 | diff | 2.667 | 37.452 ms | 11 |
| explain | native top-k8 | 35.69 | diff | 2.667 | 35.865 ms | 10 |
| code | serial | 37.70 | oracle | - | - | - |
| code | current MTP | 35.85 | diff | - | - | - |
| code | native top-k1 | 33.94 | match | 2.739 | 40.268 ms | 0 |
| code | native top-k2 | 35.78 | diff | 2.864 | 40.382 ms | 1 |
| code | native top-k4 | 33.64 | diff | 2.909 | 43.766 ms | 3 |
| code | native top-k8 | 33.68 | diff | 2.909 | 43.608 ms | 3 |

Five-task EvalPlus quality slice:

| Mode | syntax | base pass@1 | plus pass@1 | agg t/s | mean t/s | median t/s |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| serial | 5/5 | 1.000 | 1.000 | 24.92 | 24.53 | 23.41 |
| current MTP | 5/5 | 1.000 | 1.000 | 26.37 | 24.37 | 23.75 |
| native top-k2 | 5/5 | 1.000 | 1.000 | 24.69 | 24.24 | 23.08 |
| native top-k4 | 3/5 | 0.600 | 0.600 | 26.35 | 22.77 | 22.84 |
| native top-k8 | 3/5 | 0.400 | 0.400 | 28.70 | 23.32 | 22.92 |

Decision:

- `top-k2` is the only approximate row that passed the small quality gate, but it does not beat serial or current MTP on the three-prompt matrix or the EvalPlus slice.
- `top-k4` and `top-k8` can raise acceptance and sometimes improve prompt throughput, but both fail the 5-task quality slice and are not promotable.
- Direction 2 status: drop as a promotable architecture in this form. Keep the `DS4_MTP_NATIVE_APPROX_TOPK` hook as a research probe only.

### 2026-05-16 First-Depth Pretarget State-Contract Probe

Implemented `DS4_MTP_NATIVE_PRETARGET_DRAFT=1` to test whether the first-depth native MTP step can be driven from persistent MTP hidden state before the target root step finishes.

Implementation notes:

- The probe only runs in `owned` cache mode, production mode, `K<=3`, when the previous native preview token matches the current exact target frontier token.
- It drafts the suffix from persistent MTP state before running `ds4_session_eval_internal()` for the current target token.
- Exact acceptance is still guarded by target top ids. If the pretarget suffix's first token does not match the target top id after the root target step, the speculative suffix is discarded and only the root token is committed.
- Timing lines now include `pretarget_requested`, `pretarget_possible`, and `pretarget`.

Checks:

```sh
make ds4_test ds4
./ds4_test --metal-mtp-cache-contract
git diff --check
```

Studio artifact:

- `/tmp/ds4-native-pretarget-studio-20260516_015027`

Current-binary Studio `K=3`, `owned`, `attn_fused_routed`, `auto` matrix:

| Prompt | Mode | generation t/s | stdout | cycles | avg pretarget | avg accepted | verifier decode GPU | avg draft |
| --- | --- | ---: | --- | ---: | ---: | ---: | ---: | ---: |
| count | serial | 36.74 | oracle | - | - | - | - | - |
| count | MTP-disabled | 36.35 | match | - | - | - | - | - |
| count | current MTP | 36.08 | match | - | - | - | - | - |
| count | native normal | 36.25 | match | 21 | 0.000 | 3.000 | 43.240 ms | 6.567 ms |
| count | native pretarget | 34.49 | match | 31 | 0.484 | 2.032 | 21.383 ms | 6.592 ms |
| explain | serial | 36.61 | oracle | - | - | - | - | - |
| explain | MTP-disabled | 36.16 | match | - | - | - | - | - |
| explain | current MTP | 35.39 | diff | - | - | - | - | - |
| explain | native normal | 31.29 | match | 26 | 0.000 | 2.423 | 38.013 ms | 6.565 ms |
| explain | native pretarget | 29.86 | match | 29 | 0.207 | 2.172 | 33.917 ms | 6.581 ms |
| code | serial | 36.84 | oracle | - | - | - | - | - |
| code | MTP-disabled | 36.14 | match | - | - | - | - | - |
| code | current MTP | 35.46 | diff | - | - | - | - | - |
| code | native normal | 34.11 | match | 23 | 0.000 | 2.739 | 40.022 ms | 6.553 ms |
| code | native pretarget | 32.07 | match | 27 | 0.259 | 2.333 | 33.477 ms | 6.525 ms |

Decision:

- The probe proves the path can fire and remain exact, but when it fires it often predicts the first suffix token from MTP state differently from the target-root hidden-state contract.
- The apparent verifier-GPU reduction is not useful: it comes from rejecting the first suffix token and skipping deeper verification, which increases cycle count and reduces average accepted depth.
- Direction 3 status: drop this state-contract variant. For this DS4 MTP tensor contract, first-depth drafting still needs the target-root hidden state if we want the high-acceptance native behavior.

### 2026-05-16 Three-Direction Summary

Promote/drop status:

- Direction 1, exact K=3 verifier/commit optimization: drop as a standalone promotable path. It is correct and useful as a micro-optimization, but does not beat serial/current baselines across the matrix.
- Direction 2, quality-gated approximate top-k acceptance: drop. `top-k2` preserves the small quality gate but does not improve TPS; wider top-k fails quality.
- Direction 3, first-depth pretarget MTP-state contract: drop. It remains exact but lowers acceptance enough to lose throughput.

Next implementation plan:

- Stop adding schedule-only or acceptance-only variants to the current native loop.
- The remaining credible path is a deeper verifier-kernel rewrite: reduce the actual target block verifier decode cost for small `M=2..4`, especially the MLP/MoE and small-matrix quantized matvec shapes, while preserving the exact target-root hidden-state contract.
- If that rewrite is pursued, use the current `DS4_MTP_NATIVE=1`, `K=3`, `owned`, `top-k1`, `commit_opt=auto` path as the correctness harness and compare against the same Studio matrix above.

## Prior Evidence Preserved

Earlier raw target-KV grafting was not promotable:

- Layer-0 graft gave some signal but did not beat the serial quality/TPS target.
- Layer-1 graft generally degraded quality.
- The experiment copied target raw rows into MTP-owned cache storage, which is not equivalent to Gemma-style target-prefix memory sharing.

Earlier K=16 approximate acceptance remains non-promotable:

- It can be fast, but quality was not sufficient under the exact-output target.
- The native track is deliberately starting with `K=2..5`, especially `K=3`, because native MTP is sequential and the verifier shapes of interest are small.

## Current Promotion Gates

The native cache-contract path remains a useful harness. Promotion is not decided by the short three-prompt `-n 64` matrix:

- Correctness gate: exact native rows pass temp-0 stdout/top-id checks; approximate rows are quality-gated instead of byte-exact.
- Cache gate: `owned` persistence did not materially improve acceptance over `reset` in the initial sweep, and `target_prefix_read` is unsupported by the current tensor contract.
- Performance gate: pending representative quality/TPS eval. The short matrix is retained as smoke evidence only: it proves the path runs and gives an early directional signal, not an architecture-level promote/drop decision.

## 2026-05-16 Small-M Verifier Kernel Track

Goal:

- Preserve the target-root hidden-state contract and exact temp-0 acceptance.
- Replace the hottest `M=2` verifier matvec shape used by native `K=3` with fixed-shape Metal kernels before moving to `M=3..4`.

Implementation:

- Added `DS4_MTP_NATIVE_VERIFY_OPT=smallm`.
- `smallm` keeps the existing exact `attn_fused_routed` verifier contract, including verifier-produced target state and no serial replay on accepted prefixes.
- Added fixed two-row Metal matvec kernels:
  - `kernel_mul_mv_q8_0_f32_rows2`
  - `kernel_mul_mv_f16_f32_rows2_4`
- During scoped native `K=3/M=2` verification, these kernels compute both verifier rows in one dispatch over the same Q8_0/F16 weight rows instead of launching the ordinary matvec separately per row.
- Full-logit validation remains available with `DS4_MTP_NATIVE_VALIDATE=1`.

Validation artifact:

- `/tmp/ds4-smallm-rows2q8f16-validate-20260516_024429`

Validation smoke, `K=3`, `owned`, `smallm`, `capture`, `DS4_MTP_NATIVE_VALIDATE=1`, `-n 16`:

| Prompt | stdout | cycles | max_delta | mismatches | avg verifier decode GPU | avg verifier head GPU |
| --- | --- | ---: | ---: | ---: | ---: | ---: |
| count | match | 5 | 0 | 0 | 42.171 ms | 0.941 ms |
| explain | match | 6 | 0 | 0 | 34.534 ms | 0.780 ms |
| code | match | 5 | 0 | 0 | 42.180 ms | 0.936 ms |

Studio matrix artifact:

- `/tmp/ds4-smallm-rows2q8f16-matrix-20260516_024505`

Current-binary Studio matrix, `K=3`, `owned`, `auto`, `-n 64`:

| Prompt | Mode | generation t/s | stdout | cycles | avg accepted | verifier decode GPU | verifier head GPU | draft | commit | replay |
| --- | --- | ---: | --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| count | serial | 36.80 | oracle | - | - | - | - | - | - | - |
| count | MTP-disabled | 36.52 | match | - | - | - | - | - | - | - |
| count | current MTP | 35.95 | match | - | - | - | - | - | - | - |
| count | native existing | 36.26 | match | 21 | 3.000 | 43.191 ms | 1.693 ms | 6.562 ms | 0.056 ms | 0.000 ms |
| count | native smallm | 37.78 | match | 21 | 3.000 | 40.649 ms | 0.942 ms | 6.551 ms | 0.055 ms | 0.000 ms |
| explain | serial | 36.77 | oracle | - | - | - | - | - | - | - |
| explain | MTP-disabled | 36.07 | match | - | - | - | - | - | - | - |
| explain | current MTP | 35.38 | diff | - | - | - | - | - | - | - |
| explain | native existing | 31.37 | match | 26 | 2.423 | 38.043 ms | 1.431 ms | 6.499 ms | 0.331 ms | 0.000 ms |
| explain | native smallm | 32.52 | match | 26 | 2.423 | 35.889 ms | 0.796 ms | 6.509 ms | 0.332 ms | 0.000 ms |
| code | serial | 36.94 | oracle | - | - | - | - | - | - | - |
| code | MTP-disabled | 36.23 | match | - | - | - | - | - | - | - |
| code | current MTP | 35.77 | diff | - | - | - | - | - | - | - |
| code | native existing | 33.86 | match | 23 | 2.739 | 40.534 ms | 1.548 ms | 6.518 ms | 1.389 ms | 1.177 ms |
| code | native smallm | 35.35 | match | 23 | 2.739 | 37.727 ms | 0.860 ms | 6.543 ms | 1.377 ms | 1.165 ms |

Interpretation:

- The fixed `M=2` rows2 matvec path is correct and useful. It cuts verifier decode GPU by about `2.2-2.8 ms` and verifier head GPU by about `0.6-0.75 ms` per native cycle.
- End-to-end native throughput improves on all prompts:
  - `count`: `36.26 -> 37.78 t/s`, now above serial and MTP-disabled for this full-accept prompt.
  - `explain`: `31.37 -> 32.52 t/s`, still below serial/MTP-disabled because average accepted depth is only `2.423`.
  - `code`: `33.86 -> 35.35 t/s`, still below serial/MTP-disabled because partial accepts still pay replay/commit overhead.
- Promotion status: unresolved. The kernel direction is validated, but this `-n 64` matrix is only a smoke/perf sanity slice. It is not representative enough to decide promotion or rejection.

Next plan:

- Extend the same fixed-shape idea to `M=3` and `M=4` verifier rows before dropping the kernel track.
- The likely next kernels are rows3/rows4 variants for Q8_0/F16 matvecs, or a rows4 kernel that handles `M<=4` with masked output rows, so native `K=4/5` can be re-tested without falling back to the generic verifier.
- Keep `K=3/M=2 smallm` as the correctness and benchmark baseline for future small-M work.

### 2026-05-16 M=3/M=4 Small-M Extension

Implementation update:

- Generalized the decode2 verifier prebatch graph into a bounded `M=2..4` small-M verifier path.
- `DS4_MTP_NATIVE_VERIFY_OPT=smallm` now enables the prebatch graph for native suffix sizes `2..4`, covering native `K=3`, `K=4`, and `K=5`.
- Added Q8_0 rows-specialized verifier matvec kernels:
  - `kernel_mul_mv_q8_0_f32_rows2`
  - `kernel_mul_mv_q8_0_f32_rows3`
  - `kernel_mul_mv_q8_0_f32_rows4`
- Added F16 rows-specialized kernels as an explicit probe, gated by `DS4_MTP_NATIVE_SMALLM_F16_ROWS=1`.
  - The default small-M path keeps F16 rows off because the first F16-active sweep did not improve the default K=3/K=4 path.
- Fixed the graph hazard check for batched shared FFN output so M=3/M=4 checks every verifier row, not only the first two positions.

Validation artifact:

- `/tmp/ds4-smallm-m234-q8default-validate-20260516_030043`

Validation smoke, `K=3..5`, `owned`, `smallm`, `capture`, `DS4_MTP_NATIVE_VALIDATE=1`, `-n 16`:

| Prompt | K | stdout | cycles | max_delta | mismatches | avg verifier decode GPU | avg verifier head GPU |
| --- | ---: | --- | ---: | ---: | ---: | ---: | ---: |
| count | 3 | match | 5 | 0 | 0 | 42.150 ms | 0.945 ms |
| count | 4 | match | 4 | 0 | 0 | 56.888 ms | 1.025 ms |
| count | 5 | match | 4 | 0 | 0 | 66.760 ms | 1.138 ms |
| explain | 3 | match | 6 | 0 | 0 | 34.950 ms | 0.791 ms |
| explain | 4 | match | 6 | 0 | 0 | 43.745 ms | 0.841 ms |
| explain | 5 | match | 6 | 0 | 0 | 53.704 ms | 0.943 ms |
| code | 3 | match | 5 | 0 | 0 | 42.836 ms | 0.949 ms |
| code | 4 | match | 4 | 0 | 0 | 56.266 ms | 1.022 ms |
| code | 5 | match | 5 | 0 | 0 | 50.614 ms | 0.897 ms |

Exact K=4/K=5 comparator artifact:

- `/tmp/ds4-smallm-m34-exact-compare-20260516_025819`

Generic exact verifier versus small-M default, `-n 64`:

| Prompt | Mode | generation t/s | stdout | avg accepted | verifier decode GPU | verifier head GPU |
| --- | --- | ---: | --- | ---: | ---: | ---: |
| count | exact K=4 | 31.84 | match | 4.000 | 81.630 ms | 1.094 ms |
| count | smallm K=4 | 40.26 | match | 4.000 | 56.657 ms | 1.025 ms |
| count | exact K=5 | 25.91 | match | 4.000 | 106.683 ms | 1.448 ms |
| count | smallm K=5 | 34.73 | match | 4.000 | 69.519 ms | 1.171 ms |
| explain | exact K=4 | 24.26 | match | 2.520 | 61.615 ms | 0.834 ms |
| explain | smallm K=4 | 30.14 | match | 2.520 | 42.239 ms | 0.780 ms |
| explain | exact K=5 | 19.00 | match | 2.625 | 92.454 ms | 1.264 ms |
| explain | smallm K=5 | 25.04 | match | 2.625 | 60.236 ms | 1.023 ms |
| code | exact K=4 | 27.47 | match | 3.316 | 77.391 ms | 1.037 ms |
| code | smallm K=4 | 34.74 | match | 3.316 | 53.359 ms | 0.973 ms |
| code | exact K=5 | 23.78 | match | 3.368 | 95.065 ms | 1.290 ms |
| code | smallm K=5 | 31.53 | match | 3.368 | 61.898 ms | 1.050 ms |

Final Studio matrix artifact:

- `/tmp/ds4-smallm-m234-q8default-matrix-20260516_030142`

Current-binary Studio matrix, `owned`, `smallm`, `-n 64`:

| Prompt | Mode | generation t/s | stdout | cycles | avg accepted | avg drafted | verifier decode GPU | verifier head GPU | draft | commit | replay |
| --- | --- | ---: | --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| count | serial | 37.04 | oracle | - | - | - | - | - | - | - | - |
| count | MTP-disabled | 36.09 | match | - | - | - | - | - | - | - | - |
| count | current MTP | 35.85 | match | - | - | - | - | - | - | - | - |
| count | native existing K=3 | 35.17 | match | 21 | 3.000 | 3.000 | 44.926 ms | 1.693 ms | 6.556 ms | 0.407 ms | 0.000 ms |
| count | smallm K=3 | 36.45 | match | 21 | 3.000 | 3.000 | 42.474 ms | 0.948 ms | 6.564 ms | 0.400 ms | 0.000 ms |
| count | smallm K=3 auto | 37.67 | match | 21 | 3.000 | 3.000 | 40.776 ms | 0.951 ms | 6.608 ms | 0.055 ms | 0.000 ms |
| count | smallm K=4 | 40.26 | match | 16 | 4.000 | 4.000 | 56.657 ms | 1.025 ms | 8.742 ms | 0.412 ms | 0.000 ms |
| count | smallm K=4 auto | 42.13 | match | 16 | 4.000 | 4.000 | 53.854 ms | 1.025 ms | 8.747 ms | 0.067 ms | 0.000 ms |
| count | smallm K=5 | 34.73 | match | 16 | 4.000 | 4.938 | 69.519 ms | 1.171 ms | 10.705 ms | 0.407 ms | 0.000 ms |
| explain | serial | 37.05 | oracle | - | - | - | - | - | - | - | - |
| explain | MTP-disabled | 36.18 | match | - | - | - | - | - | - | - | - |
| explain | current MTP | 34.59 | diff | - | - | - | - | - | - | - | - |
| explain | native existing K=3 | 31.51 | match | 26 | 2.423 | 3.000 | 37.843 ms | 1.434 ms | 6.493 ms | 0.332 ms | 0.000 ms |
| explain | smallm K=3 | 32.38 | match | 26 | 2.423 | 3.000 | 36.152 ms | 0.804 ms | 6.546 ms | 0.334 ms | 0.000 ms |
| explain | smallm K=3 auto | 32.50 | match | 26 | 2.423 | 3.000 | 36.050 ms | 0.805 ms | 6.492 ms | 0.337 ms | 0.000 ms |
| explain | smallm K=4 | 30.14 | match | 25 | 2.520 | 4.000 | 42.239 ms | 0.780 ms | 8.646 ms | 0.316 ms | 0.000 ms |
| explain | smallm K=4 auto | 30.12 | match | 25 | 2.520 | 4.000 | 42.212 ms | 0.780 ms | 8.614 ms | 0.296 ms | 0.000 ms |
| explain | smallm K=5 | 25.04 | match | 24 | 2.625 | 4.917 | 60.236 ms | 1.023 ms | 10.575 ms | 0.355 ms | 0.000 ms |
| code | serial | 36.68 | oracle | - | - | - | - | - | - | - | - |
| code | MTP-disabled | 36.43 | match | - | - | - | - | - | - | - | - |
| code | current MTP | 35.49 | diff | - | - | - | - | - | - | - | - |
| code | native existing K=3 | 33.89 | match | 23 | 2.739 | 3.000 | 41.153 ms | 1.549 ms | 6.568 ms | 0.356 ms | 0.000 ms |
| code | smallm K=3 | 35.30 | match | 23 | 2.739 | 3.000 | 38.704 ms | 0.865 ms | 6.467 ms | 0.361 ms | 0.000 ms |
| code | smallm K=3 auto | 35.07 | match | 23 | 2.739 | 3.000 | 38.125 ms | 0.868 ms | 6.559 ms | 1.400 ms | 1.186 ms |
| code | smallm K=4 | 34.74 | match | 19 | 3.316 | 4.000 | 53.359 ms | 0.973 ms | 8.647 ms | 0.372 ms | 0.000 ms |
| code | smallm K=4 auto | 34.77 | match | 19 | 3.316 | 4.000 | 52.929 ms | 0.973 ms | 8.761 ms | 0.399 ms | 0.000 ms |
| code | smallm K=5 | 31.53 | match | 19 | 3.368 | 4.947 | 61.898 ms | 1.050 ms | 10.696 ms | 0.366 ms | 0.000 ms |

Interpretation:

- The M=3/M=4 graph rewrite is correct and materially faster than the generic exact verifier. The K=4 exact-to-smallm improvement is large: `31.84 -> 40.26 t/s` on count, `24.26 -> 30.14 t/s` on explain, and `27.47 -> 34.74 t/s` on code.
- Deeper verifier rows are economically useful in this smoke run when enough drafted rows are accepted. `count` fully accepts K=4 and reaches `42.13 t/s` with `commit_opt=auto`, beating serial and MTP-disabled.
- `explain` and `code` are slower in this smoke run. Their average accepted depths are `2.42..3.32`, so K=4/K=5 pay more verifier and draft work than they save in this short sample. This is diagnostic, not a promotion/drop criterion.
- F16 rows-specialized kernels are retained only as a gated probe. The default small-M path uses the Q8 rows kernels plus graph prebatch, which is the faster measured default.

Status:

- Small-M verifier rewrite status: correct and validated by smoke tests; promotion/drop remains undecided pending representative eval.
- Keep the M=2..4 small-M path as valuable evidence and a reusable harness. It proves verifier-kernel economics can move by double-digit percentages.
- Required next step: run HumanEval/EvalPlus and longer generation workloads before using final language such as promotable, not promotable, drop, or final architecture decision.

### 2026-05-16 HumanEval+ 20-Task Quality/TPS Slice

Methodology correction:

- The earlier three tiny prompts (`count`, `explain`, `code`) with `-n 64` are smoke/perf sanity checks only.
- They are not representative enough to decide promotion, rejection, or final architecture status.
- Promotion/drop language must be based on representative quality/TPS evidence, not smoke runs.

Artifact:

- `/tmp/ds4-smallm-humaneval20-20260516_032220`

Command shape:

- `tools/mtp_quality_gate.sh --dataset humaneval --limit 20 --api chat --max-tokens 1024`
- Base model: `/Users/studio/.ds4/cache/gguf/DeepSeek-V4-Flash-IQ2XXS-w2Q2K-AProjQ8-SExpQ8-OutQ8-chat-v2.gguf`
- MTP model: `/Users/studio/.ds4/cache/gguf/DeepSeek-V4-Flash-MTP-Q4K-Q8_0-F32.gguf`

Modes:

| Mode | Extra settings |
| --- | --- |
| serial | no MTP |
| MTP-disabled | `DS4_MTP_SPEC_DISABLE=1 --mtp {MTP} --mtp-draft 3` |
| current MTP K=3 | `--mtp {MTP} --mtp-draft 3` |
| smallm K=3 auto | `DS4_MTP_NATIVE=1 DS4_MTP_NATIVE_VERIFY_OPT=smallm DS4_MTP_NATIVE_CACHE_MODE=owned DS4_MTP_NATIVE_COMMIT_OPT=auto --mtp {MTP} --mtp-draft 3` |
| smallm K=4 auto | same native settings with `--mtp-draft 4` |

Quality results:

| Mode | syntax | HumanEval+ base pass@1 | HumanEval+ plus pass@1 | failed tasks |
| --- | ---: | ---: | ---: | --- |
| serial | 20/20 | 0.900 | 0.900 | HumanEval/10, HumanEval/17 |
| MTP-disabled | 20/20 | 0.900 | 0.900 | HumanEval/10, HumanEval/17 |
| current MTP K=3 | 20/20 | 0.900 | 0.900 | HumanEval/10, HumanEval/17 |
| smallm K=3 auto | 20/20 | 0.900 | 0.900 | HumanEval/10, HumanEval/17 |
| smallm K=4 auto | 20/20 | 0.900 | 0.900 | HumanEval/10, HumanEval/17 |

TPS results:

| Mode | tasks | completion tokens | elapsed | aggregate completion TPS | mean TPS | median TPS |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| serial | 20 | 1610 | 59.54 s | 27.04 | 25.80 | 25.69 |
| MTP-disabled | 20 | 1610 | 59.67 s | 26.98 | 25.75 | 25.60 |
| current MTP K=3 | 20 | 1557 | 58.69 s | 26.53 | 25.05 | 25.13 |
| smallm K=3 auto | 20 | 1610 | 62.02 s | 25.96 | 24.79 | 24.59 |
| smallm K=4 auto | 20 | 1610 | 63.05 s | 25.54 | 24.49 | 24.95 |

Interpretation:

- This is the first proper EvalPlus quality/TPS slice for the small-M verifier track.
- Quality is unchanged across the tested modes on these 20 HumanEval+ tasks.
- On this coding slice, smallm K=3/K=4 do not improve throughput; serial and MTP-disabled are slightly faster.
- This does not by itself reject the architecture, but it replaces the earlier toy-prompt inference with real evidence: small-M is correct, quality-preserving on this slice, and not yet faster on HumanEval+ coding tasks.

Next eval requirements:

- Run a larger HumanEval+ or EvalPlus coding slice if we need tighter confidence.
- Add MBPP+ for a second coding distribution.
- Add longer non-code generations before making broad performance claims about chat/explanation workloads.

### 2026-05-16 GSM8K 20-Task Reasoning Slice

Artifact:

- `/tmp/ds4-smallm-gsm8k20-20260516_033442`

Dataset/source:

- First 20 tasks from the GSM8K test JSONL at `https://raw.githubusercontent.com/openai/grade-school-math/master/grade_school_math/data/test.jsonl`.
- The evaluator prompted each mode to solve the problem and end with `#### <answer>`, then compared the normalized numeric answer against the GSM8K gold answer.

Modes:

| Mode | Extra settings |
| --- | --- |
| serial | no MTP |
| MTP-disabled | `DS4_MTP_SPEC_DISABLE=1 --mtp {MTP} --mtp-draft 3` |
| current MTP K=3 | `--mtp {MTP} --mtp-draft 3` |
| smallm K=3 auto | `DS4_MTP_NATIVE=1 DS4_MTP_NATIVE_VERIFY_OPT=smallm DS4_MTP_NATIVE_CACHE_MODE=owned DS4_MTP_NATIVE_COMMIT_OPT=auto --mtp {MTP} --mtp-draft 3` |
| smallm K=4 auto | same native settings with `--mtp-draft 4` |

Results:

| Mode | tasks | accuracy | completion tokens | elapsed | aggregate completion TPS | mean TPS | median TPS | failed |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | --- |
| serial | 20 | 0.950 | 2231 | 74.11 s | 30.10 | 29.16 | 29.10 | 9 |
| MTP-disabled | 20 | 0.950 | 2231 | 74.16 s | 30.08 | 29.12 | 29.08 | 9 |
| current MTP K=3 | 20 | 0.950 | 2234 | 75.08 s | 29.76 | 28.78 | 28.87 | 9 |
| smallm K=3 auto | 20 | 0.950 | 2231 | 77.96 s | 28.62 | 27.84 | 27.67 | 9 |
| smallm K=4 auto | 20 | 0.950 | 2231 | 78.84 s | 28.30 | 27.61 | 27.47 | 9 |

Interpretation:

- GSM8K adds a reasoning-style distribution that is different from HumanEval+ coding and the earlier tiny smoke prompts.
- Quality was identical across all tested modes on this slice: every mode missed only task index `9`.
- Throughput still does not favor the current smallm native path on this reasoning slice. Serial and MTP-disabled were effectively tied around `30.1` aggregate completion TPS, current MTP was slightly slower, and smallm K=3/K=4 were slower.
- This reinforces the corrected framing: smallm is a correct verifier-kernel improvement, but representative quality/TPS evidence so far does not show an end-to-end win on coding or GSM8K reasoning slices.

Next eval requirements:

- Add MBPP+ as another coding distribution.
- Add longer open-ended chat/explanation generations, because neither HumanEval+ nor GSM8K represents free-form assistant workloads.

### 2026-05-16 Code-Inefficiency Pass

Scope:

- Focused on concrete waste in the native/smallm code path rather than new acceptance policy.
- Added three source-level changes and tested one shader experiment:
  - Native draft rows now write directly into `mtp_native_state_hc[slot]` instead of drafting into the ping-pong MTP HC tensors and copying into the transaction slot.
  - MTP draft top-1 selection now uses the dedicated `ds4_gpu_top1_tensor()` kernel when `want_top == 1` instead of the generic top-k indexer.
  - Native MTP ledger commit swaps the accepted transaction HC slot into `mtp_state_hc` instead of copying a full HC tensor.
  - Added gated `DS4_MTP_NATIVE_CHAIN_DRAFT=1` prototype to draft the native chain with one GPU-token chain instead of per-token CPU readbacks.
  - Followed up by removing the chain helper's full MTP raw-cache clone. The clone-free chain writes speculative MTP rows into the live native raw cache, keeps them invisible via `mtp_n_raw` until commit, rolls back by restoring the row count on failure/partial accept, and swaps only the final async HC into `mtp_state_hc` on full accept.

Reviewer findings:

- Independent reviewers identified the same hot waste: the native draft loop was CPU-driven one token at a time, with one command buffer and one top-id readback per draft row.
- They also identified the draft top-1 generic top-k call, HC commit copy, verifier checkpoint bookkeeping, and rows2/3/4 matvec reduction shape as possible code inefficiencies.

Artifacts:

- Direct-slot draft smoke: `/tmp/ds4-native-directslot-20260516_034826`
- Corrected native chain-draft smoke: `/tmp/ds4-native-chaindraft2-20260516_035606`
- Top-1 plus chain smoke: `/tmp/ds4-native-top1-chain-20260516_035836`
- Commit-swap smoke: `/tmp/ds4-native-swapcommit-20260516_040034`
- Reverted shader-reduction experiment: `/tmp/ds4-native-reduction-20260516_040242`
- Clone-free in-place chain smoke: `/tmp/ds4-native-inplace-chain-20260516_043344`

Selected Studio smoke results, `K=3`, `owned`, `smallm`, `auto`, `-n 64`:

| Prompt | Variant | stdout | generation t/s | avg draft | avg verifier decode GPU | avg mtp_commit | avg accepted |
| --- | --- | --- | ---: | ---: | ---: | ---: | ---: |
| count | normal after top1+swap | match | 36.60 | 5.834 ms | 43.105 ms | 0.000 ms | 3.000 |
| explain | normal after top1+swap | match | 30.92 | 5.740 ms | 31.078 ms | 0.000 ms | 2.207 |
| code | normal after top1+swap | match | 35.15 | 5.840 ms | 41.317 ms | 0.000 ms | 2.864 |
| count | chain prototype after top1+swap | match | 36.95 | 5.473 ms | 42.819 ms | 0.000 ms | 3.000 |
| explain | chain prototype after top1+swap | match | 31.59 | 5.438 ms | 31.095 ms | 1.180 ms | 2.286 |
| code | chain prototype after top1+swap | match | 35.20 | 5.493 ms | 41.295 ms | 0.282 ms | 2.864 |

Clone-free in-place chain follow-up, `owned`, `smallm`, `auto`, `-n 64`:

| Prompt | K | stdout | serial t/s | chain t/s | avg draft | avg verifier decode GPU | avg mtp_commit | avg accepted | avg discarded |
| --- | ---: | --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| count | 3 | match | 35.72 | 36.81 | 5.297 ms | 43.173 ms | 0.000 ms | 3.000 | 0.000 |
| count | 4 | match | 35.72 | 40.98 | 6.928 ms | 57.380 ms | 0.000 ms | 4.000 | 0.000 |
| explain | 3 | match | 35.57 | 31.92 | 5.233 ms | 30.896 ms | 1.095 ms | 2.286 | 0.714 |
| explain | 4 | match | 35.57 | 30.79 | 6.720 ms | 32.497 ms | 2.586 ms | 2.286 | 1.643 |
| code | 3 | match | 35.48 | 35.32 | 5.288 ms | 41.326 ms | 0.263 ms | 2.864 | 0.136 |
| code | 4 | match | 35.48 | 35.13 | 6.896 ms | 46.742 ms | 1.834 ms | 3.150 | 0.850 |

### 2026-05-16 Chain Prefix-HC Commit Follow-Up

Scope:

- Removed the remaining chain partial-accept rebuild. Native chain draft now writes each produced HC row directly into `mtp_native_state_hc[row]`, so commit can promote any accepted prefix by swapping `mtp_native_state_hc[accepted_tokens - 1]` into `mtp_state_hc`.
- The chain still writes speculative raw rows in place and rolls them back by restoring `mtp_n_raw` to the accepted row count. Unaccepted rows remain overwritten/ignored beyond the visible frontier.

Artifact:

- `/tmp/ds4-native-prefix-chain-20260516_095946`

Studio smoke, `owned`, `smallm`, `auto`, `DS4_MTP_NATIVE_CHAIN_DRAFT=1`, `-n 64`:

| Prompt | K | stdout | serial t/s | chain t/s | avg draft | avg verifier decode GPU | avg mtp_commit | avg accepted | avg discarded |
| --- | ---: | --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| count | 3 | match | 35.51 | 36.79 | 5.283 ms | 43.112 ms | 0.000 ms | 3.000 | 0.000 |
| count | 4 | match | 35.51 | 40.96 | 6.930 ms | 57.511 ms | 0.000 ms | 4.000 | 0.000 |
| count | 5 | match | 35.51 | 33.89 | 8.454 ms | 73.739 ms | 0.000 ms | 4.000 | 0.938 |
| explain | 3 | match | 35.74 | 31.33 | 5.161 ms | 31.006 ms | 0.000 ms | 2.207 | 0.759 |
| explain | 4 | match | 35.74 | 32.08 | 6.719 ms | 32.238 ms | 0.000 ms | 2.286 | 1.643 |
| explain | 5 | match | 35.74 | 26.25 | 8.231 ms | 45.306 ms | 0.000 ms | 2.286 | 2.571 |
| code | 3 | match | 35.52 | 35.48 | 5.255 ms | 41.268 ms | 0.000 ms | 2.864 | 0.136 |
| code | 4 | match | 35.52 | 35.82 | 6.885 ms | 46.753 ms | 0.000 ms | 3.150 | 0.850 |
| code | 5 | match | 35.52 | 33.68 | 8.365 ms | 60.124 ms | 0.000 ms | 3.500 | 1.389 |

Interpretation:

- The prefix-HC commit fixed the intended code inefficiency: `avg_mtp_commit` is now `0.000 ms` even for partial-accept prompts.
- K=4 remains the interesting smoke candidate: it preserves the strong perfect-accept count result and turns code slightly positive on this short smoke. Explain remains below serial because verifier span dominates at its lower acceptance.
- K=5 is not attractive in this shape: draft and verifier spans grow more than accepted-prefix length on all three smoke prompts.

Interpretation:

- The code-inefficiency pass found real waste and removed some of it. The counters moved in the expected places: direct-slot/top1 trimmed draft time, and swap commit removed `mtp_commit` copy time on the normal path.
- These are micro-optimizations, not a promotion result. The retained normal path remains roughly in the same throughput band on the short smoke prompts; representative eval slices remain the better evidence for promotion.
- The chain-draft prototype is useful evidence but not ready as the default. It can reduce draft time and remove commit cost on full accepts. After the clone-free and prefix-HC follow-ups, it no longer pays full raw-cache transaction overhead or partial-accept rebuild cost.
- The clone-free K=4 count smoke is a useful positive counterexample: when acceptance is perfect, the deeper chain can beat serial by a visible margin on the tiny count prompt. Explain/code show why this cannot be treated as promotion evidence: partial accepts erase the gain quickly.
- The rows2/3/4 shader final-reduction experiment was reverted: it passed correctness but worsened the measured verifier spans on the target shapes, so the original reduction stays in place.

Next code-efficiency targets:

- Re-run representative HumanEval+/GSM8K slices with the K=4 prefix-HC chain as a candidate; keep the short prompt matrix as smoke only.
- Remove duplicate checkpoint suffix push/reset bookkeeping in the verifier path if a source audit confirms no hidden consumer depends on transient `checkpoint.len`.
- Investigate batching consecutive verifier capture copies to avoid compute/blit/compute ping-pong in `ds4_gpu_tensor_copy()`.

### 2026-05-16 Prefix-Chain Representative Bench

Artifact:

- `/tmp/ds4-prefix-chain-extensive-20260516_103351`

Modes:

| Mode | Settings |
| --- | --- |
| serial | no MTP |
| MTP-disabled | `DS4_MTP_SPEC_DISABLE=1 --mtp {MTP} --mtp-draft 3` |
| current MTP K=3 | `--mtp {MTP} --mtp-draft 3` |
| native prefix-chain K=4 | `DS4_MTP_NATIVE=1 DS4_MTP_NATIVE_VERIFY_OPT=smallm DS4_MTP_NATIVE_CACHE_MODE=owned DS4_MTP_NATIVE_COMMIT_OPT=auto DS4_MTP_NATIVE_CHAIN_DRAFT=1 --mtp {MTP} --mtp-draft 4` |

HumanEval+ 50-task slice:

| Mode | tasks | base pass@1 | plus pass@1 | tokens | elapsed | aggregate TPS | mean TPS | median TPS |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| serial | 50 | 0.64 | 0.62 | 4410 | 164.50 s | 26.81 | 25.31 | 26.02 |
| MTP-disabled | 50 | 0.64 | 0.62 | 4410 | 164.85 s | 26.75 | 25.25 | 25.93 |
| current MTP K=3 | 50 | 0.64 | 0.62 | 4216 | 161.05 s | 26.18 | 24.80 | 25.25 |
| native prefix-chain K=4 | 50 | 0.64 | 0.62 | 4410 | 169.35 s | 26.04 | 24.59 | 25.39 |

MBPP+ 50-task slice:

| Mode | tasks | base pass@1 | plus pass@1 | tokens | elapsed | aggregate TPS | mean TPS | median TPS |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| serial | 50 | 0.96 | 0.90 | 2023 | 87.84 s | 23.03 | 20.81 | 20.10 |
| MTP-disabled | 50 | 0.96 | 0.90 | 2023 | 87.64 s | 23.08 | 20.86 | 20.15 |
| current MTP K=3 | 50 | 0.98 | 0.92 | 2000 | 88.12 s | 22.70 | 20.56 | 19.74 |
| native prefix-chain K=4 | 50 | 0.96 | 0.90 | 2023 | 91.12 s | 22.20 | 20.10 | 19.22 |

Long free-form `-n 512` CLI slice:

| Prompt | serial | MTP-disabled | current MTP K=3 | native prefix-chain K=4 | Native stdout |
| --- | ---: | ---: | ---: | ---: | --- |
| explain | 34.45 | 34.50 | 34.28 | 29.13 | match |
| code_design | 34.30 | 34.30 | 34.24 | 31.12 | match |
| debug_plan | 34.27 | 34.32 | 34.40 | 28.23 | match |
| gsm_style | 34.21 | 34.32 | 34.23 | 30.93 | match |
| mean | 34.31 | 34.36 | 34.29 | 29.85 | match |

Notes:

- MBPP+ evaluation required a no-mini EvalPlus path because `evalplus` does not provide mini MBPP+ ground truth. The serial MBPP samples from the original wrapper were reused; the other MBPP modes were generated/evaluated through the same no-mini path.
- Native prefix-chain K=4 preserved exact temp-0 stdout against serial on the long free-form prompts and matched serial quality on both EvalPlus slices.
- The representative bench does not show a throughput win. HumanEval+ and MBPP+ put native prefix-chain K=4 behind serial/MTP-disabled/current MTP, and the long free-form slice shows a larger regression (`29.85` mean TPS versus about `34.3` for serial/current MTP).
- The short count/code smoke remains useful as a positive microbenchmark, but this broader bench says the current native prefix-chain K=4 implementation should not be promoted as-is.

### 2026-05-16 Root-Inclusive Native Contract Probe

Hypothesis:

- The prefix-chain path still pays a serial target root decode before MTP drafts the suffix. That makes a native cycle pay both `target(first_token)` and `verify(suffix)`.
- `DS4_MTP_NATIVE_ROOT_INCLUSIVE=1` tests the stronger contract: use the current target frontier only to choose/confirm `first_token`, draft the suffix from persistent owned MTP state, then verify `[first_token, d2..dK]` in one target block verifier call.
- On accept, the runtime commits the verifier-produced target prefix state and promotes the matching MTP ledger prefix. It does not call serial target decode for accepted tokens in this branch.

Implementation notes:

- The probe is greenfield and gated by `DS4_MTP_NATIVE_ROOT_INCLUSIVE=1`; normal native MTP remains unchanged without the flag.
- The first implementation only runs for `DS4_MTP_NATIVE_CACHE_MODE=owned`, depth greater than one, and non-EOS first tokens.
- The verifier acceptance remains exact temp-0 top-id acceptance. The first token is accepted only because it already equals the current target frontier; suffix tokens are accepted by shifted verifier rows.
- `DS4_MTP_NATIVE_VALIDATE=1` compares every root-inclusive verifier row against serial target rows before commit. Logit deltas remain diagnostic; committed rows must have matching target top ids.

Command shape for the first Studio smoke:

```sh
DS4_MTP_NATIVE=1 \
DS4_MTP_NATIVE_ROOT_INCLUSIVE=1 \
DS4_MTP_NATIVE_VERIFY_OPT=smallm \
DS4_MTP_NATIVE_CACHE_MODE=owned \
DS4_MTP_NATIVE_TIMING=1 \
./ds4 --metal -m "$MODEL" --mtp "$MTP" --mtp-draft 3 \
  --ctx 1024 --nothink -sys "" --temp 0 -n 64 \
  --prompt-file "$PROMPT"
```

Decision rule for this probe:

- If acceptance collapses compared with the post-root prefix-chain path, DS4's native MTP head needs post-token target hidden state as its seed, and root-inclusive native drafting should be dropped.
- If acceptance holds, the next optimization target is the root-inclusive verifier itself, because the serial target-root cost has been removed from the native cycle.

Studio smoke artifact:

- `/tmp/ds4-root-inclusive-20260516_114346`
- Validation artifact: `/tmp/ds4-root-inclusive-validate-20260516_114433`

`DS4_MTP_NATIVE_ROOT_INCLUSIVE=1`, `K=3`, `owned`, `smallm`, `-n 64`:

| Prompt | stdout | serial t/s | root-inclusive t/s | cycles | avg accepted | avg discarded | avg draft | avg verifier decode GPU | avg total |
| --- | --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| count | match | 36.88 | 20.94 | 45 | 1.400 | 1.578 | 5.739 ms | 55.488 ms | 67.118 ms |
| explain | match | 37.00 | 15.45 | 61 | 1.033 | 1.951 | 5.731 ms | 55.617 ms | 67.298 ms |
| code | match | 36.87 | 15.50 | 61 | 1.033 | 1.951 | 5.738 ms | 55.663 ms | 67.103 ms |

Validation smoke:

- `count`, `K=3`, `-n 16`, `DS4_MTP_NATIVE_VALIDATE=1`: stdout matched serial, `max_delta=0`, `mismatches=0`.

Interpretation:

- This removes the serial target-root decode (`target=0.000 ms`) and still preserves exact temp-0 output, so the transactional verifier/commit path is working.
- Acceptance collapses versus the post-root prefix-chain path. On the prior prefix-chain smoke, `K=3` averaged `3.000/2.207/2.864` accepted tokens on count/explain/code; root-inclusive drops to `1.400/1.033/1.033`.
- The contract result is clear: in this DS4 tensor shape, native MTP needs the post-token target hidden state as the seed for high-quality suffix prediction. The root-inclusive seed-from-owned-MTP-state contract should be rejected unless a different MTP-head input contract is discovered.

### 2026-05-16 Verifier Kernel Review

Scope:

- Requested a fresh multi-agent review of the native verifier code with emphasis on small-M kernel math inefficiency, output-head/top-id overhead, capture/commit costs, and Metal API scheduling details.
- This is a source-level optimization review, not a new performance verdict. Candidates below need implementation and Studio measurement before any promotion/drop language.

Findings:

- The post-root small-M verifier still scalarizes router work in the FFN tail. `metal_graph_encode_decode_smallm_layer_prebatch()` batches much of the prework, but then loops each verifier row through `metal_graph_encode_decode_layer_ffn_tail_from_norm()`, which performs one-row router logits and one-row router select. The full batched router equivalent already exists in the normal batched decode path. This is the highest-priority exact optimization candidate because it can reduce roughly `2*M` router launches per layer to `2`.
- The FFN post-combine is still per-row. The small-M path loops over `ds4_gpu_hc_expand_add_split_tensor()` even though the Metal helper accepts multi-token tensors and the normal batched path already calls it once. This should be a clean exact change for `M=2..4`.
- F16 rows2/rows3/rows4 small-M kernels exist but are still env-gated, while Q8 rows kernels are automatically selected in the verifier scope. Promote the validated F16 row kernels to the same automatic verifier policy if row-logit validation stays clean.
- The shared-expert path still pays separate shared gate, shared up, SwiGLU, and shared down passes for the small-M verifier. Existing single-row fused primitives suggest a rows2/rows3/rows4 shared gate/up fusion and a small-M shared-down+HC fusion are plausible follow-up kernels after router and HC-post batching.
- Prefix-state capture remains a major non-math cost. Capturing block-prefix state copies full attention/index tensors through `ds4_gpu_tensor_copy()`, which closes compute encoders and emits blits. The earlier `optimistic_full` result already showed this cost is real on full accepts, so a slot-based promotion or in-kernel snapshot design remains worth prototyping.
- The exact greedy output head still materializes full `spec_logits` for every verifier row, then runs a separate top-id pass. Exact top-1 still requires full-vocab math, but intermediate rows do not need full-vocab storage. A fused projection+argmax for rows `0..K-2`, with only the final row materialized for `s->logits`, is an exact low-to-medium upside candidate.
- The standalone top1 kernel is not the next target. It is correct, but it scans an already materialized logits buffer in a second pass. It only becomes interesting if fused into the projection kernel.
- Metal API review found no evidence that `MTLDispatchTypeConcurrent` alone will solve this. Apple's documented contract is that concurrent dispatches may overlap if independent, while synchronization and hazards are the app's responsibility. For the verifier, likely API-level gains are fewer command buffers/encoders, less binding churn, and possibly argument tables/Metal 4 only if the target machines support that path.

Implementation order:

1. Hoist small-M router logits/select into a batched verifier router path and feed the existing exact batched MoE tail.
2. Replace per-row FFN post-combine with one batched `hc_expand_add_split` call for `M=2..4`.
3. Promote verifier-scoped F16 rows2/rows3/rows4 kernels from env-only to automatic after validation.
4. Measure capture cost with `capture_block_prefixes=0/1/K`, then prototype slot-based prefix promotion if the isolated cost remains material.
5. Fuse output projection and argmax for non-final verifier rows, while keeping final-row logits materialized.
6. Only after those are measured, consider deeper shared-expert fusion or Metal 4 argument-table work.

Measurement plan:

- Validate each candidate with `DS4_MTP_NATIVE_VALIDATE=1` against serial target top ids and deterministic stdout on Studio.
- Measure the same representative suite used after the short-prompt correction: HumanEval/EvalPlus slice, GSM-style prompt, longer free-form prompts, and any short `-n 64` smoke only as a smoke signal.
- Track verifier decode GPU, verifier head GPU, capture/commit/replay cost, average accepted depth, and end-to-end TPS separately so a math-kernel win is not hidden by acceptance or prompt-length effects.

### 2026-05-16 Batched Small-M Router And FFN Post-Combine

Implemented the first two review candidates in the exact small-M verifier path:

- Added an opt-in batched-router prototype behind `DS4_MTP_NATIVE_SMALLM_BATCH_ROUTER=1`. When enabled and `can_batch_routed` is true, `metal_graph_encode_decode_smallm_layer_prebatch()` computes router logits and router selection for all verifier rows in one batched call. The previous path looped over rows, ran one-row router logits/select, then used the batched routed-MoE tail.
- The token-aware router path writes the tiny verifier token array into the existing `prefill_tokens` scratch buffer before calling `ds4_gpu_router_select_batch_tensor()`.
- The shared FFN post-combine now calls `ds4_gpu_hc_expand_add_split_tensor()` once over the bounded `M=2..4` batch. The previous path created per-row views and launched HC expand/add/split once per verifier row.

Local checks:

```sh
make ds4_test ds4
./ds4_test --metal-block-verifier
./ds4_test --metal-kernels
./ds4_test --metal-sched2
git diff --check -- ds4.c MTP_NATIVE_CACHE_CONTRACT_PROGRESS.md
```

Studio correctness smoke:

- Worktree: `/Users/studio/git/.worktrees/antirez/ds4/mtp-native-cache-contract`
- Model: `/Users/studio/.ds4/cache/gguf/DeepSeek-V4-Flash-Q4KExperts-F16HC-F16Compressor-F16Indexer-Q8Attn-Q8Shared-Q8Out-chat-v2.gguf`
- MTP: `/Users/studio/git/antirez/ds4/gguf/DeepSeek-V4-Flash-MTP-Q4K-Q8_0-F32.gguf`
- Prompt: `/tmp/ds4-mtp-matrix/count.txt`
- `K=4`, `DS4_MTP_NATIVE_VALIDATE=1`, `-n 16`: stdout matched serial. Artifact: `/tmp/ds4-smallm-batch-router-smoke-20260516_121923`
- `K=5`, `DS4_MTP_NATIVE_VALIDATE=1`, `-n 16`: stdout matched serial. Artifact: `/tmp/ds4-smallm-batch-router-smoke-k5-20260516_121949`

Status:

- Correctness smoke is clean for the touched `M=3` and `M=4` verifier shapes.
- This is not yet a performance result. The next step is a Studio timing matrix with `DS4_MTP_NATIVE_TIMING=1` on representative prompts/evals, comparing before/after verifier decode GPU and end-to-end TPS.

### 2026-05-16 Batched Router Rejected As Default; HC Post Timing

The first timing matrix with batched router enabled found a production correctness regression:

- Artifact: `/tmp/ds4-smallm-batch-router-timing-20260516_122237`
- `count` and `code` matched serial, but all `explain` native runs diverged from serial.
- The visible diff was small (`memory-bandwidth-bound` versus `memory-bandwidth bound`), but exact native temp-0 mode requires deterministic stdout match, so this cannot be a default path.
- A validation-mode run matched serial because validation changes the verifier/readback path enough to mask this production divergence. The production matrix is the correct gate here.

Decision:

- Keep `DS4_MTP_NATIVE_SMALLM_BATCH_ROUTER=1` only as an opt-in diagnostic.
- Do not enable batched router by default until it is made production-exact on the three-prompt matrix without validation mode.
- Keep the batched HC post-combine enabled; it is correctness-clean in the safe-default matrix below.

Safe-default Studio timing matrix:

- Artifact: `/tmp/ds4-smallm-batch-hcpost-timing-20260516_122754`
- Settings: `DS4_MTP_NATIVE=1 DS4_MTP_NATIVE_VERIFY_OPT=smallm DS4_MTP_NATIVE_CACHE_MODE=owned DS4_MTP_NATIVE_COMMIT_OPT=auto DS4_MTP_NATIVE_TIMING=1`
- Model: `/Users/studio/.ds4/cache/gguf/DeepSeek-V4-Flash-Q4KExperts-F16HC-F16Compressor-F16Indexer-Q8Attn-Q8Shared-Q8Out-chat-v2.gguf`
- MTP: `/Users/studio/git/antirez/ds4/gguf/DeepSeek-V4-Flash-MTP-Q4K-Q8_0-F32.gguf`

| Prompt | Mode | stdout | generation t/s | cycles | avg accepted | verifier decode GPU | verifier head GPU | draft | commit | replay |
| --- | --- | --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| count | serial | match | 35.62 | - | - | - | - | - | - | - |
| count | K=3 | match | 36.94 | 21 | 3.000 | 42.374 ms | 0.948 ms | 5.872 ms | 0.053 ms | 0.000 ms |
| count | K=4 | match | 41.12 | 16 | 4.000 | 56.405 ms | 1.021 ms | 7.757 ms | 0.069 ms | 0.000 ms |
| count | K=5 | match | 33.82 | 16 | 4.000 | 73.082 ms | 1.169 ms | 9.585 ms | 0.407 ms | 0.000 ms |
| explain | serial | match | 35.72 | - | - | - | - | - | - | - |
| explain | K=3 | match | 31.00 | 32 | 2.000 | 26.098 ms | 0.565 ms | 5.805 ms | 1.193 ms | 0.963 ms |
| explain | K=4 | match | 31.43 | 27 | 2.370 | 35.460 ms | 0.641 ms | 7.602 ms | 0.257 ms | 0.000 ms |
| explain | K=5 | match | 28.88 | 26 | 2.462 | 42.970 ms | 0.715 ms | 9.362 ms | 0.246 ms | 0.000 ms |
| code | serial | match | 35.69 | - | - | - | - | - | - | - |
| code | K=3 | match | 36.30 | 24 | 2.667 | 35.469 ms | 0.793 ms | 5.844 ms | 0.049 ms | 0.000 ms |
| code | K=4 | match | 35.08 | 21 | 3.048 | 43.313 ms | 0.778 ms | 7.607 ms | 3.021 ms | 2.760 ms |
| code | K=5 | match | 30.81 | 19 | 3.368 | 64.217 ms | 1.050 ms | 9.536 ms | 0.354 ms | 0.000 ms |

Representative quality/TPS smoke:

- Artifact: `/tmp/ds4-smallm-hcpost-humaneval20-20260516_122943`
- Dataset: HumanEval+ first 20 tasks, chat API, `max_tokens=1024`

| Mode | syntax | base pass@1 | plus pass@1 | tokens | elapsed | aggregate TPS | mean TPS | median TPS |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| serial | 20/20 | 0.500 | 0.500 | 1317 | 52.75 s | 24.97 | 24.20 | 24.33 |
| smallm K=4 hcpost | 20/20 | 0.500 | 0.500 | 1317 | 54.90 s | 23.99 | 23.34 | 23.27 |

Interpretation:

- The HC post-combine batching is safe but not enough to move representative end-to-end performance.
- The short prompt matrix remains mixed: `count K=4` is faster than serial, `code K=3` is slightly faster, and `explain` remains slower.
- HumanEval quality is unchanged in the same-run comparison, but native K=4 is still slower by about `4%` aggregate TPS.
- The next optimization should not be more router batching unless the production exactness issue is solved. Better next candidates are verifier-scoped F16 rows kernels, capture-copy reduction, or output-head fused argmax for non-final rows.

### 2026-05-16 Batched Router Exactness Investigation

The production-only `explain` mismatch was localized to tiny router-weight drift in the
batched-router path, not to accepted-row top-id validation:

- Repro artifact: `/tmp/ds4-router-fusion-disable-explain-20260516_123811`
- Forcing the row router onto the generic non-fused path still matched serial.
- The batched router still mismatched serial, so the bug was specific to the multi-row
  router contract.
- Audit artifact: `/tmp/ds4-router-audit-bit-20260516_124208`
- Expert ids matched, but route weights differed by 1 ULP to a few ULPs starting at
  layer 0, e.g. `0x3ec6b01f` versus `0x3ec6b020`.

Fix:

- `ds4_gpu_encode_router_select()` now uses the row-proven fused
  `kernel_dsv4_softplus_sqrt_f32_4` transform for multi-row router probabilities in
  non-quality mode.
- Weight normalization for multi-row production mode now encodes the existing
  `kernel_dsv4_router_weights_one` once per row. A separate batched weight kernel was
  tested and rejected because compiling the same formula as a different Metal function
  still produced different float bits.
- `DS4_MTP_NATIVE_SMALLM_ROUTER_AUDIT=1` compares batched logits, probabilities,
  selected experts, and weights against the row router boundary.

Post-fix evidence:

- Router audit artifact: `/tmp/ds4-router-audit-rowweight-20260516_125056`
- Full failing case artifact: `/tmp/ds4-router-rowweight-explain64-20260516_125114`
- Three-prompt production matrix artifact: `/tmp/ds4-router-rowweight-matrix-20260516_125206`

| Prompt | Serial t/s | Batched-router t/s | stdout |
| --- | ---: | ---: | --- |
| count | 35.69 | 40.02 | match |
| explain | 35.55 | 31.66 | match |
| code | 35.85 | 36.61 | match |

Decision:

- The original batched-router implementation was correctly rejected as unsafe because
  validation mode did not expose the production state drift.
- The corrected path is exact on the three-prompt `-n 64` production smoke, but remains
  behind `DS4_MTP_NATIVE_SMALLM_BATCH_ROUTER=1` until it is evaluated on the broader
  representative benchmark/eval set.
- This is a useful lesson for future verifier optimizations: for DS4 routed MoE,
  "mathematically equivalent" tiny kernels are not necessarily temp-0 exact unless the
  float operation shape is kept bit-identical to the row path.

### 2026-05-16 Verifier Efficiency Goal: Copy, Top-1, And F16 Row Probes

Active goal for the next efficiency pass:

- Keep optimizing the exact native verifier hot path without treating short `-n 64`
  runs as promotion evidence.
- Model-backed performance tests continue to run on `studio.local`, not the local
  workstation.
- Representative evals remain required before any promotion decision.

Implementation changes:

- Deferred native validation `suffix_logits` allocation until after the first suffix
  draft passes the frontier rank gate and the verifier is actually about to run.
- Reused a batch `MTLBlitCommandEncoder` across consecutive `ds4_gpu_tensor_copy()`
  calls. This targets snapshot/capture/commit copy churn without changing tensor
  contents. `DS4_METAL_BATCH_BLIT_REUSE_DISABLE=1` restores the previous one-blit-pass
  per copy behavior for A/B measurement.

Checks:

```sh
make ds4_test ds4
./ds4_test --metal-kernels
./ds4_test --metal-sched2
./ds4_test --metal-block-verifier
git diff --check
```

The same checks passed on Studio in:

- Worktree: `/Users/studio/git/.worktrees/antirez/ds4/mtp-native-cache-contract`

Standalone top-1 output-head probe:

- Artifact: `/tmp/ds4-top1-native-probe-20260516_144757`
- Settings: `DS4_MTP_NATIVE=1`, `DS4_MTP_NATIVE_VERIFY_OPT=smallm`,
  `DS4_MTP_NATIVE_CACHE_MODE=owned`, `DS4_MTP_NATIVE_COMMIT_OPT=auto`,
  `DS4_MTP_NATIVE_SMALLM_BATCH_ROUTER=1`, `K=4`, `-n 64`.

| Prompt | top1 off t/s | top1 on t/s | head GPU off | head GPU on |
| --- | ---: | ---: | ---: | ---: |
| count | 41.67 | 41.53 | 1.028 ms | 0.933 ms |
| explain | 31.79 | 31.80 | 0.912 ms | 0.875 ms |
| code | 35.66 | 35.44 | 0.947 ms | 0.880 ms |

Decision: the standalone top-1 reducer is exact but not a meaningful efficiency lever.
It still scans already materialized full logits. The useful version would need to fuse
projection and argmax for non-final verifier rows.

Batch blit reuse A/B:

- Artifact: `/tmp/ds4-native-blit-reuse-20260516_145433`
- Settings: `DS4_MTP_NATIVE=1`, `DS4_MTP_NATIVE_VERIFY_OPT=smallm`,
  `DS4_MTP_NATIVE_CACHE_MODE=owned`, `DS4_MTP_NATIVE_COMMIT_OPT=capture`,
  `DS4_MTP_NATIVE_SMALLM_BATCH_ROUTER=1`, `K=4`, `-n 64`.

| Prompt | target t/s | old-copy t/s | reuse t/s | stdout | old commit | reuse commit | old decode GPU | reuse decode GPU |
| --- | ---: | ---: | ---: | --- | ---: | ---: | ---: | ---: |
| count | 35.49 | 40.11 | 40.06 | match | 0.411 ms | 0.371 ms | 56.825 ms | 58.526 ms |
| explain | 35.47 | 31.73 | 31.75 | match | 0.392 ms | 0.387 ms | 27.482 ms | 27.718 ms |
| code | 35.88 | 36.52 | 36.54 | match | 0.411 ms | 0.378 ms | 43.992 ms | 45.055 ms |

Decision: blit reuse is a safe cleanup and slightly reduces measured commit/copy cost,
but it is not an end-to-end throughput lever. Keep it as a small implementation cleanup,
not as promotion evidence.

F16 verifier row-kernel A/B:

- Artifact: `/tmp/ds4-native-f16rows-current-20260516_145637`
- Settings: `DS4_MTP_NATIVE=1`, `DS4_MTP_NATIVE_VERIFY_OPT=smallm`,
  `DS4_MTP_NATIVE_CACHE_MODE=owned`, `DS4_MTP_NATIVE_COMMIT_OPT=auto`,
  `DS4_MTP_NATIVE_SMALLM_BATCH_ROUTER=1`, `K=4`, `-n 64`.

| Prompt | target t/s | F16 off t/s | F16 on t/s | stdout | off decode GPU | F16 decode GPU |
| --- | ---: | ---: | ---: | --- | ---: | ---: |
| count | 35.67 | 41.61 | 41.53 | match | 55.699 ms | 56.082 ms |
| explain | 35.75 | 31.91 | 31.99 | match | 27.735 ms | 27.494 ms |
| code | 35.46 | 35.55 | 35.42 | match | 44.785 ms | 44.213 ms |

Decision: F16 rows remain gated behind `DS4_MTP_NATIVE_SMALLM_F16_ROWS=1`. The current
same-binary A/B is exact but flat/noisy, so it should not become default.

Next implementation targets:

1. Fused output projection plus argmax for non-final verifier rows, while still
   materializing final-row logits.
2. A dedicated invariant test before cutting the verifier's temporary checkpoint
   bookkeeping, because the first-token/native-start contract is subtle.
3. If output fusion is too large for the next pass, profile command-buffer boundaries
   in the verifier decode/head split before attempting Metal 4 argument tables.

### 2026-05-16 Single-Command Verifier Probe

Implementation:

- Added `DS4_MTP_NATIVE_VERIFY_SINGLE_COMMAND=1` as a gated verifier experiment.
- The flag applies only when the native verifier is already using the batched output-head
  path. It keeps target verifier decode plus output-head/top-id work in one command
  sequence instead of ending the verifier command buffer before starting the head pass.
- Timing now reports `single_command=0|1` and `verifier_total_gpu`. In this mode,
  `verifier_total_gpu` is the combined decode+head GPU span; the split
  `verifier_decode_gpu` and `verifier_head_gpu` counters are intentionally zero because
  the verifier no longer has a command-buffer boundary between them.
- This does not change verifier math, accepted-token semantics, or the exact
  temp-0 contract. It is a command submission/boundary probe.

Local and Studio checks:

```sh
make ds4_test ds4
./ds4_test --metal-kernels
./ds4_test --metal-sched2
./ds4_test --metal-block-verifier
git diff --check
```

The same checks passed in the Studio worktree:

- `/Users/studio/git/.worktrees/antirez/ds4/mtp-native-cache-contract`

Three-prompt production smoke:

- Artifact: `/tmp/ds4-native-single-command-20260516_150227`
- Settings: `DS4_MTP_NATIVE=1`, `DS4_MTP_NATIVE_VERIFY_OPT=smallm`,
  `DS4_MTP_NATIVE_CACHE_MODE=owned`, `DS4_MTP_NATIVE_COMMIT_OPT=auto`,
  `DS4_MTP_NATIVE_SMALLM_BATCH_ROUTER=1`, `K=4`, `-n 64`.

| Prompt | target t/s | single off t/s | single on t/s | stdout | off total GPU | single total GPU | accepted |
| --- | ---: | ---: | ---: | --- | ---: | ---: | ---: |
| count | 35.50 | 41.76 | 41.72 | match | 56.702 ms | 56.973 ms | 4/4 |
| explain | 35.55 | 31.82 | 32.01 | match | 28.807 ms | 28.542 ms | 2 |
| code | 35.42 | 35.57 | 35.63 | match | 46.524 ms | 45.210 ms | 3 |

Validation smoke:

- Artifact: `/tmp/ds4-native-single-command-validate-20260516_150322`
- Settings: same native mode, `K=4`, `DS4_MTP_NATIVE_VALIDATE=1`, `-n 32`.

| Prompt | stdout | cycles | last max_delta | total mismatches |
| --- | --- | ---: | ---: | ---: |
| count | match | 8 | 0 | 0 |
| explain | match | 9 | 0 | 0 |
| code | match | 8 | 0 | 0 |

HumanEval+ 20-task A/B:

- Artifact: `/tmp/ds4-native-single-command-humaneval20-20260516_150411`
- Dataset: HumanEval+ first 20 tasks, chat API, `max_tokens=1024`.

| Mode | syntax | base pass@1 | plus pass@1 | tokens | elapsed | aggregate TPS | mean TPS | median TPS |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| single off | 20/20 | 0.450 | 0.450 | 1394 | 57.90 s | 24.08 | 23.67 | 23.77 |
| single on | 20/20 | 0.450 | 0.450 | 1394 | 56.79 s | 24.55 | 24.05 | 23.78 |

GSM8K 20-task A/B:

- Artifact: `/tmp/ds4-native-single-command-gsm8k20-20260516_151226`
- Note: the initial summary files in that artifact used the raw GSM8K rationale as the
  gold string and therefore report `0/20`. The corrected rescored summaries are
  `summary.rescored.json` in each mode directory.

| Mode | accuracy | tokens | elapsed | aggregate TPS | mean TPS | median TPS |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| single off | 1.000 | 1965 | 85.34 s | 23.02 | 22.53 | 22.26 |
| single on | 1.000 | 1965 | 86.34 s | 22.76 | 22.29 | 22.03 |

Decision:

- `DS4_MTP_NATIVE_VERIFY_SINGLE_COMMAND=1` is correctness-clean in production smoke,
  validation smoke, HumanEval+ 20, and GSM8K 20.
- The speed result is mixed: about `+2%` aggregate TPS on this HumanEval+ slice,
  flat on the tiny count prompt, small positive on code/explain smoke, and slightly
  negative on GSM8K.
- Keep the flag as a safe gated efficiency probe. Do not promote it as the default
  from this evidence alone.
- The next higher-upside target remains fused output projection plus argmax for
  non-final verifier rows. Single-command batching mainly reduces command-boundary
  overhead; it does not remove the full-vocab projection/storage work.

### 2026-05-16 Fused Output Top-1 Probe

Implementation:

- Added `DS4_MTP_NATIVE_OUTPUT_FUSED_TOP1=1` as a gated native verifier experiment.
- Added `DS4_MTP_NATIVE_OUTPUT_FUSED_TOP1_AUDIT=1` to materialize every verifier row
  and compare the fused top id against the full-logit top id during debug runs.
- Added row-specialized Q8_0 output-projection kernels for verifier `M=2..4`.
  The fused path writes top-1 ids directly for all verifier rows and materializes
  full logits only for the row whose logits must become the next committed `s->logits`.
- If a partial accept commits a row different from the initially materialized row,
  the runtime re-materializes just that row and records it as `remat_row`.
- Timing now reports `output_fused_top1`, `logits_row`, and `remat_row`. The row fields
  are initialized to `-1` when the verifier does not run, so timing output does not
  imply row `0` work in first-token reject cycles.

Local and Studio checks:

```sh
make ds4_test ds4
./ds4_test --metal-kernels
./ds4_test --metal-sched2
./ds4_test --metal-block-verifier
git diff --check -- ds4.c ds4_gpu.h ds4_metal.m metal/argsort.metal metal/dense.metal MTP_NATIVE_CACHE_CONTRACT_PROGRESS.md
```

The same checks passed in the Studio worktree:

- `/Users/studio/git/.worktrees/antirez/ds4/mtp-native-cache-contract`

Three-prompt production smoke:

- Artifact: `/tmp/ds4-native-output-fused-top1-20260516_152837`
- Settings: `DS4_MTP_NATIVE=1`, `DS4_MTP_NATIVE_VERIFY_OPT=smallm`,
  `DS4_MTP_NATIVE_CACHE_MODE=owned`, `DS4_MTP_NATIVE_COMMIT_OPT=auto`,
  `DS4_MTP_NATIVE_SMALLM_BATCH_ROUTER=1`, `K=4`, `-n 64`.

| Prompt | target t/s | fused off t/s | audit t/s | fused on t/s | stdout | audit mismatches | off head GPU | on head GPU |
| --- | ---: | ---: | ---: | ---: | --- | ---: | ---: | ---: |
| count | 35.58 | 41.78 | 38.90 | 41.64 | match | 0 | 1.024 ms | 0.916 ms |
| explain | 35.42 | 31.75 | 30.73 | 31.58 | match | 0 | 0.642 ms | 0.579 ms |
| code | 35.79 | 35.74 | 33.74 | 35.56 | match | 0 | 0.780 ms | 0.697 ms |

Timing notes:

- The fused kernels reduced measured verifier output-head GPU span by roughly
  `0.06..0.11 ms` per verifier call on this smoke.
- End-to-end TPS was flat to slightly negative. Total verifier GPU span did not move
  enough to pay for the extra control flow and occasional partial-accept row
  re-materialization.
- Audit mode was deliberately slower because it materializes every row for top-id
  comparison; it is a correctness harness, not an optimized path.

Decision:

- Keep `DS4_MTP_NATIVE_OUTPUT_FUSED_TOP1=1` as a correctness-clean gated probe.
- Do not default it from this evidence. The result is a useful narrowing result:
  full-logit storage/readback/top-1 scan is not the dominant native-verifier cost in
  the current small-M shape.
- The next efficiency work should target verifier body math and transaction overhead,
  especially the repeated target-root/verify interaction and small-M FFN/MoE kernels,
  rather than only the output-head readback path.

Follow-up cleanup:

- `metal_graph_verify_decode_exact()` now creates its per-row verifier tensor views after
  any prefix-batch layers have run. The old order created views, ran a prefix batch that
  could swap the backing batch buffers, then freed and recreated the views. This removes
  avoidable Objective-C tensor-view churn without changing verifier math or state order.
- Verified locally and on Studio with `make ds4_test ds4`, the three Metal harnesses,
  and `git diff --check`.

### 2026-05-16 Batched Router Promotion

Change:

- Promoted the corrected native small-M batched-router path to the default when the
  existing safety predicates allow routed/shared verifier batching.
- `DS4_MTP_NATIVE_SMALLM_BATCH_ROUTER=1` is no longer required for the promoted path.
- Added `DS4_MTP_NATIVE_SMALLM_BATCH_ROUTER_DISABLE=1` as the A/B and rollback switch.
- The prior correctness fix is still important: multi-row router probabilities use the
  row-proven transform, and weights are normalized by encoding the row-proven weight
  kernel once per verifier row. The earlier fully batched weight kernel remains rejected
  because it changed float bits.

Representative HumanEval+ 20-task A/B:

- Artifact: `/tmp/ds4-router-representative-20260516_153817`
- Dataset: HumanEval+ first 20 tasks, chat API, `max_tokens=1024`.
- Both modes used the same current binary and native small-M `K=4`.

| Mode | syntax | base pass@1 | plus pass@1 | tokens | elapsed | aggregate TPS | mean TPS | median TPS |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| router disabled | 20/20 | 0.450 | 0.450 | 1394 | 58.53 s | 23.82 | 23.42 | 23.49 |
| router default/on | 20/20 | 0.450 | 0.450 | 1394 | 57.00 s | 24.46 | 23.96 | 23.70 |

GSM8K 20-task A/B:

- Artifact: `/tmp/ds4-router-gsm8k20-20260516_154140`
- Dataset: first 20 GSM8K test tasks from the existing local artifact
  `/tmp/ds4-smallm-gsm8k20-20260516_033442/gsm8k_test.jsonl`.
- Prompt required the model to end with `#### <answer>`.

| Mode | accuracy | tokens | elapsed | aggregate TPS | mean TPS | median TPS |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| router disabled | 1.000 | 1817 | 81.35 s | 22.33 | 21.77 | 21.84 |
| router default/on | 1.000 | 1817 | 65.63 s | 27.69 | 26.78 | 26.57 |

Three-prompt production smoke after default flip:

- Artifact: `/tmp/ds4-router-default-smoke-20260516_154936`
- Settings: `DS4_MTP_NATIVE=1`, `DS4_MTP_NATIVE_VERIFY_OPT=smallm`,
  `DS4_MTP_NATIVE_CACHE_MODE=owned`, `DS4_MTP_NATIVE_COMMIT_OPT=auto`, `K=4`,
  `-n 64 --temp 0 --ctx 1024 --nothink -sys ""`.

| Prompt | serial t/s | router default t/s | router disabled t/s | default stdout | disabled stdout |
| --- | ---: | ---: | ---: | --- | --- |
| count | 35.94 | 41.43 | 40.74 | match | match |
| explain | 35.53 | 31.78 | 31.40 | match | match |
| code | 35.44 | 35.51 | 35.02 | match | match |

Validation/audit smoke:

- Artifact: `/tmp/ds4-router-default-audit-20260516_155056`
- Prompt: explain, `K=4`, `-n 32`, `DS4_MTP_NATIVE_VALIDATE=1`,
  `DS4_MTP_NATIVE_TIMING=1`, `DS4_MTP_NATIVE_SMALLM_ROUTER_AUDIT=1`.
- Stdout matched serial.
- Accepted-row validation reported `mismatches=0`.
- No router audit mismatch lines were emitted.

Decision:

- The corrected batched-router path is now promoted to the default native small-M
  verifier path.
- This is a real efficiency promotion, not an architecture-final promotion. It improves
  the verifier hot path while preserving exact temp-0 output on the current smoke and
  representative slices, but native MTP still needs additional verifier-body and
  transaction work before the full architecture can be judged promotable.
- Next targets: small-M Q8/F16 dispatch-shape tuning with bit/top-id audits, then
  capture/commit transaction cost reduction.

### 2026-05-16 Q8 Small-M NSG Dispatch Probe

Implementation:

- Added `DS4_MTP_NATIVE_SMALLM_Q8_NSG=1|2|4|8|auto` as a verifier-only diagnostic
  probe for Q8_0 small-M rows kernels.
- The default path remains unchanged: Q8 small-M uses `nsg=4`, except for very large
  output dimensions where the existing code uses `nsg=8`.
- The probe applies to the rows2/rows3/rows4 Q8 verifier kernels and the fused
  output-top1 Q8 probe path. It does not touch CUDA.

Local and Studio checks:

```sh
make ds4_test ds4 ds4-server
./ds4_test --metal-kernels
./ds4_test --metal-sched2
./ds4_test --metal-block-verifier
git diff --check -- ds4.c ds4_gpu.h ds4_metal.m metal/argsort.metal metal/dense.metal MTP_NATIVE_CACHE_CONTRACT_PROGRESS.md
```

The same checks passed in the Studio worktree.

Three-prompt production/timing sweep:

- Artifact: `/tmp/ds4-smallm-q8nsg-sweep-20260516_155328`
- Settings: native small-M default router, `K=4`, `-n 64 --temp 0 --ctx 1024 --nothink -sys ""`.

| Prompt | Q8 NSG | stdout | t/s | cycles | avg accepted | verifier decode GPU | verifier head GPU | verifier total GPU |
| --- | --- | --- | ---: | ---: | ---: | ---: | ---: | ---: |
| count | default | match | 41.63 | 16 | 4.000 | 55.599 ms | 1.026 ms | 56.981 ms |
| count | 1 | match | 40.36 | 16 | 4.000 | 55.384 ms | 0.981 ms | 56.718 ms |
| count | 2 | match | 40.26 | 16 | 4.000 | 54.923 ms | 0.977 ms | 56.260 ms |
| count | 4 | match | 41.72 | 16 | 4.000 | 55.257 ms | 0.980 ms | 56.599 ms |
| count | 8 | match | 40.97 | 16 | 4.000 | 56.904 ms | 1.024 ms | 58.285 ms |
| count | auto | match | 40.87 | 16 | 4.000 | 57.022 ms | 1.025 ms | 58.408 ms |
| explain | default | match | 31.81 | 27 | 2.370 | 34.682 ms | 0.644 ms | 35.514 ms |
| explain | 1 | DIFF | 30.14 | 24 | 2.667 | 47.108 ms | 0.816 ms | 48.155 ms |
| explain | 2 | DIFF | 28.76 | 28 | 2.250 | 38.132 ms | 0.663 ms | 38.986 ms |
| explain | 4 | match | 31.76 | 27 | 2.370 | 34.873 ms | 0.617 ms | 35.672 ms |
| explain | 8 | DIFF | 28.88 | 26 | 2.462 | 44.225 ms | 0.790 ms | 45.231 ms |
| explain | auto | DIFF | 28.95 | 26 | 2.462 | 44.176 ms | 0.788 ms | 45.187 ms |
| code | default | match | 35.39 | 21 | 3.048 | 42.702 ms | 0.781 ms | 43.721 ms |
| code | 1 | match | 34.60 | 21 | 3.048 | 42.468 ms | 0.747 ms | 43.445 ms |
| code | 2 | match | 34.66 | 21 | 3.048 | 42.137 ms | 0.744 ms | 43.107 ms |
| code | 4 | match | 35.58 | 21 | 3.048 | 42.412 ms | 0.748 ms | 43.388 ms |
| code | 8 | match | 34.95 | 21 | 3.048 | 43.648 ms | 0.782 ms | 44.663 ms |
| code | auto | match | 34.92 | 21 | 3.048 | 43.814 ms | 0.779 ms | 44.831 ms |

Validation drilldown:

- Artifact: `/tmp/ds4-q8nsg-validate-20260516_155535`
- Prompt: explain, `K=4`, `-n 32`, `DS4_MTP_NATIVE_VALIDATE=1`,
  `DS4_MTP_NATIVE_TIMING=1`, `DS4_MTP_NATIVE_SMALLM_Q8_NSG=2`.
- Stdout differed from serial.
- Accepted-row top ids still matched (`mismatches=0`), but diagnostic logit deltas
  reached `max_delta=1.77332`. The likely failure mode is state/logit drift that does
  not flip the current accepted-row top id, then changes a later greedy decision.

Decision:

- Do not promote alternate Q8 small-M `nsg` values. The current `nsg=4` shape remains
  the only production-safe default from this sweep.
- Keep `DS4_MTP_NATIVE_SMALLM_Q8_NSG` as a diagnostic probe only. Treat it as
  non-promotable unless future bit/top-id audits prove an alternate reduction shape can
  preserve deterministic temp-0 output.
- This narrows the small-M matvec path: tuning SIMD-group count is correctness-sensitive
  and not the next high-upside efficiency lever.

### 2026-05-16 Batched Shared Gate/Up Fusion Probe

Implementation:

- Added `DS4_MTP_NATIVE_SMALLM_BATCH_SHARED_GATEUP=1` as a gated native small-M
  verifier probe.
- Added `ds4_gpu_shared_gate_up_swiglu_q8_0_batch_tensor()`, which reuses the
  existing exact shared-expert fused Q8 gate/up/SwiGLU Metal kernel but dispatches it
  over the verifier's `M=2..4` rows in one call.
- The default path is unchanged. Without the flag, small-M shared work still runs the
  row-batched gate projection, row-batched up projection, separate SwiGLU, shared-down,
  then HC post-combine.

Local and Studio checks:

```sh
make ds4_test ds4 ds4-server
./ds4_test --metal-kernels
./ds4_test --metal-sched2
./ds4_test --metal-block-verifier
git diff --check -- ds4.c ds4_gpu.h ds4_metal.m metal/argsort.metal metal/dense.metal MTP_NATIVE_CACHE_CONTRACT_PROGRESS.md
```

The same checks passed in the Studio worktree.

Three-prompt production/timing A/B:

- Artifact: `/tmp/ds4-smallm-shared-gateup-20260516_160149`
- Settings: native small-M default router, `K=4`, `-n 64 --temp 0 --ctx 1024 --nothink -sys ""`.

| Prompt | Mode | stdout | t/s | cycles | avg accepted | verifier decode GPU | verifier head GPU | verifier total GPU | draft | commit | replay |
| --- | --- | --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| count | default | match | 41.79 | 16 | 4.000 | 55.235 ms | 1.021 ms | 56.620 ms | 7.816 ms | 0.067 ms | 0.000 ms |
| count | shared_gateup | match | 41.55 | 16 | 4.000 | 55.776 ms | 1.025 ms | 57.168 ms | 7.790 ms | 0.063 ms | 0.000 ms |
| explain | default | match | 31.64 | 27 | 2.370 | 34.931 ms | 0.644 ms | 35.757 ms | 7.587 ms | 0.246 ms | 0.000 ms |
| explain | shared_gateup | match | 31.58 | 27 | 2.370 | 35.155 ms | 0.643 ms | 35.982 ms | 7.594 ms | 0.245 ms | 0.000 ms |
| code | default | match | 35.52 | 21 | 3.048 | 42.489 ms | 0.779 ms | 43.499 ms | 7.585 ms | 3.074 ms | 2.813 ms |
| code | shared_gateup | match | 35.55 | 21 | 3.048 | 42.638 ms | 0.778 ms | 43.646 ms | 7.603 ms | 3.028 ms | 2.774 ms |

Validation drilldown:

- Artifact: `/tmp/ds4-shared-gateup-validate-20260516_160242`
- Prompt: explain, `K=4`, `-n 32`, `DS4_MTP_NATIVE_VALIDATE=1`,
  `DS4_MTP_NATIVE_TIMING=1`, `DS4_MTP_NATIVE_SMALLM_BATCH_SHARED_GATEUP=1`.
- Stdout matched serial.
- Accepted-row validation reported `cycles=12`, `max_delta=0`, `mismatches=0`.

Decision:

- Keep `DS4_MTP_NATIVE_SMALLM_BATCH_SHARED_GATEUP=1` as a safe gated probe only.
- Do not promote it to default. It removes a launch/pass conceptually, but the fused
  two-weight Q8 kernel is slightly slower than the current separate batched gate/up
  projections plus SwiGLU on the measured verifier shapes.
- The next verifier-body target should be deeper than launch folding: either fused
  shared-down plus HC post for the batched small-M path, or a capture/commit design
  that avoids full frontier copies without paying optimistic replay on partial accepts.

### 2026-05-16 Batched Shared-Down HC Fusion Probe

Implementation:

- Added `DS4_MTP_NATIVE_SMALLM_BATCH_SHARED_DOWN_HC=1` as a gated native small-M
  verifier probe.
- Extended `kernel_dsv4_shared_down_hc_expand4_q8_0` so its `z` grid dimension
  carries verifier rows. The previous kernel contract accepted only `n_tokens=1`;
  the new path keeps the same exact math and offsets every input/output tensor by
  the verifier row.
- Added `ds4_gpu_shared_down_hc_expand_q8_0_batch_tensor()` and wired it into
  `metal_graph_encode_decode_smallm_layer_prebatch()` after the existing batched
  shared gate/up/SwiGLU work. The default path is unchanged unless the flag is set.

Local and Studio checks:

```sh
make ds4_test ds4 ds4-server
./ds4_test --metal-kernels
./ds4_test --metal-sched2
./ds4_test --metal-block-verifier
git diff --check -- ds4.c ds4_gpu.h ds4_metal.m metal/dsv4_hc.metal MTP_NATIVE_CACHE_CONTRACT_PROGRESS.md
```

The same build and Metal harness checks passed in the Studio worktree.

Three-prompt production/timing A/B:

- Artifact: `/tmp/ds4-smallm-shared-down-hc-20260516_160825`
- Settings: native small-M default router, `K=4`, `-n 64 --temp 0 --ctx 1024 --nothink -sys ""`.

| Prompt | Mode | stdout | t/s | cycles | avg accepted | verifier decode GPU | verifier head GPU | verifier total GPU | draft | commit | replay |
| --- | --- | --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| count | default | match | 41.50 | 16 | 4.000 | 55.564 ms | 1.023 ms | 56.947 ms | 7.866 ms | 0.064 ms | 0.000 ms |
| count | shared_down_hc | match | 41.35 | 16 | 4.000 | 56.009 ms | 1.026 ms | 57.394 ms | 7.803 ms | 0.063 ms | 0.000 ms |
| explain | default | match | 31.71 | 27 | 2.370 | 34.792 ms | 0.644 ms | 35.618 ms | 7.564 ms | 0.248 ms | 0.000 ms |
| explain | shared_down_hc | match | 31.79 | 27 | 2.370 | 34.680 ms | 0.641 ms | 35.510 ms | 7.587 ms | 0.246 ms | 0.000 ms |
| code | default | match | 35.47 | 21 | 3.048 | 42.476 ms | 0.781 ms | 43.488 ms | 7.638 ms | 3.068 ms | 2.800 ms |
| code | shared_down_hc | match | 35.32 | 21 | 3.048 | 42.892 ms | 0.778 ms | 43.915 ms | 7.678 ms | 3.074 ms | 2.819 ms |

Validation drilldown:

- Artifact: `/tmp/ds4-shared-down-hc-validate-20260516_160919`
- Prompt: explain, `K=4`, `-n 32`, `DS4_MTP_NATIVE_VALIDATE=1`,
  `DS4_MTP_NATIVE_TIMING=1`, `DS4_MTP_NATIVE_SMALLM_BATCH_SHARED_DOWN_HC=1`.
- Stdout matched serial.
- Accepted-row validation reported `cycles=12`, `avg_accept=2.667`,
  `max_delta=0`, `mismatches=0`.

Decision:

- Keep `DS4_MTP_NATIVE_SMALLM_BATCH_SHARED_DOWN_HC=1` as a safe gated probe only.
- Do not promote it to default from this smoke evidence. It is exact, but verifier
  total GPU time is essentially flat: one prompt improved by `0.108 ms`, while
  count and code regressed by `0.447 ms` and `0.427 ms` per verifier cycle.
- This closes the simple shared-expert launch-folding thread for now. The next
  efficiency work should focus on larger per-layer math fusion or state/capture
  costs, where there is enough work to pay for a custom kernel shape.

### 2026-05-16 Hybrid Tail-Capture Commit Probe

Implementation:

- Added `DS4_MTP_NATIVE_COMMIT_OPT=hybrid_tail` as a gated transaction-policy probe.
- The mode captures only non-final verifier prefixes, then keeps the verifier-produced
  live target state on full accept. For `K=4`, that means capturing two suffix prefix
  states instead of all three rows.
- On partial accept, the runtime restores the frontier and commits the captured
  verifier-produced prefix, so accepted suffix tokens do not use serial replay.
- On full accept, the runtime keeps the verifier state in place and only copies the
  committed logits to the session, matching the useful part of `optimistic_full`
  without the partial-accept replay penalty.
- In `DS4_MTP_NATIVE_VALIDATE=1`, the mode falls back to conservative full capture so
  accepted-row serial-oracle validation stays simple and diagnostic.

Local and Studio checks:

```sh
make ds4_test ds4 ds4-server
./ds4_test --metal-kernels
./ds4_test --metal-sched2
./ds4_test --metal-block-verifier
git diff --check -- ds4.c ds4_gpu.h ds4_metal.m metal/dsv4_hc.metal MTP_NATIVE_CACHE_CONTRACT_PROGRESS.md
```

The same build and Metal harness checks passed in the Studio worktree.

Three-prompt production/timing A/B:

- Artifact: `/tmp/ds4-native-hybrid-tail-20260516_161347`
- Settings: native small-M default router, `K=4`, `-n 64 --temp 0 --ctx 1024 --nothink -sys ""`.

| Prompt | Mode | stdout | t/s | cycles | avg accepted | avg capture rows | kept state | verifier decode GPU | verifier head GPU | verifier total GPU | draft | commit | replay |
| --- | --- | --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| count | capture | match | 40.80 | 16 | 4.000 | 3.000 | 0.000 | 56.838 ms | 1.021 ms | 58.144 ms | 7.670 ms | 0.373 ms | 0.000 ms |
| count | auto | match | 41.95 | 16 | 4.000 | 0.375 | 0.875 | 55.023 ms | 1.023 ms | 56.400 ms | 7.767 ms | 0.064 ms | 0.000 ms |
| count | optimistic_full | match | 42.24 | 16 | 4.000 | 0.000 | 1.000 | 54.772 ms | 1.020 ms | 56.144 ms | 7.732 ms | 0.015 ms | 0.000 ms |
| count | hybrid_tail | match | 41.17 | 16 | 4.000 | 2.000 | 1.000 | 56.728 ms | 1.022 ms | 58.044 ms | 7.701 ms | 0.016 ms | 0.000 ms |
| explain | capture | match | 32.01 | 27 | 2.370 | 2.926 | 0.000 | 34.614 ms | 0.641 ms | 35.448 ms | 7.520 ms | 0.250 ms | 0.000 ms |
| explain | auto | match | 32.05 | 27 | 2.370 | 2.926 | 0.000 | 34.652 ms | 0.641 ms | 35.475 ms | 7.451 ms | 0.236 ms | 0.000 ms |
| explain | optimistic_full | match | 29.29 | 27 | 2.370 | 0.000 | 0.370 | 32.807 ms | 0.640 ms | 33.674 ms | 7.525 ms | 9.615 ms | 9.609 ms |
| explain | hybrid_tail | match | 32.46 | 27 | 2.370 | 1.926 | 0.370 | 34.076 ms | 0.640 ms | 34.902 ms | 7.488 ms | 0.104 ms | 0.000 ms |
| code | capture | match | 36.84 | 21 | 3.048 | 2.952 | 0.000 | 42.318 ms | 0.776 ms | 43.338 ms | 7.645 ms | 0.302 ms | 0.000 ms |
| code | auto | match | 35.83 | 21 | 3.048 | 2.667 | 0.048 | 42.111 ms | 0.776 ms | 43.122 ms | 7.631 ms | 3.022 ms | 2.761 ms |
| code | optimistic_full | match | 35.34 | 21 | 3.048 | 0.000 | 0.619 | 40.275 ms | 0.776 ms | 41.323 ms | 7.628 ms | 6.856 ms | 6.847 ms |
| code | hybrid_tail | match | 37.40 | 21 | 3.048 | 1.952 | 0.619 | 41.801 ms | 0.777 ms | 42.812 ms | 7.648 ms | 0.066 ms | 0.000 ms |

Validation drilldown:

- Artifact: `/tmp/ds4-hybrid-tail-validate-20260516_161522`
- Settings: count/explain/code, `K=4`, `-n 32`, `DS4_MTP_NATIVE_VALIDATE=1`,
  `DS4_MTP_NATIVE_TIMING=1`, `DS4_MTP_NATIVE_COMMIT_OPT=hybrid_tail`.
- All three prompts matched serial stdout.
- Accepted-row validation reported `max_delta=0` and `mismatches=0` for all three
  prompts.

Representative HumanEval+ 20-task A/B:

- Artifact: `/tmp/ds4-hybrid-tail-humaneval20-20260516_161653`
- Dataset: HumanEval+ first 20 tasks, chat API, `max_tokens=1024`.

| Mode | syntax | base pass@1 | plus pass@1 | tokens | elapsed | aggregate TPS | mean TPS | median TPS |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| native auto | 20/20 | 0.450 | 0.450 | 1394 | 56.63 s | 24.62 | 24.12 | 23.84 |
| native hybrid_tail | 20/20 | 0.450 | 0.450 | 1394 | 55.92 s | 24.93 | 24.43 | 24.04 |

Representative GSM8K 20-task A/B:

- Artifact: `/tmp/ds4-hybrid-tail-gsm8k20-20260516_161946`
- Dataset: first 20 tasks from
  `/tmp/ds4-smallm-gsm8k20-20260516_033442/gsm8k_test.jsonl`.

| Mode | accuracy | tokens | elapsed | aggregate TPS | mean TPS | median TPS |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| native auto | 1.000 | 1876 | 67.31 s | 27.87 | 26.94 | 26.51 |
| native hybrid_tail | 1.000 | 1876 | 67.44 s | 27.82 | 26.88 | 26.47 |

Decision:

- Keep `DS4_MTP_NATIVE_COMMIT_OPT=hybrid_tail` as a safe gated transaction probe.
- It is a real improvement for partial-accept-heavy prompt shapes: it removes
  `optimistic_full` replay, improves explain/code smoke versus `auto`, and gives a
  small positive HumanEval+ 20-task signal without changing quality.
- Do not promote it to default yet. It is worse than `auto`/`optimistic_full` on the
  all-full-accept count prompt, and GSM8K 20 is essentially flat/slightly negative.
- The next transaction step should be adaptive: choose zero-capture optimistic state
  after strong full-accept streaks, switch to hybrid tail-capture when partial accepts
  recur, and keep conservative capture for validation or unstable acceptance windows.

### 2026-05-16 Adaptive Commit Policy Probe

Implementation:

- Added `DS4_MTP_NATIVE_COMMIT_OPT=adaptive` as a gated transaction-policy probe.
- Added session-local streak counters:
  - `mtp_native_full_accept_streak`
  - `mtp_native_partial_accept_streak`
- Added `DS4_MTP_NATIVE_ADAPTIVE_OPTIMISTIC_STREAK=N`, default `8`.
- Production policy:
  - use `hybrid_tail` while the full-accept streak is below the threshold;
  - switch to `optimistic_full` only after `N` consecutive full accepts;
  - any partial accept resets the full streak and moves the next cycle back to
    `hybrid_tail`.
- Validation policy:
  - `DS4_MTP_NATIVE_VALIDATE=1` forces the effective commit mode to conservative
    `capture`, so accepted-row serial-oracle validation remains direct.
- Timing lines now include `adaptive_full_streak`, `adaptive_partial_streak`, and
  `adaptive_threshold`.

Rejected first attempt:

- Artifact: `/tmp/ds4-native-adaptive-20260516_162545`
- The initial policy switched to `optimistic_full` after only two full accepts.
- It matched serial stdout, but reproduced the replay problem on partial-accept
  prompts: `explain` averaged `2.144 ms` replay per cycle and `code` averaged
  `2.701 ms` replay per cycle.
- That threshold was too aggressive and was replaced by the default threshold-8
  policy above.

Local and Studio checks:

```sh
make ds4_test ds4 ds4-server
./ds4_test --metal-kernels
./ds4_test --metal-sched2
./ds4_test --metal-block-verifier
git diff --check -- ds4.c ds4_gpu.h ds4_metal.m metal/dsv4_hc.metal MTP_NATIVE_CACHE_CONTRACT_PROGRESS.md
```

The same build and Metal harness checks passed in the Studio worktree.

Three-prompt production/timing A/B:

- Artifact: `/tmp/ds4-native-adaptive-v2-20260516_162824`
- Settings: native small-M default router, `K=4`, `-n 64 --temp 0 --ctx 1024 --nothink -sys ""`.

| Prompt | Mode | stdout | t/s | cycles | avg accepted | avg capture rows | kept state | final full streak | final partial streak | verifier decode GPU | verifier head GPU | verifier total GPU | draft | commit | replay |
| --- | --- | --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| count | auto | match | 42.00 | 16 | 4.000 | 0.375 | 0.875 | 0 | 0 | 54.967 ms | 1.022 ms | 56.354 ms | 7.733 ms | 0.064 ms | 0.000 ms |
| count | hybrid_tail | match | 41.18 | 16 | 4.000 | 2.000 | 1.000 | 0 | 0 | 56.593 ms | 1.024 ms | 57.924 ms | 7.731 ms | 0.019 ms | 0.000 ms |
| count | adaptive | match | 41.74 | 16 | 4.000 | 1.000 | 1.000 | 16 | 0 | 55.526 ms | 1.025 ms | 56.893 ms | 7.747 ms | 0.016 ms | 0.000 ms |
| count | optimistic_full | match | 42.34 | 16 | 4.000 | 0.000 | 1.000 | 0 | 0 | 54.542 ms | 1.023 ms | 55.927 ms | 7.714 ms | 0.016 ms | 0.000 ms |
| explain | auto | match | 31.98 | 27 | 2.370 | 2.926 | 0.000 | 0 | 0 | 34.555 ms | 0.641 ms | 35.383 ms | 7.553 ms | 0.246 ms | 0.000 ms |
| explain | hybrid_tail | match | 32.45 | 27 | 2.370 | 1.926 | 0.370 | 0 | 0 | 33.883 ms | 0.643 ms | 34.717 ms | 7.609 ms | 0.106 ms | 0.000 ms |
| explain | adaptive | match | 32.38 | 27 | 2.370 | 1.926 | 0.370 | 3 | 0 | 34.086 ms | 0.642 ms | 34.921 ms | 7.533 ms | 0.104 ms | 0.000 ms |
| explain | optimistic_full | match | 29.25 | 27 | 2.370 | 0.000 | 0.370 | 0 | 0 | 32.877 ms | 0.641 ms | 33.742 ms | 7.496 ms | 9.624 ms | 9.619 ms |
| code | auto | match | 35.98 | 21 | 3.048 | 2.667 | 0.048 | 0 | 0 | 41.855 ms | 0.778 ms | 42.866 ms | 7.578 ms | 2.999 ms | 2.746 ms |
| code | hybrid_tail | match | 37.53 | 21 | 3.048 | 1.952 | 0.619 | 0 | 0 | 41.661 ms | 0.777 ms | 42.663 ms | 7.593 ms | 0.064 ms | 0.000 ms |
| code | adaptive | match | 37.52 | 21 | 3.048 | 1.952 | 0.619 | 3 | 0 | 41.611 ms | 0.779 ms | 42.619 ms | 7.580 ms | 0.061 ms | 0.000 ms |
| code | optimistic_full | match | 35.42 | 21 | 3.048 | 0.000 | 0.619 | 0 | 0 | 40.139 ms | 0.777 ms | 41.200 ms | 7.618 ms | 6.825 ms | 6.817 ms |

Validation drilldown:

- Artifact: `/tmp/ds4-adaptive-validate-20260516_162953`
- Settings: count/explain/code, `K=4`, `-n 32`, `DS4_MTP_NATIVE_VALIDATE=1`,
  `DS4_MTP_NATIVE_TIMING=1`, `DS4_MTP_NATIVE_COMMIT_OPT=adaptive`.
- All three prompts matched serial stdout.
- Accepted-row validation reported `max_delta=0` and `mismatches=0` for all three
  prompts.

Representative HumanEval+ 20-task A/B:

- Artifact: `/tmp/ds4-adaptive-humaneval20-20260516_163029`
- Dataset: HumanEval+ first 20 tasks, chat API, `max_tokens=1024`.

| Mode | syntax | base pass@1 | plus pass@1 | tokens | elapsed | aggregate TPS | mean TPS | median TPS |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| native auto | 20/20 | 0.450 | 0.450 | 1394 | 56.62 s | 24.62 | 24.12 | 23.84 |
| native adaptive | 20/20 | 0.450 | 0.450 | 1394 | 56.03 s | 24.88 | 24.38 | 24.00 |

Representative GSM8K 20-task A/B:

- Artifact: `/tmp/ds4-adaptive-gsm8k20-20260516_163325`
- Dataset: first 20 tasks from
  `/tmp/ds4-smallm-gsm8k20-20260516_033442/gsm8k_test.jsonl`.

| Mode | accuracy | tokens | elapsed | aggregate TPS | mean TPS | median TPS |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| native auto | 1.000 | 1876 | 67.37 s | 27.85 | 26.91 | 26.46 |
| native adaptive | 1.000 | 1876 | 67.31 s | 27.87 | 26.94 | 26.51 |

Decision:

- Keep `DS4_MTP_NATIVE_COMMIT_OPT=adaptive` as the best general transaction-policy
  candidate so far, but do not make it the default yet.
- It avoids the severe replay failures of `optimistic_full`, tracks the `hybrid_tail`
  benefit on partial-accept prompts, and recovers part of the full-accept benefit on
  count after the threshold is crossed.
- Representative slices are quality-stable and mildly positive versus `auto`, but the
  gain is small. Before any default promotion, run a larger mixed suite and tune
  `DS4_MTP_NATIVE_ADAPTIVE_OPTIMISTIC_STREAK` across at least `4,8,12` to see whether
  the threshold can improve count without reintroducing replay on explain/code.

### 2026-05-16 Adaptive Threshold Sweep

Purpose:

- Tune `DS4_MTP_NATIVE_ADAPTIVE_OPTIMISTIC_STREAK` now that the adaptive transaction
  mode is correct.
- The short prompt sweep is a smoke/A-B guide only. Representative HumanEval+/GSM8K
  slices decide whether the threshold signal is worth keeping.

Three-prompt threshold smoke:

- Artifact: `/tmp/ds4-adaptive-threshold-smoke-20260516_163736`
- Settings: native small-M default router, `K=4`, `-n 64 --temp 0 --ctx 1024 --nothink -sys ""`.

| Prompt | Mode | threshold | stdout | t/s | cycles | avg accepted | avg capture rows | kept state | verifier decode GPU | verifier total GPU | commit | replay |
| --- | --- | ---: | --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| count | auto | 0 | match | 41.96 | 16 | 4.000 | 0.375 | 0.875 | 55.020 ms | 56.411 ms | 0.064 ms | 0.000 ms |
| count | adaptive | 4 | match | 41.83 | 16 | 4.000 | 0.500 | 1.000 | 55.421 ms | 56.779 ms | 0.014 ms | 0.000 ms |
| count | adaptive | 8 | match | 41.63 | 16 | 4.000 | 1.000 | 1.000 | 55.721 ms | 57.061 ms | 0.014 ms | 0.000 ms |
| count | adaptive | 12 | match | 41.26 | 16 | 4.000 | 1.500 | 1.000 | 56.489 ms | 57.829 ms | 0.015 ms | 0.000 ms |
| explain | auto | 0 | match | 31.98 | 27 | 2.370 | 2.926 | 0.000 | 34.562 ms | 35.380 ms | 0.237 ms | 0.000 ms |
| explain | adaptive | 4 | match | 32.46 | 27 | 2.370 | 1.926 | 0.370 | 34.045 ms | 34.866 ms | 0.103 ms | 0.000 ms |
| explain | adaptive | 8 | match | 32.37 | 27 | 2.370 | 1.926 | 0.370 | 34.143 ms | 34.966 ms | 0.106 ms | 0.000 ms |
| explain | adaptive | 12 | match | 32.42 | 27 | 2.370 | 1.926 | 0.370 | 34.009 ms | 34.839 ms | 0.104 ms | 0.000 ms |
| code | auto | 0 | match | 35.73 | 21 | 3.048 | 2.667 | 0.048 | 42.221 ms | 43.257 ms | 3.047 ms | 2.783 ms |
| code | adaptive | 4 | match | 37.50 | 21 | 3.048 | 1.952 | 0.619 | 41.583 ms | 42.600 ms | 0.067 ms | 0.000 ms |
| code | adaptive | 8 | match | 37.46 | 21 | 3.048 | 1.952 | 0.619 | 41.645 ms | 42.649 ms | 0.067 ms | 0.000 ms |
| code | adaptive | 12 | match | 37.40 | 21 | 3.048 | 1.952 | 0.619 | 41.733 ms | 42.744 ms | 0.064 ms | 0.000 ms |

Smoke interpretation:

- Threshold `4` is the best short-prompt setting because it reaches optimistic mode
  earlier on the all-full-accept count prompt without reintroducing replay on
  explain/code.
- Threshold `12` is not useful in this sweep: it delays optimistic mode and does not
  improve the partial-accept prompts.
- Representative follow-up should compare `4` and `8`; `12` can be dropped from the
  next pass.

Representative HumanEval+ 20-task threshold A/B:

- Artifact: `/tmp/ds4-adaptive-threshold-humaneval20-20260516_163940`
- Dataset: HumanEval+ first 20 tasks, chat API, `max_tokens=1024`.

| Mode | syntax | base pass@1 | plus pass@1 | tokens | elapsed | aggregate TPS | mean TPS | median TPS |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| native auto | 20/20 | 0.450 | 0.450 | 1394 | 56.57 s | 24.64 | 24.14 | 23.87 |
| native adaptive t4 | 20/20 | 0.450 | 0.450 | 1394 | 56.26 s | 24.78 | 24.27 | 24.04 |
| native adaptive t8 | 20/20 | 0.450 | 0.450 | 1394 | 55.92 s | 24.93 | 24.43 | 24.03 |

Representative GSM8K 20-task threshold A/B:

- Artifact: `/tmp/ds4-adaptive-threshold-gsm8k20-20260516_164318`
- Dataset: first 20 tasks from
  `/tmp/ds4-smallm-gsm8k20-20260516_033442/gsm8k_test.jsonl`.

| Mode | accuracy | tokens | elapsed | aggregate TPS | mean TPS | median TPS |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| native auto | 1.000 | 1876 | 67.36 s | 27.85 | 26.91 | 26.46 |
| native adaptive t4 | 1.000 | 1876 | 67.46 s | 27.81 | 26.88 | 26.51 |
| native adaptive t8 | 1.000 | 1876 | 67.52 s | 27.78 | 26.85 | 26.42 |

Decision:

- Keep `DS4_MTP_NATIVE_COMMIT_OPT=adaptive` gated.
- Keep the default adaptive threshold at `8` for now. It was the best HumanEval+
  setting and remains safer than lower thresholds for avoiding replay-prone switches,
  but the GSM8K slice does not show a throughput win over `auto`.
- Do not promote adaptive to default from this threshold sweep. The candidate is
  correctness-clean and useful on coding/partial-accept prompt shapes, but the
  representative signal is still distribution-dependent and small.
- Next transaction work should either run a larger mixed suite before promotion or
  make the policy cost-aware using observed replay/capture timings rather than only
  full-accept streak length.

### 2026-05-16 Skip Redundant Prefix-Restore

Implementation:

- In the native verifier transaction path, captured-prefix commits no longer restore
  the whole pre-verifier frontier before committing the captured accepted-prefix
  state.
- The restore is still performed when it is semantically required:
  - verifier failure or first suffix rejection (`commit_drafts <= 1`);
  - `optimistic_full` partial accept, where the accepted suffix is replayed from the
    clean starting frontier;
  - explicit A/B rollback with `DS4_MTP_NATIVE_RESTORE_BEFORE_PREFIX_COMMIT=1`.
- Added `restore=... ms` to `DS4_MTP_NATIVE_TIMING=1` cycle lines so the transaction
  cost is visible instead of hidden in total wall time.

Rationale:

- For capture/hybrid/adaptive partial accepts with `commit_drafts > 1`,
  `spec_frontier_commit_block_prefix()` overwrites the live compressed-attention and
  ratio-4 indexer frontiers with the verifier-captured accepted-prefix state. The
  prior `spec_frontier_restore()` copied the start frontier and then immediately
  overwrote it with that captured prefix.
- Raw SWA rows are still not restored by `spec_frontier_restore()`. The existing
  verifier contract relies on the larger raw ring leaving speculative future rows as
  invisible garbage, so skipping the redundant compressed-frontier restore does not
  weaken the raw-cache contract.

Local and Studio checks:

```sh
make ds4_test ds4 ds4-server
./ds4_test --metal-kernels
./ds4_test --metal-sched2
./ds4_test --metal-block-verifier
./ds4_test --metal-mtp-cache-contract
git diff --check -- ds4.c
```

The same build and harness checks passed in the Studio worktree.

Three-prompt production/timing A/B:

- Artifact: `/tmp/ds4-native-skip-restore-smoke-20260516_165440`
- Settings: native small-M default router, adaptive commit, `K=4`,
  `-n 64 --temp 0 --ctx 1024 --nothink -sys ""`.
- `old_restore` sets `DS4_MTP_NATIVE_RESTORE_BEFORE_PREFIX_COMMIT=1`; `skip_restore`
  is the new default path.

| Prompt | Mode | stdout | t/s | cycles | avg accepted | avg capture rows | kept state | restore | commit | replay | verifier decode GPU | verifier total GPU |
| --- | --- | --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| count | serial | oracle | 37.20 | - | - | - | - | - | - | - | - | - |
| count | old_restore | match | 43.58 | 16 | 4.000 | 1.000 | 1.000 | 0.000 ms | 0.016 ms | 0.000 ms | 52.334 ms | 53.685 ms |
| count | skip_restore | match | 43.41 | 16 | 4.000 | 1.000 | 1.000 | 0.000 ms | 0.016 ms | 0.000 ms | 52.664 ms | 54.023 ms |
| explain | serial | oracle | 37.03 | - | - | - | - | - | - | - | - | - |
| explain | old_restore | match | 31.60 | 27 | 2.370 | 1.926 | 0.222 | 0.187 ms | 0.194 ms | 0.000 ms | 36.115 ms | 37.040 ms |
| explain | skip_restore | match | 31.78 | 27 | 2.370 | 1.926 | 0.222 | 0.000 ms | 0.202 ms | 0.000 ms | 35.992 ms | 36.911 ms |
| code | serial | oracle | 37.15 | - | - | - | - | - | - | - | - | - |
| code | old_restore | match | 39.47 | 19 | 3.316 | 2.000 | 0.632 | 0.081 ms | 0.092 ms | 0.000 ms | 44.512 ms | 45.614 ms |
| code | skip_restore | match | 39.58 | 19 | 3.316 | 2.000 | 0.632 | 0.000 ms | 0.094 ms | 0.000 ms | 44.597 ms | 45.707 ms |

Validation drilldown:

- Artifact: `/tmp/ds4-native-skip-restore-validate-20260516_165539`
- Settings: `DS4_MTP_NATIVE_VALIDATE=1`, native small-M, adaptive commit, `K=4`,
  `-n 32`.

| Prompt | stdout | cycles | max_delta | mismatches | restore |
| --- | --- | ---: | ---: | ---: | ---: |
| count | match | 8 | 0 | 0 | 0.000 ms |
| explain | match | 13 | 0 | 0 | 0.000 ms |
| code | match | 10 | 0 | 0 | 0.000 ms |

Representative HumanEval+ 20-task A/B:

- Artifact: `/tmp/ds4-native-skip-restore-humaneval20-20260516_165658`

| Mode | syntax | base pass@1 | plus pass@1 | tokens | elapsed | aggregate TPS | mean TPS | median TPS |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| old_restore | 20/20 | 1.000 | 1.000 | 2173 | 74.99 s | 28.98 | 27.87 | 29.28 |
| skip_restore | 20/20 | 1.000 | 1.000 | 2173 | 75.11 s | 28.93 | 27.82 | 29.19 |

Representative GSM8K 20-task A/B:

- Artifact: `/tmp/ds4-native-skip-restore-gsm8k20-20260516_170007`

| Mode | accuracy | tokens | elapsed | aggregate TPS | mean TPS | median TPS |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| old_restore | 1.000 | 2829 | 92.37 s | 30.63 | 29.71 | 29.55 |
| skip_restore | 1.000 | 2829 | 92.08 s | 30.72 | 29.79 | 29.65 |

Decision:

- Keep skip-redundant-restore as the default native transaction behavior.
- Keep `DS4_MTP_NATIVE_RESTORE_BEFORE_PREFIX_COMMIT=1` as the A/B rollback switch
  while this native track remains experimental.
- This is a correct efficiency cleanup, not an architecture-level promotion. It
  removes a proven redundant restore on partial captured-prefix commits, but the
  measured representative throughput change is small and distribution-dependent.
- Next high-upside transaction work is still the larger scratch/slot frontier
  rewrite that avoids snapshot/capture copying itself, not just the redundant
  restore after verification.

### 2026-05-16 Snapshot Elision Probe

Implementation:

- Added `DS4_MTP_NATIVE_SNAPSHOT_ELIDE=1` as a fail-fast production probe.
- The probe elides `spec_frontier_snapshot()` only when all of these are true:
  production/top-id mode (`DS4_MTP_NATIVE_VALIDATE` is off), top-1 acceptance,
  adaptive/captured-prefix commit is not using `optimistic_full`, and the legacy
  restore-before-prefix-commit rollback flag is off.
- If the verifier fails or the first suffix token cannot be accepted while the
  snapshot is elided, the path returns an error instead of trying to recover from
  a missing frontier snapshot.
- Added `snapshot_elided=...` to `DS4_MTP_NATIVE_TIMING=1` cycle lines.

Rationale:

- The previous default still took a full compressed/indexer frontier snapshot
  before running the verifier, even on cycles where the verifier-captured target
  prefix or kept verifier state would be the committed target state.
- Unlike the skipped restore optimization above, eliding the snapshot is only
  safe for cycles that can commit at least the first verifier row. A first-token
  reject needs the original frontier, so this remains an explicit probe rather
  than default behavior.

Local and Studio checks:

```sh
git diff --check -- ds4.c
make ds4_test ds4 ds4-server
./ds4_test --metal-kernels
./ds4_test --metal-sched2
./ds4_test --metal-block-verifier
./ds4_test --metal-mtp-cache-contract
```

The same build and harness checks passed in the Studio worktree.

Three-prompt production/timing A/B:

- Artifact: `/tmp/ds4-native-snapshot-elide-smoke-20260516_170909`
- Settings: native small-M default router, adaptive commit, `K=4`,
  `-n 64 --temp 0 --ctx 1024 --nothink -sys ""`.

| Prompt | serial t/s | default t/s | elide t/s | default stdout | elide stdout | cycles | elided cycles | default snapshot | elide snapshot | default verifier GPU | elide verifier GPU |
| --- | ---: | ---: | ---: | --- | --- | ---: | ---: | ---: | ---: | ---: | ---: |
| count | 37.13 | 43.43 | 43.60 | match | match | 16 | 8 | 0.396 ms | 0.199 ms | 53.971 ms | 53.841 ms |
| explain | 36.98 | 31.60 | 31.87 | match | match | 27 | 19 | 0.276 ms | 0.000 ms | 37.204 ms | 36.963 ms |
| code | 37.12 | 39.55 | 39.74 | match | match | 19 | 16 | 0.319 ms | 0.000 ms | 45.649 ms | 45.640 ms |

Validation drilldown:

- Artifact: `/tmp/ds4-native-snapshot-elide-validate-20260516_171013`
- Settings: `DS4_MTP_NATIVE_VALIDATE=1`,
  `DS4_MTP_NATIVE_SNAPSHOT_ELIDE=1`, native small-M, adaptive commit, `K=4`,
  `-n 64`.
- Validation mode intentionally disables snapshot elision so serial verifier-row
  comparisons can restore the pre-verifier frontier.

| Prompt | stdout | cycles | snapshot_elided | max_delta | mismatches | validation t/s |
| --- | --- | ---: | ---: | ---: | ---: | ---: |
| count | match | 16 | 0 | 0 | 0 | 22.80 |
| explain | match | 27 | 0 | 0 | 0 | 18.22 |
| code | match | 19 | 0 | 0 | 0 | 21.80 |

Decision:

- Keep `DS4_MTP_NATIVE_SNAPSHOT_ELIDE=1` as a gated experiment for now.
- Do not make it default from this smoke. It is correctness-clean in the three
  standard prompts, but the gain is only about 0.17-0.27 t/s and depends on how
  often cycles are eligible for elision.
- Do not spend representative HumanEval/GSM8K time on this isolated flag yet.
  Because the flag preserves exact stdout on the smoke and is not promoted, the
  next useful work is a larger scratch/slot frontier rewrite that removes more
  of the snapshot/capture copy family instead of shaving only the eligible
  snapshot call.
- Superseded by the 2026-05-16 snapshot-elision promotion below after the
  compute-copy capture change made the remaining transaction cost clearer.

### 2026-05-16 Slot-Promotion Prefix Commit Probe

Implementation:

- Added `DS4_MTP_NATIVE_SLOT_COMMIT=1` as a gated prefix-commit experiment.
- The normal captured-prefix commit copies verifier-produced compressor/indexer
  frontier tensors from `spec_block_*[accepted_prefix - 1]` back into the live
  target frontier. The slot-commit probe instead swaps the live frontier tensor
  pointers with the captured prefix slot and updates the live row counters.
- The old copy path remains the default and rollback path. The swap path is used
  only for captured block-prefix commits; full-verifier keep-state commits do
  not need it.
- Added `slot_commit=...` to `DS4_MTP_NATIVE_TIMING=1` cycle lines. It reports
  actual swap-path use, not merely that the flag was requested.

Rationale:

- Commit copying is small but measurable on partial-accept and forced-capture
  cycles. Swapping the live frontier with the captured prefix slot avoids the
  GPU blit work and leaves the old live speculative-suffix state in the scratch
  slot, which is overwritten by the next verifier capture before reuse.
- This is a direct step toward the slot-frontier design: it removes commit-copy
  cost, but it does not yet remove the verifier-time capture copies that fill the
  slots in the first place.

Local and Studio checks:

```sh
git diff --check -- ds4.c
make ds4_test ds4 ds4-server
./ds4_test --metal-kernels
./ds4_test --metal-sched2
./ds4_test --metal-block-verifier
./ds4_test --metal-mtp-cache-contract
```

The same build and harness checks passed in the Studio worktree.

Three-prompt production/timing A/B:

- Artifact: `/tmp/ds4-native-slot-commit-smoke-20260516_171445`
- Settings: native small-M default router, `K=4`,
  `-n 64 --temp 0 --ctx 1024 --nothink -sys ""`.

Forced capture mode isolates commit-copy cost:

| Prompt | copy t/s | slot t/s | stdout | copy commit | slot commit | slot cycles | verifier GPU copy | verifier GPU slot |
| --- | ---: | ---: | --- | ---: | ---: | ---: | ---: | ---: |
| count | 42.25 | 42.46 | match | 0.382 ms | 0.017 ms | 16/16 | 55.626 ms | 55.580 ms |
| explain | 31.36 | 31.50 | match | 0.272 ms | 0.010 ms | 19/27 | 37.627 ms | 37.603 ms |
| code | 39.07 | 39.09 | match | 0.333 ms | 0.014 ms | 16/19 | 46.308 ms | 46.431 ms |

Adaptive production-shaped mode:

| Prompt | copy t/s | slot t/s | stdout | copy commit | slot commit | slot cycles | avg accepted | kept verifier state |
| --- | ---: | ---: | --- | ---: | ---: | ---: | ---: | ---: |
| count | 43.73 | 43.67 | match | 0.016 ms | 0.013 ms | 0/16 | 4.000 | 1.000 |
| explain | 31.78 | 31.92 | match | 0.188 ms | 0.010 ms | 13/27 | 2.370 | 0.222 |
| code | 39.52 | 39.63 | match | 0.094 ms | 0.013 ms | 4/19 | 3.316 | 0.632 |

Validation drilldown:

- Artifact: `/tmp/ds4-native-slot-commit-validate-20260516_171605`
- Settings: `DS4_MTP_NATIVE_VALIDATE=1`, `DS4_MTP_NATIVE_SLOT_COMMIT=1`,
  native small-M, adaptive commit, `K=4`, `-n 64`.

| Prompt | stdout | cycles | slot cycles | max_delta | mismatches | avg commit |
| --- | --- | ---: | ---: | ---: | ---: | ---: |
| count | match | 16 | 16 | 0 | 0 | 0.007 ms |
| explain | match | 27 | 19 | 0 | 0 | 0.006 ms |
| code | match | 19 | 16 | 0 | 0 | 0.006 ms |

Representative HumanEval+ 20-task A/B:

- Artifact: `/tmp/ds4-slot-commit-humaneval20-20260516_171722`
- Dataset: HumanEval+ first 20 tasks, chat API, `max_tokens=1024`.

| Mode | syntax | base pass@1 | plus pass@1 | tokens | elapsed | aggregate TPS | mean TPS | median TPS |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| adaptive copy | 20/20 | 1.000 | 1.000 | 2173 | 74.99 s | 28.98 | 27.87 | 29.26 |
| adaptive slot | 20/20 | 1.000 | 1.000 | 2173 | 75.09 s | 28.94 | 27.83 | 29.20 |

Representative GSM8K 20-task A/B:

- Artifact: `/tmp/ds4-slot-commit-gsm8k20-20260516_172034`
- Dataset: first 20 GSM8K test tasks from the existing local artifact
  `/tmp/ds4-smallm-gsm8k20-20260516_033442/gsm8k_test.jsonl`.

| Mode | accuracy | tokens | elapsed | aggregate TPS | mean TPS | median TPS |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| adaptive copy | 1.000 | 2332 | 77.66 s | 30.03 | 29.20 | 29.40 |
| adaptive slot | 1.000 | 2332 | 77.41 s | 30.12 | 29.29 | 29.36 |

Decision:

- Keep `DS4_MTP_NATIVE_SLOT_COMMIT=1` gated for now.
- The optimization is correctness-clean and removes the intended commit-copy cost
  when the captured-prefix commit path is used.
- It is not yet a default promotion: representative throughput is flat/noisy
  (`-0.04` aggregate TPS on HumanEval+ 20, `+0.10` aggregate TPS on GSM8K 20),
  because adaptive already keeps full verifier state on many high-accept cycles.
- Next transaction work should target verifier-time capture copies or a true
  multi-slot live frontier so capture and commit both become pointer/index
  operations rather than GPU copies.

### 2026-05-16 Compute-Copy Verifier Capture Promotion

Implementation:

- Added `ds4_gpu_tensor_copy_compute()` in the Metal backend. It copies F32 tensor
  ranges through the existing compute copy kernel instead of opening a Metal blit
  encoder.
- Verifier prefix/block capture helpers now use compute-copy by default for the
  small compressor/indexer frontier slots.
- Added `DS4_MTP_NATIVE_CAPTURE_COMPUTE_COPY_DISABLE=1` as the rollback/A-B flag.
- Native timing lines include `capture_compute_copy=...`.

Rationale:

- Capturing verifier-produced prefix state was still doing many small
  `ds4_gpu_tensor_copy()` blits inside the target verifier. Each blit closes the
  active compute encoder, opens a blit encoder, then returns to compute for the
  next layer/token work.
- The compute-copy path preserves exact bytes while keeping capture work in the
  compute stream. It does not eliminate the copy itself, but it removes a chunk
  of command-encoder churn from verifier-time capture.

Local and Studio checks:

```sh
git diff --check -- ds4.c ds4_gpu.h ds4_metal.m
make ds4_test ds4 ds4-server
./ds4_test --metal-kernels
./ds4_test --metal-sched2
./ds4_test --metal-block-verifier
./ds4_test --metal-mtp-cache-contract
```

The same build and harness checks passed in the Studio worktree.

Opt-in probe smoke before default flip:

- Artifact: `/tmp/ds4-native-capture-compute-smoke-20260516_172757`
- Settings: native small-M default router, `K=4`,
  `-n 64 --temp 0 --ctx 1024 --nothink -sys ""`,
  `DS4_MTP_NATIVE_SLOT_COMMIT=1`.

Forced `capture` mode with slot commit isolates capture cost:

| Prompt | blit t/s | compute t/s | stdout | blit verify | compute verify | blit decode GPU | compute decode GPU |
| --- | ---: | ---: | --- | ---: | ---: | ---: | ---: |
| count | 42.48 | 42.92 | match | 59.001 ms | 58.113 ms | 54.225 ms | 54.083 ms |
| explain | 31.48 | 31.87 | match | 39.861 ms | 39.082 ms | 36.637 ms | 36.338 ms |
| code | 39.29 | 39.48 | match | 49.080 ms | 48.440 ms | 45.125 ms | 45.099 ms |

Adaptive production-shaped mode with slot commit:

| Prompt | blit t/s | compute t/s | stdout | blit verify | compute verify | blit decode GPU | compute decode GPU |
| --- | ---: | ---: | --- | ---: | ---: | ---: | ---: |
| count | 43.46 | 43.83 | match | 56.756 ms | 56.225 ms | 52.538 ms | 52.325 ms |
| explain | 31.83 | 31.99 | match | 39.111 ms | 38.658 ms | 36.056 ms | 35.936 ms |
| code | 39.67 | 40.05 | match | 48.214 ms | 47.463 ms | 44.441 ms | 44.068 ms |

Opt-in validation:

- Artifact: `/tmp/ds4-native-capture-compute-validate-20260516_172905`
- Settings: `DS4_MTP_NATIVE_VALIDATE=1`,
  `DS4_MTP_NATIVE_CAPTURE_COMPUTE_COPY=1`, native small-M, adaptive commit,
  slot commit, `K=4`, `-n 64`.

| Prompt | stdout | cycles | compute cycles | max_delta | mismatches |
| --- | --- | ---: | ---: | ---: | ---: |
| count | match | 16 | 16 | 0 | 0 |
| explain | match | 27 | 27 | 0 | 0 |
| code | match | 19 | 19 | 0 | 0 |

Representative HumanEval+ 20-task A/B:

- Artifact: `/tmp/ds4-capture-compute-humaneval20-20260516_172939`
- Dataset: HumanEval+ first 20 tasks, chat API, `max_tokens=1024`.

| Mode | syntax | base pass@1 | plus pass@1 | tokens | elapsed | aggregate TPS | mean TPS | median TPS |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| slot blit | 20/20 | 1.000 | 1.000 | 2173 | 76.03 s | 28.58 | 27.48 | 29.12 |
| slot compute | 20/20 | 1.000 | 1.000 | 2173 | 75.29 s | 28.86 | 27.76 | 29.20 |

Representative GSM8K 20-task A/B:

- Artifact: `/tmp/ds4-capture-compute-gsm8k20-20260516_173246`
- Dataset: first 20 GSM8K test tasks from the existing local artifact
  `/tmp/ds4-smallm-gsm8k20-20260516_033442/gsm8k_test.jsonl`.

| Mode | accuracy | tokens | elapsed | aggregate TPS | mean TPS | median TPS |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| slot blit | 1.000 | 2332 | 78.33 s | 29.77 | 28.96 | 29.16 |
| slot compute | 1.000 | 2332 | 77.85 s | 29.96 | 29.12 | 29.28 |

Default-vs-disable smoke after promotion:

- Artifact: `/tmp/ds4-capture-compute-default-smoke-20260516_173633`
- Default means compute-copy enabled; disabled sets
  `DS4_MTP_NATIVE_CAPTURE_COMPUTE_COPY_DISABLE=1`.

| Prompt | default t/s | disabled t/s | stdout | default verify | disabled verify | default compute cycles |
| --- | ---: | ---: | --- | ---: | ---: | ---: |
| count | 43.30 | 43.08 | match | 56.829 ms | 57.324 ms | 16/16 |
| explain | 31.64 | 31.40 | match | 39.202 ms | 39.540 ms | 27/27 |
| code | 39.26 | 39.11 | match | 48.502 ms | 48.828 ms | 19/19 |

Default validation:

- Artifact: `/tmp/ds4-capture-compute-default-validate-20260516_173717`
- Settings: `DS4_MTP_NATIVE_VALIDATE=1`, native small-M, adaptive commit,
  slot commit, `K=4`, `-n 64`.

| Prompt | stdout | cycles | compute cycles | max_delta | mismatches |
| --- | --- | ---: | ---: | ---: | ---: |
| count | match | 16 | 16 | 0 | 0 |
| explain | match | 27 | 27 | 0 | 0 |
| code | match | 19 | 19 | 0 | 0 |

Decision:

- Promote compute-copy verifier capture to the native default.
- Keep `DS4_MTP_NATIVE_CAPTURE_COMPUTE_COPY_DISABLE=1` as the rollback/A-B flag
  while this native track remains experimental.
- This is a real verifier-efficiency promotion: it is exact in validation,
  improves verifier wall time on the three-prompt smoke, and improves both
  representative 20-task slices without changing quality.
- It is still not an architecture-level MTP promotion. The next higher-upside
  capture work is a true multi-slot live frontier or in-kernel capture design
  that avoids copying frontier state at all.

### 2026-05-16 Snapshot-Elision Default Promotion

Implementation:

- Promoted safe native snapshot elision to the default production path.
- Replaced the opt-in `DS4_MTP_NATIVE_SNAPSHOT_ELIDE=1` probe with
  `DS4_MTP_NATIVE_SNAPSHOT_ELIDE_DISABLE=1` as the rollback/A-B flag.
- The elision predicate is still narrow:
  - production mode only (`DS4_MTP_NATIVE_VALIDATE` off);
  - exact top-1 acceptance (`native_approx_topk == 1`);
  - no forced restore-before-prefix-commit rollback;
  - not `optimistic_full` replay mode;
  - the first suffix token is already known accepted from the serial target
    logits before the verifier mutates target state.
- Validation mode intentionally keeps the snapshot, so verifier rows can still be
  restored and compared against the serial target oracle.

Rationale:

- At this point the native loop already knows the first suffix token is accepted
  before entering the verifier branch. If a later suffix row fails, the runtime
  commits verifier-captured prefix state; if all rows accept, it keeps verifier
  state. In both cases the original compressed/indexer frontier snapshot is
  redundant.
- First-suffix reject cycles never enter this branch, so they still keep the
  original frontier and do not need snapshot recovery.
- A verifier execution failure while elided remains fail-fast, which is correct
  for an internal graph error rather than a normal accept/reject outcome.

Local and Studio checks:

```sh
git diff --check -- ds4.c
make ds4_test ds4 ds4-server
./ds4_test --metal-kernels
./ds4_test --metal-sched2
./ds4_test --metal-block-verifier
./ds4_test --metal-mtp-cache-contract
```

The same checks passed in the Studio worktree.

Default-vs-disable smoke:

- Artifact: `/tmp/ds4-snapshot-elide-default-smoke-20260516_174352`
- Settings: native small-M, adaptive commit, slot commit, compute-copy capture
  default, `K=4`, `-n 64 --temp 0 --ctx 1024 --nothink -sys ""`.
- Disabled mode sets `DS4_MTP_NATIVE_SNAPSHOT_ELIDE_DISABLE=1`.

| Prompt | default t/s | disabled t/s | stdout | elided cycles | default snapshot | disabled snapshot | default verify | disabled verify |
| --- | ---: | ---: | --- | ---: | ---: | ---: | ---: | ---: |
| count | 43.41 | 43.20 | match | 8/16 | 0.224 ms | 0.406 ms | 56.798 ms | 57.051 ms |
| explain | 30.60 | 30.52 | match | 18/29 | 0.000 ms | 0.241 ms | 35.169 ms | 35.209 ms |
| code | 35.53 | 35.45 | match | 18/21 | 0.000 ms | 0.328 ms | 48.126 ms | 48.051 ms |

Validation drilldown:

- Artifact: `/tmp/ds4-snapshot-elide-default-validate-20260516_174504`
- Settings: `DS4_MTP_NATIVE_VALIDATE=1`, native small-M, adaptive commit, slot
  commit, `K=4`, `-n 64`.

| Prompt | stdout | cycles | snapshot_elided | max_delta | mismatches |
| --- | --- | ---: | ---: | ---: | ---: |
| count | match | 16 | 0 | 0 | 0 |
| explain | match | 29 | 0 | 0 | 0 |
| code | match | 21 | 0 | 0 | 0 |

Representative HumanEval+ 20-task A/B:

- Artifact: `/tmp/ds4-snapshot-elide-default-humaneval20-20260516_174558`
- Dataset: HumanEval+ first 20 tasks, chat API, `max_tokens=1024`.

| Mode | syntax | base pass@1 | plus pass@1 | tokens | elapsed | aggregate TPS | mean TPS | median TPS |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| snapshot default | 20/20 | 0.900 | 0.900 | 1492 | 56.38 s | 26.46 | 24.95 | 26.03 |
| snapshot disabled | 20/20 | 0.900 | 0.900 | 1492 | 56.60 s | 26.36 | 24.86 | 25.98 |

Representative GSM8K 20-task A/B:

- Artifact: `/tmp/ds4-snapshot-elide-default-gsm8k20-20260516_174834`
- Dataset: first 20 GSM8K test tasks from the existing local artifact
  `/tmp/ds4-smallm-gsm8k20-20260516_033442/gsm8k_test.jsonl`.

| Mode | accuracy | tokens | elapsed | aggregate TPS | mean TPS | median TPS |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| snapshot default | 1.000 | 2025 | 69.99 s | 28.93 | 28.11 | 27.88 |
| snapshot disabled | 1.000 | 2025 | 70.19 s | 28.85 | 28.04 | 27.86 |

Decision:

- Promote snapshot elision to the native default.
- Keep `DS4_MTP_NATIVE_SNAPSHOT_ELIDE_DISABLE=1` as the rollback/A-B flag while
  this native track remains experimental.
- This is a small but verified state-transaction efficiency improvement: exact
  smoke and validation pass, and both representative 20-task slices are
  quality-equal with a small positive TPS signal.
- This still does not close the architecture-level MTP decision. The next
  higher-upside work remains eliminating verifier-time capture copies via a
  true multi-slot live frontier or in-kernel capture, because snapshot elision
  only removes the pre-verifier copy on cycles whose first suffix is already
  accepted.

### 2026-05-16 Prefix1-Replay Capture Policy Probe

Hypothesis:

- Hybrid/adaptive commit still captures more verifier prefix state than it needs
  on many partial-accept cycles. A narrower policy might capture only the first
  suffix prefix and use exact replay only when a later partial accept needs a
  deeper prefix.
- This would trade one captured slot every verifier cycle for occasional serial
  replay on deeper partial accepts.

Implementation tried:

- Added a temporary `DS4_MTP_NATIVE_COMMIT_OPT=prefix1_replay` mode.
- The mode captured only one block-prefix slot, kept verifier state on full
  accept, committed the captured first suffix prefix when only one suffix token
  was accepted, and restored/replayed accepted suffix tokens for deeper partial
  accepts.
- The first smoke exposed a transaction bug in the temporary mode: replayed
  suffix tokens were appended twice by the common commit loop. Fixing the replay
  bookkeeping restored exact stdout.

Corrected smoke:

- Artifact: `/tmp/ds4-prefix1-replay-smoke-fix-20260516_175531`
- Settings: native small-M, slot commit, compute-copy capture default, `K=4`,
  `-n 64 --temp 0 --ctx 1024 --nothink -sys ""`.
- Baseline is current `DS4_MTP_NATIVE_COMMIT_OPT=adaptive`.

| Prompt | adaptive t/s | prefix1 t/s | stdout | adaptive capture rows | prefix1 capture rows | prefix1 replay | prefix1 snapshot |
| --- | ---: | ---: | --- | ---: | ---: | ---: | ---: |
| count | 43.29 | 43.21 | match | 1.000 | 1.000 | 0.000 ms | 0.393 ms |
| explain | 30.60 | 24.27 | match | 1.931 | 1.000 | 19.115 ms | 0.247 ms |
| code | 35.46 | 30.09 | match | 2.000 | 1.000 | 15.654 ms | 0.340 ms |

Decision:

- Drop the prefix1-replay policy and remove the temporary code.
- The accepted-prefix replay cost swamps the capture-copy savings on the partial
  prompts that matter. It also disables snapshot elision because the replay path
  needs a saved frontier, which adds another fixed cost.
- This is useful negative evidence: the next capture optimization should not
  fall back to serial target replay for deeper partial accepts. It needs either
  real multi-slot frontier state or in-kernel capture so the accepted prefix can
  be promoted by pointer/index movement without replay.

### 2026-05-16 Native Checkpoint Mutation Cleanup and Fused-Top1 Audit

Implementation cleanup:

- Removed the temporary speculative `s->checkpoint` appends immediately before
  native block verification in both the root-inclusive and normal suffix paths.
- The verifier receives the draft block and start position directly, and the
  common commit path rewinds/appends the accepted tokens after verification. The
  pre-verifier checkpoint mutation was therefore redundant hot-loop work and an
  unnecessary transient state change.

Local and Studio harness checks:

```sh
git diff --check -- ds4.c MTP_NATIVE_CACHE_CONTRACT_PROGRESS.md
make ds4_test ds4 ds4-server
./ds4_test --metal-kernels
./ds4_test --metal-sched2
./ds4_test --metal-block-verifier
./ds4_test --metal-mtp-cache-contract
```

The same build and harness checks passed in the Studio worktree after syncing
`ds4.c`.

Production smoke:

- Artifact: `/tmp/ds4-native-eff-cleanup-smoke-20260516_180338`
- Settings: serial target vs native small-M default vs native small-M with
  `DS4_MTP_NATIVE_VERIFY_SINGLE_COMMAND=1`
  `DS4_MTP_NATIVE_OUTPUT_FUSED_TOP1=1`; `K=4`, `-n 64 --temp 0 --ctx 1024
  --nothink -sys ""`.

| Prompt | serial t/s | native t/s | combined t/s | native stdout | combined stdout |
| --- | ---: | ---: | ---: | --- | --- |
| count | 35.46 | 41.42 | 41.48 | match | match |
| explain | 35.48 | 32.59 | 32.60 | match | match |
| code | 35.45 | 37.55 | 37.54 | match | match |

Native timing summary:

| Prompt | Mode | cycles | avg accepted | avg verify | avg decode | avg head | single/fused cycles |
| --- | --- | ---: | ---: | ---: | ---: | ---: | ---: |
| count | native | 16 | 4.000 | 60.288 ms | 58.946 ms | 1.323 ms | 0/0 |
| count | combined | 16 | 4.000 | 60.007 ms | 2.209 ms | 57.781 ms | 16/16 |
| explain | native | 27 | 2.370 | 36.491 ms | 35.660 ms | 0.816 ms | 0/0 |
| explain | combined | 27 | 2.370 | 36.356 ms | 1.399 ms | 34.944 ms | 17/16 |
| code | native | 21 | 3.048 | 44.948 ms | 43.934 ms | 1.000 ms | 0/0 |
| code | combined | 21 | 3.048 | 44.771 ms | 1.652 ms | 43.106 ms | 16/16 |

Validation and fused-top1 audit:

- Artifact: `/tmp/ds4-native-eff-cleanup-validate-20260516_180546`
- `DS4_MTP_NATIVE_VALIDATE=1` full-logit validation matched serial stdout on
  all three prompts and reported `mismatches=0`, `max_delta=0`.
- `DS4_MTP_NATIVE_OUTPUT_FUSED_TOP1_AUDIT=1` also matched serial stdout on all
  three prompts and reported `mismatches=0`, proving the fused-top1 acceptance
  result is bit-safe for these rows.

| Prompt | validate stdout | validate mismatches | validate max_delta | fused audit stdout | fused audit mismatches |
| --- | --- | ---: | ---: | --- | ---: |
| count | match | 0 | 0 | match | 0 |
| explain | match | 0 | 0 | match | 0 |
| code | match | 0 | 0 | match | 0 |

Decision:

- Keep the checkpoint cleanup. It removes redundant hot-loop state mutation and
  preserves exact output.
- Keep `DS4_MTP_NATIVE_VERIFY_SINGLE_COMMAND=1` and
  `DS4_MTP_NATIVE_OUTPUT_FUSED_TOP1=1` gated for now. They are exact under the
  audit path, but this smoke shows flat production throughput: the timing moves
  cost accounting from `verifier_decode` to `verifier_head` without reducing the
  overall verifier span.
- Do not run a representative promotion matrix for the combined flags yet. They
  are not a default candidate from the smoke signal. The next higher-value target
  remains the real small-M verifier decode span: target block verification still
  costs about 45-60 ms on accepting cycles, dwarfing checkpoint/logit readback
  overhead.

### 2026-05-16 Small-M Dense Final-Reduction Probe

Hypothesis:

- The Q8/F16 rows2/3/4 verifier kernels were doing redundant final reductions:
  after each simdgroup wrote its partial to threadgroup memory, every simdgroup
  ran the same final `simd_sum`, even though only simdgroup 0 lane 0 wrote the
  result.
- A bit-preserving version can let only simdgroup 0 run that final 32-lane
  reduction while keeping the exact same lane order and output write.

Implementation tried:

- Temporarily changed the shared-memory final reduction in
  `metal/dense.metal` for the generic matvec helper, Q8 rows2/3/4, Q8
  rows2/3/4 top1-final, and F16 rows2/3/4.
- The change was compiled and run on Studio, then reverted because the timing
  signal was negative/mixed.

Local and Studio checks:

```sh
git diff --check -- metal/dense.metal ds4.c MTP_NATIVE_CACHE_CONTRACT_PROGRESS.md
make ds4_test ds4 ds4-server
./ds4_test --metal-kernels
```

The same build and `--metal-kernels` check passed on Studio.

Production smoke while the patch was applied:

- Artifact: `/tmp/ds4-native-smallm-reduce-smoke-20260516_180931`
- Settings: serial target vs native small-M default vs combined
  single-command/fused-top1; `K=4`, `-n 64 --temp 0 --ctx 1024 --nothink
  -sys ""`.

| Prompt | serial t/s | native t/s | combined t/s | native stdout | combined stdout |
| --- | ---: | ---: | ---: | --- | --- |
| count | 34.39 | 39.01 | 40.61 | match | match |
| explain | 35.66 | 32.92 | 32.94 | match | match |
| code | 35.79 | 36.12 | 36.75 | match | match |

Compared with the prior same-shape smoke
`/tmp/ds4-native-eff-cleanup-smoke-20260516_180338`, native default regressed on
`count` and `code` and only moved within noise on `explain`.

Validation and audit while the patch was applied:

- Artifact: `/tmp/ds4-native-smallm-reduce-validate-20260516_181028`
- Full validation matched serial stdout with `mismatches=0`, `max_delta=0` on
  all three prompts.
- Fused-top1 audit also matched serial stdout with `mismatches=0` on all three
  prompts.

| Prompt | validate stdout | validate mismatches | validate max_delta | fused audit stdout | fused audit mismatches |
| --- | --- | ---: | ---: | --- | ---: |
| count | match | 0 | 0 | match | 0 |
| explain | match | 0 | 0 | match | 0 |
| code | match | 0 | 0 | match | 0 |

Decision:

- Drop and revert the dense final-reduction probe.
- The idea was bit-clean, but the production timing was not a win. Avoiding
  redundant simdgroup final reductions is not where this verifier is losing
  meaningful time, or it perturbs occupancy enough to erase the theoretical
  savings.
- Do not revisit this exact patch unless a Metal shader counter profile shows
  final threadgroup reduction traffic as a real bottleneck.

### 2026-05-16 Native Small-M Row-View Elision

Implementation cleanup:

- In `metal_graph_verify_decode_exact()`, the native small-M prebatch verifier
  with batched output head operates on `batch_cur_hc`/`batch_next_hc` base
  tensors directly.
- The function still allocated per-row `cur[]`/`next[]` tensor views and swapped
  them after every layer, even though the native small-M batched-output path
  never consumed those views.
- Added a `verifier_needs_row_views` guard so row views are created and swapped
  only for the per-row decode path or the row-head fallback. This removes hot
  CPU/object churn from the production native small-M verifier without changing
  GPU math.

Local and Studio checks:

```sh
git diff --check -- ds4.c metal/dense.metal MTP_NATIVE_CACHE_CONTRACT_PROGRESS.md
make ds4_test ds4 ds4-server
./ds4_test --metal-kernels
./ds4_test --metal-sched2
./ds4_test --metal-block-verifier
./ds4_test --metal-mtp-cache-contract
```

The same build and harness checks passed in the Studio worktree after syncing
`ds4.c`.

Production smoke:

- Artifact: `/tmp/ds4-native-rowview-elide-smoke-20260516_181317`
- Settings: serial target vs native small-M default vs combined
  single-command/fused-top1; `K=4`, `-n 64 --temp 0 --ctx 1024 --nothink
  -sys ""`.

| Prompt | serial t/s | native t/s | combined t/s | native stdout | combined stdout |
| --- | ---: | ---: | ---: | --- | --- |
| count | 35.70 | 41.49 | 42.13 | match | match |
| explain | 35.28 | 32.41 | 32.38 | match | match |
| code | 35.54 | 37.58 | 37.67 | match | match |

Native timing summary:

| Prompt | Mode | cycles | avg accepted | avg verify | avg decode | avg head | total avg |
| --- | --- | ---: | ---: | ---: | ---: | ---: | ---: |
| count | native | 16 | 4.000 | 60.154 ms | 58.834 ms | 1.304 ms | 96.236 ms |
| count | combined | 16 | 4.000 | 59.081 ms | 2.298 ms | 56.767 ms | 94.762 ms |
| explain | native | 27 | 2.370 | 36.853 ms | 36.030 ms | 0.812 ms | 72.988 ms |
| explain | combined | 27 | 2.370 | 36.666 ms | 1.383 ms | 35.271 ms | 73.042 ms |
| code | native | 21 | 3.048 | 45.055 ms | 44.043 ms | 1.000 ms | 80.935 ms |
| code | combined | 21 | 3.048 | 44.665 ms | 1.703 ms | 42.950 ms | 80.741 ms |

Validation:

- Artifact: `/tmp/ds4-native-rowview-elide-validate-20260516_181412`
- Full validation matched serial stdout on all three prompts and reported
  `mismatches=0`, `max_delta=0`.

| Prompt | validate stdout | mismatches | max_delta |
| --- | --- | ---: | ---: |
| count | match | 0 | 0 |
| explain | match | 0 | 0 |
| code | match | 0 | 0 |

Decision:

- Keep row-view elision as a native verifier cleanup.
- This is not an architecture-level win by itself; it is a small exact cleanup
  that removes avoidable hot-loop object churn and showed no correctness risk.
- Representative HumanEval/GSM evaluation remains required for the cumulative
  native candidate, not for declaring this single CPU-churn cleanup promotable.

Representative HumanEval+ 20-task current-candidate slice:

- Artifact: `/tmp/ds4-rowview-humaneval20-20260516_181552`
- Dataset: HumanEval+ first 20 tasks, chat API, `max_tokens=1024`.
- Model: `/Users/studio/.ds4/cache/gguf/DeepSeek-V4-Flash-Q4KExperts-F16HC-F16Compressor-F16Indexer-Q8Attn-Q8Shared-Q8Out-chat-v2.gguf`.
- Native mode: `DS4_MTP_NATIVE=1 DS4_MTP_NATIVE_VERIFY_OPT=smallm
  DS4_MTP_NATIVE_CACHE_MODE=owned DS4_MTP_NATIVE_COMMIT_OPT=adaptive
  DS4_MTP_NATIVE_SLOT_COMMIT=1 --mtp {MTP} --mtp-draft 4`.

| Mode | syntax | base pass@1 | plus pass@1 | tokens | elapsed | aggregate TPS | mean TPS | median TPS |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| serial | 20/20 | 0.450 | 0.450 | 1394 | 55.40 s | 25.16 | 24.53 | 24.70 |
| native rowview | 20/20 | 0.450 | 0.450 | 1394 | 55.79 s | 24.99 | 24.48 | 24.13 |

Representative GSM8K 20-task current-candidate slice:

- Artifact: `/tmp/ds4-rowview-gsm8k20-20260516_181845`
- Dataset: first 20 GSM8K test tasks from the existing local artifact
  `/tmp/ds4-smallm-gsm8k20-20260516_033442/gsm8k_test.jsonl`.
- Prompt required the model to end with `#### <answer>`.
- Same Q4KExperts model and native mode as the HumanEval+ slice.

| Mode | accuracy | tokens | elapsed | aggregate TPS | mean TPS | median TPS |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| serial | 1.000 | 2706 | 90.10 s | 30.03 | 29.51 | 29.09 |
| native rowview | 1.000 | 2706 | 91.40 s | 29.61 | 28.97 | 28.26 |

Representative decision:

- The cumulative exact native candidate remains quality-equal on these
  HumanEval+ and GSM8K 20-task slices, but it still does not beat serial
  throughput on either slice.
- Keep the row-view cleanup because it is exact and reduces needless CPU object
  work, but do not treat it as changing the broader architecture economics.
- The next performance work should target work that can materially reduce the
  45-60 ms accepting-cycle verifier span or reduce the number of verifier calls,
  not more host-side micro-cleanups.

### 2026-05-16 Prefix1 Serial Verifier-Call Count Probe

Question:

- Can exact native avoid the block verifier entirely when the first suffix draft
  already matches the current target frontier, by accepting only that first
  suffix token with a serial target advance?

Implementation:

- Added a temporary `DS4_MTP_NATIVE_PREFIX1_SERIAL=1` probe.
- The probe ran only in production mode, after target root decode, when
  `drafts[1]` matched the target frontier top id.
- It skipped suffix block verification, advanced target state for `drafts[1]`
  through the normal serial target path, committed two draft entries in the MTP
  ledger, and discarded the deeper speculative suffix.
- This is structurally exact for the accepted token because the accepted suffix
  token is the target top id and the target state is advanced by serial target
  decode. It intentionally sacrifices accepted depth to test verifier-call
  economics.

Studio smoke:

- Artifact: `/tmp/ds4-prefix1-serial-smoke-20260516_182507`
- Settings: native small-M default versus prefix1 serial; `K=4`, `-n 64
  --temp 0 --ctx 1024 --nothink -sys ""`.

| Prompt | serial t/s | native t/s | prefix1 t/s | native cycles | prefix1 cycles | native avg accepted | prefix1 avg accepted | native verify cycles | prefix1 verify cycles |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| count | 35.53 | 41.50 | 30.70 | 16 | 32 | 4.000 | 2.000 | 16 | 0 |
| explain | 35.28 | 32.49 | 29.80 | 27 | 39 | 2.370 | 1.641 | 17 | 0 |
| code | 35.48 | 37.38 | 30.43 | 21 | 34 | 3.048 | 1.853 | 16 | 0 |

Decision:

- Drop the prefix1 serial probe. It reduced verifier calls to zero in the
  smoke, but the accepted-depth collapse roughly doubled cycle count on count
  and was slower than both serial and native on all three prompts.
- The result is useful evidence: verifier-call reduction only helps if it keeps
  most of the native accepted prefix. Replacing a `K=4` accepting verifier with
  repeated exact one-token serial advances loses the core MTP advantage.
- Removed `DS4_MTP_NATIVE_PREFIX1_SERIAL` from the code after recording the
  measurement. The next viable call-count work needs to preserve depth, not
  trade block verification for serial prefix-only progression.

### 2026-05-16 Small-M Verifier Stage Profiler

Implementation:

- Added `DS4_METAL_SMALLM_STAGE_PROFILE=1`.
- The flag instruments only `metal_graph_encode_decode_smallm_layer_prebatch()`,
  the exact native small-M verifier body used for `M=2..4`.
- The profiler intentionally ends and restarts Metal command streams at stage
  boundaries, so it is diagnostic only and must not be used as an end-to-end TPS
  measurement.

Checks:

```sh
make ds4_test ds4 ds4-server
./ds4_test --metal-kernels
./ds4_test --metal-sched2
./ds4_test --metal-block-verifier
./ds4_test --metal-mtp-cache-contract
git diff --check -- ds4.c MTP_NATIVE_CACHE_CONTRACT_PROGRESS.md
```

The same build and harness checks passed on Studio.

Studio profiler artifact:

- Artifact: `/tmp/ds4-native-smallm-stage-profile-20260516_183347`
- Prompt: `explain`, `K=4`, `-n 16`, native small-M, adaptive commit,
  `DS4_MTP_NATIVE_TIMING=1`, `DS4_METAL_SMALLM_STAGE_PROFILE=1`.
- The run produced four accepting verifier calls at `tokens=3`, for
  `172` layer-stage samples.

Aggregated small-M stage time:

| Stage | samples | sum | avg per layer |
| --- | ---: | ---: | ---: |
| `attn_tail_rows` | 172 | 121.020 ms | 0.704 ms |
| `routed_moe` | 172 | 102.494 ms | 0.596 ms |
| `attn_batch_out` | 172 | 75.621 ms | 0.440 ms |
| `attn_qkv` | 172 | 57.845 ms | 0.336 ms |
| `shared_hc_post` | 172 | 52.427 ms | 0.305 ms |
| `ffn_hc_pre` | 172 | 43.713 ms | 0.254 ms |
| `router` | 172 | 42.730 ms | 0.248 ms |
| `attn_hc_pre` | 172 | 41.866 ms | 0.243 ms |
| `ffn_tail_rows` | 172 | 3.724 ms | 0.022 ms |
| `batch_ffn` | 172 | 3.710 ms | 0.022 ms |

Decision:

- Keep the profiler as a gated diagnostic tool.
- This confirms the next high-upside verifier work is not output-head readback
  or host object churn. The biggest remaining stage is `attn_tail_rows`, which
  still runs the per-row tail path for cache/compressor/indexer/attention and
  prefix capture after Q/KV have already been batched.
- The next implementation target should split and then reduce `attn_tail_rows`:
  first add substage evidence inside `metal_graph_encode_decode_layer_tail_from_qkv()`
  for small-M verifier calls, then prototype batching or slotting around the
  largest substage rather than continuing with full-layer aggregate guesses.

### 2026-05-16 Small-M Tail Substage Profiler

Implementation:

- Added `DS4_METAL_SMALLM_TAIL_STAGE_PROFILE=1`.
- The flag instruments the row-tail helper used by the small-M verifier after
  batched Q/KV setup. It splits the previous `attn_tail_rows` bucket into raw
  KV store, attention compressor, indexer, attention-head decode, and the
  non-head tail stages when those are active.
- Like `DS4_METAL_SMALLM_STAGE_PROFILE=1`, this is a sync-heavy diagnostic and
  must not be interpreted as throughput timing.

Checks:

```sh
make ds4_test ds4 ds4-server
./ds4_test --metal-kernels
./ds4_test --metal-sched2
./ds4_test --metal-block-verifier
./ds4_test --metal-mtp-cache-contract
git diff --check -- ds4.c MTP_NATIVE_CACHE_CONTRACT_PROGRESS.md
```

The same build and harness checks passed on Studio.

Studio profiler artifact:

- Artifact: `/tmp/ds4-native-smallm-tail-profile-20260516_183847`
- Prompt: `explain`, `K=4`, `-n 16`, native small-M, adaptive commit,
  `DS4_MTP_NATIVE_TIMING=1`, `DS4_METAL_SMALLM_STAGE_PROFILE=1`,
  `DS4_METAL_SMALLM_TAIL_STAGE_PROFILE=1`.

Aggregated tail substage time:

| Stage | samples | sum | avg per row/layer |
| --- | ---: | ---: | ---: |
| `attention_heads` | 516 | 144.737 ms | 0.280 ms |
| `attn_compressor` | 492 | 119.243 ms | 0.242 ms |
| `raw_store` | 516 | 119.058 ms | 0.231 ms |
| `indexer` | 492 | 64.365 ms | 0.131 ms |

Decision:

- The previous `attn_tail_rows` bucket is not one kernel. It is mostly repeated
  row-tail work: attention heads, compressor/indexer maintenance, and raw KV
  finalization/store.
- The first concrete optimization target is raw KV finalization/store because
  the small-M verifier already has all `M` KV rows available as a batch before
  entering the row loop.

### 2026-05-16 Small-M Batched KV Finalize/Raw-Store Probe

Implementation:

- Added `DS4_MTP_NATIVE_SMALLM_BATCH_KV_STORE=1`.
- Added a Metal kernel, `kernel_dsv4_kv_fp8_store_batch_f32`, plus
  `ds4_gpu_kv_fp8_store_raw_batch_tensor()`.
- The kernel applies the same decode-side FP8 non-RoPE round-trip and F16 raw
  cache store as the one-row `kernel_dsv4_kv_fp8_store_f32`, but for `M` small-M
  verifier rows in one dispatch.
- The verifier only uses the batch path when a conservative raw-ring safety
  check proves that pre-storing future verifier rows will not overwrite raw
  rows still needed by earlier verifier rows. Unsafe ring-wrap cases fall back
  to the existing per-row path.
- First smoke caught an implementation bug: the batch kernel token index was
  launched on the Metal y dimension while the scalar
  `threadgroup_position_in_grid` reads x. That produced exact stdout mismatches
  and a large slowdown. The dispatch was fixed to launch token rows on x before
  the results below.

Checks:

```sh
make ds4_test ds4 ds4-server
./ds4_test --metal-kernels
./ds4_test --metal-sched2
./ds4_test --metal-block-verifier
./ds4_test --metal-mtp-cache-contract
git diff --check -- ds4.c ds4_gpu.h ds4_metal.m metal/dsv4_kv.metal
```

The same build and harness checks passed on Studio.

Corrected Studio smoke:

- Artifact: `/tmp/ds4-native-batch-kv-smoke-20260516_184516`
- Settings: serial target, native small-M default, and native small-M with
  `DS4_MTP_NATIVE_SMALLM_BATCH_KV_STORE=1`; `K=4`, `-n 64 --temp 0 --ctx 1024
  --nothink -sys ""`.
- Batch-KV stdout matched serial on count, explain, and code.

| Prompt | serial t/s | native t/s | batch-KV t/s | stdout |
| --- | ---: | ---: | ---: | --- |
| count | 35.57 | 41.48 | 42.14 | match |
| explain | 35.36 | 32.56 | 33.06 | match |
| code | 35.54 | 37.48 | 37.91 | match |

Validation smoke:

- Artifact: `/tmp/ds4-native-batch-kv-validate-20260516_184606`
- Settings: batch-KV plus `DS4_MTP_NATIVE_VALIDATE=1`, `K=4`, `-n 64`.
- count, explain, and code all exited with status 0.

Longer native-vs-batch check:

- Artifact: `/tmp/ds4-native-batch-kv-n256-20260516_184714`
- Settings: native small-M default versus batch-KV, `K=4`, `-n 256`.
- Batch-KV stdout matched native on count, explain, and code.

| Prompt | native t/s | batch-KV t/s | stdout vs native |
| --- | ---: | ---: | --- |
| count | 38.40 | 38.97 | match |
| explain | 30.46 | 30.84 | match |
| code | 35.84 | 36.24 | match |

Representative HumanEval+ 20-task A/B:

- Artifact: `/tmp/ds4-batchkv-humaneval20-20260516_184953`
- Dataset: HumanEval+ first 20 tasks, chat API, `max_tokens=1024`.
- Batch-KV produced byte-identical generated solutions to native on all 20
  tasks.

| Mode | syntax | base pass@1 | plus pass@1 | tokens | elapsed | aggregate TPS | mean TPS | median TPS |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| native | 20/20 | 0.450 | 0.450 | 1394 | 55.62 s | 25.06 | 24.57 | 24.19 |
| batch-KV | 20/20 | 0.450 | 0.450 | 1394 | 55.25 s | 25.23 | 24.71 | 24.36 |

Representative GSM8K 20-task chat A/B:

- Artifact: `/tmp/ds4-batchkv-gsm8k20-20260516_185256`
- Dataset: first 20 tasks from
  `/tmp/ds4-smallm-gsm8k20-20260516_033442/gsm8k_test.jsonl`.
- This run used the server chat path without a server-side `--nothink` flag
  because `ds4-server` rejected `--nothink`. The prompt triggered hidden
  thinking for long completions, so this is useful quality/long-trajectory
  evidence but is not directly comparable to the earlier no-think CLI-style
  prompt slices.
- Batch-KV did not preserve byte-identical outputs here: all 20 outputs differed
  from native, with one prediction difference. Quality did not regress on this
  slice, but this is enough to keep the flag opt-in until the long chat path is
  understood.

| Mode | accuracy | tokens | elapsed | aggregate TPS | mean TPS | median TPS | failed |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | --- |
| native | 0.950 | 8214 | 271.69 s | 30.23 | 29.07 | 29.35 | 8 |
| batch-KV | 1.000 | 7292 | 245.42 s | 29.71 | 28.74 | 29.19 | none |

Follow-up on the chat divergence:

- Artifact: `/tmp/ds4-batchkv-server-repro-20260516_190900`
- Fresh one-request server repro showed:
  - default-thinking requests diverged between native and batch-KV.
  - request-level `think:false` matched exactly.
- Artifact: `/tmp/ds4-batchkv-server-serialcmp-20260516_191220`
- A fresh default-thinking serial/native/batch comparison showed that current
  native MTP already does not match serial target in this server default-thinking
  path. Therefore the earlier hidden-thinking GSM divergence is not valid
  evidence against batch-KV default promotion for the exact no-think native
  verifier path. It is a separate server/thinking-mode exactness issue.

Representative GSM8K 20-task request-level no-think A/B:

- Artifact: `/tmp/ds4-batchkv-gsm8k20-thinkfalse-20260516_191312`
- Dataset: first 20 tasks from
  `/tmp/ds4-smallm-gsm8k20-20260516_033442/gsm8k_test.jsonl`.
- The server request set `think:false`, which is the API equivalent of the
  no-think contract used by the standard CLI benchmarks.
- Batch-KV produced byte-identical outputs to native on all 20 tasks.

| Mode | accuracy | tokens | elapsed | aggregate TPS | mean TPS | median TPS | output diffs |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| native | 1.000 | 2804 | 94.19 s | 29.77 | 29.00 | 28.93 | 0 |
| batch-KV | 1.000 | 2804 | 93.06 s | 30.13 | 29.35 | 29.31 | 0 |

Default promotion:

- Changed `DS4_MTP_NATIVE_SMALLM_BATCH_KV_STORE=1` from opt-in to the default
  native small-M path.
- Added `DS4_MTP_NATIVE_SMALLM_BATCH_KV_STORE_DISABLE=1` as the rollback flag.
- Local and Studio checks passed after promotion:

```sh
make ds4_test ds4 ds4-server
./ds4_test --metal-kernels
./ds4_test --metal-sched2
./ds4_test --metal-block-verifier
./ds4_test --metal-mtp-cache-contract
git diff --check -- ds4.c ds4_gpu.h ds4_metal.m metal/dsv4_kv.metal MTP_NATIVE_CACHE_CONTRACT_PROGRESS.md
```

Default-vs-disable smoke after promotion:

- Artifact: `/tmp/ds4-batchkv-default-smoke-20260516_191732`
- Settings: native small-M default versus
  `DS4_MTP_NATIVE_SMALLM_BATCH_KV_STORE_DISABLE=1`, `K=4`, `-n 64 --nothink`.
- Default stdout matched disabled stdout on all three prompts.

| Prompt | default t/s | disabled t/s | stdout |
| --- | ---: | ---: | --- |
| count | 42.49 | 41.80 | match |
| explain | 33.41 | 33.05 | match |
| code | 38.17 | 37.52 | match |

Profile check:

- Artifact: `/tmp/ds4-native-batch-kv-stage-profile-20260516_184637`
- Same profile setup as the tail substage run, plus
  `DS4_MTP_NATIVE_SMALLM_BATCH_KV_STORE=1`.
- The profile is diagnostic only. It shows the expected movement from repeated
  row-store dispatches into a single `batch_kv_store` stage, although later
  `raw_store` buckets still include some synchronization wait from work issued
  between row-tail calls.

| Stage | samples | sum | avg |
| --- | ---: | ---: | ---: |
| `batch_kv_store` | 172 | 38.056 ms | 0.221 ms |
| `raw_store` | 516 | 73.011 ms | 0.141 ms |
| `attention_heads` | 516 | 140.292 ms | 0.272 ms |
| `attn_compressor` | 492 | 115.775 ms | 0.235 ms |
| `indexer` | 492 | 62.465 ms | 0.127 ms |

Decision:

- Promote batch-KV to the native small-M default. The rollback flag is
  `DS4_MTP_NATIVE_SMALLM_BATCH_KV_STORE_DISABLE=1`.
- The optimization is real but modest: about `+0.4` to `+0.7 t/s` on `-n64`,
  and about `+0.38` to `+0.57 t/s` on `-n256` in this smoke matrix.
- HumanEval+ and request-level no-think GSM8K are byte-identical to native and
  slightly faster.
- The default-thinking server divergence should be tracked separately because
  serial target and native MTP already diverge in that path; it is not caused by
  batch-KV alone and is outside the standard no-think verifier benchmark
  contract used here.
- The next efficiency work should target `attention_heads` and compressor/indexer
  batching or reduce capture waits now visible in the tail profile. Batch KV
  alone is a useful cleanup, not the full verifier-economics breakthrough.

### 2026-05-16 Post-Batch-KV Depth Re-Sweep

Rationale:

- Batch-KV changed per-row verifier economics, so the prior "best depth" was
  stale. A fresh depth sweep is required before choosing a representative eval
  candidate.
- This sweep uses the current default native small-M candidate: batch-KV on by
  default, `DS4_MTP_NATIVE_COMMIT_OPT=adaptive`, and
  `DS4_MTP_NATIVE_SLOT_COMMIT=1`.

Studio smoke artifact:

- `/tmp/ds4-post-batchkv-depth-sweep-20260516_192842`
- Settings: serial oracle plus native `K=2..5`; count/explain/code prompts,
  `-n 64 --temp 0 --ctx 1024 --nothink -sys ""`.
- Native settings:
  `DS4_MTP_NATIVE=1 DS4_MTP_NATIVE_VERIFY_OPT=smallm
  DS4_MTP_NATIVE_CACHE_MODE=owned DS4_MTP_NATIVE_COMMIT_OPT=adaptive
  DS4_MTP_NATIVE_SLOT_COMMIT=1 DS4_MTP_NATIVE_TIMING=1`.

Results:

| Prompt | Mode | t/s | stdout | cycles | avg accepted | verifier decode GPU | verifier total GPU | draft |
| --- | --- | ---: | --- | ---: | ---: | ---: | ---: | ---: |
| count | serial | 35.51 | oracle | - | - | - | - | - |
| count | K=2 | 31.65 | match | 32 | 2.000 | 28.005 ms | 29.216 ms | 3.885 ms |
| count | K=3 | 37.33 | match | 21 | 3.000 | 41.752 ms | 43.042 ms | 5.893 ms |
| count | K=4 | 42.06 | match | 16 | 4.000 | 54.827 ms | 56.218 ms | 7.807 ms |
| count | K=5 | 36.31 | match | 16 | 4.000 | 67.697 ms | 69.247 ms | 9.598 ms |
| explain | serial | 35.41 | oracle | - | - | - | - | - |
| explain | K=2 | 31.59 | match | 39 | 1.641 | 17.705 ms | 18.485 ms | 3.862 ms |
| explain | K=3 | 32.48 | match | 32 | 2.000 | 25.012 ms | 25.780 ms | 5.817 ms |
| explain | K=4 | 32.95 | match | 27 | 2.370 | 33.243 ms | 34.102 ms | 7.527 ms |
| explain | K=5 | 30.06 | match | 25 | 2.560 | 44.104 ms | 45.142 ms | 9.392 ms |
| code | serial | 35.53 | oracle | - | - | - | - | - |
| code | K=2 | 31.78 | match | 34 | 1.853 | 23.581 ms | 24.648 ms | 3.897 ms |
| code | K=3 | 36.93 | match | 24 | 2.667 | 34.895 ms | 35.975 ms | 5.831 ms |
| code | K=4 | 37.97 | match | 21 | 3.048 | 40.869 ms | 41.934 ms | 7.661 ms |
| code | K=5 | 33.19 | match | 19 | 3.368 | 59.103 ms | 60.493 ms | 9.369 ms |

Smoke interpretation:

- The user's concern was correct: post-batch-KV, K=4 is the smoke winner on all
  three prompts, and all K values matched serial stdout.
- K=5 accepts slightly more on explain/code, but the extra verifier and draft
  cost overwhelms the accepted-depth gain.
- K=4 should be the representative-eval candidate until a later optimization
  changes verifier economics again.

Representative HumanEval+ 20-task slice for K=4:

- Artifact: `/tmp/ds4-post-batchkv-k4-humaneval20-20260516_193046`
- Dataset: HumanEval+ first 20 tasks, chat API, `max_tokens=1024`,
  request-level `think:false`.

| Mode | syntax | base pass@1 | plus pass@1 | tokens | elapsed | aggregate TPS | mean TPS | median TPS |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| serial | 20/20 | 0.450 | 0.450 | 1394 | 56.43 s | 24.70 | 24.20 | 24.65 |
| native K=4 | 20/20 | 0.450 | 0.450 | 1394 | 55.30 s | 25.21 | 24.69 | 24.29 |

Representative GSM8K 20-task no-think slice for K=4:

- Artifact: `/tmp/ds4-post-batchkv-k4-gsm8k20-20260516_193357`
- Dataset: first 20 tasks from
  `/tmp/ds4-smallm-gsm8k20-20260516_033442/gsm8k_test.jsonl`.
- The API request set `think:false`.
- Native K=4 produced byte-identical outputs to serial on all 20 tasks.

| Mode | accuracy | tokens | elapsed | aggregate TPS | mean TPS | median TPS | output diffs |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| serial | 1.000 | 2377 | 80.65 s | 29.47 | 28.80 | 28.94 | - |
| native K=4 | 1.000 | 2377 | 80.72 s | 29.45 | 28.73 | 29.23 | 0 |

Updated decision:

- K=4 is now the current native small-M candidate after batch-KV.
- The representative slices are more encouraging than the pre-batch-KV
  candidate: HumanEval+ 20 is quality-equal and slightly faster than serial,
  while GSM8K 20 is quality-equal, byte-identical, and essentially flat.
- This is still not an architecture-level promotion. The evidence justifies
  continuing optimization and running larger/longer representative workloads,
  not declaring success from the smoke matrix.
- A brief opt-in raw-attention batch probe was tried and removed from the
  candidate: it matched count/code but diverged on explain and was slower there
  (`/tmp/ds4-smallm-rawattn-smoke-20260516_192414`;
  validation artifact `/tmp/ds4-smallm-rawattn-validate-20260516_192520`).
  The likely issue is that the existing batch raw-attention helper changes the
  effective reduction/mask shape by including future block rows under mask,
  which is not exact enough for greedy output.

### 2026-05-16 Post-Batch-KV Fused Top-1 Recheck

Rationale:

- After batch-KV shifted the native verifier economics and made `K=4` the
  current smoke winner, the older fused-output-top1 result had to be rechecked
  against the current candidate instead of judged from stale settings.
- This recheck used the same post-batch-KV native `K=4` settings as the depth
  sweep, adding only `DS4_MTP_NATIVE_OUTPUT_FUSED_TOP1=1`.

Studio artifacts:

- Production A/B: `/tmp/ds4-post-batchkv-k4-fused-top1-20260516_194040`
- Audit: `/tmp/ds4-post-batchkv-k4-fused-top1-audit-20260516_194126`

Production smoke:

| Prompt | fused off t/s | fused on t/s | stdout vs serial | off avg head GPU | on avg head GPU | off total verifier GPU | on total verifier GPU |
| --- | ---: | ---: | --- | ---: | ---: | ---: | ---: |
| count | 41.95 | 42.19 | match | 1.028 ms | 0.920 ms | 56.471 ms | 55.927 ms |
| explain | 32.90 | 32.83 | match | 0.643 ms | 0.579 ms | 34.207 ms | 34.120 ms |
| code | 37.85 | 37.94 | match | 0.781 ms | 0.697 ms | 42.205 ms | 41.907 ms |

Audit:

| Prompt | stdout vs serial | cycles | audit mismatches | max_delta |
| --- | --- | ---: | ---: | ---: |
| count | match | 16 | 0 | 0 |
| explain | match | 27 | 0 | 0 |
| code | match | 21 | 0 | 0 |

Decision:

- The fused top-1 path remains correctness-clean under the current post-batch-KV
  `K=4` candidate.
- It does not change the candidate selection or justify a representative
  promotion matrix by itself. The output-head cost reduction is real, but
  end-to-end throughput moved by only noise-scale amounts (`+0.24`, `-0.07`,
  `+0.09 t/s` across count/explain/code).
- Keep `DS4_MTP_NATIVE_OUTPUT_FUSED_TOP1=1` gated. The main remaining
  optimization surface is still verifier body math and transaction/capture
  overhead, not full-vocab top-id materialization alone.

### 2026-05-16 K=4 Small-M Stage Profile and Row-View Elision

Profile:

- Artifact: `/tmp/ds4-k4-smallm-stage-profile-20260516_194441`
- Settings: current native small-M `K=4` candidate with
  `DS4_METAL_SMALLM_STAGE_PROFILE=1`.
- The profiler inserts command boundaries and makes absolute TPS meaningless,
  but the per-layer stage ordering is useful.

| Prompt | attn tail rows | routed MoE | attn batch out | attn QKV | shared HC post | FFN HC pre | router | attn HC pre | batch KV store |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| count | 0.631 ms | 0.620 ms | 0.436 ms | 0.333 ms | 0.310 ms | 0.255 ms | 0.251 ms | 0.239 ms | 0.219 ms |
| explain | 0.639 ms | 0.587 ms | 0.431 ms | 0.329 ms | 0.294 ms | 0.252 ms | 0.251 ms | 0.235 ms | 0.218 ms |
| code | 0.629 ms | 0.582 ms | 0.430 ms | 0.335 ms | 0.298 ms | 0.251 ms | 0.250 ms | 0.241 ms | 0.223 ms |

Implementation:

- Removed unnecessary per-row tensor views in the small-M attention-tail-only
  path. The current native small-M verifier uses the batched attention-output
  route; for that route, the row tail only needs `attn_norm`, `qr_norm`, `q`,
  `kv`, and the destination heads row. It was still creating `cur`, `next`,
  `after_attn`, and HC split/pre/post/comb row views that are only consumed by
  the non-batched FFN tail.
- This does not change Metal math or resource order; it only reduces CPU-side
  view allocation/free churn in the verifier encoder.

Checks:

```sh
make ds4_test ds4
./ds4_test --metal-kernels
```

The same synced patch passed on Studio:

```sh
make ds4_test ds4 ds4-server
./ds4_test --metal-kernels
./ds4_test --metal-mtp-cache-contract
git diff --check -- ds4.c MTP_NATIVE_CACHE_CONTRACT_PROGRESS.md
```

Studio production smoke:

- Artifact: `/tmp/ds4-smallm-rowview-elide-smoke-20260516_194820`
- Settings: current native small-M `K=4`, `-n 64 --temp 0 --ctx 1024
  --nothink -sys ""`.

| Prompt | t/s | stdout vs serial | stdout vs previous K=4 | cycles | avg accept | verifier decode GPU | verifier total GPU |
| --- | ---: | --- | --- | ---: | ---: | ---: | ---: |
| count | 42.06 | match | match | 16 | 4.000 | 54.819 ms | 56.207 ms |
| explain | 32.97 | match | match | 27 | 2.370 | 33.306 ms | 34.169 ms |
| code | 37.93 | match | match | 21 | 3.048 | 41.098 ms | 42.160 ms |

Validation:

- Artifact: `/tmp/ds4-smallm-rowview-elide-validate-20260516_194847`

| Prompt | stdout | cycles | avg accept | mismatches | max_delta |
| --- | --- | ---: | ---: | ---: | ---: |
| count | match | 16 | 4.000 | 0 | 0 |
| explain | match | 27 | 2.370 | 0 | 0 |
| code | match | 21 | 3.048 | 0 | 0 |

Decision:

- Keep the row-view elision cleanup. It is exact and simplifies the current
  hot path.
- Do not treat it as a throughput lever. The smoke result is effectively flat
  versus the previous K=4 candidate, which is expected because the main
  remaining cost is GPU verifier body math.
- Next target: an exact batched attention-head/body path or routed-MoE math
  reduction with a boundary audit. The stage profile now points there more
  clearly than output-head or CPU view churn.

### 2026-05-16 Rejected Q4 Pair-SwiGLU Inline MoE Rewrite

Rationale:

- The small-M stage profile made routed MoE one of the largest remaining GPU
  costs, so the next experiment tried to remove the Q4 pair-SwiGLU gate/up
  scratch write/read by computing gate and up in one inline kernel and writing
  only the routed SwiGLU intermediate.
- This changed routed-MoE math, so it was treated as a boundary-audited
  candidate rather than a cleanup.

Artifacts:

- Initial broken smoke: `/tmp/ds4-k4-moe-nostore-smoke-20260516_195632`
- Corrected reduction smoke:
  `/tmp/ds4-k4-moe-inline-simd-sum-smoke-20260516_195919`
- Corrected validation:
  `/tmp/ds4-k4-moe-inline-simd-sum-validate-20260516_200000`
- Corrected MoE stage profile:
  `/tmp/ds4-k4-moe-inline-simd-sum-stage-profile-20260516_200032`

Audit notes:

- The first inline attempt was invalid: it called `simd_sum()` only inside the
  `tiisg == 0` write branch, so the SIMD reduction did not include all lanes.
  This produced obvious output collapse on all three smoke prompts.
- Moving the reduction back outside the lane-0 branch restored exact stdout and
  validation, but the rewrite was slower than the existing pair-SwiGLU path.

Corrected production smoke:

| Prompt | previous K=4 t/s | inline t/s | stdout vs serial | stdout vs previous K=4 | cycles | avg accept | inline verifier decode GPU | inline verifier total GPU |
| --- | ---: | ---: | --- | --- | ---: | ---: | ---: | ---: |
| count | 42.06 | 41.03 | match | match | 16 | 4.000 | 56.035 ms | 57.429 ms |
| explain | 32.97 | 32.08 | match | match | 27 | 2.370 | 33.993 ms | 34.859 ms |
| code | 37.93 | 37.10 | match | match | 21 | 3.048 | 41.658 ms | 42.727 ms |

Validation:

| Prompt | stdout | cycles | avg accept | mismatches | max_delta |
| --- | --- | ---: | ---: | ---: | ---: |
| count | match | 16 | 4.000 | 0 | 0 |
| explain | match | 27 | 2.370 | 0 | 0 |
| code | match | 21 | 3.048 | 0 | 0 |

MoE stage profile, corrected inline kernel:

| Prompt | tokens | gate_up | down | activation_weight | sum |
| --- | ---: | ---: | ---: | ---: | ---: |
| explain | 3 | 0.489 ms | 0.343 ms | 0.024 ms | 0.020 ms |
| code | 3 | 0.488 ms | 0.343 ms | 0.024 ms | 0.020 ms |

Decision:

- Rejected and removed from the candidate path.
- The corrected inline kernel was exact, but it regressed all three smoke
  prompts and made the profiled `gate_up` stage slower than the prior
  `~0.462 ms` baseline. The likely cost is extra register pressure / duplicated
  dequant bookkeeping outweighing the avoided scratch memory traffic.
- This rules out the naive no-scratch inline Q4 pair-SwiGLU rewrite as a
  default optimization. A future routed-MoE attempt should target less invasive
  batching or down-projection reuse rather than duplicating the existing Q4
  matvec body inside the pair-SwiGLU kernel.

### 2026-05-16 Rejected Batched Attention-Output/HC Fusion Hook

Rationale:

- Before writing a new attention-head batching path, the existing gated
  `DS4_MTP_VERIFIER_DECODE2_BATCH_ATTN_OUT_FUSED=1` branch was rechecked
  against the current post-batch-KV `K=4` candidate.
- This branch fuses the batched attention output projection with HC expansion,
  so in principle it could reduce the `attn_batch_out` stage without touching
  the attention-head decode itself.

Studio artifact:

- `/tmp/ds4-k4-attn-out-hc-fused-smoke-20260516_200505`

Production smoke:

| Prompt | fused t/s | stdout vs serial | stdout vs previous K=4 | cycles | avg accept | verifier decode GPU | verifier total GPU |
| --- | ---: | --- | --- | ---: | ---: | ---: | ---: |
| count | 28.33 | diff | diff | 27 | 2.333 | 42.891 ms | 43.950 ms |
| explain | 28.83 | diff | diff | 27 | 2.370 | 42.687 ms | 43.755 ms |
| code | 25.83 | diff | diff | 35 | 1.800 | 31.525 ms | 32.310 ms |

Decision:

- Rejected as a default and left gated.
- The branch is not output-exact in the current small-M native verifier shape,
  and the altered states also reduce acceptance. Do not use it as the next
  attention optimization.
- The remaining attention work should focus on exact attention-head batching or
  compressor/indexer batching with per-row visibility/top-k contracts, not this
  fused output/HC hook.

### 2026-05-16 Rejected Batched Attention-Compressor Projection

Rationale:

- `attn_tail_rows` still spends a large share of verifier time in repeated
  row-tail work. The first low-risk split tried to batch only the F16
  attention-compressor projections for the `M` verifier rows, while keeping the
  stateful compressor update and prefix captures sequential.
- This should have preserved the state contract if the projection rows were
  interchangeable with the current per-row paired projection.

Artifacts:

- Generic batched F16 projection smoke:
  `/tmp/ds4-k4-batch-attn-comp-proj-smoke-20260516_201034`
- Batched paired-F16 projection smoke:
  `/tmp/ds4-k4-batch-pair-attn-comp-proj-smoke-20260516_201408`

Results:

| Variant | Prompt | t/s | stdout vs serial | stdout vs previous K=4 | cycles | avg accept | verifier decode GPU |
| --- | --- | ---: | --- | --- | ---: | ---: | ---: |
| generic F16 batch | count | 26.69 | diff | diff | 38 | 1.658 | 24.140 ms |
| generic F16 batch | explain | 26.16 | diff | diff | 39 | 1.641 | 24.684 ms |
| generic F16 batch | code | 26.91 | diff | diff | 37 | 1.703 | 25.165 ms |
| paired F16 batch | count | 29.05 | diff | diff | 31 | 2.065 | 32.641 ms |
| paired F16 batch | explain | 26.09 | diff | diff | 35 | 1.800 | 30.735 ms |
| paired F16 batch | code | 27.64 | diff | diff | 34 | 1.882 | 29.828 ms |

Decision:

- Rejected and removed from the candidate path.
- The generic batched F16 matmul was not contract-equivalent to the per-row
  paired compressor projection and immediately changed output.
- Widening the paired F16 wrapper to run several token rows in one dispatch
  passed a local synthetic serial-vs-batch check, but the model-level verifier
  still diverged when the projections were moved before the sequential
  compressor updates. That means the safe optimization target is not simply
  "precompute all compressor projections"; the current tail has a tighter
  ordering contract than this split respected.
- Future compressor/indexer batching should move a complete proven subgraph
  together, including state visibility and top-k contracts, rather than moving
  only the projection producer.

### 2026-05-16 HumanEval/0 Small-M Correctness Regression

Rationale:

- The post-batch-KV `K=4` smoke sweep was correct on count/explain/code, but
  those prompts are not representative enough to prove promotion quality.
- Before spending more time on fused-top1 or other output-head details, a
  one-task HumanEval sanity check was run on Studio to compare serial,
  MTP-disabled, native small-M, and verifier variants.

Implementation note:

- Added `DS4_MTP_NATIVE_FULL_LOGITS=1` as a diagnostic flag. It keeps
  production acceptance but disables `top_only=1`, forcing verifier full-logit
  materialization without running the expensive serial validation loop. This
  isolates "top-only/readback" from "verifier top-id correctness".

Artifacts:

- Initial native/fused sanity:
  `/tmp/ds4-fusedtop1-humaneval0-sanity-20260516_202829`
- Native validate run:
  `/tmp/ds4-native-k4-humaneval0-validate-20260516_202935`
- Commit-mode probe:
  `/tmp/ds4-native-k4-humaneval0-commitmodes-20260516_203024`
- Full-logits diagnostic:
  `/tmp/ds4-native-k4-humaneval0-fulllogits-20260516_203433`
- Verify-opt comparison:
  `/tmp/ds4-native-humaneval0-verifyopts-20260516_203456`
- MTP-loaded-disabled control:
  `/tmp/ds4-humaneval0-mtpdisabled-20260516_203227`

One-task HumanEval/0 results:

| Mode | pass@1 | syntax | tokens | aggregate TPS | notes |
| --- | ---: | ---: | ---: | ---: | --- |
| serial | 1.000 | 1/1 | 61 | 23.09 | correct code |
| MTP-disabled | 1.000 | 1/1 | 61 | 23.09 | correct code |
| native small-M K=4 | 0.000 | 0/1 | 256 | 23.64 | diverged into invalid code |
| native small-M K=4 + validate | 1.000 | 1/1 | 61 | 6.89 | validation rejected bad suffix accepts |
| native small-M K=4 + full logits | 0.000 | 1/1 | 4 | 3.83 | still wrong; not just top-only readback |
| native exact verifier K=4 | 1.000 | 1/1 | 61 | 20.05 | correct |
| native attn_fused_routed K=4 | 1.000 | 1/1 | 61 | 20.11 | correct in this shape |

Diagnosis:

- The unsafe path is the current `smallm` verifier, not the MTP-loaded baseline
  or the native cache ledger as a whole.
- `DS4_MTP_NATIVE_VALIDATE=1` fixes the output by catching verifier rows whose
  top id does not match the serial target and reducing the commit to the serial
  root token. The timing log showed large verifier-vs-serial logit deltas
  (`max_delta` often tens of logits) and validation mismatches on this task.
- `DS4_MTP_NATIVE_FULL_LOGITS=1` did not fix output, so the issue is not simply
  that production skipped full-logit readback. The small-M verifier can produce
  invalid acceptance decisions on representative code-generation text.
- `verify_opt=exact` and `verify_opt=attn_fused_routed` both produced the same
  correct HumanEval/0 solution as serial/MTP-disabled, so the broader native
  cache contract is still salvageable. The optimized small-M kernel path is the
  suspect.

Decision:

- The post-batch-KV `smallm K=4` candidate is invalidated for promotion until
  the small-M verifier top-id mismatch is fixed or guarded.
- Do not run larger HumanEval/GSM promotion slices with `smallm` production mode
  as-is; they would be measuring a known correctness regression.
- Next implementation work should either:
  1. audit the small-M matvec/attention-tail path against the exact verifier on
     HumanEval/0 and repair the top-id mismatch, or
  2. temporarily fall back to the exact/attn_fused_routed verifier for
     representative-quality runs while keeping small-M behind an unsafe
     diagnostic flag.

### 2026-05-16 Corrective Post-Batch-KV Depth Re-Sweep

Rationale:

- Batch-KV changes per-row verifier economics, so the native depth choice must
  be re-swept instead of reusing the previous best depth.
- The three standard `-n 64` prompts remain smoke only. The smoke winner must
  then be checked on representative HumanEval/GSM slices.
- This supersedes the earlier K=4 post-batch-KV candidate section above,
  because the later HumanEval/0 regression and the current binary both show
  that K=3..5 small-M production accepts can diverge.

Artifacts:

- Three-prompt depth sweep:
  `/tmp/ds4-post-batchkv-depth-sweep-20260516_204015`
- HumanEval+ first-20 slice for K=2:
  `/tmp/ds4-post-batchkv-k2-humaneval20-20260516_204227`
- GSM8K first-20 no-think slice for K=2:
  `/tmp/ds4-post-batchkv-k2-gsm8k20-20260516_204606`

Settings:

- Base model:
  `/Users/studio/.ds4/cache/gguf/DeepSeek-V4-Flash-Q4KExperts-F16HC-F16Compressor-F16Indexer-Q8Attn-Q8Shared-Q8Out-chat-v2.gguf`
- MTP model:
  `/Users/studio/.ds4/cache/gguf/DeepSeek-V4-Flash-MTP-Q4K-Q8_0-F32.gguf`
- Native mode:
  `DS4_MTP_NATIVE=1 DS4_MTP_NATIVE_VERIFY_OPT=smallm DS4_MTP_NATIVE_CACHE_MODE=owned DS4_MTP_NATIVE_COMMIT_OPT=capture DS4_MTP_NATIVE_SLOT_COMMIT=1`

Three-prompt smoke:

| Prompt | Serial t/s | Current MTP t/s | K=2 t/s | K=2 stdout | K=3 t/s | K=3 stdout | K=4 t/s | K=4 stdout | K=5 t/s | K=5 stdout |
| --- | ---: | ---: | ---: | --- | ---: | --- | ---: | --- | ---: | --- |
| count | 35.49 | 34.76 | 31.07 | match | 31.98 | diff | 28.78 | diff | 27.61 | diff |
| explain | 35.38 | 34.52 | 30.95 | match | 30.64 | diff | 26.73 | diff | 24.70 | diff |
| code | 35.42 | 34.32 | 31.20 | match | 29.94 | diff | 26.13 | diff | 22.22 | diff |

Native timing summary:

| Prompt | K | cycles | avg accepted | avg verifier decode GPU |
| --- | ---: | ---: | ---: | ---: |
| count | 2 | 32 | 2.000 | 29.185 ms |
| count | 3 | 27 | 2.333 | 35.309 ms |
| count | 4 | 30 | 2.100 | 33.992 ms |
| count | 5 | 21 | 3.000 | 66.180 ms |
| explain | 2 | 40 | 1.600 | 17.043 ms |
| explain | 3 | 33 | 1.939 | 26.545 ms |
| explain | 4 | 35 | 1.829 | 29.788 ms |
| explain | 5 | 34 | 1.853 | 34.257 ms |
| code | 2 | 34 | 1.853 | 24.624 ms |
| code | 3 | 36 | 1.778 | 22.763 ms |
| code | 4 | 36 | 1.778 | 29.269 ms |
| code | 5 | 43 | 1.465 | 25.957 ms |

Representative HumanEval+ first-20:

| Mode | syntax | base pass@1 | plus pass@1 | tokens | elapsed | aggregate TPS | mean TPS | median TPS |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| serial | 20/20 | 0.450 | 0.450 | 1394 | 55.62 s | 25.06 | 24.44 | 24.57 |
| current MTP K=3 | 6/20 | 0.000 | 0.000 | 1247 | 52.05 s | 23.96 | 23.03 | 23.44 |
| native smallm K=2 | 20/20 | 0.450 | 0.450 | 1394 | 60.60 s | 23.00 | 22.44 | 22.52 |

Representative GSM8K first-20, request-level `think:false`:

| Mode | accuracy | tokens | elapsed | aggregate TPS | mean TPS | median TPS | failed |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | --- |
| serial | 0.950 | 2060 | 72.05 s | 28.59 | 27.70 | 27.64 | 8 |
| native smallm K=2 | 0.950 | 2060 | 78.95 s | 26.09 | 25.33 | 25.19 | 8 |

Interpretation:

- The fresh post-batch-KV depth sweep changes the working assumption: K=4 is
  not the current best candidate and is not even byte-exact on the smoke run.
- K=2 is the only smallm depth that matched serial stdout on all three standard
  prompts, so it was the correct winner to carry into representative slices.
- K=2 preserves HumanEval+/GSM8K quality against serial on these 20-task
  slices, including identical HumanEval failures and identical GSM8K failed
  task index.
- K=2 is still slower than serial on both representative slices:
  `25.06 -> 23.00` aggregate TPS on HumanEval+ and `28.59 -> 26.09` aggregate
  TPS on GSM8K.

Decision:

- Keep K=2 as the current safe smallm depth for representative-quality work.
- Do not treat any previous K=4 "best" claim as current after batch-KV and the
  small-M correctness regression.
- The next optimization target is still the small-M verifier correctness and
  economics: either fix K=3/K=4 top-id safety, or make the K=2 verifier cheap
  enough to beat serial on representative slices.

### 2026-05-16 Routed Small-M Safety Split

Rationale:

- The corrective depth sweep showed `smallm` K=3..5 could diverge, but it did
  not identify which optimized subpath was unsafe.
- The next isolation split compared exact native verification, attention-fused
  decode2 verification, routed attention-fused decode2 verification, and the
  broader `smallm` path on the HumanEval/0 task that catches bad accepts.

Implementation:

- Changed the implicit native verifier default from `attn_fused_routed` to
  `exact`.
- Added diagnostic `DS4_MTP_NATIVE_SMALLM_MATVEC_DISABLE=1`, which keeps the
  small-M graph shape but disables the custom rows2/3/4 small-M matvec kernels.

Artifacts:

- Batch-KV/router A/B:
  `/tmp/ds4-smallm-k4-component-humaneval0-20260516_205400`
- Small-M matvec-disable A/B:
  `/tmp/ds4-smallm-matvec-disable-humaneval0-20260516_205538`
- Native verify-opt split:
  `/tmp/ds4-native-verifyopt-k3-humaneval0-20260516_205702`
- Default-safety and attention-fused split:
  `/tmp/ds4-native-default-verifyopt-humaneval0-20260516_205840`
- K=3 attention-fused smoke:
  `/tmp/ds4-native-k3-attnfused-smoke-20260516_205924`
- K=3 attention-fused HumanEval+ first-20:
  `/tmp/ds4-native-k3-attnfused-humaneval20-20260516_210026`
- K=3 attention-fused GSM8K first-20:
  `/tmp/ds4-native-k3-attnfused-gsm8k20-20260516_210257`

HumanEval/0 isolation:

| Mode | pass@1 | syntax | tokens | aggregate TPS | Result |
| --- | ---: | ---: | ---: | ---: | --- |
| native K=3 exact | 1.000 | 1/1 | 61 | 21.33 | safe |
| native K=3 default after patch | 1.000 | 1/1 | 61 | 21.35 | safe, now exact |
| native K=3 attn_fused | 1.000 | 1/1 | 61 | 22.65 | safe on this task |
| native K=3 attn_fused_routed | 0.000 | 1/1 | 4 | 4.09 | wrong completion |
| native K=3 smallm | 0.000 | 0/1 | 128 | 25.50 | invalid completion |
| native K=4 smallm default | 0.000 | 0/1 | 128 | 22.31 | invalid completion |
| native K=4 smallm no batch-KV | 0.000 | 0/1 | 21 | 12.62 | invalid completion |
| native K=4 smallm no batch-router | 0.000 | 0/1 | 128 | 19.90 | invalid completion |
| native K=4 smallm no rows matvec | 0.000 | 0/1 | 13 | 9.46 | invalid completion |

K=3 attention-fused three-prompt smoke:

| Prompt | Serial t/s | Native exact t/s | Native attn_fused t/s | stdout | exact decode GPU | attn_fused decode GPU |
| --- | ---: | ---: | ---: | --- | ---: | ---: |
| count | 35.31 | 31.46 | 34.46 | match | 56.727 ms | 48.193 ms |
| explain | 35.45 | 27.26 | 29.36 | match | 40.547 ms | 34.803 ms |
| code | 35.34 | 31.31 | 33.75 | match | 47.162 ms | 40.686 ms |

Representative HumanEval+ first-20:

| Mode | syntax | base pass@1 | plus pass@1 | tokens | elapsed | aggregate TPS | mean TPS | median TPS |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| serial | 20/20 | 0.450 | 0.450 | 1394 | 55.57 s | 25.08 | 24.46 | 24.64 |
| native K=3 attn_fused | 20/20 | 0.450 | 0.450 | 1394 | 57.91 s | 24.07 | 23.51 | 23.70 |

Representative GSM8K first-20, request-level `think:false`:

| Mode | accuracy | tokens | elapsed | aggregate TPS | mean TPS | median TPS | failed |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | --- |
| serial | 0.950 | 2060 | 71.95 s | 28.63 | 27.73 | 27.62 | 8 |
| native K=3 attn_fused | 0.950 | 2060 | 76.26 s | 27.01 | 26.18 | 26.21 | 8 |

Interpretation:

- The unsafe boundary is routed-MoE batching/direct-sum inside the optimized
  verifier path, not batch-KV, batch-router, or the custom rows2/3/4 matvec
  kernels alone.
- `attn_fused` without routed batching is currently the safest optimized M=2
  verifier candidate: it preserved stdout on the three-prompt smoke and matched
  serial quality on HumanEval+ and GSM8K first-20.
- The economics are still insufficient. `attn_fused` improves native exact
  verifier GPU time by about `5.7-8.5 ms` per verifier cycle on the smoke, but
  remains slower than serial on representative HumanEval+ and GSM8K slices.

Decision:

- Native default stays exact until an optimized verifier mode is representative
  quality-safe.
- Keep `attn_fused` as an explicit candidate for further M=2 efficiency work.
- Treat `attn_fused_routed` and `smallm` routed paths as unsafe until the routed
  MoE math is audited and fixed with bit/ULP evidence against the row-exact
  verifier.

### 2026-05-16 Current-Binary Depth Re-Sweep After Routed Guard

Rationale:

- The earlier post-batch-KV re-sweep was the right corrective move, but it was
  invalidated again by the routed-MoE safety split: the current binary no
  longer uses the unsafe tiny batched pair-SwiGLU fusion by default.
- The depth choice therefore needed a fresh `--mtp-draft 2,3,4,5` sweep on the
  same three-prompt smoke before selecting a representative HumanEval/GSM
  candidate.
- The three-prompt smoke remains a smoke signal only; representative quality
  and throughput are recorded below for the smoke winner.

Implementation note:

- The tiny batched routed pair-SwiGLU fusion is now opt-in via
  `DS4_METAL_MOE_BATCH_PAIR_SWIGLU=1`. The default `smallm` routed path uses
  the safer unfused routed computation.
- Native default remains exact. This sweep explicitly used
  `DS4_MTP_NATIVE_VERIFY_OPT=smallm` to test the optimized candidate affected by
  batch-KV economics.

Artifacts:

- Three-prompt current-binary depth sweep:
  `/tmp/ds4-current-smallm-depth-sweep-20260516_212128`
- K=4 accepted-row validation:
  `/tmp/ds4-current-smallm-k4-validate-20260516_213127`
- K=4 HumanEval+ first-20 slice:
  `/tmp/ds4-current-smallm-k4-humaneval20-20260516_212317`
- K=4 GSM8K first-20 no-think slice:
  `/tmp/ds4-current-smallm-k4-gsm8k20-20260516_212705`

Settings:

- Base model:
  `/Users/studio/.ds4/cache/gguf/DeepSeek-V4-Flash-Q4KExperts-F16HC-F16Compressor-F16Indexer-Q8Attn-Q8Shared-Q8Out-chat-v2.gguf`
- MTP model:
  `/Users/studio/.ds4/cache/gguf/DeepSeek-V4-Flash-MTP-Q4K-Q8_0-F32.gguf`
- Native mode:
  `DS4_MTP_NATIVE=1 DS4_MTP_NATIVE_VERIFY_OPT=smallm DS4_MTP_NATIVE_CACHE_MODE=owned DS4_MTP_NATIVE_COMMIT_OPT=capture DS4_MTP_NATIVE_SLOT_COMMIT=1`

Three-prompt smoke:

| Prompt | Serial t/s | Current MTP t/s | K=2 t/s | K=2 stdout | K=3 t/s | K=3 stdout | K=4 t/s | K=4 stdout | K=5 t/s | K=5 stdout |
| --- | ---: | ---: | ---: | --- | ---: | --- | ---: | --- | ---: | --- |
| count | 35.39 | 38.00 | 31.20 | match | 36.47 | match | 40.86 | match | 35.94 | match |
| explain | 35.43 | 34.60 | 31.09 | match | 31.15 | match | 29.70 | match | 28.55 | match |
| code | 35.32 | 35.46 | 31.19 | match | 36.19 | match | 37.40 | match | 32.81 | match |

Native timing summary:

| Prompt | K | cycles | avg accepted | avg verifier decode GPU |
| --- | ---: | ---: | ---: | ---: |
| count | 2 | 32 | 2.000 | 29.191 ms |
| count | 3 | 21 | 3.000 | 43.811 ms |
| count | 4 | 16 | 4.000 | 57.502 ms |
| count | 5 | 16 | 4.000 | 68.822 ms |
| explain | 2 | 40 | 1.600 | 17.026 ms |
| explain | 3 | 30 | 2.133 | 31.209 ms |
| explain | 4 | 27 | 2.370 | 40.409 ms |
| explain | 5 | 27 | 2.370 | 41.698 ms |
| code | 2 | 34 | 1.853 | 24.738 ms |
| code | 3 | 24 | 2.667 | 36.178 ms |
| code | 4 | 21 | 3.048 | 42.164 ms |
| code | 5 | 19 | 3.368 | 60.359 ms |

K=4 validation:

| Prompt | cycles | avg accepted | max mismatches | max delta | avg verifier decode GPU |
| --- | ---: | ---: | ---: | ---: | ---: |
| count | 16 | 4.000 | 0 | 0 | 57.004 ms |
| explain | 27 | 2.370 | 0 | 0 | 40.414 ms |
| code | 21 | 3.048 | 0 | 0 | 42.209 ms |

Representative HumanEval+ first-20:

| Mode | syntax | base pass@1 | plus pass@1 | tokens | elapsed | aggregate TPS | mean TPS | median TPS |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| serial | 20/20 | 0.450 | 0.450 | 1394 | 55.52 s | 25.11 | 24.48 | 24.64 |
| current MTP K=3 | 20/20 | 0.450 | 0.450 | 1346 | 54.56 s | 24.67 | 24.01 | 23.92 |
| native smallm K=4 | 20/20 | 0.450 | 0.450 | 1394 | 55.86 s | 24.95 | 24.46 | 24.07 |

Representative GSM8K first-20, request-level `think:false`:

| Mode | accuracy | tokens | elapsed | aggregate TPS | mean TPS | median TPS | failed |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | --- |
| serial | 1.000 | 2265 | 77.45 s | 29.25 | 28.49 | 28.37 | none |
| current MTP K=3 | 1.000 | 2219 | 77.00 s | 28.82 | 28.05 | 27.96 | none |
| native smallm K=4 | 1.000 | 2265 | 78.75 s | 28.76 | 27.95 | 28.23 | none |

Interpretation:

- The user's concern was correct, and the current-binary result supersedes the
  earlier corrective sweep: after the routed guard, K=4 is again byte-clean on
  the smoke matrix and is the smoke throughput winner overall.
- K=4 also passes accepted-row validation on the smoke prompts with
  `max_delta=0` and `mismatches=0`, so the previous K=4 correctness regression
  is isolated to the old unsafe routed fusion, not to K=4 as a depth by itself.
- The representative slices are quality-preserving on this first-20 sample, but
  K=4 is still essentially flat/slightly slower than serial:
  `25.11 -> 24.95` aggregate TPS on HumanEval+ and `29.25 -> 28.76` aggregate
  TPS on GSM8K.

Decision:

- Keep native `smallm K=4` as the current optimized candidate for continued
  efficiency work. It is no longer invalidated by the earlier K=4 smoke/HumanEval
  regression.
- Do not call it promotable from this evidence: the smoke win does not carry to
  the representative slices yet.
- Next optimization should focus on turning the now-correct K=4 path into an
  actual representative-speed win, especially by reducing verifier decode time
  or avoiding the remaining serial-root plus verifier double cost.

### 2026-05-16 Native Default Transaction Stack Promotion

Rationale:

- The current-binary K=4 re-sweep used conservative `capture` commit to isolate
  verifier correctness after the routed guard. That was a good safety baseline,
  but it was not the strongest exact native transaction stack.
- The earlier transaction work had already shown that adaptive commit and chain
  drafting reduce capture/replay/draft overhead without changing verifier math.
  This pass recomposed those pieces against the current safe K=4 small-M verifier.

Implementation:

- Changed native mode's default commit policy from `capture` to `adaptive`.
  Rollback/control remains explicit with `DS4_MTP_NATIVE_COMMIT_OPT=capture`.
- Changed native chain drafting from opt-in to default-on for owned-cache native
  mode. Rollback is `DS4_MTP_NATIVE_CHAIN_DRAFT_DISABLE=1`.
- Changed slot commit from opt-in to default-on. Rollback is
  `DS4_MTP_NATIVE_SLOT_COMMIT_DISABLE=1`.
- Removed the validation-only chain-draft guard, so
  `DS4_MTP_NATIVE_VALIDATE=1` now validates the same chain-draft source used by
  production native default.
- Kept `DS4_MTP_NATIVE_OUTPUT_FUSED_TOP1=1` gated. It remains exact and useful
  as a probe, but the current default promotion is based on adaptive commit,
  chain drafting, and slot commit only.

Artifacts:

- K=4 stack sweep before default patch:
  `/tmp/ds4-current-k4-stack-sweep-20260516_213531`
- Chain+fused validation before default patch:
  `/tmp/ds4-current-k4-chain-fused-validate-20260516_213800`
- Chain+fused HumanEval+ first-20:
  `/tmp/ds4-current-k4-chain-fused-humaneval20-20260516_213846`
- Chain+fused GSM8K first-20:
  `/tmp/ds4-current-k4-chain-fused-gsm8k20-20260516_214216`
- Default adaptive+chain smoke after patch:
  `/tmp/ds4-native-default-adaptive-chain-smoke-20260516_214836`
- Default chain validation after patch:
  `/tmp/ds4-native-default-chain-validate-20260516_215022`
- Default adaptive+chain HumanEval+ first-20:
  `/tmp/ds4-native-default-adaptive-chain-humaneval20-20260516_215107`
- Default adaptive+chain GSM8K first-20:
  `/tmp/ds4-native-default-adaptive-chain-gsm8k20-20260516_215331`

K=4 stack smoke before default patch:

| Prompt | Serial | Capture | Adaptive | Adaptive t4 | Adaptive fused | Adaptive chain | Adaptive chain+fused |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| count | 35.39 | 41.08 | 41.80 | 42.06 | 42.11 | 42.39 | 42.58 |
| explain | 35.43 | 29.62 | 30.01 | 29.96 | 29.72 | 30.15 | 30.00 |
| code | 35.35 | 37.21 | 37.78 | 37.67 | 37.90 | 38.15 | 38.22 |

All stack-sweep variants matched serial stdout. Correctly parsed timing showed
that chain drafting reduced draft time from about `7.6-7.8 ms/cycle` to about
`6.8-7.0 ms/cycle`, while adaptive commit reduced capture rows and avoided
replay on the partial-accept prompts.

Default adaptive+chain smoke after patch:

| Prompt | Serial | New default | Rollback capture/no-chain/no-slot | stdout | avg accepted | avg verifier decode GPU | avg draft | avg total |
| --- | ---: | ---: | ---: | --- | ---: | ---: | ---: | ---: |
| count | 35.36 | 42.24 | 41.01 | match | 4.000 | 55.241 ms | 6.973 ms | 94.516 ms |
| explain | 35.56 | 30.50 | 29.69 | match | 2.370 | 39.395 ms | 6.724 ms | 77.545 ms |
| code | 35.29 | 38.25 | 37.22 | match | 3.048 | 41.220 ms | 6.846 ms | 79.506 ms |

Default chain validation:

| Prompt | stdout | cycles | avg accepted | chain draft | mismatches | max delta |
| --- | --- | ---: | ---: | ---: | ---: | ---: |
| count | match | 8 | 4.000 | 1.000 | 0 | 0 |
| explain | match | 11 | 2.818 | 1.000 | 0 | 0 |
| code | match | 9 | 3.444 | 1.000 | 0 | 0 |

Representative HumanEval+ first-20, patched default:

| Mode | syntax | base pass@1 | plus pass@1 | tokens | elapsed | aggregate TPS | mean TPS | median TPS |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| serial | 20/20 | 0.450 | 0.450 | 1394 | 55.65 s | 25.05 | 24.42 | 24.55 |
| native default K=4 | 20/20 | 0.450 | 0.450 | 1394 | 54.75 s | 25.46 | 24.93 | 24.62 |

Representative GSM8K first-20, request-level `think:false`, patched default:

| Mode | accuracy | tokens | elapsed | aggregate TPS | mean TPS | median TPS | failed |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | --- |
| serial | 1.000 | 2265 | 77.08 s | 29.386 | 28.612 | 28.385 | none |
| native default K=4 | 1.000 | 2265 | 77.07 s | 29.391 | 28.550 | 28.878 | none |

Checks:

```sh
make ds4_test ds4 ds4-server
./ds4_test --metal-kernels
./ds4_test --metal-sched2
./ds4_test --metal-block-verifier
./ds4_test --metal-mtp-cache-contract
git diff --check -- ds4.c MTP_NATIVE_CACHE_CONTRACT_PROGRESS.md
```

The same synced `ds4.c` and progress doc passed on Studio:

```sh
make ds4 ds4-server ds4_test
./ds4_test --metal-kernels
./ds4_test --metal-sched2
./ds4_test --metal-block-verifier
./ds4_test --metal-mtp-cache-contract
git diff --check -- ds4.c MTP_NATIVE_CACHE_CONTRACT_PROGRESS.md
```

Decision:

- Promote adaptive commit, chain drafting, and slot commit to the default inside
  the already experimental `DS4_MTP_NATIVE=1` path.
- This is not the final architecture-level promotion yet. The result is now
  representative-positive on the HumanEval+ first-20 slice and flat/quality-equal
  on GSM8K first-20, but the larger goal still needs broader eval coverage and
  more verifier-body cost reduction.
- Next target remains GPU verifier body math: attention-head/compressor/indexer
  work and routed-MoE reductions are still the dominant costs after transaction
  overhead is trimmed.

### 2026-05-16 Fixed-Depth Post-Batch-KV Re-Sweep

Rationale:

- After batch-KV and the default transaction stack promotion, the previous depth
  choice was stale. The verifier per-row economics changed, so `--mtp-draft`
  needed a fresh sweep before carrying a depth into representative eval.
- This sweep explicitly disabled the MTP governor with
  `DS4_MTP_GOVERNOR_DISABLE=1`. That makes the run a fixed-depth comparison of
  `--mtp-draft 2,3,4,5`, not a production-cap measurement where the governor can
  silently reduce the effective depth after low-acceptance cycles.

Artifacts:

- Fixed-depth three-prompt sweep:
  `/tmp/ds4-post-batchkv-depth-sweep-20260516_220115`
- HumanEval+ 50-task slice:
  `/tmp/ds4-post-batchkv-humaneval50-20260516_220318`
- GSM8K 50-task no-think slice:
  `/tmp/ds4-post-batchkv-gsm8k50-20260516_221613`

Native fixed-depth settings:

```sh
DS4_MTP_NATIVE=1
DS4_MTP_NATIVE_VERIFY_OPT=smallm
DS4_MTP_NATIVE_CACHE_MODE=owned
DS4_MTP_GOVERNOR_DISABLE=1
```

Three-prompt fixed-depth smoke, `-n 64 --temp 0 --ctx 1024 --nothink -sys ""`:

| Prompt | Serial t/s | K=2 t/s | K=2 avg accept | K=3 t/s | K=3 avg accept | K=4 t/s | K=4 avg accept | K=5 t/s | K=5 avg accept |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| count | 35.64 | 31.77 | 2.000 | 37.66 | 3.000 | 42.20 | 4.000 | 36.48 | 4.000 |
| explain | 35.56 | 31.63 | 1.600 | 31.71 | 2.133 | 29.70 | 2.133 | 28.49 | 2.560 |
| code | 35.49 | 31.94 | 1.853 | 37.01 | 2.667 | 38.25 | 3.048 | 31.82 | 3.200 |

All fixed-depth native rows matched serial stdout.

Representative HumanEval+ 50-task slice:

| Mode | syntax | base pass@1 | plus pass@1 | tokens | elapsed | aggregate TPS | mean TPS | median TPS | output diffs vs serial |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| serial | 50/50 | 0.640 | 0.620 | 4581 | 171.15 s | 26.77 | 25.04 | 25.34 | - |
| current MTP K=3 | 50/50 | 0.640 | 0.620 | 4613 | 173.13 s | 26.65 | 24.78 | 24.94 | 4 |
| native fixed K=4 | 50/50 | 0.640 | 0.620 | 4581 | 166.73 s | 27.48 | 25.73 | 25.69 | 0 |
| native fixed K=3 | 50/50 | 0.640 | 0.620 | 4581 | 166.61 s | 27.50 | 25.73 | 26.10 | 0 |

Representative GSM8K first-50, request-level `think:false`:

| Mode | accuracy | correct | tokens | elapsed | aggregate TPS | mean TPS | median TPS | output diffs vs serial | failed |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | --- |
| serial | 0.920 | 46/50 | 4036 | 146.93 s | 27.47 | 26.91 | 26.94 | - | 24, 26, 27, 36 |
| current MTP K=3 | 0.920 | 46/50 | 4069 | 149.39 s | 27.24 | 26.69 | 26.54 | 9 | 24, 26, 27, 36 |
| native fixed K=4 | 0.920 | 46/50 | 4036 | 143.39 s | 28.15 | 27.62 | 27.73 | 0 | 24, 26, 27, 36 |
| native fixed K=3 | 0.920 | 46/50 | 4036 | 145.72 s | 27.70 | 27.12 | 27.09 | 0 | 24, 26, 27, 36 |

Interpretation:

- The fixed-depth sweep confirms the user's concern: depth must be re-swept
  after verifier-economics changes. The current answer is not the old stale
  depth choice.
- K=4 is the fresh native candidate. It wins count/code and the three-prompt
  mean, and it is the clear GSM8K 50-task winner while preserving byte-identical
  outputs against serial on both representative slices.
- K=3 is still a serious challenger: it narrowly wins the tiny explain smoke and
  is essentially tied with K=4 on HumanEval+ 50. It loses to K=4 on GSM8K 50 and
  on the count/code smoke.
- K=5 is not attractive in this shape. It accepts slightly more on some prompts,
  but the added draft/verifier work overwhelms the accepted-depth gain.

Updated decision:

- Carry `native fixed K=4` as the current post-batch-KV candidate for the next
  optimization/eval loop, with K=3 kept as the near-tie challenger for coding
  workloads.
- This is materially stronger evidence than the first-20 slices: native K=4 is
  quality-equal and faster than serial/current MTP on HumanEval+ 50 and GSM8K 50.
- Do not finalize architecture-level promotion yet. The next gate should add at
  least MBPP+ 50 and longer open-ended/chat-style generations, while continuing
  verifier-body optimization.

### 2026-05-16 MBPP+/Long-Form Gate and Native Governor Cap Fix

Rationale:

- The fixed-depth K=4 result looked good on HumanEval+ 50 and GSM8K 50, but the
  architecture decision still needed a second coding distribution and long-form
  open-ended prompts. Earlier native versions regressed badly on `-n 512`
  free-form generations.
- The long-form run exposed a code inefficiency: the native path logged governor
  depth reductions, but `ds4_session_eval_mtp_native()` never applied
  `s->mtp_governor_draft_cap` when deriving `depth_cap`. Older MTP paths already
  applied the cap. Native K=4 therefore kept drafting/verifying depth 4 even
  after the governor had selected depth 3.

Implementation:

- Applied `s->mtp_governor_draft_cap` inside `ds4_session_eval_mtp_native()`
  before the native depth clamps.
- This only changes governed native runs. Fixed-depth experiments with
  `DS4_MTP_GOVERNOR_DISABLE=1` remain fixed-depth evidence.

Artifacts:

- MBPP+ 50-task slice:
  `/tmp/ds4-post-batchkv-mbpp50-20260516_223011`
- Fixed-depth long-form `-n 512` slice:
  `/tmp/ds4-post-batchkv-long512-20260516_223719`
- Fixed-depth native K=2 long-form check:
  `/tmp/ds4-post-batchkv-long512-k2-20260516_224516`
- Governed K=4 long-form before cap fix:
  `/tmp/ds4-post-batchkv-long512-governor-20260516_224230`
- Governed K=4 long-form after cap fix:
  `/tmp/ds4-native-governor-capfix-long512-20260516_224824`
- Three-prompt smoke after cap fix:
  `/tmp/ds4-native-governor-capfix-smoke-20260516_225003`
- Accepted-row validation after cap fix:
  `/tmp/ds4-native-governor-capfix-validate-20260516_225045`

Representative MBPP+ 50-task slice:

| Mode | syntax | base pass@1 | plus pass@1 | tokens | elapsed | aggregate TPS | mean TPS | median TPS | output diffs vs serial |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| serial | 50/50 | 0.960 | 0.900 | 2118 | 90.94 s | 23.29 | 20.88 | 20.05 | - |
| current MTP K=3 | 50/50 | 0.980 | 0.920 | 1953 | 87.18 s | 22.40 | 20.41 | 19.55 | 5 |
| native fixed K=4 | 50/50 | 0.960 | 0.900 | 2118 | 90.51 s | 23.40 | 20.98 | 20.00 | 0 |
| native fixed K=3 | 50/50 | 0.960 | 0.900 | 2118 | 90.47 s | 23.41 | 20.98 | 20.02 | 0 |

Long open-ended `-n 512` fixed-depth slice:

| Prompt | serial | current MTP K=3 | native fixed K=2 | native fixed K=3 | native fixed K=4 | Native exactness |
| --- | ---: | ---: | ---: | ---: | ---: | --- |
| explain | 34.18 | 34.20 | 31.25 | 31.12 | 28.62 | all native match serial |
| code_design | 34.29 | 34.15 | 31.30 | 33.25 | 30.64 | all native match serial |
| debug_plan | 34.24 | 34.07 | 31.30 | 31.70 | 28.71 | all native match serial |
| gsm_style | 34.05 | 34.07 | 31.29 | 33.93 | 33.56 | all native match serial |
| mean | 34.19 | 34.12 | 31.29 | 32.50 | 30.38 | all native match serial |

Governed K=4 long-form before and after cap fix:

| Mode | explain | code_design | debug_plan | gsm_style | mean | exactness |
| --- | ---: | ---: | ---: | ---: | ---: | --- |
| native K=4 governed before fix | 29.66 | 30.28 | 30.24 | 33.59 | 30.94 | match |
| native K=4 governed after fix | 31.24 | 33.24 | 31.24 | 34.32 | 32.51 | match |

Post-fix governed K=4 events:

- explain: one `4 -> 3` governor reduction.
- code_design: one `4 -> 3` governor reduction.
- debug_plan: one `4 -> 3` and one `3 -> 2` governor reduction.
- gsm_style: one `4 -> 3` governor reduction.

Three-prompt smoke after cap fix:

| Prompt | serial | native default K=4 | stdout | governor events |
| --- | ---: | ---: | --- | --- |
| count | 35.53 | 42.23 | match | none |
| explain | 35.34 | 30.98 | match | `4 -> 3` |
| code | 35.58 | 37.90 | match | `4 -> 3` |

Accepted-row validation after cap fix, `DS4_MTP_NATIVE_VALIDATE=1`:

| Prompt | stdout | cycles | avg accepted | max mismatches | max delta | observed depths |
| --- | --- | ---: | ---: | ---: | ---: | --- |
| count | match | 8 | 4.000 | 0 | 0 | 4 |
| explain | match | 13 | 2.385 | 0 | 0 | 4, 3 |
| code | match | 9 | 3.444 | 0 | 0 | 4 |

Checks:

```sh
make ds4_test ds4
./ds4_test --metal-kernels
./ds4_test --metal-mtp-cache-contract
git diff --check -- ds4.c
```

Studio checks after syncing `ds4.c`:

```sh
make ds4 ds4_test
./ds4_test --metal-kernels
./ds4_test --metal-mtp-cache-contract
git diff --check -- ds4.c
```

Interpretation:

- MBPP+ 50 supports the coding-slice result: native K=3/K=4 are exact against
  serial and slightly faster, but the gain is small. Current MTP changes a few
  outputs and scores slightly better on this MBPP slice, so quality policy still
  matters when comparing exact-native against non-exact current MTP.
- Long-form `-n 512` remains the blocker for architecture-level promotion.
  Native is exact, but even after the governor cap fix it trails serial/current
  MTP on open-ended explanations and debugging text. K3 is the best fixed native
  depth for long-form, but still below serial. K2 is not a solution.
- The cap fix is still worth keeping: it removes a real native-only governor
  bug and improves governed K=4 long-form mean TPS from `30.94` to `32.51`
  without changing output.

Updated decision:

- Keep K=4 as the main exact-native candidate for coding/math eval slices and
  keep K=3 as the long-form challenger.
- Do not promote the architecture yet. The next optimization must reduce
  verifier body cost on low/medium-acceptance long-form workloads, especially
  attention/compressor/indexer and routed-MoE math, or introduce a stronger
  workload/depth policy that can select K=3 early without losing K=4 gains on
  high-acceptance workloads.

### 2026-05-16 Output Fused Top-1 Default Cleanup

Rationale:

- The fused output top-1 path had already passed two post-small-M audits, but it
  was still opt-in. That meant the production native verifier continued to
  materialize full-vocab rows just to discover exact top ids for non-committed
  verifier rows.
- This is not expected to fix the architecture by itself. It targets only the
  output-head/top-id acceptance item from the verifier-efficiency goal.

Implementation:

- Changed `metal_graph_mtp_verifier_output_fused_top1_enabled()` to default on.
- Added `DS4_MTP_NATIVE_OUTPUT_FUSED_TOP1_DISABLE=1` as the explicit rollback
  switch. Existing `DS4_MTP_NATIVE_OUTPUT_FUSED_TOP1=0` also disables the path.
- Validation/full-logit diagnostic mode still bypasses the fused production
  path so accepted-row serial logit checks can materialize full verifier rows.

Artifacts:

- Studio production/audit/validation matrix:
  `/tmp/ds4-output-fused-default-20260516_225835`

Three-prompt production A/B, governed native K=4:

| Prompt | serial | fused default | fused disabled | stdout | default output-fused cycles | default remat rows | disabled head GPU | default head GPU | disabled total GPU | default total GPU |
| --- | ---: | ---: | ---: | --- | ---: | ---: | ---: | ---: | ---: | ---: |
| count | 35.58 | 42.65 | 42.38 | match | 16/16 | 0 | 1.024 ms | 0.913 ms | 56.499 ms | 55.972 ms |
| explain | 35.51 | 30.88 | 31.04 | match | 21/30 | 12 | 0.703 ms | 0.636 ms | 32.737 ms | 32.723 ms |
| code | 35.68 | 38.01 | 38.00 | match | 17/22 | 2 | 0.767 ms | 0.686 ms | 38.909 ms | 38.813 ms |

Audit mode, `DS4_MTP_NATIVE_OUTPUT_FUSED_TOP1_AUDIT=1`, materialized verifier
rows and compared fused top ids against full-logit top ids:

| Prompt | stdout | cycles | audit mismatches |
| --- | --- | ---: | ---: |
| count | match | 16 | 0 |
| explain | match | 30 | 0 |
| code | match | 22 | 0 |

Accepted-row serial validation, same-length `-n 32` oracle:

| Prompt | stdout | cycles | mismatches | max delta |
| --- | --- | ---: | ---: | ---: |
| count | match | 8 | 0 | 0 |
| explain | match | 13 | 0 | 0 |
| code | match | 9 | 0 | 0 |

Checks:

```sh
make ds4_test ds4 ds4-server
./ds4_test --metal-kernels
./ds4_test --metal-mtp-cache-contract
git diff --check -- ds4.c
```

Studio checks after syncing `ds4.c`:

```sh
make ds4 ds4-server ds4_test
./ds4_test --metal-kernels
./ds4_test --metal-mtp-cache-contract
git diff --check -- ds4.c
```

Decision:

- Keep fused output top-1 default-on because it is exact under audit and reduces
  verifier output-head GPU span by about `0.07..0.11 ms` per verifier call on
  this matrix.
- This is a cleanup, not an architecture-level promotion. End-to-end TPS is
  mixed because partial accepts can require full-logit row re-materialization
  for the committed row. The next material optimization still needs to attack
  verifier body math and/or reduce the need to materialize committed-row logits.

### 2026-05-16 Top-ID Frontier Probe

Rationale:

- Fused output top-1 avoids full-vocab work for verifier rows whose only
  production purpose is exact greedy acceptance, but the runtime still
  materialized/read the committed verifier row so `s->logits` could provide the
  next temp-0 frontier token.
- At temp 0 the next frontier only needs the exact top id, not the whole logits
  vector. This probe tests whether carrying the verifier-proven top id across
  the next sampling boundary removes meaningful committed-row rematerialization
  and readback cost.

Implementation:

- Added gated `DS4_MTP_NATIVE_TOPID_FRONTIER=1`.
- Added a session-local top-id cache used by `ds4_session_argmax()` and
  `ds4_session_sample(... temperature <= 0 ...)`.
- The native verifier can now skip committed-row full-logit materialization and
  mark the session as top-id-only when the verifier produced an exact top-1 row.
- The path is deliberately opt-in because full logits are stale between the
  speculative commit and the next target eval. APIs that need full logprobs are
  not the intended consumer of this probe.
- Timing now reports `topid_frontier=1` on native cycles that use the cached
  top-id contract.

Artifacts:

- Three-prompt smoke/audit/validation:
  `/tmp/ds4-native-topid-frontier-smoke-20260516_230540`
- Long open-ended `-n 512` slice:
  `/tmp/ds4-native-topid-frontier-long512-20260516_230741`
- HumanEval+ 20-task slice:
  `/tmp/ds4-native-topid-frontier-humaneval20-20260516_231313`
- GSM8K first-20 no-think slice:
  `/tmp/ds4-native-topid-frontier-gsm8k20-20260516_231651`

Three-prompt smoke, governed native K=4:

| Prompt | serial | default | top-id frontier | stdout | default remat rows | top-id remat rows | default logits read | top-id logits read |
| --- | ---: | ---: | ---: | --- | ---: | ---: | ---: | ---: |
| count | 35.60 | 42.43 | 42.42 | match | 0 | 0 | 0.021 ms | 0.000 ms |
| explain | 35.52 | 31.08 | 31.27 | match | 12 | 0 | 0.432 ms | 0.000 ms |
| code | 35.59 | 37.92 | 38.23 | match | 2 | 0 | 0.113 ms | 0.000 ms |

Audit/validation:

- `DS4_MTP_NATIVE_OUTPUT_FUSED_TOP1_AUDIT=1` with top-id frontier matched serial
  stdout on count/explain/code and reported `mismatches=0`.
- `DS4_MTP_NATIVE_VALIDATE=1` with the top-id env present uses the full-logit
  validation path; same-length `-n 32` serial stdout matched on all three prompts,
  with `mismatches=0` and `max_delta=0`.

Long open-ended `-n 512` slice:

| Prompt | serial | native K4 default | native K4 top-id | native K3 top-id | exactness |
| --- | ---: | ---: | ---: | ---: | --- |
| explain | 34.34 | 31.18 | 31.29 | 31.35 | all native match serial |
| code_design | 34.24 | 33.02 | 33.23 | 33.36 | all native match serial |
| debug_plan | 34.24 | 31.15 | 31.24 | 32.00 | all native match serial |
| gsm_style | 34.12 | 34.30 | 34.51 | 34.08 | all native match serial |
| mean | 34.24 | 32.41 | 32.57 | 32.70 | all native match serial |

Representative HumanEval+ first-20:

| Mode | syntax | base pass@1 | plus pass@1 | tokens | elapsed | aggregate TPS | output diffs vs serial |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| serial | 20/20 | 0.450 | 0.450 | 1394 | 55.35 s | 25.19 | - |
| native K4 top-id | 20/20 | 0.450 | 0.450 | 1394 | 55.19 s | 25.26 | 0 |
| native K3 top-id | 20/20 | 0.450 | 0.450 | 1394 | 55.32 s | 25.20 | 0 |

Representative GSM8K first-20, request-level `think:false`:

| Mode | accuracy | tokens | elapsed | aggregate TPS | output diffs vs serial |
| --- | ---: | ---: | ---: | ---: | ---: |
| serial | 1.000 | 1978 | 70.19 s | 28.18 | - |
| native K4 top-id | 1.000 | 1978 | 70.75 s | 27.96 | 0 |
| native K3 top-id | 1.000 | 1978 | 72.11 s | 27.43 | 0 |

Checks:

```sh
make ds4_test ds4 ds4-server
./ds4_test --metal-kernels
./ds4_test --metal-mtp-cache-contract
git diff --check -- ds4.c
```

Studio checks after syncing `ds4.c`:

```sh
make ds4 ds4-server ds4_test
./ds4_test --metal-kernels
./ds4_test --metal-mtp-cache-contract
git diff --check -- ds4.c
```

Decision:

- Keep `DS4_MTP_NATIVE_TOPID_FRONTIER=1` gated. It is correctness-clean for
  temp-0 output in the tested matrix and it removes the intended
  committed-row rematerialization/readback work.
- Do not promote it to default. The largest smoke win was only about `+0.31 t/s`
  on the code prompt, the long-form mean moved only `32.41 -> 32.57 t/s` for K4,
  HumanEval+ first-20 was essentially flat, and GSM8K first-20 was slightly
  slower.
- This result narrows the bottleneck: committed-row full-logit readback is real
  waste, but not the dominant cost. Continue with verifier body math, especially
  the small-M attention/compressor/indexer and routed-MoE portions that dominate
  the GPU span.

### 2026-05-16 Current-Stack Depth Re-Sweep After Top-ID Frontier

Rationale:

- Batch-KV changed verifier per-row economics, and the later top-id frontier
  probe changed committed-row readback economics. Therefore the working depth
  could not be inherited from an older K sweep.
- This is a fixed-depth selector with `DS4_MTP_GOVERNOR_DISABLE=1`, not a
  production governor measurement.

Artifacts:

- Fixed-depth three-prompt sweep:
  `/tmp/ds4-current-stack-depth-sweep-1778988376`
- HumanEval+ 50-task slice for the current K=4 winner:
  `/tmp/ds4-current-stack-humaneval50-20260516_232831`
- GSM8K first-50 slice for serial versus current K=4 winner:
  `/tmp/ds4-current-stack-gsm8k50-1778988718`

Native fixed-depth settings:

```sh
DS4_MTP_NATIVE=1
DS4_MTP_NATIVE_VERIFY_OPT=smallm
DS4_MTP_NATIVE_CACHE_MODE=owned
DS4_MTP_GOVERNOR_DISABLE=1
DS4_MTP_NATIVE_TOPID_FRONTIER=1
```

Three-prompt fixed-depth smoke, `-n 64 --temp 0 --ctx 1024 --nothink -sys ""`:

| Prompt | Serial t/s | K=2 t/s | K=2 avg accept | K=3 t/s | K=3 avg accept | K=4 t/s | K=4 avg accept | K=5 t/s | K=5 avg accept |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| count | 35.57 | 31.78 | 2.000 | 37.77 | 3.000 | 42.43 | 4.000 | 35.53 | 4.000 |
| explain | 35.56 | 31.79 | 1.600 | 31.94 | 2.133 | 29.81 | 2.133 | 28.51 | 2.560 |
| code | 35.33 | 31.91 | 1.853 | 36.93 | 2.667 | 38.38 | 3.048 | 31.79 | 3.200 |

All native rows matched serial stdout and reported `mismatches_sum=0`.

Representative HumanEval+ 50-task slice:

| Mode | syntax | base pass@1 | plus pass@1 | tokens | elapsed | aggregate TPS | mean TPS | median TPS | output diffs vs serial |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| native K4 top-id fixed | 50/50 | 0.640 | 0.620 | 4581 | 166.63 s | 27.49 | 25.74 | 25.73 | 0 |

This matches the prior same-depth serial/native quality result (`0.640` base,
`0.620` plus) while preserving the native exact-output contract.

Representative GSM8K first-50, same harness for serial and native:

| Mode | accuracy | correct | tokens | elapsed | aggregate TPS | mean TPS | median TPS | output diffs vs serial |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| serial | 1.000 | 50/50 | 5637 | 192.74 s | 29.25 | 28.56 | 28.76 | - |
| native K4 top-id fixed | 1.000 | 50/50 | 5637 | 189.73 s | 29.71 | 29.06 | 29.34 | 0 |

Interpretation:

- The user's requested re-sweep changes the evidence from "K=4 was best under
  an older stack" to "K=4 remains the current-stack three-prompt winner."
- K=3 is still the long-form/explain challenger. It beats K=4 on the explain
  smoke, but K=4 wins count, code, and the three-prompt mean, then preserves
  HumanEval+ and GSM8K quality on the representative slices.
- K=5 is still unattractive: it accepts no more than K=4 on count, only modestly
  more on explain/code, and loses throughput badly from extra verifier work.
- This is stronger than a smoke-only claim, but still not final architecture
  promotion. The remaining work is verifier-body efficiency and longer/open
  generations, where K=3 has previously been more competitive than K=4.

### 2026-05-16 Small-M Batched Attention-Compressor Projection

Rationale:

- Current-stack profiling with top-id frontier still showed the row-tail
  verifier body as the dominant surface:
  `/tmp/ds4-current-stack-smallm-profile-20260516_234052`.
- After batch-KV, `raw_store` was no longer the largest row-tail piece. The
  biggest remaining row-tail buckets were `attention_heads` and
  `attn_compressor`.
- The compressor state update must stay sequential, but the attention
  compressor KV/gate projections are independent for the `M=2..4` verifier
  rows. This probe batches only those paired F16 projections and still feeds
  each row through the existing ordered compressor update.

Implementation:

- Added gated `DS4_MTP_NATIVE_SMALLM_BATCH_COMPRESSOR_PROJ=1`.
- Extended the existing Metal paired F16 matvec wrapper so
  `ds4_gpu_matmul_f16_pair_tensor()` can dispatch `n_tok=2..4` rows using the
  same `kernel_mul_mv_f16_f32_pair_4` reduction shape across the Metal y grid.
- In the native small-M verifier, precomputes attention-compressor
  `kv/sc` projections into `g->batch_comp_kv/g->batch_comp_sc` for compressed
  layers, then passes per-row views into the unchanged sequential compressor
  update path.
- The path is opt-in because it changes compressor projection scheduling, even
  though it does not change routed-MoE/router math.

Artifacts:

- Production three-prompt A/B:
  `/tmp/ds4-smallm-batch-compressor-proj-smoke-1778989514`
- Accepted-row validation:
  `/tmp/ds4-smallm-batch-compressor-proj-validate-1778989576`
- Sync-heavy stage profile:
  `/tmp/ds4-smallm-batch-compressor-proj-profile-20260516_234653`
- Long open-ended `-n 512` A/B:
  `/tmp/ds4-smallm-batch-compressor-proj-long512-1778989702`

Checks:

```sh
make ds4_test ds4 ds4-server
./ds4_test --metal-kernels
./ds4_test --metal-mtp-cache-contract
git diff --check -- ds4.c ds4_metal.m
```

The same build/check sequence passed after syncing `ds4.c` and `ds4_metal.m`
to the Studio worktree.

Three-prompt production smoke, fixed native K=4 with top-id frontier:

| Prompt | serial | native default | batch comp proj | stdout | default decode GPU | batch decode GPU |
| --- | ---: | ---: | ---: | --- | ---: | ---: |
| count | 35.50 | 42.31 | 42.77 | match | 55.416 ms | 54.555 ms |
| explain | 35.40 | 29.87 | 30.00 | match | 33.626 ms | 33.290 ms |
| code | 35.48 | 38.32 | 38.48 | match | 41.214 ms | 40.844 ms |

Accepted-row validation, `DS4_MTP_NATIVE_VALIDATE=1`, `-n 32`:

| Prompt | stdout | cycles | avg accept | mismatches | max delta |
| --- | --- | ---: | ---: | ---: | ---: |
| count | match | 8 | 4.000 | 0 | 0 |
| explain | match | 13 | 2.385 | 0 | 0 |
| code | match | 9 | 3.444 | 0 | 0 |

Profile comparison on `explain`, K=4, `-n 16`:

| Stage | default sum | batch-proj sum | note |
| --- | ---: | ---: | --- |
| `smallm_tail:attn_compressor` | 137.353 ms | 122.281 ms | row-tail compressor work reduced |
| `smallm:batch_attn_comp_proj` | - | 51.064 ms | new batched projection stage |
| `smallm_tail:attention_heads` | 169.032 ms | 172.294 ms | still largest row-tail cost |

Long open-ended `-n 512` A/B:

| Prompt | serial | native default | batch comp proj | stdout | default decode GPU | batch decode GPU |
| --- | ---: | ---: | ---: | --- | ---: | ---: |
| explain | 34.32 | 31.38 | 31.43 | match | 29.092 ms | 28.930 ms |
| code_design | 34.23 | 33.18 | 33.43 | match | 33.547 ms | 33.074 ms |
| debug_plan | 34.22 | 31.24 | 31.23 | match | 18.678 ms | 18.695 ms |
| gsm_style | 34.15 | 34.42 | 34.54 | match | 33.724 ms | 33.489 ms |

Decision:

- Keep `DS4_MTP_NATIVE_SMALLM_BATCH_COMPRESSOR_PROJ=1` as a gated,
  correctness-clean efficiency probe. It consistently reduces verifier decode
  GPU span in the smoke and in three of four long prompts, with exact stdout and
  accepted-row validation.
- Do not promote it to default by itself. The end-to-end gain is small
  (`+0.05` to `+0.25 t/s` on the long-form prompts that improved), and
  `attention_heads` remains the largest row-tail cost.
- Next verifier-body target should focus on an exact attention-head reduction
  or a safer policy that avoids expensive K=4 verification on low-acceptance
  long-form regions.

### 2026-05-16 Post-Batch-Projection Depth Re-Sweep

Rationale:

- The previous K choice was made after batch-KV/top-id, before the gated
  attention-compressor projection batch path.
- Because `DS4_MTP_NATIVE_SMALLM_BATCH_COMPRESSOR_PROJ=1` changes per-row
  verifier economics, the native depth needed a fresh `K=2..5` sweep before
  sending a winner to representative HumanEval/GSM slices.
- The short `-n 64` prompt matrix remains a smoke/triage signal only, not a
  promotion criterion.

Artifacts:

- Three-prompt depth sweep:
  `/tmp/ds4-batchcomp-depth-sweep-1778990168`
- HumanEval+ first-50:
  `/tmp/ds4-batchcomp-k4-humaneval50-20260516_235751`
- GSM8K first-50:
  `/tmp/ds4-batchcomp-k4-gsm8k50-1778990663`

Native settings:

```sh
DS4_MTP_NATIVE=1
DS4_MTP_NATIVE_VERIFY_OPT=smallm
DS4_MTP_NATIVE_CACHE_MODE=owned
DS4_MTP_GOVERNOR_DISABLE=1
DS4_MTP_NATIVE_TOPID_FRONTIER=1
DS4_MTP_NATIVE_SMALLM_BATCH_COMPRESSOR_PROJ=1
```

Three-prompt smoke, `--ctx 1024 --nothink -sys "" --temp 0 -n 64`:

| Prompt | serial | K=2 | K=3 | K=4 | K=5 | best | stdout |
| --- | ---: | ---: | ---: | ---: | ---: | --- | --- |
| count | 35.71 | 31.82 | 37.92 | 42.85 | 36.62 | K=4 | all match |
| explain | 35.58 | 31.67 | 31.36 | 29.65 | 28.54 | K=2/K=3 | all match |
| code | 35.51 | 31.76 | 37.37 | 37.07 | 31.87 | K=3 | all match |

Native timing details:

| Prompt | K | avg accepted | avg discarded | avg draft | verifier decode GPU | verifier head GPU |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| count | 2 | 2.000 | 0.000 | 3.622 ms | 28.037 ms | 0.905 ms |
| count | 3 | 3.000 | 0.000 | 5.266 ms | 41.306 ms | 0.854 ms |
| count | 4 | 4.000 | 0.000 | 6.976 ms | 53.953 ms | 0.917 ms |
| count | 5 | 4.000 | 0.938 | 8.514 ms | 68.240 ms | 1.040 ms |
| explain | 2 | 1.600 | 0.400 | 3.598 ms | 16.326 ms | 0.541 ms |
| explain | 3 | 2.133 | 0.833 | 5.196 ms | 30.635 ms | 0.629 ms |
| explain | 4 | 2.133 | 1.800 | 6.799 ms | 33.994 ms | 0.581 ms |
| explain | 5 | 2.560 | 2.280 | 8.287 ms | 49.337 ms | 0.785 ms |
| code | 2 | 1.853 | 0.147 | 3.686 ms | 23.748 ms | 0.772 ms |
| code | 3 | 2.667 | 0.333 | 5.283 ms | 34.358 ms | 0.715 ms |
| code | 4 | 3.048 | 0.905 | 6.856 ms | 41.009 ms | 0.700 ms |
| code | 5 | 3.200 | 1.700 | 8.410 ms | 59.957 ms | 0.935 ms |

Interpretation:

- K=4 remains the three-prompt mean winner under the current batch-projection
  stack, mostly because the count prompt is a full-accept case and K=4 benefits
  strongly there.
- K=3 is still the safer long-form challenger: it wins code narrowly and stays
  ahead of K=4 on explain. The smoke matrix should not be used to erase that
  distributional caveat.
- K=5 remains unattractive: it adds verifier/draft cost faster than it adds
  accepted useful work on these prompts.

HumanEval+ first-50, same-run serial versus native K=4 batch projection:

| Mode | syntax | base pass@1 | plus pass@1 | tokens | elapsed | aggregate TPS | mean TPS | median TPS |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| serial | 50/50 | 0.640 | 0.620 | 4581 | 171.02 s | 26.79 | 25.05 | 25.34 |
| native K=4 batch comp | 50/50 | 0.640 | 0.620 | 4581 | 165.98 s | 27.60 | 25.84 | 25.84 |

GSM8K first-50, request-level `think:false`, same-run serial versus native K=4
batch projection:

| Mode | accuracy | correct | tokens | elapsed | aggregate TPS | mean TPS | median TPS | output diffs vs serial |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| serial | 1.000 | 50/50 | 6223 | 214.40 s | 29.03 | 28.30 | 28.40 | - |
| native K=4 batch comp | 1.000 | 50/50 | 6223 | 210.25 s | 29.60 | 28.99 | 29.53 | 0 |

Decision:

- The fresh post-batch-projection evidence supports keeping K=4 as the current
  exact native representative candidate, not as a final architecture promotion.
- The representative slices are positive: exact same HumanEval+ pass/fail set,
  exact GSM8K outputs, and modest aggregate TPS wins over same-run serial on
  both slices.
- K=3 should remain in the candidate set for long free-form/explanation-heavy
  workloads because the smoke matrix shows lower verifier waste there.
- Next work should continue verifier-body efficiency, especially
  `attention_heads`, and should include a longer free-form K=3/K=4 comparison
  before defaulting a depth policy.

### 2026-05-17 Indexed Attention RB8 Probe

Rationale:

- Small-M tail profiling still shows `smallm_tail:attention_heads` as the
  largest row-tail stage after batch-KV and batched attention-compressor
  projection.
- The current indexed mixed attention decode specialization stages four raw or
  compressed K/V rows per threadgroup barrier (`rb4`).
- A safe bounded experiment is to stage eight rows per barrier (`rb8`) while
  preserving row order and the same online softmax/value update sequence. This
  does not revive the unsafe multi-token attention batch path.

Implementation:

- Added `kernel_dsv4_indexed_mixed_attention_heads8_rb8` in
  `metal/dsv4_misc.metal`.
- Added a new Metal pipeline and opt-in selector behind
  `DS4_MTP_NATIVE_SMALLM_ATTN_RB8=1`.
- The default remains the existing `rb4` kernel unless the flag is explicitly
  set.

Checks:

```sh
make ds4_test ds4 ds4-server
./ds4_test --metal-kernels
./ds4_test --metal-mtp-cache-contract
git diff --check -- ds4_metal.m metal/dsv4_misc.metal
```

The same build/check sequence passed after syncing `ds4_metal.m` and
`metal/dsv4_misc.metal` to the Studio worktree.

Artifact:

- `/tmp/ds4-native-attn-rb8-smoke-1778991304`

Settings:

```sh
DS4_MTP_NATIVE=1
DS4_MTP_NATIVE_VERIFY_OPT=smallm
DS4_MTP_NATIVE_CACHE_MODE=owned
DS4_MTP_NATIVE_COMMIT_OPT=auto
DS4_MTP_NATIVE_TOPID_FRONTIER=1
DS4_MTP_NATIVE_SMALLM_BATCH_COMPRESSOR_PROJ=1
DS4_MTP_GOVERNOR_DISABLE=1
# rb8 variant only:
DS4_MTP_NATIVE_SMALLM_ATTN_RB8=1
```

Three-prompt K=4 production A/B, `-n 64`:

| Prompt | serial | rb4 t/s | rb8 t/s | rb4 decode GPU | rb8 decode GPU | stdout |
| --- | ---: | ---: | ---: | ---: | ---: | --- |
| count | 35.40 | 43.10 | 42.96 | 53.587 ms | 53.911 ms | match |
| explain | 35.39 | 29.72 | 29.67 | 33.872 ms | 33.872 ms | match |
| code | 35.37 | 36.88 | 36.91 | 41.425 ms | 41.297 ms | match |

Accepted-row validation, `DS4_MTP_NATIVE_VALIDATE=1`, `-n 32`:

| Prompt | Variant | mismatches | max delta |
| --- | --- | ---: | ---: |
| count | rb4 | 0 | 0 |
| count | rb8 | 0 | 0 |
| explain | rb4 | 0 | 0 |
| explain | rb8 | 0 | 0 |
| code | rb4 | 0 | 0 |
| code | rb8 | 0 | 0 |

Decision:

- Drop `rb8`. It is correctness-clean, but it does not materially reduce
  verifier decode GPU time and slightly hurts the full-accept count case.
- The rb8 code path was removed after preserving the evidence here, so the
  greenfield native path does not accumulate another dead experimental branch.
- The attention-head bottleneck is probably not just per-four-row barrier
  overhead. The next useful attention work needs a different shape, such as a
  true small-M multi-row exact indexed attention contract with explicit causal
  row visibility, or it should move to another verifier-body hotspot.

### 2026-05-17 Long Free-Form K=3/K=4 Depth Check

Rationale:

- The post-batch-projection `-n 64` smoke made K=4 the mean winner, but it was
  heavily influenced by the full-accept count prompt.
- The same smoke already showed K=3 ahead on code and ahead of K=4 on explain.
- Before defaulting any depth policy, the exact native path needs longer
  free-form evidence where acceptance is lower and verifier waste is more
  representative.

Artifact:

- `/tmp/ds4-batchcomp-k3-k4-long512-1778991781`

Settings:

```sh
DS4_MTP_NATIVE=1
DS4_MTP_NATIVE_VERIFY_OPT=smallm
DS4_MTP_NATIVE_CACHE_MODE=owned
DS4_MTP_NATIVE_COMMIT_OPT=auto
DS4_MTP_NATIVE_TOPID_FRONTIER=1
DS4_MTP_NATIVE_SMALLM_BATCH_COMPRESSOR_PROJ=1
DS4_MTP_GOVERNOR_DISABLE=1
```

Long prompts, `--ctx 1024 --nothink -sys "" --temp 0 -n 512`:

| Prompt | serial | K=3 | K=4 | K=3 avg accept | K=4 avg accept | K=3 decode GPU | K=4 decode GPU | stdout |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | --- |
| explain | 34.47 | 33.02 | 30.28 | 2.165 | 2.344 | 29.152 ms | 38.991 ms | all match |
| code_design | 34.36 | 33.39 | 30.50 | 2.271 | 2.410 | 31.029 ms | 40.410 ms | all match |
| debug_plan | 34.26 | 32.10 | 29.52 | 2.103 | 2.265 | 28.885 ms | 38.073 ms | all match |
| gsm_style | 34.52 | 35.39 | 35.73 | 2.571 | 2.964 | 34.668 ms | 43.896 ms | all match |

Interpretation:

- K=3 is clearly better than K=4 on longer explanation/design/debug prompts,
  even though it still trails serial there. K=4 accepts only slightly more work
  but pays much more verifier decode GPU per cycle.
- K=4 still wins the arithmetic-style prompt where acceptance is higher, and it
  remains the representative winner for HumanEval+ first-50 and GSM8K first-50.
- The evidence now argues against a single fixed depth as the final policy.
  The likely promotable direction is an adaptive depth controller or a cheaper
  K=4 verifier body. Until that exists, K=4 should be treated as a
  high-acceptance candidate, while K=3 is the safer long-form candidate.

### 2026-05-17 Adaptive K3/K4 Depth Probe

Rationale:

- Fixed K=4 wins high-acceptance count/GSM-style prompts.
- Fixed K=3 wins longer explanation/design/debug prompts by avoiding expensive
  discarded K=4 verifier rows.
- A natural exact policy probe is to start at K=3, promote to K=4 after a
  short full-accept streak, and fall back after partial accepts.

Implementation:

- Added a gated `DS4_MTP_NATIVE_DEPTH_POLICY=adaptive_k34` probe.
- The probe kept acceptance semantics exact: it changed only the per-cycle
  native `depth_cap` before drafting/verifying.
- Timing output reported `depth_policy`, depth streaks, and selected depth
  counts for audit.

Artifacts:

- Performance A/B:
  `/tmp/ds4-native-adaptive-depth-1778992171`
- Accepted-row validation:
  `/tmp/ds4-native-adaptive-depth-validate-1778992507`

Settings:

```sh
DS4_MTP_NATIVE=1
DS4_MTP_NATIVE_VERIFY_OPT=smallm
DS4_MTP_NATIVE_CACHE_MODE=owned
DS4_MTP_NATIVE_COMMIT_OPT=auto
DS4_MTP_NATIVE_TOPID_FRONTIER=1
DS4_MTP_NATIVE_SMALLM_BATCH_COMPRESSOR_PROJ=1
DS4_MTP_NATIVE_DEPTH_POLICY=adaptive_k34
DS4_MTP_GOVERNOR_DISABLE=1
```

Three-prompt smoke, `-n 64`:

| Prompt | serial | fixed K3 | fixed K4 | adaptive K3/K4 | adaptive depth mix | stdout |
| --- | ---: | ---: | ---: | ---: | --- | --- |
| count | 35.75 | 37.87 | 42.88 | 42.06 | K3 2 / K4 14 | all match |
| explain | 35.54 | 31.49 | 29.88 | 30.61 | K3 24 / K4 5 | all match |
| code | 35.59 | 37.35 | 36.90 | 34.59 | K3 16 / K4 7 | all match |

Long prompts, `-n 512`:

| Prompt | serial | fixed K3 | fixed K4 | adaptive K3/K4 | adaptive depth mix | stdout |
| --- | ---: | ---: | ---: | ---: | --- | --- |
| long_explain | 34.34 | 32.90 | 30.21 | 32.48 | K3 184 / K4 46 | all match |
| long_code_design | 34.25 | 33.36 | 30.46 | 32.36 | K3 195 / K4 33 | all match |
| long_debug_plan | 34.27 | 32.09 | 29.53 | 31.64 | K3 205 / K4 32 | all match |
| long_gsm_style | 34.38 | 35.25 | 35.59 | 35.35 | K3 37 / K4 22 | all match |

Accepted-row validation, adaptive policy, `DS4_MTP_NATIVE_VALIDATE=1`, `-n 32`:

| Prompt | stdout | depths | mismatches | max delta |
| --- | --- | --- | ---: | ---: |
| count | match | 2,3,4 | 0 | 0 |
| explain | match | 2,3,4 | 0 | 0 |
| code | match | 3,4 | 0 | 0 |

Decision:

- Drop this adaptive depth policy. It is correctness-clean, but it does not
  beat the best fixed depth on any tested prompt family.
- The controller entered K=4 too often for long-form prompts and too late for
  the full-accept count prompt. Tuning thresholds would mostly move it toward
  either fixed K3 or fixed K4 rather than create a robust policy.
- The probe code was removed after preserving the evidence here. The remaining
  actionable path is either a cheaper K=4 verifier body or an external policy
  signal strong enough to choose fixed K3 versus fixed K4 by workload class.

### 2026-05-17 Fresh Current-Binary Post-Batch-KV Depth Re-Sweep

Rationale:

- Batch-KV and batched attention-compressor projection changed per-row verifier
  economics, so the depth choice must be re-swept after those changes rather
  than inherited from an earlier K decision.
- The short three-prompt `-n 64` matrix is still smoke only. Its job here is to
  choose the representative candidate to send into HumanEval/GSM slices, not to
  decide promotion by itself.
- This run used the current Studio binary after the adaptive-depth probe code
  was removed, so the result reflects the current fixed-depth native path.

Artifacts:

- Three-prompt depth sweep:
  `/tmp/ds4-fresh-post-batchkv-depth-sweep-1778992872`
- HumanEval+ first-50 for the smoke winner:
  `/tmp/ds4-fresh-post-batchkv-k4-humaneval50-20260517_004251`
- GSM8K first-50 no-think for the smoke winner:
  `/tmp/ds4-fresh-post-batchkv-k4-gsm8k50-1778993362`

Native settings:

```sh
DS4_MTP_NATIVE=1
DS4_MTP_NATIVE_VERIFY_OPT=smallm
DS4_MTP_NATIVE_CACHE_MODE=owned
DS4_MTP_NATIVE_COMMIT_OPT=adaptive
DS4_MTP_NATIVE_SLOT_COMMIT=1
DS4_MTP_NATIVE_TOPID_FRONTIER=1
DS4_MTP_NATIVE_SMALLM_BATCH_COMPRESSOR_PROJ=1
DS4_MTP_GOVERNOR_DISABLE=1
```

Three-prompt smoke, `--ctx 1024 --nothink -sys "" --temp 0 -n 64`:

| Prompt | serial | K=2 | K=3 | K=4 | K=5 | smoke winner | stdout |
| --- | ---: | ---: | ---: | ---: | ---: | --- | --- |
| count | 35.50 | 31.87 | 37.77 | 42.76 | 36.91 | K=4 | all match |
| explain | 35.42 | 31.69 | 31.97 | 29.83 | 28.87 | K=3 | all match |
| code | 35.42 | 31.92 | 37.21 | 38.69 | 32.24 | K=4 | all match |

Native timing details:

| Prompt | K | cycles | avg accepted | avg discarded | verifier decode GPU | verifier total GPU |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| count | 2 | 32 | 2.000 | 0.000 | 27.979 ms | 29.188 ms |
| count | 3 | 21 | 3.000 | 0.000 | 41.794 ms | 42.976 ms |
| count | 4 | 16 | 4.000 | 0.000 | 54.516 ms | 55.785 ms |
| count | 5 | 16 | 4.000 | 0.938 | 67.334 ms | 68.752 ms |
| explain | 2 | 40 | 1.600 | 0.400 | 16.357 ms | 17.089 ms |
| explain | 3 | 30 | 2.133 | 0.833 | 30.255 ms | 31.145 ms |
| explain | 4 | 30 | 2.133 | 1.800 | 33.417 ms | 34.223 ms |
| explain | 5 | 25 | 2.560 | 2.280 | 48.350 ms | 49.414 ms |
| code | 2 | 34 | 1.853 | 0.147 | 23.599 ms | 24.637 ms |
| code | 3 | 24 | 2.667 | 0.333 | 34.992 ms | 35.997 ms |
| code | 4 | 21 | 3.048 | 0.905 | 40.578 ms | 41.569 ms |
| code | 5 | 20 | 3.200 | 1.700 | 58.675 ms | 59.958 ms |

HumanEval+ first-50, same-run serial versus native K=4:

| Mode | syntax | base pass@1 | plus pass@1 | tokens | elapsed | aggregate TPS | mean TPS | median TPS |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| serial | 50/50 | 0.640 | 0.620 | 4581 | 171.31 s | 26.74 | 25.02 | 25.36 |
| native K=4 | 50/50 | 0.640 | 0.620 | 4581 | 165.94 s | 27.61 | 25.84 | 25.87 |

GSM8K first-50, request-level no-think, same-run serial versus native K=4:

| Mode | accuracy | correct | tokens | elapsed | aggregate TPS | mean TPS | median TPS | output diffs vs serial |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| serial | 0.940 | 47/50 | 5983 | 202.68 s | 29.52 | 29.00 | 29.03 | - |
| native K=4 | 0.940 | 47/50 | 5983 | 199.55 s | 29.98 | 29.49 | 29.43 | 0 |

Interpretation:

- The user's depth-sweep concern is correct and should remain part of the
  workflow: whenever verifier row economics change, re-run K=2..5 before
  calling a depth "best."
- Under the current batch-KV plus batched-compressor-projection stack, K=4 is
  the current representative fixed-depth candidate: it wins two of the three
  smoke prompts and the smoke mean, then improves both representative
  first-50 slices without changing HumanEval pass/fail or GSM8K outputs.
- K=3 remains a real caveat for explanation-heavy workloads. It beats K=4 on
  the explain smoke because K=4 pays expensive discarded verifier rows there.
  That argues for either a cheaper K=4 verifier body or a better workload/depth
  selector, not for pretending the smoke mean settles every workload.
- This is positive current evidence, not a final architecture promotion. The
  next performance work should keep optimizing the K=4 verifier body and should
  include longer free-form prompts before choosing any default depth policy.

### 2026-05-17 Post-Sweep Small-M Tail Micro-Probes

Rationale:

- After the post-batch-KV sweep, `smallm_tail:attention_heads` remained one of
  the largest verifier stages. I tried two narrow probes that targeted the
  indexed-attention/indexer side of that tail while preserving exact stdout.
- Both probes were kept behind temporary env flags during measurement and then
  removed after evidence showed they did not materially improve the current
  workloads. This keeps the greenfield path small instead of accumulating
  dormant switches.

Artifacts:

- Batched indexed-attention smoke:
  `/tmp/ds4-smallm-batch-indexed-attn-smoke-1778994211`
- Batched indexed-attention validation:
  `/tmp/ds4-smallm-batch-indexed-attn-validate-1778994281`
- Batched indexed-attention profile:
  `/tmp/ds4-smallm-batch-indexed-attn-profile2-1778994379`
- Batched indexed-attention long-128 A/B:
  `/tmp/ds4-smallm-batch-indexed-attn-long128-1778994408`
- Batched indexer-projection smoke:
  `/tmp/ds4-smallm-batch-indexer-proj-smoke-1778994580`
- Batched indexer-projection long-256 A/B:
  `/tmp/ds4-smallm-batch-indexer-proj-long256-1778994659`

Probe A: batched indexed attention:

- Attempted to defer row-local indexed attention and run one
  `ds4_gpu_attention_indexed_mixed_batch_heads_tensor` over the small-M block.
- Required the same top-k row selection for each row; validation on count,
  explain, and code matched serial stdout, with accepted-row mismatch sum `0`
  and `max_delta=0`.
- It was effectively flat: count `42.91 -> 43.14 t/s`, explain
  `29.89 -> 29.76 t/s`, code `38.42 -> 38.53 t/s`. The profile did not show a
  durable new batched-attention stage on the tested shapes, so this is not a
  useful default path.

Probe B: batched indexer projection:

- Attempted to batch the indexer query and indexer-weight projections for the
  small-M verifier rows, while keeping compressor state updates, index scores,
  top-k selection, and row visibility ordered.
- Three-prompt smoke matched serial stdout but was mixed/noise-level:

| Prompt | serial | default K=4 | indexer-proj K=4 | default decode GPU | indexer-proj decode GPU |
| --- | ---: | ---: | ---: | ---: | ---: |
| count | 35.69 | 42.92 | 42.95 | 54.293 ms | 54.193 ms |
| explain | 35.65 | 29.77 | 29.92 | 33.606 ms | 33.338 ms |
| code | 35.65 | 38.74 | 38.62 | 40.467 ms | 40.767 ms |

- Longer `-n 256` A/B was also flat:

| Prompt | serial | default K=4 | indexer-proj K=4 | default decode GPU | indexer-proj decode GPU |
| --- | ---: | ---: | ---: | ---: | ---: |
| debug_plan | 34.70 | 29.58 | 29.55 | 36.866 ms | 36.936 ms |
| explain_long | 34.63 | 31.49 | 31.49 | 38.239 ms | 38.273 ms |
| gsm_style | 34.48 | 33.39 | 33.41 | 43.538 ms | 43.531 ms |

Decision:

- Drop both probes. They were correctness-clean on the tested prompts, but
  neither changed the verifier economics enough to justify carrying new
  runtime flags.
- The next optimizer should look lower in the actual indexed-attention math or
  at larger verifier-stage fusion. Batching these two dispatch-level fragments
  alone does not move K=4.

### 2026-05-17 Batched Compressor Projection Default Promotion

Rationale:

- The fresh post-batch-KV depth sweep and representative first-50 HumanEval/GSM
  slices used `DS4_MTP_NATIVE_SMALLM_BATCH_COMPRESSOR_PROJ=1`, but the code
  still required that env flag. That made the documented current winner depend
  on an opt-in flag even after the path had passed stdout and accepted-row
  validation.
- Promoting the path to default makes the optimized verifier body the native
  small-M baseline while preserving an explicit rollback switch.

Implementation:

- `metal_graph_mtp_native_smallm_batch_compressor_proj_enabled()` now defaults
  to enabled.
- `DS4_MTP_NATIVE_SMALLM_BATCH_COMPRESSOR_PROJ=0` and
  `DS4_MTP_NATIVE_SMALLM_BATCH_COMPRESSOR_PROJ_DISABLE=1` both force the old
  per-row projection path.
- No CUDA code was touched.

Artifacts:

- Current cleaned small-M profile before this default flip:
  `/tmp/ds4-current-clean-smallm-profile-20260517_011649`
- Default-vs-disable smoke and validation:
  `/tmp/ds4-batch-compressor-default-20260517_011935`

Current cleaned profile, `explain`, K=4, sync-heavy stage profiling:

| Stage | count | sum | avg |
| --- | ---: | ---: | ---: |
| `smallm:attn_tail_rows` | 258 | 592.822 ms | 2.298 ms |
| `smallm_tail:attention_heads` | 774 | 217.799 ms | 0.281 ms |
| `smallm:routed_moe` | 258 | 158.492 ms | 0.614 ms |
| `smallm_tail:attn_compressor` | 738 | 155.364 ms | 0.211 ms |
| `smallm_tail:raw_store` | 774 | 112.651 ms | 0.146 ms |
| `smallm:attn_batch_out` | 258 | 111.473 ms | 0.432 ms |
| `smallm_tail:indexer` | 738 | 96.366 ms | 0.131 ms |

Three-prompt smoke, default-on versus explicit disable, `K=4`, `-n 64`:

| Prompt | serial | default-on | disable flag | stdout | default decode GPU | disabled decode GPU |
| --- | ---: | ---: | ---: | --- | ---: | ---: |
| count | 35.51 | 42.69 | 42.32 | match | 54.519 ms | 55.325 ms |
| explain | 35.71 | 30.19 | 29.80 | match | 32.934 ms | 33.783 ms |
| code | 35.32 | 38.37 | 38.14 | match | 40.960 ms | 41.480 ms |

Accepted-row validation, default-on, `DS4_MTP_NATIVE_VALIDATE=1`, `-n 32`:

| Prompt | stdout vs serial `-n 32` | cycles | avg accepted | mismatches | max delta |
| --- | --- | ---: | ---: | ---: | ---: |
| count | match | 8 | 4.000 | 0 | 0 |
| explain | match | 13 | 2.385 | 0 | 0 |
| code | match | 9 | 3.444 | 0 | 0 |

Checks:

```sh
make ds4_test ds4 ds4-server
./ds4_test --metal-kernels
./ds4_test --metal-mtp-cache-contract
git diff --check -- ds4.c ds4_metal.m MTP_NATIVE_CACHE_CONTRACT_PROGRESS.md
```

The same build/check sequence passed in the Studio worktree after syncing
`ds4.c`.

Decision:

- Keep batched attention-compressor projection as the default native small-M
  verifier behavior. It is correctness-clean on the current matrix and reduces
  verifier decode GPU span by roughly `0.5..0.85 ms` per K=4 verifier call.
- This is an implementation promotion, not an architecture promotion. It
  removes avoidable verifier cost and makes the current K=4 baseline honest,
  but long-form workloads still need lower row-tail/indexed-attention cost or a
  better depth policy before a final promote/drop decision.

### 2026-05-17 Top-ID Frontier Defaultability Audit

Rationale:

- `DS4_MTP_NATIVE_TOPID_FRONTIER=1` is correctness-clean for temp-0 stdout and
  removes committed-row full-logit materialization/readback. It is tempting to
  promote it after making the compressor projection default.
- Unlike compressor projection, this changes the public session contract:
  after a speculative commit, the session may know only the exact top id rather
  than a fresh full logits vector.

Current code contract:

- `ds4_session_sample()` can consume `logits_top_id_valid` for
  `temperature <= 0`, so greedy generation is safe.
- `ds4_session_argmax()` also consumes the cached top id.
- `ds4_session_top_logprobs()` returns `0` when `logits_full_valid` is false and
  only `logits_top_id_valid` is set. This protects against serving stale logits,
  but it means logprob consumers lose the top-logprobs vector.
- CLI `--dump-logprobs` calls `ds4_session_top_logprobs()` before each greedy
  token. Server/API logprob-style consumers use the same public session surface.

Decision:

- Keep top-id frontier gated behind `DS4_MTP_NATIVE_TOPID_FRONTIER=1` for now.
- Do not promote it to default until the runtime has an explicit caller-visible
  contract for "greedy top-id-only is acceptable" versus "full logits/logprobs
  are required", or until top-only sessions can rematerialize full logits from
  committed target state without serial replay.
- This preserves the output-head optimization as an available exact temp-0
  speed knob while avoiding a hidden API/logprobs regression.

### 2026-05-17 Top-ID Frontier Stale-Logit Guard

Rationale:

- The top-id frontier path is a useful exact temp-0 verifier optimization, but
  it deliberately leaves `s->logits` stale after a speculative commit.
- The earlier audit showed that `ds4_session_top_logprobs()` already returned
  no data in top-id-only state, but other public consumers could still read the
  stale full-logit buffer.
- That made `DS4_MTP_NATIVE_TOPID_FRONTIER=1` unsafe outside the pure greedy
  generation path and blocked any future default promotion.

Implementation:

- `ds4_session_argmax()` still returns the cached exact top id in top-id-only
  state.
- `ds4_session_argmax_excluding()` now returns the cached top id only when it is
  not excluded; if the excluded token is the cached top id, it returns `-1`
  because the second-best token is not known without full logits.
- `ds4_session_sample()` now returns `-1` for non-greedy sampling if only top-id
  state is available, rather than sampling from stale logits.
- `ds4_session_top_logprobs()` and `ds4_session_token_logprob()` now require
  `logits_full_valid`.
- CLI and server decode loops now treat a negative sample id as a decode error,
  so accidental mixed-mode reuse fails visibly instead of emitting tokens from
  stale logits.
- The existing native MTP cache-contract selftest now covers full-logit versus
  top-id-only session behavior.

Checks:

```sh
make ds4_test ds4 ds4-server
./ds4_test --metal-mtp-cache-contract --server
./ds4_test --metal-kernels
git diff --check -- ds4.c ds4_cli.c ds4_server.c MTP_NATIVE_CACHE_CONTRACT_PROGRESS.md
```

Studio model-backed smoke:

- Artifact: `/tmp/ds4-topid-stale-guard-20260517_012951`
- Settings: native small-M, `K=4`, `DS4_MTP_NATIVE_TOPID_FRONTIER=1`,
  `DS4_MTP_NATIVE_COMMIT_OPT=adaptive`, `--ctx 1024 --nothink -sys "" --temp 0 -n 32`.

| Prompt | serial t/s | native top-id t/s | production stdout | validation stdout | validation mismatches | validation max delta |
| --- | ---: | ---: | --- | --- | ---: | ---: |
| count | 36.25 | 42.36 | match | match | 0 | 0 |
| explain | 36.09 | 30.23 | match | match | 0 | 0 |
| code | 35.92 | 39.38 | match | match | 0 | 0 |

Decision:

- Keep `DS4_MTP_NATIVE_TOPID_FRONTIER=1` gated for now.
- This patch removes the stale-logit safety blocker around the gated path, but
  it still does not provide a caller-visible "top-id-only is acceptable"
  contract or cheap full-logit rematerialization. Those remain required before
  making top-id frontier the default production behavior.

### 2026-05-17 Rejected Indexed-Attention RB8 Probe

Rationale:

- The cleaned profile still shows `smallm:attn_tail_rows` as the largest
  verifier bucket, and `smallm_tail:attention_heads` is a meaningful part of
  that tail.
- The existing decode indexed-attention kernel stages raw/compressed rows in
  groups of four (`rb4`). I tested an experimental rows-eight staging variant
  to cut threadgroup barriers while preserving the exact sequential
  row-consumption order.

Implementation:

- Added a temporary `kernel_dsv4_indexed_mixed_attention_heads8_rb8` Metal
  kernel and selected it with `DS4_METAL_INDEXED_ATTENTION_RB8=1`.
- The probe used the same online-softmax row order as `rb4`; only the staging
  group size changed from four rows to eight rows.
- After measurement, the code was removed to avoid carrying a dormant switch.

Artifact:

- `/tmp/ds4-indexed-attn-rb8-20260517_013541`

Three-prompt A/B, native small-M `K=4`, `DS4_MTP_NATIVE_TOPID_FRONTIER=1`,
`--ctx 1024 --nothink -sys "" --temp 0 -n 64`:

| Prompt | serial t/s | default t/s | rb8 t/s | default stdout | rb8 stdout | default decode GPU | rb8 decode GPU | validation mismatches | validation max delta |
| --- | ---: | ---: | ---: | --- | --- | ---: | ---: | ---: | ---: |
| count | 35.38 | 42.77 | 42.63 | match | match | 54.349 ms | 54.544 ms | 0 | 0 |
| explain | 35.34 | 29.92 | 29.87 | match | match | 33.204 ms | 33.338 ms | 0 | 0 |
| code | 35.43 | 38.41 | 38.69 | match | match | 40.912 ms | 40.539 ms | 0 | 0 |

Decision:

- Drop the RB8 probe. It was correctness-clean, but the performance signal is
  mixed and too small: code improved slightly, while count and explain regressed
  in both t/s and verifier decode GPU span.
- This suggests the current `rb4` staging is near the useful occupancy/barrier
  tradeoff for these verifier shapes. The next attention-tail attempt should
  target a larger structural issue, not just a bigger staging tile.

### 2026-05-17 Current Top-ID/Single-Command Depth Re-Sweep

Rationale:

- After verifier-economics changes such as batch-KV, batched compressor
  projection, top-id frontier work, and command-layout probes, the prior
  "best depth" is stale until `--mtp-draft 2..5` is swept again.
- The three-prompt `-n 64` run is only a selector for representative eval. It
  is not a promotion criterion.
- Because `DS4_MTP_NATIVE_VERIFY_SINGLE_COMMAND=1` was the live command-layout
  candidate, this sweep tested both the current default verifier layout and the
  single-command variant.

Artifacts:

- Three-prompt depth sweep:
  `/tmp/ds4-current-topid-depth-resweep-20260517_014410`
- HumanEval+ first-50:
  `/tmp/ds4-current-single-k4-humaneval50-20260517_014706`
- GSM8K first-50:
  `/tmp/ds4-current-single-k4-gsm8k50-20260517_015640`

Fixed-depth native settings:

```sh
DS4_MTP_NATIVE=1
DS4_MTP_NATIVE_VERIFY_OPT=smallm
DS4_MTP_NATIVE_CACHE_MODE=owned
DS4_MTP_NATIVE_COMMIT_OPT=adaptive
DS4_MTP_NATIVE_TOPID_FRONTIER=1
DS4_MTP_GOVERNOR_DISABLE=1
```

Three-prompt smoke, `--ctx 1024 --nothink -sys "" --temp 0 -n 64`:

| Prompt | serial | default K2 | default K3 | default K4 | default K5 | single K2 | single K3 | single K4 | single K5 |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| count | 35.44 | 31.69 | 37.63 | 42.71 | 37.01 | 31.93 | 38.10 | 42.98 | 36.97 |
| explain | 35.57 | 31.80 | 32.05 | 30.08 | 29.00 | 31.74 | 32.01 | 29.91 | 28.86 |
| code | 35.36 | 31.89 | 37.27 | 38.41 | 32.10 | 32.04 | 37.35 | 38.63 | 32.31 |

All native rows matched serial stdout. Smoke means:

| Candidate | mean t/s |
| --- | ---: |
| single K4 | 37.17 |
| default K4 | 37.07 |
| single K3 | 35.82 |
| default K3 | 35.65 |
| single K5 | 32.71 |
| default K5 | 32.70 |
| single K2 | 31.90 |
| default K2 | 31.79 |

HumanEval+ first-50, same-run serial/default/single:

| Mode | syntax | base pass@1 | plus pass@1 | tokens | elapsed | aggregate TPS | mean TPS | median TPS |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| serial | 50/50 | 0.640 | 0.620 | 4581 | 170.82 s | 26.82 | 25.09 | 25.39 |
| default K4 | 50/50 | 0.640 | 0.620 | 4581 | 166.08 s | 27.58 | 25.82 | 25.78 |
| single K4 | 50/50 | 0.640 | 0.620 | 4581 | 165.78 s | 27.63 | 25.87 | 25.83 |

The failed HumanEval base/plus task sets were identical across all three modes.

GSM8K first-50, request-level `think:false`, same-run serial/default/single:

| Mode | accuracy | correct | tokens | elapsed | aggregate TPS | mean TPS | median TPS | output diffs vs serial |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| serial | 0.920 | 46/50 | 4504 | 159.81 s | 28.18 | 27.51 | 27.70 | - |
| default K4 | 0.920 | 46/50 | 4504 | 155.99 s | 28.87 | 28.33 | 28.26 | 0 |
| single K4 | 0.920 | 46/50 | 4504 | 155.84 s | 28.90 | 28.36 | 28.21 | 0 |

Interpretation:

- The required post-change sweep confirms that K4 is still the current
  fixed-depth winner under this exact stack. K5 remains too expensive, while K3
  still matters for explanation-heavy prompts.
- `DS4_MTP_NATIVE_VERIFY_SINGLE_COMMAND=1` is the current measured winner, but
  the advantage over default K4 is very small: about `+0.10 t/s` on the smoke
  mean, `+0.05 aggregate TPS` on HumanEval+ first-50, and `+0.03 aggregate TPS`
  on GSM8K first-50.
- The representative slices are positive versus same-run serial and preserve
  quality/output on the tested distributions. This keeps native K4 plus
  single-command as the current candidate for the next verifier-efficiency
  iteration.
- Do not treat this as a final default-depth policy yet. Before promoting a
  default setting, add longer free-form generation coverage and decide whether
  the tiny single-command gain is enough to make it default-on or whether it
  should remain an optimization knob while larger verifier-body work continues.

### 2026-05-17 Greedy Top-ID Frontier Session Contract

Rationale:

- `DS4_MTP_NATIVE_TOPID_FRONTIER=1` had become a useful exact temp-0 output-head
  optimization, but it was still env-only because a speculative commit can leave
  the session with only the exact top id rather than a fresh full-logit vector.
- The stale-logit guard made this safe from accidental stale reads, but callers
  still needed a visible contract: greedy generation may accept top-id-only
  state; sampled/logprob callers require full logits.

Implementation:

- Added `ds4_session_set_greedy_top_id_frontier(session, enabled)`.
- `DS4_MTP_NATIVE_TOPID_FRONTIER` remains an explicit override. If it is unset,
  native MTP now enables top-id frontier only when the session has opted into
  the greedy contract.
- CLI one-shot generation and CLI chat opt in when `--temp 0` is active.
- Server requests opt in before prompt sync only for whole-request deterministic
  no-thinking/no-tools requests. Tool-call requests are excluded because their
  effective sampling mode can change inside a turn.
- Turning the greedy contract off while a reused session holds top-id-only
  logits invalidates the checkpoint and clears speculative MTP state, forcing
  the next `ds4_session_sync()` to rebuild full logits instead of sampling from
  unavailable logits.
- `--dump-logprobs`, sampled CLI/API generation, and dynamic tool-call requests
  keep the full-logit contract.

Checks:

```sh
make ds4_test ds4 ds4-server
./ds4_test --metal-mtp-cache-contract --server
./ds4_test --metal-kernels
git diff --check -- ds4.h ds4.c ds4_cli.c ds4_server.c
```

The same checks passed locally and in the Studio worktree.

Studio artifacts:

- CLI auto-top-id smoke:
  `/tmp/ds4-auto-topid-contract-smoke-final-20260517_021341`
- Server auto-top-id/sampled-mode smoke:
  `/tmp/ds4-auto-topid-server-smoke-final-20260517_021416`

CLI three-prompt smoke, native small-M fixed K4, single-command, no
`DS4_MTP_NATIVE_TOPID_FRONTIER` env var:

| Prompt | serial t/s | native auto top-id t/s | stdout | topid_frontier | logits_read | mismatches |
| --- | ---: | ---: | --- | --- | ---: | ---: |
| count | 36.33 | 43.73 | match | 1 | 0.000 ms | 0 |
| explain | 36.68 | 30.50 | match | 1 | 0.000 ms | 0 |
| code | 36.04 | 39.67 | match | 1 | 0.000 ms | 0 |

Server smoke:

- First request: temp-0 chat, no top-id env var, produced 64 tokens with native
  timing rows reporting `topid_frontier=1` and `single_command=1`.
- Second request on the same server: temp-0.7 chat completed successfully
  (`Hello!`, 2 tokens), proving the contract switch does not leave the session
  stuck in top-id-only state before sampled generation.

Decision:

- Promote top-id frontier from env-only to caller-contract default for exact
  greedy CLI/server generation.
- Keep the explicit env override for diagnostics and keep full-logit behavior
  for logprob/sampled/dynamic requests.
- This does not change the verifier math, so the representative first-50
  HumanEval+/GSM8K evidence from the current top-id/single-command K4 path still
  applies. The next performance work should now treat top-id frontier as part of
  the greedy native baseline and continue attacking verifier-body cost.

### 2026-05-17 Native Single-Command Default and Current K Sweep

Rationale:

- The current small-M hot-path profile still points at verifier body cost rather
  than replay/commit: `attn_tail_rows`, attention heads, routed MoE,
  compressor/indexer, and attention output remain the material stages.
- The previously rejected attention-output/HC fused hook was rechecked because
  later verifier fixes could have invalidated that rejection. It is now exact on
  the current stack, but not useful as a default: it was flat/mixed on the
  three-prompt matrix.
- The safe command-layout optimization was still opt-in through
  `DS4_MTP_NATIVE_VERIFY_SINGLE_COMMAND=1`. Earlier evidence showed it was the
  measured K4 winner, so this pass makes it the production default for native
  small-M greedy top-id-only verification while keeping the env var as an
  explicit override.

Implementation:

- Added an internal native verifier single-command default latch.
- `DS4_MTP_NATIVE_VERIFY_SINGLE_COMMAND` still overrides explicitly when set.
- Native small-M sets the default only for production greedy top-id verification
  (`top_only`, no debug validation). Validation/full-logit diagnostic paths keep
  the split-command shape unless the env var forces otherwise.
- No CUDA changes.

Checks:

```sh
make ds4_test ds4 ds4-server
./ds4_test --metal-mtp-cache-contract --server
./ds4_test --metal-kernels
git diff --check -- ds4.c MTP_NATIVE_CACHE_CONTRACT_PROGRESS.md
```

The same checks passed locally and in the Studio worktree.

Studio artifacts:

- Hot-path profile:
  `/tmp/ds4-current-smallm-hotpath-profile-20260517_021931`
- Attention-output/HC fused recheck:
  `/tmp/ds4-current-attn-out-fused-ab-20260517_022405`
- Default single-command depth sweep:
  `/tmp/ds4-singlecmd-default-depth-sweep-20260517_022641`
- HumanEval+ first-50, native default K4:
  `/tmp/ds4-singlecmd-default-k4-humaneval50-20260517_022834`
- GSM8K first-50, same-run serial/native default K4:
  `/tmp/ds4-singlecmd-default-k4-gsm8k50-20260517_023216`

Current small-M hot-path profile, explain K4 with stage profiling:

| Stage | Count | Sum | Avg |
| --- | ---: | ---: | ---: |
| `smallm:attn_tail_rows` | 774 | 1757.618 ms | 2.271 ms |
| `smallm_tail:attention_heads` | 2322 | 650.497 ms | 0.280 ms |
| `smallm:routed_moe` | 774 | 479.268 ms | 0.619 ms |
| `smallm_tail:attn_compressor` | 2214 | 456.691 ms | 0.206 ms |
| `smallm:attn_batch_out` | 774 | 332.952 ms | 0.430 ms |
| `smallm_tail:indexer` | 2214 | 284.366 ms | 0.128 ms |

The `raw_store` stage also appears in the profile, but with batch-KV enabled it
is mostly a profiling-boundary cost rather than the old per-row store.

Attention-output/HC fused recheck, K4, no default change:

| Prompt | serial | default K4 | fused K4 | default stdout | fused stdout | mismatches |
| --- | ---: | ---: | ---: | --- | --- | ---: |
| count | 35.45 | 42.97 | 42.88 | match | match | 0 |
| explain | 35.42 | 30.05 | 29.97 | match | match | 0 |
| code | 35.39 | 38.54 | 38.58 | match | match | 0 |

Decision: leave the attention-output/HC fused hook gated. It is no longer a
correctness failure on this stack, but the speed signal is flat/mixed.

Default single-command depth sweep, no `DS4_MTP_NATIVE_VERIFY_SINGLE_COMMAND`
env var, `--ctx 1024 --nothink -sys "" --temp 0 -n 64`:

| Prompt | serial | K2 | K3 | K4 | K5 | stdout |
| --- | ---: | ---: | ---: | ---: | ---: | --- |
| count | 35.69 | 31.93 | 37.95 | 43.07 | 37.19 | all match |
| explain | 35.50 | 31.74 | 31.96 | 29.98 | 28.76 | all match |
| code | 35.62 | 31.87 | 37.31 | 38.65 | 32.23 | all match |

Smoke means:

| Depth | mean t/s |
| --- | ---: |
| K2 | 31.847 |
| K3 | 35.740 |
| K4 | 37.233 |
| K5 | 32.727 |

All native rows reported `topid_frontier=1`, mismatch sum `0`, and stdout match
against serial. K2 reports `single_command=0` because its native verifier shape
often collapses to a one-row suffix; K3/K4/K5 report single-command use for the
M=2..4 verifier cycles.

HumanEval+ first-50, current native default K4 compared with the prior serial
artifact:

| Mode | syntax | base pass@1 | plus pass@1 | tokens | elapsed | aggregate TPS | mean TPS | median TPS | output diffs vs serial |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| native default K4 | 50/50 | 0.640 | 0.620 | 4581 | 165.44 s | 27.69 | 25.93 | 25.99 | 0 |

The failed HumanEval base/plus task sets match the previous serial/single K4
evidence.

GSM8K first-50, request-level `think:false`, same-run serial/native default K4:

| Mode | accuracy | correct | tokens | elapsed | aggregate TPS | mean TPS | median TPS | output diffs vs serial |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| serial | 0.980 | 49/50 | 4977 | 174.12 s | 28.58 | 28.07 | 28.27 | - |
| native default K4 | 0.980 | 49/50 | 4977 | 169.95 s | 29.28 | 28.82 | 28.81 | 0 |

Decision:

- Promote single-command verifier execution from env-only to the native greedy
  small-M default.
- Keep K4 as the current fixed-depth candidate after the required K=2..5
  re-sweep. K3 remains the caveat for explanation-heavy prompts.
- This is a small but real cleanup/performance default, not the final
  architecture-level promote/drop decision. The next verifier-efficiency target
  remains GPU body math: exact attention-head/compressor/indexer batching or a
  routed/shared FFN improvement with bit/ULP audits.

### 2026-05-17 Depth Sweep Harness

Rationale:

- Verifier-economics changes can shift the optimal native depth. Batch-KV,
  compressor projection, output-head/top-id, command-layout, and small-M kernel
  changes all alter the per-row cost curve, so the current depth winner must not
  be reused blindly after those changes.
- Added `tools/mtp_native_depth_sweep.sh` to make the required selector
  repeatable: serial target plus native `--mtp-draft 2,3,4,5` on the standard
  count/explain/code smoke prompts, with stdout comparison and native-cycle
  metric aggregation.

Default command shape for `studio.local`:

```sh
tools/mtp_native_depth_sweep.sh \
  --model "$MODEL" \
  --mtp "$MTP" \
  --prompt-dir /tmp/ds4-mtp-matrix
```

The generated `summary.tsv` records t/s, stdout match/diff, cycle count, average
accepted/discarded rows, verifier GPU spans, mismatch sum, and top-id-frontier
coverage. The generated `depth-ranking.tsv` selects the smoke winner by mean
t/s across prompts. That winner then goes to the representative HumanEval/EvalPlus
and GSM8K slices; the smoke is only a selector, not promotion evidence by itself.

Harness validation run on `studio.local`:

- Artifact: `/tmp/ds4-native-depth-sweep-harness-q4mtp-20260517_025327`
- Model: `DeepSeek-V4-Flash-IQ2XXS-w2Q2K-AProjQ8-SExpQ8-OutQ8-chat-v2.gguf`
- MTP: `DeepSeek-V4-Flash-MTP-Q4K-Q8_0-F32.gguf`
- Command: `tools/mtp_native_depth_sweep.sh --model "$MODEL" --mtp "$MTP" --prompt-dir /tmp/ds4-mtp-matrix`

| Prompt | serial | K2 | K3 | K4 | K5 | stdout |
| --- | ---: | ---: | ---: | ---: | ---: | --- |
| count | 36.88 | 32.84 | 39.53 | 44.82 | 39.07 | all match |
| explain | 36.45 | 33.10 | 32.91 | 32.14 | 31.50 | all match |
| code | 36.76 | 32.78 | 37.73 | 33.41 | 36.98 | all match |

Harness ranking:

| Depth | mean t/s |
| --- | ---: |
| K2 | 32.907 |
| K3 | 36.723 |
| K4 | 36.790 |
| K5 | 35.850 |

This validates the harness and reinforces the depth-selection rule: K4 narrowly
wins this exact smoke by mean, but K3/K5 remain workload-sensitive challengers.
Any next verifier-economics patch must re-run this selector before choosing the
representative HumanEval/GSM depth.

### 2026-05-17 Native Output Top-Id-Only Verifier Rows

Implementation:

- Added an exact native verifier output-head path for `top_k=1` rows that only
  need target top ids. The path still performs the exact Q8 output projection
  and argmax, but it avoids writing full-vocab logits for verifier rows when
  native top-id frontier mode can carry the committed top id.
- Added a `rows1` top-id kernel entry so the common one-row native suffix
  shape can avoid falling back to a full-logits output projection.
- Validation/debug modes keep the full-logit path unless explicitly audited.
- Added a durable GSM8K gate:
  `tools/gsm8k_ds4.py` plus `tools/mtp_gsm8k_gate.sh`.

Local/Studio checks:

- Local: `make ds4_test ds4`, `make ds4-server`,
  `./ds4_test --metal-kernels`, `./ds4_test --metal-sched2`,
  `./ds4_test --metal-block-verifier`, `git diff --check`.
- Studio: `make ds4_test ds4`, `./ds4_test --metal-kernels`,
  `./ds4_test --metal-sched2`, `./ds4_test --metal-block-verifier`,
  `git diff --check`.

Audited top-id-only smoke:

- Artifact: `/tmp/ds4-native-top1only-audit-20260517_030301`
- Command shape: `tools/mtp_native_depth_sweep.sh ... -n 16`
  with `DS4_MTP_NATIVE_OUTPUT_FUSED_TOP1_AUDIT=1`.
- Result: all K2..K5 rows matched serial stdout; audit mismatch sum was `0`.

Fresh required post-change depth sweep:

- Artifact: `/tmp/ds4-native-top1only-depth-sweep-20260517_030351`
- Model: `DeepSeek-V4-Flash-IQ2XXS-w2Q2K-AProjQ8-SExpQ8-OutQ8-chat-v2.gguf`
- MTP: `DeepSeek-V4-Flash-MTP-Q4K-Q8_0-F32.gguf`
- Command: `tools/mtp_native_depth_sweep.sh --model "$MODEL" --mtp "$MTP" --prompt-dir /tmp/ds4-mtp-matrix -n 64`

| Prompt | serial | K2 | K3 | K4 | K5 | stdout |
| --- | ---: | ---: | ---: | ---: | ---: | --- |
| count | 36.86 | 32.95 | 39.63 | 44.89 | 39.36 | all match |
| explain | 36.94 | 33.11 | 32.84 | 32.24 | 31.43 | all match |
| code | 36.70 | 32.88 | 37.89 | 33.45 | 37.17 | all match |

Ranking:

| Depth | mean t/s |
| --- | ---: |
| K2 | 32.980 |
| K3 | 36.787 |
| K4 | 36.860 |
| K5 | 35.987 |

Timing detail:

| Prompt | K | cycles | avg accepted | avg discarded | verifier decode GPU | verifier total GPU |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| count | 2 | 32 | 2.000 | 0.000 | 27.041 ms | 28.183 ms |
| count | 3 | 21 | 3.000 | 0.000 | 0.000 ms | 40.204 ms |
| count | 4 | 16 | 4.000 | 0.000 | 0.000 ms | 52.141 ms |
| count | 5 | 16 | 4.000 | 0.938 | 0.000 ms | 62.992 ms |
| explain | 2 | 34 | 1.882 | 0.118 | 22.981 ms | 23.993 ms |
| explain | 3 | 30 | 2.133 | 0.833 | 0.865 ms | 30.588 ms |
| explain | 4 | 30 | 2.133 | 0.867 | 0.871 ms | 31.414 ms |
| explain | 5 | 30 | 2.133 | 1.000 | 0.849 ms | 32.650 ms |
| code | 2 | 34 | 1.882 | 0.118 | 23.364 ms | 24.381 ms |
| code | 3 | 23 | 2.739 | 0.261 | 0.000 ms | 37.175 ms |
| code | 4 | 34 | 1.882 | 0.265 | 20.970 ms | 23.458 ms |
| code | 5 | 20 | 3.150 | 0.950 | 0.000 ms | 47.761 ms |

Representative HumanEval+ first-50, same-run serial/native K4:

- Artifact: `/tmp/ds4-top1only-k4-humaneval50-20260517_030615`
- Native settings: `DS4_MTP_NATIVE=1 DS4_MTP_NATIVE_VERIFY_OPT=smallm DS4_MTP_NATIVE_CACHE_MODE=owned --mtp-draft 4`
- Output samples: byte-identical, `sample_diffs=0`.

| Mode | syntax | base pass@1 | plus pass@1 | tokens | elapsed | aggregate TPS | mean TPS | median TPS |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| serial | 50/50 | 0.940 | 0.900 | 4212 | 155.66 s | 27.06 | 25.17 | 26.00 |
| native K4 top-id-only | 50/50 | 0.940 | 0.900 | 4212 | 151.19 s | 27.86 | 25.86 | 26.75 |

Failed tasks matched exactly:

- Base: `HumanEval/10`, `HumanEval/17`, `HumanEval/32`
- Plus: `HumanEval/10`, `HumanEval/17`, `HumanEval/32`,
  `HumanEval/39`, `HumanEval/49`

Representative GSM8K first-50, request-level `think:false`, same-run serial/native K4:

- Artifact: `/tmp/ds4-top1only-k4-gsm8k50-20260517_031305`
- Dataset: `/tmp/ds4-smallm-gsm8k20-20260516_033442/gsm8k_test.jsonl`
- Native settings: `DS4_MTP_NATIVE=1 DS4_MTP_NATIVE_VERIFY_OPT=smallm DS4_MTP_NATIVE_CACHE_MODE=owned --mtp-draft 4`
- Output samples: byte-identical, `output_diffs=0`.

| Mode | accuracy | correct | tokens | elapsed | aggregate TPS | mean TPS | median TPS | failed |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | --- |
| serial | 0.980 | 49/50 | 6673 | 217.12 s | 30.73 | 29.82 | 29.89 | `8` |
| native K4 top-id-only | 0.980 | 49/50 | 6673 | 218.06 s | 30.60 | 29.81 | 29.75 | `8` |

Interpretation:

- The user's depth-selection concern is confirmed and is now encoded as a
  harness rule: after verifier-economics changes, re-sweep K2..K5 before
  choosing the representative eval depth.
- The top-id-only output-head path is correct under audit and helps avoid
  needless full-vocab writes/readbacks, but it does not materially shift the
  optimal depth. K4 remains the narrow smoke-mean winner; K3 remains the
  explanation-heavy challenger.
- HumanEval+ first-50 shows a clean same-quality speedup for K4 in this run.
- GSM8K first-50 is quality-identical and byte-identical but essentially flat
  to slightly negative versus serial in this run.
- This is useful verifier cleanup, not final architecture promotion. The next
  meaningful TPS work is still inside verifier-body GPU math and workload/depth
  policy, not another decision based on short prompt smoke alone.

### 2026-05-17 Native + Sched2 Continuation Overlap Probe

Implementation:

- Added `DS4_MTP_NATIVE_SCHED2_CONT=1` as a native-only overlap probe.
- The probe keeps normal native output semantics unchanged. After the native
  chain has produced the current suffix plus its normal preview token, it starts
  an MTP async continuation command buffer from the preview while the target
  verifier checks the current suffix.
- The continuation is discarded in this first probe. This intentionally avoids
  committing a lower-fidelity pretarget MTP state before proving that the GPU can
  overlap the work.
- `DS4_MTP_NATIVE_SCHED2_CONT_M=N` controls continuation depth. Timing lines now
  report `sched2_cont_*` fields, including target/MTP GPU spans and overlap.

Local/Studio checks:

- Local: `make ds4_test ds4`, `./ds4_test --metal-kernels`,
  `./ds4_test --metal-sched2`, `./ds4_test --metal-block-verifier`,
  `./ds4_test --metal-mtp-cache-contract`, `git diff --check -- ds4.c`.
- Studio: same build and Metal harness checks in
  `/Users/studio/git/.worktrees/antirez/ds4/mtp-native-cache-contract`.

Studio smoke matrix:

- Baseline/probe artifact: `/tmp/ds4-native-sched2-cont-20260517_034543`
- Heavier continuation artifact: `/tmp/ds4-native-sched2-contM4-20260517_034804`
- Timeline artifact: `/tmp/ds4-native-sched2-cont-timeline-20260517_034913`
- Validation artifact: `/tmp/ds4-native-sched2-cont-validate-20260517_034954`

`cont_m=1`:

| Prompt | K | baseline t/s | cont t/s | stdout | started/cycles | avg target GPU | avg MTP GPU | avg overlap |
| --- | ---: | ---: | ---: | --- | ---: | ---: | ---: | ---: |
| count | 3 | 39.33 | 39.28 | match | 21/21 | 40.462 ms | 1.601 ms | 0.000 ms |
| count | 4 | 44.90 | 44.85 | match | 16/16 | 52.003 ms | 1.601 ms | 0.000 ms |
| explain | 3 | 32.42 | 32.27 | match | 23/30 | 31.135 ms | 1.223 ms | 0.000 ms |
| explain | 4 | 32.16 | 32.17 | match | 23/30 | 31.348 ms | 1.219 ms | 0.000 ms |
| code | 3 | 37.89 | 37.66 | match | 21/23 | 37.383 ms | 1.453 ms | 0.000 ms |
| code | 4 | 33.25 | 33.25 | match | 28/34 | 23.477 ms | 1.312 ms | 0.000 ms |

`cont_m=4`:

| Prompt | K | t/s | stdout | started/cycles | avg target GPU | avg MTP GPU | avg overlap |
| --- | ---: | ---: | --- | ---: | ---: | ---: | ---: |
| count | 3 | 37.25 | match | 21/21 | 40.333 ms | 6.430 ms | 0.000 ms |
| count | 4 | 42.89 | match | 16/16 | 52.138 ms | 6.419 ms | 0.000 ms |
| explain | 3 | 30.70 | match | 23/30 | 31.012 ms | 4.923 ms | 0.000 ms |
| explain | 4 | 30.65 | match | 23/30 | 31.187 ms | 4.915 ms | 0.000 ms |
| code | 3 | 35.73 | match | 21/23 | 37.316 ms | 5.889 ms | 0.000 ms |
| code | 4 | 30.99 | match | 28/34 | 23.499 ms | 5.303 ms | 0.000 ms |

Timeline evidence:

- With `DS4_METAL_COMMAND_TIMELINE=1`, the MTP continuation command buffer is
  committed before the target verifier, but Metal executes it immediately before
  the target verifier rather than overlapping it.
- Example K4 cycle from the timeline artifact:
  - MTP continuation GPU span: about `6.390 ms`
  - Target verifier GPU span: about `52.811 ms`
  - Gap: about `0.001 ms`
  - Overlap: `0.000 ms`

Interpretation:

- Combining native with the existing async/sched2-style continuation does not
  currently produce useful GPU overlap. Making the continuation heavier only
  serializes more MTP work in front of the target verifier and reduces TPS.
- This does not prove a dependency-aware target-first scheduler is impossible,
  but it does show that the current two-queue commit shape is not enough to make
  native verification and native continuation run concurrently on Studio.
- Keep this path as an evidence probe only. Do not promote it unless a later
  scheduler rewrite can show nonzero GPU overlap with the same native verifier
  contract.

### 2026-05-17 Target-First Native Scheduler Probe

Implementation:

- Added an Apple-only target command-buffer primitive:
  - `ds4_gpu_end_commands_no_wait()`
  - `ds4_gpu_wait_target_async_commands()`
- Added `DS4_MTP_NATIVE_TARGET_FIRST_CONT=1`.
- Unlike `DS4_MTP_NATIVE_SCHED2_CONT=1`, this submits the target verifier first
  without waiting, then submits the MTP continuation while the target verifier is
  still pending.
- The hook is deliberately narrow: it only arms for the native small-M,
  top-id-only, single-command verifier path. It does not alter default native
  decoding and does not promote the continuation tokens yet.

Local/Studio checks:

- Local: `make ds4_test ds4`, `./ds4_test --metal-kernels`,
  `./ds4_test --metal-sched2`, `./ds4_test --metal-block-verifier`,
  `./ds4_test --metal-mtp-cache-contract`, `git diff --check`.
- Studio: same build and Metal harness checks in
  `/Users/studio/git/.worktrees/antirez/ds4/mtp-native-cache-contract`.

Timeline evidence:

- Artifact: `/tmp/ds4-native-targetfirst-timeline-20260517_041558`
- Settings: `DS4_MTP_NATIVE_TARGET_FIRST_CONT=1`,
  `DS4_MTP_NATIVE_SCHED2_CONT_M=4`, native `K=4`, count prompt, `-n 8`.
- Result: real GPU overlap is achieved.

Example first cycle:

- Target verifier GPU span: `51.266 ms`
- MTP continuation GPU span: `57.938 ms`
- Overlap: `51.266 ms`
- Overlap percentage: `100.00%`

The important caveat is resource contention: the same continuation work that
previously took about `6.4 ms` when serialized stretches to about `58 ms` when
run concurrently with the target verifier.

Studio smoke matrix:

- Artifact: `/tmp/ds4-native-targetfirst-20260517_041620`
- Baseline native settings: `DS4_MTP_NATIVE=1 DS4_MTP_NATIVE_TIMING=1
  DS4_MTP_NATIVE_VERIFY_OPT=smallm DS4_MTP_NATIVE_CACHE_MODE=owned`
- Target-first settings add `DS4_MTP_NATIVE_TARGET_FIRST_CONT=1`.

| Prompt | K | baseline t/s | target-first M1 t/s | target-first M4 t/s | stdout |
| --- | ---: | ---: | ---: | ---: | --- |
| count | 3 | 39.53 | 38.63 | 36.44 | match |
| count | 4 | 44.80 | 44.33 | 42.10 | match |
| explain | 3 | 32.42 | 31.76 | 30.35 | match |
| explain | 4 | 32.29 | 31.72 | 30.18 | match |
| code | 3 | 37.68 | 37.00 | 34.98 | match |
| code | 4 | 33.05 | 33.37 | 33.30 | match |

Overlap summary:

| Mode | Prompt | K | started/cycles | avg target GPU | avg MTP GPU | avg overlap | avg overlap pct |
| --- | --- | ---: | ---: | ---: | ---: | ---: | ---: |
| M1 | count | 3 | 21/21 | 40.250 ms | 41.859 ms | 40.247 ms | 99.99% |
| M1 | count | 4 | 16/16 | 51.765 ms | 53.398 ms | 51.763 ms | 99.99% |
| M1 | explain | 3 | 22/30 | 30.232 ms | 31.414 ms | 30.230 ms | 73.33% |
| M1 | explain | 4 | 22/30 | 30.412 ms | 31.606 ms | 30.410 ms | 73.33% |
| M1 | code | 3 | 21/23 | 37.405 ms | 38.877 ms | 37.403 ms | 91.30% |
| M4 | count | 3 | 21/21 | 40.350 ms | 46.632 ms | 40.218 ms | 99.67% |
| M4 | count | 4 | 16/16 | 51.875 ms | 58.157 ms | 51.750 ms | 99.76% |
| M4 | explain | 3 | 22/30 | 30.081 ms | 34.692 ms | 29.993 ms | 73.12% |
| M4 | explain | 4 | 22/30 | 30.313 ms | 34.953 ms | 30.234 ms | 73.14% |
| M4 | code | 3 | 21/23 | 37.451 ms | 43.194 ms | 37.344 ms | 91.04% |

Audit:

- Artifact: `/tmp/ds4-native-targetfirst-audit-20260517_041842`
- `K=4`, count prompt, `-n 16`, `DS4_MTP_NATIVE_OUTPUT_FUSED_TOP1_AUDIT=1`
- Stdout matched serial and audit mismatch count stayed `0`.

Interpretation:

- The scheduler question has changed: target-first no-wait submission can
  produce real target/MTP overlap on Studio.
- The current continuation probe is not throughput-positive because overlapped
  MTP work contends so heavily with target verification that it lengthens to
  almost the target span. Since the continuation is also discarded in this probe,
  M1/M4 are expected to be neutral-to-negative.
- A promotable follow-up would need to store and consume the continuation queue
  transactionally, then measure whether avoiding the next-cycle draft work
  offsets the contention. That is a state-contract problem, not another Metal
  scheduling proof.
