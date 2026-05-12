# MTP q4 Findings on studio.local

Date: 2026-05-10

Host: `studio.local` (M3 Ultra, q4 base model)

Model paths:

- Base: `/Users/studio/.ds4/cache/gguf/DeepSeek-V4-Flash-Q4KExperts-F16HC-F16Compressor-F16Indexer-Q8Attn-Q8Shared-Q8Out-chat-v2.gguf`
- MTP: `/Users/studio/.ds4/cache/gguf/DeepSeek-V4-Flash-MTP-Q4K-Q8_0-F32.gguf`

Prompt:

```text
Write a concise technical explanation of how Redis Streams support consumer groups, pending entries, and message acknowledgement.
```

## Current Result

Exact row-preserving N=2 verification is hash-identical, but still slower than
baseline. The best single-path exact policy tested so far is the promoted
strict default: split-head N=2 verification, target-first probing, adaptive
cooldown, and exact-safe target-margin probe gating. The default target-margin
threshold is `5.0` for the target-first path and `2.0` for the older
non-target-first diagnostic path:

```sh
tools/mtp_benchmark.sh --runs 5 --tokens 128 --draft 2 --margin 4 ...
```

Latest 5-run rotated interleaved medians for the current default after the
`DS4_MTP_SPEC_DISABLE=1` no-open cleanup:

| Mode | Median generation TPS | Output |
| --- | ---: | --- |
| baseline | 35.00 | baseline hash |
| MTP spec disabled, no-open | 35.01 | identical |
| exact MTP | 34.64 | identical |
| `--mtp-speed` | 33.50 | drifted |

The no-open disabled control is now baseline-clean
(`disabled_vs_baseline=1.000`), but the current exact path still misses the
active parity gate (`exact_vs_baseline=0.990`) over repeated interleaved runs.

An N=2 split-head verifier variant was then promoted as the default strict
path. It keeps the exact row-preserving layer pass, checks row 0 before
computing row 1's output head, and falls back to the captured prefix-1 state on
partial accept. A direct A/B run showed a small exact-mode improvement with no
hash drift:

| Exact N=2 path | Median generation TPS | Output |
| --- | ---: | --- |
| previous exact default | 34.13 | baseline hash |
| split-head exact | 34.27 | baseline hash |

The full interleaved split-head run still did not reach baseline parity:
baseline `34.90 TPS`, disabled `34.47 TPS`, exact `34.32 TPS`,
`--mtp-speed` `34.60 TPS` with drift.

`tools/mtp_benchmark.sh` now resolves the q4 base model and MTP GGUF from
`$HOME/.ds4/cache` or `$HOME/.ds4/cache/gguf`, with the previous repo-local
paths kept as fallbacks for manual checkouts. It also prints
`disabled_vs_baseline`, `exact_vs_baseline`, `speed_vs_baseline`, and strict
hash-match flags so the parity gate is visible directly in the benchmark
summary.

The strict MTP policy now enables adaptive cooldown by default, with a default
cooldown of 10 target tokens after unprofitable speculative attempts. It also
uses the exact-safe target-margin pre-probe gate by default. The normal
target-first path uses threshold `5.0`; the older non-target-first diagnostic
path uses threshold `2.0`. Use `DS4_MTP_NO_ADAPTIVE=1` to disable adaptive
cooldown, `DS4_MTP_ADAPTIVE_SKIP=N` to retune the cooldown,
`DS4_MTP_TARGET_MARGIN_SKIP=N` to retune the target-margin gate, or
`DS4_MTP_NO_TARGET_MARGIN_SKIP=1` to disable the gate. The no-probe path keeps
adaptive timing and margin gating disabled, and `DS4_MTP_SPEC_DISABLE=1` now
skips MTP model open/mapping entirely, so the disabled control is a plain
no-spec baseline even when the CLI is passed `--mtp`.

The promoted no-env policy is much closer, but remains below baseline on the
same prompt over 5 interleaved runs: baseline `35.92 TPS`, disabled
`35.71 TPS`, exact `35.55 TPS`; the three correctness-preserving modes were
hash-identical.

A fresh quiet-host rerun after the FAST_FRONTIER falsification kept the same
conclusion and widened the measured exact gap: baseline `35.76 TPS`, disabled
`35.61 TPS`, exact `34.64 TPS`, and speed `34.56 TPS`. Baseline, disabled, and
exact shared hash `37043619...`; speed drifted to `2c10263f...`. This is the
current promoted-default gate result.

The matching `DS4_MTP_STATS=1` run shows why N=2 remains below parity on this
prompt. It attempted 14 speculative steps, skipped 95 via adaptive cooldown,
drafted 24 suffix tokens, and committed 12 tokens via 6 full accepts. The timing
estimate was still net negative: saved target work `335.761 ms`, extra MTP work
`360.889 ms`, net `-25.128 ms`. In other words, the strict path is now close
enough that the remaining loss is roughly the MTP probe plus verifier overhead
for a small number of full accepts, not an obvious stray readback or snapshot.

A newer per-attempt timing run after the no-open cleanup reached the same
diagnosis with cleaner controls:
`/tmp/ds4-mtp-current-attempt-profile-20260510.err` recorded 15 MTP probes,
15 target-margin pre-probe skips, 9 exact margin skips after the second draft,
and 6 verified full accepts with zero partial accepts. The verified attempts
spent `304.837 ms` in the split-head verifier and the run estimated
`334.449 ms` saved versus `353.313 ms` extra, net `-18.864 ms`. The remaining
gap is therefore full-accept verifier cost, not partial replay or state
rollback.

A follow-up validation pass on the same Studio q4 setup produced the same
conclusion after the latest diagnostic changes. The current-default benchmark
(`/tmp/ds4-mtp-current-default-20260510-063119.{log,csv}`) measured baseline
`34.75 TPS`, MTP-loaded disabled `34.38 TPS`, exact MTP `34.26 TPS`, and
`--mtp-speed` `33.92 TPS`. Baseline, disabled, and exact shared hash
`37043619...`; speed drifted to `2c10263f...`. The gated MTP oracle passed with
the q4 base and MTP cache paths, but broad `make test` is not a clean q4 signal
on this host because the long-context and official-vector fixtures fail under
the default q4 setup.

After reverting the non-promotable greedy-top-only and pair+second-top-k
diagnostics, the current no-env q4 gate still missed parity. The q4 MTP oracle
passed, and `/tmp/ds4-mtp-current-noenv-final-5run-20260510.csv` measured
baseline `35.44 TPS`, disabled/no-open `35.44 TPS`
(`disabled_vs_baseline=1.000`), exact `34.71 TPS`
(`exact_vs_baseline=0.979`), and speed `33.44 TPS`. Baseline, disabled, and
exact were hash-identical. This is the cleanest current checkpoint: the
disabled/no-open control is baseline-clean, but exact N=2 remains below
baseline with no output drift.

A later idle-host refresh after linking the MTP GGUF into
`/Users/studio/.ds4/cache/gguf` kept the same conclusion with a clean disabled
control. `/tmp/ds4-mtp-refresh-20260510-121605.csv` measured baseline
`36.13 TPS`, disabled/no-open `36.10 TPS`
(`disabled_vs_baseline=0.999`), exact `35.61 TPS`
(`exact_vs_baseline=0.986`), and speed `34.55 TPS`. The matching profile
(`/tmp/ds4-mtp-refresh-profile-20260510-121605.err`) recorded target average
`27.866 ms`, probe average `1.041 ms`, `commit_hist=0:8,2:7`, verifier time
`364.857 ms`, estimated saved target work `390.119 ms`, and estimated net
`-22.193 ms`.

The default split-head verifier now reads the verified row logits directly into
`s->logits` instead of allocating a temporary vocab buffer and copying it on
return. This is exact-safe and removes one small CPU-side hot-path cost, but the
measured gain is only tiny/noisy: a 3-run q4 benchmark
(`/tmp/ds4-mtp-direct-logits-20260510-063632.{log,csv}`) measured baseline
`34.78 TPS`, disabled `34.28 TPS`, exact `34.33 TPS`, and speed `34.00 TPS`.
Baseline, disabled, and exact shared hash `37043619...`; speed drifted to
`2c10263f...`.

A stricter greedy top-only readback diagnostic was also tried and reverted. It
computed the accepted row's top-1 on GPU and deferred the full logits readback
after full N=2 accepts behind `DS4_MTP_GREEDY_TOP_ONLY=1`. The q4 oracle
passed, but the 3-run filter
(`/tmp/ds4-mtp-greedy-top-only-3run-20260510.csv`) measured baseline
`35.43 TPS`, disabled/no-open `35.74 TPS`, exact `34.61 TPS`
(`exact_vs_baseline=0.977`), and speed `33.49 TPS`. Strict hashes matched, but
the path was slower than the promoted default and was not kept.

The promoted N=2 split-head path now also skips prefix-1 state capture by
default and replays one exact target token only if row 0 rejects draft 1. This
removes a small per-layer copy cost from the common full-accept verifier path;
`DS4_MTP_N2_CAPTURE_PREFIX1=1` restores the older capture behavior for
diagnostics. The q4 oracle passed, and a paired 3-run control showed the change
was a small exact win: default capture measured exact `35.22 TPS`, while
replay-on-partial measured exact `35.40 TPS`, with strict hashes identical.
The 5-run confirmation
(`/tmp/ds4-mtp-n2-replay-prefix1-5run-20260510-073959.{log,csv}`) still did not
reach parity: baseline `35.97 TPS`, disabled `35.51 TPS`, exact `35.35 TPS`,
and speed `34.35 TPS`.

`DS4_MTP_N2_CAPTURE_PREFIX1_MARGIN=N` is an opt-in hybrid between the older
always-capture path and the current replay-on-partial default. It captures the
prefix-1 frontier only when the second MTP draft margin is below `N`, trying to
avoid replay cost on likely partial accepts without paying capture cost on every
attempt. The q4 oracle passed with threshold `8`, but the 3-run sweep did not
beat the current best exact cluster:

| Prefix1 capture margin | Baseline | Disabled | Exact |
| ---: | ---: | ---: | ---: |
| 6 | 34.98 | 34.67 | 34.65 |
| 8 | 34.95 | 34.61 | 34.53 |
| 12 | 35.10 | 34.67 | 34.57 |

Strict hashes matched throughout. The best threshold reached
`exact_vs_baseline=0.991`, so selective prefix capture remains diagnostic-only.

A follow-up attempt to replace recursive MTP full-logit margin reads with tiny
top-2 value reads was exact on the oracle but not useful, so it was reverted.
The 3-run q4 filter (`/tmp/ds4-mtp-top2-margin-20260510-070337.{log,csv}`)
measured baseline `34.87 TPS`, disabled `34.55 TPS`, exact `34.28 TPS`, and
speed `32.37 TPS`. Baseline, disabled, and exact shared hash `37043619...`;
speed drifted to `2c10263f...`. The exact result stayed flat and speed mode
regressed, so the default full-logit margin path remains in place.

`DS4_MTP_EXACT_MARGIN_SKIP_COMMIT1=1` tried committing the already-matched
first draft with a normal exact target decode when the second draft margin was
too low. The q4 oracle passed, but the 3-run filter
(`/tmp/ds4-mtp-margin-skip-commit1-20260510-071400.{log,csv}`) measured
baseline `35.96 TPS`, disabled `35.48 TPS`, exact `35.28 TPS`, and speed
`35.09 TPS`. Baseline, disabled, and exact shared the baseline hash. Exact
remained below baseline, so the experiment was reverted and not promoted.

A small cached target top-2/argmax reuse was also correctness-preserving, but
did not close the gap. The 5-run q4 interleaved run measured baseline
`34.84 TPS`, disabled `34.41 TPS`, exact `34.32 TPS`, and speed `34.10 TPS`.
Baseline, disabled, and exact shared hash `37043619...`; speed drifted to
`2c10263f...`. This shows the remaining gap is not primarily the duplicated
CPU argmax scan after target-margin gating.

An opt-in target-top2-only eval experiment tried to avoid the full target
logits readback before verification. It was not correctness-preserving as
implemented and was not kept: with `DS4_MTP_TARGET_TOP2_ONLY=1`, exact produced
hash `7a685fdf...` and `616` bytes versus the baseline hash `37043619...` and
`635` bytes. Median exact TPS was still only `34.26` over the 3-run probe.

Prefix-2 diagnostic buffers are now allocated lazily, only when
`DS4_MTP_EXACT_PREFIX2=1` is requested. This is correctness-preserving and
keeps the default MTP graph footprint smaller, but it did not recover parity:
a 5-run default benchmark measured baseline `34.83 TPS`, disabled `34.39 TPS`,
exact `34.34 TPS`, and speed `34.02 TPS`; baseline, disabled, and exact were
hash-identical.

The full frontier snapshot buffers are also lazy now. They are only allocated
when a diagnostic or fallback path takes an actual speculative frontier
snapshot, rather than whenever MTP is loaded. This made exact MTP match the
MTP-loaded disabled path on the standard q4 run, but both were still below
plain baseline: baseline `34.90 TPS`, disabled `34.40 TPS`, exact `34.40 TPS`,
and speed `34.07 TPS`. The strict modes were hash-identical.

Strict default `spec_logits` scratch was then reduced from 16 rows to 2 rows,
with wider allocation kept for `--mtp-speed`, `DS4_MTP_EXACT_DEEP`, and
`--mtp-verify-scale`. This was also correctness-preserving, but did not recover
parity: a 5-run q4 benchmark measured baseline `36.09 TPS`, disabled
`35.51 TPS`, exact `35.26 TPS`, and speed `34.51 TPS`. Baseline, disabled, and
exact shared hash `46c58f225f...`; speed drifted to `2220da3552...`.

`DS4_MTP_SPEC_DISABLE=1` first made the disabled control allocate a plain
non-MTP graph, then skip Metal mapping/residency for the MTP GGUF. Those were
correctness/cleanup improvements but did not fully remove the floor: the
5-run q4 gate after map-skip still measured baseline `36.11 TPS`, disabled
`35.72 TPS` (`disabled_vs_baseline=0.989`), exact `35.57 TPS`
(`exact_vs_baseline=0.985`), and speed `34.54 TPS`. Baseline, disabled, and
exact shared hash `46c58f225f...`.

The final no-open cleanup moves the `DS4_MTP_SPEC_DISABLE=1` check before
`model_open()` for the MTP GGUF. That made the disabled lane baseline-clean:
`/tmp/ds4-mtp-current-no-open-5run-20260510.csv` measured baseline
`35.00 TPS`, disabled/no-open `35.01 TPS`
(`disabled_vs_baseline=1.000`), exact `34.64 TPS`
(`exact_vs_baseline=0.990`), and speed `33.50 TPS` over 5 interleaved q4 runs.
Baseline, disabled, and exact were hash-identical. This proves the no-probe
control path is no longer carrying MTP overhead, while exact N=2 itself still
does not meet the active parity gate.

A short `--mtp-draft 1` control isolates the MTP-resident/no-spec floor. With
the MTP model opened and the MTP graph allocated, but no speculative suffix
attempted, `/tmp/ds4-mtp-draft1-nospec-floor-20260510.csv` measured baseline
`36.10 TPS`, disabled/no-open `36.10 TPS`
(`disabled_vs_baseline=1.000`), exact `35.93 TPS`
(`exact_vs_baseline=0.995`), and speed `35.89 TPS`. All modes emitted the same
hash because draft depth 1 disables speculation. This suggests the current
0.990 N=2 exact result is mostly real speculative verifier overhead rather than
just the cost of loading the MTP model.

`tools/mtp_benchmark.sh --include-resident` now makes that floor explicit in the
standard interleaved harness. The resident lane opens/maps the MTP GGUF with
`--mtp-draft 1`, while the disabled lane still uses `DS4_MTP_SPEC_DISABLE=1`
and skips MTP open entirely. The q4 resident-floor run
(`/tmp/ds4-mtp-resident-floor-20260510-124749.{csv,log}`) measured baseline
`35.04 TPS`, disabled/no-open `34.99 TPS` (`0.999x`), resident no-spec
`34.96 TPS` (`0.998x`), exact `34.52 TPS` (`0.985x`), and speed `33.49 TPS`.
Baseline, disabled/no-open, resident, and exact were hash-identical. This
reconfirms that resident MTP mapping alone is within noise of baseline; the
remaining exact gap is in the draft/speculate/verify path.

`tools/mtp_benchmark.sh --include-session` adds a stronger control for the
session/speculation dispatch floor. The session lane keeps MTP loaded with the
normal draft depth, but sets `DS4_MTP_NO_SPECULATE=1`, so CLI/server still take
the MTP-capable session branch while every token is evaluated by the no-probe
normal path. The q4 session-floor run
(`/tmp/ds4-mtp-session-floor-20260510-125316.{csv,log,err}`) measured baseline
`34.99 TPS`, disabled/no-open `34.97 TPS` (`0.999x`), resident no-spec
`35.02 TPS` (`1.001x`), session no-spec `34.64 TPS` (`0.990x`), exact
`34.64 TPS` (`0.990x`), and speed `33.49 TPS` (`0.957x`). Baseline,
disabled/no-open, resident, session, and exact were hash-identical. This shows
the remaining exact N=2 gap can appear even when speculative verification is
explicitly disabled at the session branch, so the next optimization target is
the MTP-capable generation/session control path rather than MTP residency.

That control-path hypothesis was tested by adding an early target-only return
inside `ds4_session_eval_internal()` whenever no MTP probe can run, including
adaptive-skip turns. The q4 oracle still passed, but the 5-run gate
(`/tmp/ds4-mtp-target-only-fastpath-20260510-130150.csv`) did not improve:
baseline `34.98 TPS`, disabled/no-open `34.98 TPS` (`1.000x`), resident
no-spec `34.83 TPS` (`0.996x`), session no-spec `34.47 TPS` (`0.985x`), exact
`34.54 TPS` (`0.987x`), and speed `33.50 TPS` (`0.958x`). Baseline,
disabled/no-open, resident, session, and exact were hash-identical. A direct
profile (`/tmp/ds4-mtp-target-only-fastpath-direct-profile-20260510-130603.err`)
showed one near-break-even exact run, but not a promotable median win:
generation `34.78 TPS`, `target_eval=108`, `probe=17`, `full=10`,
`adaptive_skip=76`, verifier time `516.417 ms`, and estimated net
`+2.607 ms`.

The follow-up N=2 bucket profile
(`/tmp/ds4-mtp-target-only-fastpath-n2-profile-20260510-130624.err`) confirms
the remaining verifier cost is overwhelmingly in the layer pass, not upload,
head, or readback. Committed-2 split-head verifier samples were roughly:
upload `0.010-0.022 ms`, layers `47.315-60.829 ms`, row0 head
`1.144-1.265 ms`, row1 head `1.060-1.151 ms`, logits readback
`0.015-0.062 ms`, total `49.578-63.143 ms`. The same run generated at
`34.51 TPS` and estimated net `-17.608 ms`. This makes further CPU-side
readback trimming unlikely to matter; exact parity needs either fewer verifier
layer passes, a materially faster exact layer verifier, or a policy that only
enters N=2 when the expected full accept is high enough to pay for that layer
pass.

Hoisting the invariant CLI/server MTP speculation gate out of the token loop was
also exact-safe but not enough to pass the gate. The q4 oracle passed and
`/tmp/ds4-mtp-loop-hoist-20260510-131015.csv` measured baseline `34.95 TPS`,
disabled/no-open `35.01 TPS` (`1.002x`), resident no-spec `34.89 TPS`
(`0.998x`), session no-spec `34.56 TPS` (`0.989x`), exact `34.63 TPS`
(`0.991x`), and speed `33.48 TPS` (`0.958x`). Baseline, disabled/no-open,
resident, session, and exact were hash-identical. This is the best current
control-path result, but the remaining gap is still larger than the hoisted
loop overhead.

`DS4_MTP_ADAPTIVE_STOP_DIRECT=1` is an opt-in diagnostic that bypasses
`ds4_session_eval_speculative_argmax()` in CLI/server for the rest of a
generation after adaptive marks an MTP attempt unprofitable. This tests the
strongest exact fallback short of pretending `--mtp` was not loaded. It is
hash-identical, but still not promotable: the q4 oracle passed and
`/tmp/ds4-mtp-adaptive-stop-direct-20260510-131705.csv` measured baseline
`36.04 TPS`, disabled/no-open `36.06 TPS` (`1.001x`), resident no-spec
`35.90 TPS` (`0.996x`), session no-spec `35.60 TPS` (`0.988x`), exact
`35.61 TPS` (`0.988x`), and speed `34.49 TPS` (`0.957x`). A direct profile
(`/tmp/ds4-mtp-adaptive-stop-direct-profile-20260510-132026.err`) showed it
made exactly one MTP step, margin-skipped it, and then used normal target eval
for the rest of the generation: `target_eval=128`, `probe=1`, `steps=1`,
`drafted=2`, `committed=0`, generation `35.65 TPS`. This closes the
fallback-only parity route: even near-immediate opt-out remains below the plain
baseline/resident floor.

After guarding that diagnostic so it has no default hot-loop function call, the
current default was remeasured on the same faster Studio band. The q4 oracle
passed and `/tmp/ds4-mtp-current-after-stopguard-20260510-132241.csv` measured
baseline `36.03 TPS`, disabled/no-open `36.02 TPS` (`1.000x`), resident no-spec
`35.95 TPS` (`0.998x`), session no-spec `35.50 TPS` (`0.985x`), exact
`35.54 TPS` (`0.986x`), and speed `34.50 TPS` (`0.958x`). Baseline,
disabled/no-open, resident, session, and exact were hash-identical. This
confirms the remaining exact/session floor is stable across both the `35 TPS`
and `36 TPS` host bands.

An exact first-token top-k rescue experiment is available with
`DS4_MTP_EXACT_FIRST_TOPK=N`. It lets strict MTP replace an MTP top-1 miss with
the already-known exact target token when that token appears in the MTP probe's
top-N alternatives, then continues normal exact suffix verification. On q4 with
`N=4`, it was exercised but did not improve the median enough to promote:
baseline `34.87 TPS`, disabled `34.54 TPS`, exact `34.28 TPS`. A stats run saw
`first_rescue=3`, `drafted=27`, `committed=12`, and `est_net=-29.333ms`.
Larger single-run probes were also net-negative: `N=8` had `first_rescue=3`
and `est_net=-30.894ms`; `N=16` had `first_rescue=4` and
`est_net=-36.932ms`.

`DS4_MTP_FIRST_MARGIN_SKIP=N` is an opt-in exact-safe diagnostic that reads the
first MTP probe margin and skips suffix verification when that first proposal is
low-confidence. It did not produce a promotable policy on the q4 3-run filter.
The `N=0` readback-only control measured baseline/exact `35.98/35.43 TPS`;
actual thresholds were worse or flat: `N=1` `35.93/35.19`, `N=2`
`35.88/35.22`, `N=4` `35.85/35.23`, and `N=8` `35.84/35.25`. Strict hashes
matched baseline throughout, but exact remained below baseline and did not beat
the disabled lane convincingly.

Additional diagnostics:

- `DS4_MTP_STATS=1` now prints draft/commit histograms so deep exact runs can
  show where attempts land and how much verifier/replay time each commit bucket
  consumes.
- `DS4_MTP_EXACT_PREFIX2=1` enables an opt-in exact prefix-2 commit path for
  deep verification. It was hash-identical on the forced deep-4 diagnostic and
  raised that intentionally ungated run from `15.71 TPS` to `17.66 TPS` by
  eliminating `commit=2` replay, but verifier time still dominated.
- `DS4_MTP_TARGET_MARGIN_AUDIT=1` records whether target-logit confidence
  predicts MTP first-token hits. On the N=2 q4 prompt, target margin `>=5`
  produced `12/12` first-token hits, while lower buckets were weaker.
- The strict/exact path now uses a target-margin pre-probe gate by default
  with threshold `2.0`. It is exact/hash-identical because it only skips MTP
  probes when the target logits are low-confidence; use
  `DS4_MTP_TARGET_MARGIN_SKIP=N` to retune it or
  `DS4_MTP_NO_TARGET_MARGIN_SKIP=1` to disable it. The initial opt-in 5-run
  benchmark still landed below baseline: baseline `34.90 TPS`, disabled
  `34.47 TPS`, exact `34.27 TPS`.

The best N=2 rerun with the same target-margin gate was closer but still below
baseline. The default split-head verifier remained better than forcing the
generic full-head verifier:

| N=2 verifier | Baseline | Disabled | Exact |
| --- | ---: | ---: | ---: |
| split-head + target skip 2 | 34.89 | 34.52 | 34.43 |
| full-head + target skip 2 | 34.80 | 34.26 | 34.29 |

Changing adaptive cooldown did not recover the gap. With target skip 2, 3-run
medians for adaptive skip 10/16/24/32 all left exact in the `34.20-34.32 TPS`
range while baseline stayed around `34.8-34.9 TPS`.

The promoted target-first path was also rechecked with a bounded adaptive
cooldown sweep after the later N=2 changes. Shorter cooldowns made speculation
too eager, and longer cooldowns did not recover the baseline gap:

| Adaptive skip | Baseline | Disabled | Exact |
| ---: | ---: | ---: | ---: |
| 4 | 35.03 | 34.72 | 34.28 |
| 8 | 35.06 | 34.73 | 34.61 |
| 16 | 35.01 | 34.64 | 34.41 |

Strict hashes matched throughout. The current default skip `10` remains in the
best measured band, but the cooldown lever is not a parity fix.

`DS4_MTP_ADAPTIVE_CHEAP=1` is an opt-in diagnostic that avoids the default
target/probe/verifier timing collection used by adaptive decisions. It keeps
the same exact output contract, but the q4 filter did not recover parity.
The q4 oracle passed, then `/tmp/ds4-mtp-adaptive-cheap-20260510.csv` measured
baseline `34.99 TPS`, disabled `34.76 TPS`, exact `34.46 TPS`
(`exact_vs_baseline=0.985`), and speed `33.45 TPS`; strict hashes matched.

The stronger `DS4_MTP_ADAPTIVE_CHEAP_FULL_SKIP=1` variant only keeps attempting
speculation after full accepts. It looked slightly better in the short filter:
`/tmp/ds4-mtp-adaptive-cheap-fullskip-20260510.csv` measured baseline
`35.00 TPS`, disabled `34.59 TPS`, exact `34.67 TPS`
(`exact_vs_baseline=0.991`), and speed `33.42 TPS`. The 5-run confirmation
did not hold, however:
`/tmp/ds4-mtp-adaptive-cheap-fullskip-5run-20260510.csv` measured baseline
`35.04 TPS`, disabled `34.62 TPS`, exact `34.63 TPS`
(`exact_vs_baseline=0.988`), and speed `33.43 TPS`. This keeps cheap adaptive
diagnostic-only: it reduces policy overhead, but not enough to beat the plain
baseline.

An adaptive loss-stop diagnostic was also tried and not kept. It stopped future
MTP probes for the rest of the generation after the first measured
unprofitable strict N=2 attempt. The q4 oracle passed, but the 3-run filter
(`/tmp/ds4-mtp-adaptive-stop1-20260510.csv`) measured baseline `36.11 TPS`,
disabled/no-open `36.09 TPS` (`disabled_vs_baseline=0.999`), exact
`35.52 TPS` (`exact_vs_baseline=0.984`), and speed `34.51 TPS`. This was worse
than the current default, so the code path was reverted.

The adaptive skip parser was then relaxed so explicit diagnostics can test
larger cooldowns than the old hard-coded `32` ceiling. This did not produce a
parity path. With `DS4_MTP_ADAPTIVE_SKIP=96`, the q4 oracle passed and the clean
5-run gate (`/tmp/ds4-mtp-adaptive-skip96-clean-20260510-123816.{log,csv}`)
measured baseline `35.00 TPS`, disabled/no-open `34.94 TPS`
(`disabled_vs_baseline=0.998`), exact `34.21 TPS`
(`exact_vs_baseline=0.977`), and speed `33.52 TPS`; strict hashes matched. A
single profiled exact run
(`/tmp/ds4-mtp-adaptive-skip96-single-20260510-123750.err`) attempted MTP only
twice, margin-skipped both attempts, and reported `adaptive_skip=126` with
`commit_hist=0:2`. This falsifies "stand down harder" as a q4 parity fix: even
near-no-spec exact `--mtp` still sat below baseline in that gate.

Changing the MTP margin did not recover the gap either. A 3-run q4 sweep kept
baseline/disabled/exact hash-identical, but exact stayed below baseline:

| MTP margin | Baseline | Disabled | Exact |
| ---: | ---: | ---: | ---: |
| 0 | 34.91 | 34.51 | 33.31 |
| 2 | 34.90 | 34.42 | 34.30 |
| 6 | 34.80 | 34.28 | 34.04 |

Lowering the margin to `0` tries too many drafts and is clearly worse. Margins
`2`, `4`, and `6` stay in the same below-baseline band, with default `4`
remaining the best measured setting.

Retuning the promoted target-margin threshold above `2.0` also did not recover
the gap. A short 3-run sweep measured:

| Target margin skip | Baseline | Disabled | Exact |
| ---: | ---: | ---: | ---: |
| 3 | 35.94 | 35.84 | 35.46 |
| 5 | 35.92 | 35.34 | 35.17 |

Baseline, disabled, and exact hashes matched in both cases. Threshold `2.0`
remains the best measured strict default, but still fails parity.

After target-first became the promoted default, a higher target-margin gate was
checked again to see if only the most confident N=2 attempts could beat the
draft-1/no-spec floor. `DS4_MTP_TARGET_MARGIN_SKIP=10` was worse:
`/tmp/ds4-mtp-target-margin10-20260510.csv` measured baseline `36.12 TPS`,
disabled/no-open `36.09 TPS` (`disabled_vs_baseline=0.999`), exact
`35.45 TPS` (`exact_vs_baseline=0.981`), and speed `34.53 TPS`. Strict hashes
matched, but the policy is not promotable.

The smaller current-code retune `DS4_MTP_TARGET_MARGIN_SKIP=6` also missed:
`/tmp/ds4-mtp-target-margin6-current-20260510.csv` measured baseline
`34.91 TPS`, disabled/no-open `35.04 TPS`, exact `34.53 TPS`
(`exact_vs_baseline=0.989`), and speed `33.47 TPS`. Strict hashes matched.

`DS4_MTP_EXACT_FORCE_FIRST=1` is also correctness-preserving, but worse. It
removes first-token misses by using the already-known target token as the first
draft, but creates too many verifier attempts: a 5-run benchmark with target
skip 2 measured baseline `34.89 TPS`, disabled `34.41 TPS`, exact
`34.18 TPS`.

Accepted split-head N=2 profile samples show the remaining verifier cost is
almost entirely the target layer pass:

| Bucket | Typical time |
| --- | ---: |
| layer pass | 49-54 ms |
| row0 output head + top1 | 1.2-1.4 ms |
| row1 output head | 1.1 ms |
| logits readback | ~0.02 ms after warmup |
| total accepted verifier | 52-56 ms |

This rules out another small output-head/readback tweak as the likely path to
baseline parity. The gap is in the exact layer pass plus the MTP probe cost.

`DS4_MTP_FUSED_PROBE=1` is an opt-in diagnostic that encodes the target decode
and first MTP probe in one command sequence. Because it cannot honor
CPU-side target-margin gates, it is intentionally not promoted into the strict
default path. With adaptive cooldown disabled, it was correct but too small:

| N=2 no-adaptive path | Baseline | Disabled | Exact | Speed |
| --- | ---: | ---: | ---: | ---: |
| separate probe | 34.89 | 34.39 | 32.70 | 34.06 |
| fused probe | 34.87 | 34.29 | 32.93 | 34.25 |

Baseline, disabled, and exact hashes matched in both runs. The fused probe
recovers about `0.23 TPS` in exact mode for this policy, which confirms that
standalone MTP probe submission overhead is not enough by itself to rescue the
linear N=2 path.

Allowing the fused probe under the current default exact gates was also tested
and reverted. The path remained hash-identical, but a 3-run q4 filter with
`DS4_MTP_FUSED_PROBE=1` measured baseline `34.89 TPS`, disabled `34.41 TPS`,
exact `34.12 TPS`, and speed `34.23 TPS`
(`/tmp/ds4-mtp-fused-default-gates-20260510-070654.{log,csv}`). Exact was
below the current default, so the stricter fused-probe guard remains in place.

An exact-layer kernel experiment tried reusing the release-path routed-MoE
pair+SwiGLU fusion for tiny verifier batches. The multi-token row addressing
fix is present, but the path is gated behind
`DS4_METAL_ENABLE_ROUTED_BATCH_PAIR_SWIGLU=1` because it was not
correctness-safe:

| Routed-MoE tiny-batch path | Baseline | Disabled | Exact | Exact hash |
| --- | ---: | ---: | ---: | --- |
| fallback pair + separate SwiGLU | 34.59 | 34.21 | 34.05 | baseline |
| opt-in pair+SwiGLU fusion | 34.81 | 34.46 | 33.99 | drifted, two hashes |

This falsifies that particular layer-pass shortcut for now. The safe/default
strict verifier stays on the fallback path. A follow-up opt-in run with a
device-memory barrier inside the Q4 pair+SwiGLU kernel still drifted:
baseline `35.79`, disabled `35.11`, exact `34.81`, with exact producing two
non-baseline hashes.

A synchronized stage profile of the exact N=2 verifier shows why the remaining
gap is hard to erase with one small kernel edit. The profile uses
`DS4_METAL_LAYER_STAGE_PROFILE=1 DS4_METAL_MOE_STAGE_PROFILE=1`, so absolute
times are inflated by per-stage synchronization, but the bucket proportions are
useful:

| N=2 verifier bucket | Profiled total | Per-layer average |
| --- | ---: | ---: |
| `ffn.routed_moe` | 39.535 ms | 0.919 ms |
| `attn.attention` | 16.105 ms | 0.375 ms |
| `attn.output_proj` | 15.004 ms | 0.349 ms |
| `attn.q_path` | 14.175 ms | 0.330 ms |
| `attn.compressor` | 12.851 ms | 0.313 ms |
| `ffn.router` | 10.519 ms | 0.245 ms |
| `ffn.shared_gate_up` | 10.266 ms | 0.239 ms |
| `ffn.hc_pre` | 10.207 ms | 0.237 ms |
| `attn.kv_path` | 10.059 ms | 0.234 ms |
| `ffn.shared_down` | 9.756 ms | 0.227 ms |

Within `ffn.routed_moe`, the N=2 profile split was gate/up `15.101 ms`, down
`12.871 ms`, activation/weight `8.644 ms`, and expert sum `0.990 ms`. The
failed pair+SwiGLU shortcut targeted the activation/weight bucket, which is
meaningful but not large enough by itself unless it can be made exact and
non-regressive.

Forcing the existing grouped expert-major MoE path for tiny verifier batches
was also tested behind `DS4_METAL_ROUTED_BATCH_FORCE_MM_ID=1`, with
`DS4_METAL_MOE_MID_F32=1` to avoid changing the intermediate precision at the
same time. The explicit q4 oracle passed, but the broader benchmark drifted on
the first runs: `/tmp/ds4-mtp-force-routed-mm-id-20260510.csv` emitted exact
bytes `692` versus baseline `693`, with exact TPS around `32`. The run was
stopped early because the candidate was already disqualified. This makes the
current tiny pair-MV routed-MoE path the only hash-stable routed kernel schedule
we have for strict verification.

Reusing the decode shared-expert gate/up/SwiGLU kernel for verifier rows was
tested behind `DS4_MTP_VERIFY_FUSED_SHARED_GATE_UP=1` after fixing the opt-in
predicate so the fused path actually ran. The q4 oracle passed, and the 3-run
filter was the first standalone exact-kernel probe with a small positive signal:
`/tmp/ds4-mtp-fused-shared-gate-up-fixed-20260510.csv` measured baseline
`34.96 TPS`, disabled `34.63 TPS`, exact `34.72 TPS`, and speed `33.47 TPS`.
Strict hashes matched and `exact_vs_baseline=0.993`, so this is still below
parity but worth testing in combination with other exact-safe shared-expert
changes.

The 5-run confirmation with the same opt-in remained below parity:
`/tmp/ds4-mtp-fused-shared-gate-up-fixed-5run-20260510.csv` measured baseline
`34.91 TPS`, disabled `34.59 TPS`, exact `34.67 TPS`, and speed `33.42 TPS`,
with `exact_vs_baseline=0.993` and strict hashes matching. A short-lived attempt
to promote it into the no-env default then failed its own 5-run gate:
`/tmp/ds4-mtp-promoted-shared-gate-up-5run-20260510.csv` measured baseline
`35.09 TPS`, disabled `34.85 TPS`, exact `34.65 TPS`, and speed `33.61 TPS`,
with `exact_vs_baseline=0.987`. Because the promoted run did not beat the
current default, shared gate/up fusion remains opt-in only.

Reusing the decode shared-down + HC-expand fusion for verifier rows was tested
behind `DS4_MTP_VERIFY_FUSED_SHARED_DOWN_HC=1`. The q4 oracle passed, and the
3-run filter remained hash-identical, but it did not beat the current default:
`/tmp/ds4-mtp-fused-shared-down-hc-20260510.csv` measured baseline
`34.95 TPS`, disabled `34.57 TPS`, exact `34.63 TPS`, and speed `33.46 TPS`.
The script reported `exact_vs_baseline=0.991`, so this path is also not
promotable as a standalone kernel change.

Combining the two exact-safe shared-expert fusions did not compound the small
gate/up signal. With `DS4_MTP_VERIFY_FUSED_SHARED_GATE_UP=1
DS4_MTP_VERIFY_FUSED_SHARED_DOWN_HC=1`,
`/tmp/ds4-mtp-fused-shared-combo-20260510.csv` measured baseline
`34.92 TPS`, disabled `34.64 TPS`, exact `34.64 TPS`, and speed `33.42 TPS`,
with `exact_vs_baseline=0.992` and strict hashes matching. The combined shared
path remains below parity and should not be promoted as-is.

Combining shared gate/up fusion with the best cheap-adaptive scheduling variant
was also exact but not enough. With
`DS4_MTP_VERIFY_FUSED_SHARED_GATE_UP=1 DS4_MTP_ADAPTIVE_CHEAP=1
DS4_MTP_ADAPTIVE_CHEAP_FULL_SKIP=1`, the q4 oracle passed and
`/tmp/ds4-mtp-fused-shared-gate-up-adaptive-fullskip-20260510.csv` measured
baseline `34.99 TPS`, disabled `34.71 TPS`, exact `34.68 TPS`, and speed
`33.46 TPS`. The script reported `exact_vs_baseline=0.991`, with strict hashes
matching. This closes the small-signal combination as diagnostic-only.

Combining all exact-safe fused shared/HC diagnostics was also worse than the
default. With `DS4_MTP_VERIFY_FUSED_HC_NORM=1
DS4_MTP_VERIFY_FUSED_SHARED_GATE_UP=1
DS4_MTP_VERIFY_FUSED_SHARED_DOWN_HC=1`, the q4 oracle passed and
`/tmp/ds4-mtp-fused-hc-shared-combo-20260510-1225.csv` measured baseline
`36.14 TPS`, disabled/no-open `36.09 TPS`, exact `35.58 TPS`
(`exact_vs_baseline=0.985`), and speed `34.68 TPS`. Strict hashes matched, but
the fused combination regressed relative to the current exact cluster, so it is
not promotable.

Rechecking shared gate/up fusion on the current promoted target-first path also
failed to recover parity. `DS4_MTP_VERIFY_FUSED_SHARED_GATE_UP=1` passed the q4
oracle, but `/tmp/ds4-mtp-current-fused-shared-gate-up-20260510.csv` measured
baseline `35.07 TPS`, disabled `34.67 TPS`, exact `34.64 TPS`, and speed
`33.53 TPS` over a 3-run filter. Strict hashes matched, but exact again tracked
the disabled lane rather than beating baseline, so this remains opt-in only.

The fused shared gate/up diagnostic was then upgraded from a host row loop to a
true Metal row-batched call using the existing kernel's token row dimension.
This was the final linear N=2 verifier experiment before pivoting to tree
oracle work. It remained correctness-clean but did not recover parity:
`/tmp/ds4-mtp-fused-shared-gate-up-rows-20260510-133011.csv` measured baseline
`35.09 TPS`, disabled/no-open `34.92 TPS` (`0.995x`), resident no-spec
`35.05 TPS` (`0.999x`), session no-spec `34.65 TPS` (`0.987x`), exact
`34.71 TPS` (`0.989x`), and speed `33.50 TPS` (`0.955x`). Baseline,
disabled/no-open, resident, session, and exact were hash-identical. This closes
the current linear N=2 kernel/policy round as correct but below baseline.

Single-stage fast verifier probes did not find a promotable shortcut. A
3-run/96-token control measured baseline `35.19 TPS`, disabled `34.34 TPS`,
and exact `34.29 TPS`, with strict hashes matching. The stage probes measured:

| Opt-in stage | Correctness | Exact TPS | Baseline TPS | Notes |
| --- | --- | ---: | ---: | --- |
| `DS4_MTP_VERIFY_FAST_F16=1` | drifted | n/a | n/a | exact bytes `526` vs baseline `532` |
| `DS4_MTP_VERIFY_FAST_Q8=1` | matched | 34.16 | 35.16 | safe but slower than control |
| `DS4_MTP_VERIFY_FAST_ATTN_OUT=1` | matched | 34.04 | 35.12 | safe but slower than control |
| `DS4_MTP_VERIFY_FAST_ROUTER=1` | matched | 34.33 | 35.13 | small gain vs control only |
| `DS4_MTP_VERIFY_FAST_FRONTIER=1` | matched at 96 tokens | 34.52 | 35.32 | failed 128-token gate |

The promising frontier switch drifted in the 128-token gate, so it is not
promotable. A rerun on the quiet Studio host reproduced the failure with the
standard q4 prompt: baseline `34.83 TPS`, disabled `34.41 TPS`, exact
`34.01 TPS`, and speed `33.98 TPS`; exact emitted `640` bytes with hash
`a776d5a2...` versus the baseline `635` bytes with hash `37043619...`.
Disabling the split-head verifier did not repair it: the generic exact verifier
with `DS4_MTP_NO_EXACT_N2_SPLIT_HEAD=1` produced the same exact hash
`a776d5a2...`.

Forcing exact replay after the fast verifier did restore the baseline hash, but
it was far too slow: with `DS4_MTP_EXACT_REPLAY=1
DS4_MTP_NO_EXACT_N2_SPLIT_HEAD=1`, exact measured `32.30 TPS` versus baseline
`34.92 TPS`. This isolates the issue to committed verifier state, not just
accept/reject decisions: the fast batched attention/frontier path can agree on
local top-1s while leaving cache/frontier state that changes later greedy text.
The hash-safe Q8, attention-output, and router switches are not large enough to
recover baseline parity on their own.

Combining the individually hash-safe stages did not produce a promotable path
either. `DS4_MTP_VERIFY_FAST_Q8=1 DS4_MTP_VERIFY_FAST_ROUTER=1
DS4_MTP_VERIFY_FAST_ATTN_OUT=1` drifted immediately at 128 tokens, with exact
bytes `664` versus baseline `694`. One-pass 128-token filters for smaller pairs
were hash-identical, but still below baseline: Q8+router measured exact
`34.25 TPS` versus baseline `34.84 TPS`, Q8+attention-output measured exact
`33.96 TPS` versus baseline `34.92 TPS`, and router+attention-output measured
exact `34.26 TPS` versus baseline `35.05 TPS`.

The older decode-shaped exact N=2 verifier remains available behind
`DS4_MTP_DECODE2_EXACT=1`, but it is also not promotable. A 3-run/128-token q4
control measured baseline `34.87 TPS`, disabled `34.62 TPS`, exact
`34.07 TPS`, and speed `33.43 TPS`. Baseline, disabled, and exact shared hash
`46c58f225f...`, so the path is correct but slower than the promoted split-head
batch verifier.

## Verifier Cost

`--mtp-verify-scale` with exact rows shows that deeper target verification has a
better micro-ceiling than N=2, but only if the drafted path is accepted:

| Draft depth | Sequential target cost | Exact verifier cost | Micro ceiling |
| ---: | ---: | ---: | ---: |
| 2 | 59.275 ms | 54.149 ms | 1.09x |
| 3 | 87.365 ms | 68.044 ms | 1.28x |
| 4 | 115.468 ms | 83.249 ms | 1.39x |
| 8 | 229.958 ms | 145.800 ms | 1.58x |

In real generation, greedy single-path MTP draft quality is not high enough for
deep verification to pay. Deep-4 without strong gating spends most of its time
on verifier work and partial-accept replay.

A fresh current-code q4 verifier-scale profile after the N=2 target-first and
prefix-capture experiments gave the same ceiling shape:
`/tmp/ds4-mtp-verify-scale-current-20260510.err` measured target average
`29.016 ms/token` and these repeated verifier costs:

| Draft depth | Sequential target cost | Verify + final row | Micro ceiling |
| ---: | ---: | ---: | ---: |
| 1 | 28.599 ms | 34.184 ms | 0.837x |
| 2 | 56.866 ms | 51.041 ms | 1.114x |
| 3 | 84.849 ms | 64.800 ms | 1.309x |
| 4 | 116.066 ms | 83.464 ms | 1.391x |

This reinforces the main constraint: single-path N=2 exact verification cannot
be the `1.25x` route by itself, and even ideal linear N=4 is short of `1.5x`
before MTP draft overhead. Longer accepted prefixes from wider tree
alternatives are the credible remaining path.

A synchronized current-code stage profile
(`/tmp/ds4-n2-stage-profile-20260510.err`) broke the exact N=2 verifier layer
pass down by bucket. The absolute times are inflated because the profiler ends
and synchronizes command buffers at every stage, but the ordering is useful:

| Verifier bucket | Total synchronized time |
| --- | ---: |
| FFN routed MoE | `81.782 ms` |
| Attention | `69.304 ms` |
| Attention output projection | `63.374 ms` |
| Q path | `59.691 ms` |
| Shared gate/up | `45.243 ms` |
| Compressor | `43.544 ms` |
| Router | `43.195 ms` |

The same run recorded four full N=2 accepts and two margin skips; a single
accepted split-head verifier reported `layers=197.638 ms` under profiling,
versus about `51 ms` without the stage synchronizations. This profile explains
why tiny output-head/readback work is no longer the target, and it also explains
why the existing shared gate/up fusion was worth rechecking but not enough.

A forced deep-4 diagnostic with no adaptive cooldown and no margin gate made
the replay cost visible:

| Path | Generation TPS | Commit histogram | Replay cost |
| --- | ---: | --- | --- |
| deep-4 exact | 15.71 | `0:22,1:22,2:34,3:17,4:4` | `commit2=1916ms`, `commit3=1491ms` |
| deep-4 + prefix-2 | 17.66 | same | `commit2=0ms`, `commit3=1492ms` |

Prefix-2 is therefore a real exact improvement, but not close to enough: the
same 3-run interleaved deep-4 policy with adaptive skip 16 still measured
baseline `34.88 TPS`, disabled `34.58 TPS`, and exact `34.20 TPS`.

Shallow depth-3 verification with prefix-2 is also not enough. With
`DS4_MTP_EXACT_DEEP=1 DS4_MTP_EXACT_PREFIX2=1 DS4_MTP_TARGET_MARGIN_SKIP=2`
and `--mtp-draft 3 --mtp-margin 4`, a 3-run interleaved q4 benchmark measured
baseline `35.53 TPS`, disabled `35.07 TPS`, exact `34.60 TPS`, and speed
`25.87 TPS`. The strict modes were hash-identical, but exact remained below
both baseline and the MTP-loaded disabled path.

The existing generic deeper batch verifier was also checked directly with
`DS4_MTP_EXACT_DEEP=1 DS4_MTP_BATCH_VERIFY=1 DS4_MTP_EXACT_PREFIX2=1` and
`--mtp-draft 4 --mtp-margin 4`. It was hash-safe but not useful as a speed
path: `/tmp/ds4-mtp-deep-batch-verify-20260510-073044.{log,csv}` measured
baseline `36.02 TPS`, disabled `35.60 TPS`, exact `34.87 TPS`, and speed
`22.16 TPS`. Baseline, disabled, and exact shared hash `cc6bd264...`; speed
drifted to `d206db1f...`. So the generic deeper batch path does not change the
conclusion: deeper verification needs either better accepted-path selection or
a true tree verifier, not just the existing linear deeper batch switch.

Target-first does not rescue the existing linear deep verifier either. With
`DS4_MTP_EXACT_TARGET_FIRST=1 DS4_MTP_TARGET_MARGIN_SKIP=5
DS4_MTP_EXACT_DEEP=1 DS4_MTP_EXACT_PREFIX2=1` and `--mtp-draft 3 --mtp-margin
4`, `/tmp/ds4-mtp-exact-target-first-deep3-20260510-0831.csv` measured baseline
`36.06 TPS`, disabled `35.65 TPS`, exact `34.79 TPS`, and speed `22.07 TPS`.
Strict hashes matched, but the exact lane was well below the best N=2
target-first result. The current linear deep path should remain a diagnostic,
not the next optimization track.

A dedicated row0 top-1 argmax Metal kernel was also tested as a replacement for
the generic `ds4_metal_indexer_topk_tensor(..., top_k=1)` call in the N=2
split-head verifier. It passed the q4 oracle, but the paired benchmark showed no
benefit, so the candidate was not kept: with the argmax path
`/tmp/ds4-mtp-n2-argmax-top1-20260510-0748.csv` measured baseline
`35.03 TPS`, disabled `34.41 TPS`, exact `34.31 TPS`, and speed `33.47 TPS`;
the immediate no-argmax control
`/tmp/ds4-mtp-n2-argmax-control-20260510-0752.csv` measured baseline
`34.95 TPS`, disabled `34.69 TPS`, exact `34.33 TPS`, and speed `33.49 TPS`.
The strict modes stayed hash-identical in both runs. This suggests the row0
top-k helper is not the bottleneck worth hand-tuning in isolation.

`DS4_MTP_N2_EAGER_ROW1_HEAD=1` is an opt-in diagnostic that computes row 1's
output head in the same command sequence as row 0's top-1 check. The idea was
to trade some wasted row1-head work on row0 misses for fewer command boundaries
on full accepts. It passed the q4 oracle, but the 3-run filter
(`/tmp/ds4-mtp-n2-eager-row1-20260510.{log,csv}`) stayed flat: baseline
`36.10 TPS`, disabled `35.75 TPS`, exact `35.55 TPS`, and speed `34.52 TPS`.
Strict hashes matched, but exact was `0.985x` baseline, so this is not
promotable.

A later margin-gated eager row1 check used the current target-first default and
only enabled eager row1 when the second MTP draft margin was at least `8`.
`/tmp/ds4-mtp-eager-row1-margin8-20260510.csv` measured baseline `35.06 TPS`,
disabled/no-open `36.10 TPS`, exact `34.81 TPS`, and speed `33.71 TPS`. Strict
hashes matched, but the baseline lane was noisy and exact remained well below
the disabled/no-open control, so this is also not promotable.

`DS4_MTP_EXACT_SECOND_TOPK=N` is a small exact tree-shaped diagnostic. It asks
the second MTP proposal for top-k alternatives; if row0 target verification
says the exact second token was in that set, it commits that second token using
captured prefix1 state plus one exact target decode. This proves the wider-MTP
alternative idea can be kept hash-safe without a full branch-local verifier,
but the sequential rescue is still not fast enough. With `N=2`, the q4 oracle
passed and the 3-run filter measured baseline `36.05 TPS`, disabled
`35.71 TPS`, exact `35.67 TPS`, and speed `34.53 TPS`. With `N=4`, the filter
measured baseline `34.95 TPS`, disabled `34.61 TPS`, exact `34.61 TPS`, and
speed `33.56 TPS`. Strict hashes matched in both runs, but exact stayed below
baseline and at/below the disabled lane. This reinforces that the useful tree
path needs branch-local batched target verification rather than sequential
exact rescue after a wrong branch.

`DS4_MTP_EXACT_SECOND_TOPK_REPLAY=N` tested the same rescue without always-on
prefix1 capture. On a rescued branch it replays the two accepted target tokens
exactly instead of committing captured prefix1 state. This also passed the q4
oracle, but it was slower than the disabled lane:
`/tmp/ds4-mtp-exact-second-topk2-replay-20260510.{log,csv}` measured baseline
`35.05 TPS`, disabled `34.84 TPS`, exact `34.63 TPS`, and speed `33.45 TPS`,
with strict hashes matching. The sequential rescue variants are therefore
falsified as speed paths; the only remaining tree route with enough upside is
branch-local batched target verification.

Combining the exact second-token top-2 rescue with the best cheap-adaptive
full-skip scheduler was also exact but not useful. With
`DS4_MTP_EXACT_SECOND_TOPK=2 DS4_MTP_ADAPTIVE_CHEAP=1
DS4_MTP_ADAPTIVE_CHEAP_FULL_SKIP=1`, the q4 oracle passed and
`/tmp/ds4-mtp-secondtopk2-cheapfullskip-20260510-1221.csv` measured baseline
`36.12 TPS`, disabled/no-open `36.09 TPS`, exact `35.68 TPS`
(`exact_vs_baseline=0.988`), and speed `34.53 TPS`. Strict hashes matched, so
this closes the policy-combination variant as diagnostic-only.

`DS4_MTP_EXACT_SECOND_TOPK_BRANCH=N` adds the first branch-local scaffold for
that route. It snapshots the pre-verifier frontier, runs the normal N=2
verifier, and when the exact row0 token appears in the MTP second-token top-k,
restores the frontier and verifies the sibling `[draft0, row0_top]` branch
before committing. This passed the q4 oracle with `N=2`, but the current
sequential scaffold is still not a speed path:
`/tmp/ds4-mtp-exact-second-topk2-branch-20260510.csv` measured baseline
`36.09 TPS`, disabled `35.78 TPS`, exact `35.68 TPS`, and speed `34.55 TPS`
over a 3-run interleaved filter, with baseline, disabled, and exact sharing
hash `46c58f225f...`. A timing probe with margin `4` saw no branch rescues in
192 generated tokens. Lowering the margin to `0` produced two branch rescues,
but each rescue paid roughly two full verifier layer passes, about `50 ms` for
the original branch plus `50 ms` for the sibling branch. This confirms the
state-isolation scaffold is correct, but also confirms that a useful tree path
must batch sibling rows in one target verifier pass instead of replaying the
branch verifier sequentially.

An exact HC split+weighted-sum+norm fusion for verifier rows was tested behind
`DS4_MTP_VERIFY_FUSED_HC_NORM=1`. This reuses the decode fused kernel for the
N=2 verifier row batch, so it preserves per-row target semantics and only
removes the separate post-HC RMSNorm kernel. The explicit q4 oracle passed, but
the benchmark did not improve: `/tmp/ds4-mtp-fused-hc-norm-20260510.csv`
measured baseline `35.07 TPS`, disabled `34.56 TPS`, exact `34.58 TPS`, and
speed `33.71 TPS`; strict hashes matched. This falsifies HC+norm fusion as a
standalone parity fix.

A fresh promoted-path profile after that falsification reached the same
conclusion. `/tmp/ds4-mtp-n2-profile-20260510-0800.err` showed accepted
split-head verifier layer passes at `48.564`, `51.790`, `58.064`, and
`51.254 ms`; row0 head stayed around `1.15-1.24 ms`, row1 head around
`1.07-1.15 ms`, and logits readback was negligible after warmup. The run spent
`219.063 ms` in verification to save an estimated `230.561 ms` of target work,
but MTP/probe overhead left `est_net=-34.532 ms`. The commit histogram was
`0:8,2:4`, so the loss is mostly from zero-commit policy attempts and the
remaining exact layer-pass cost, not top-k/readback overhead.

Retuning the exact-safe target-margin pre-probe gate to `5` was also rechecked.
The 3-run confirmation
`/tmp/ds4-mtp-target-margin5-current-20260510-0802.csv` measured baseline
`35.17 TPS`, disabled `34.44 TPS`, exact `34.59 TPS`, and speed `33.45 TPS`.
The 5-run confirmation
`/tmp/ds4-mtp-target-margin5-current-5run-20260510-0804.csv` measured baseline
`35.02 TPS`, disabled `34.57 TPS`, exact `34.53 TPS`, and speed `33.44 TPS`.
Strict hashes matched throughout. Threshold `5` avoids more bad probes and can
beat the disabled lane in a short run, but the 5-run median still missed
baseline and did not beat disabled, so it is not promoted as the default.

The strict N=2 path now uses target-first probing by default for the normal
non-deep/non-tree verifier. It uses the already-known target top-1 as the first
speculative row, still runs the MTP block for that position so recursive
drafting has the right hidden state, but skips the first MTP output-head/top-k
work. `DS4_MTP_NO_EXACT_TARGET_FIRST=1` disables it for diagnostics. This is
hash-identical on the q4 oracle and cuts probe cost, but it is still not enough
to reach parity. With `DS4_MTP_EXACT_TARGET_FIRST=1
DS4_MTP_TARGET_MARGIN_SKIP=5`, the pre-promotion 3-run benchmark
`/tmp/ds4-mtp-exact-target-first-20260510-0814.csv` measured baseline
`35.66 TPS`, disabled `34.88 TPS`, exact `35.14 TPS`, and speed `33.61 TPS`.
With the default target-margin threshold, `/tmp/ds4-mtp-exact-target-first-default-20260510-0818.csv`
measured baseline `36.08 TPS`, disabled `35.61 TPS`, exact `35.53 TPS`, and
speed `34.54 TPS`. Strict hashes matched in both runs. The matching stats run
`/tmp/ds4-mtp-exact-target-first-stats-20260510-0820.err` confirms the tradeoff:
probe average dropped to `1.039 ms` and first misses fell to `0`, but
`commit_hist=0:9,2:6` and `est_net=-19.062 ms`. Target-first improves the probe
bucket, but creates more verifier work than the current N=2 layer pass can pay
back, so it remains gated. Lowering the exact second-draft margin to `2` under
target-first was worse: `/tmp/ds4-mtp-exact-target-first-margin2-20260510-0822.csv`
measured baseline `36.07 TPS`, disabled `35.60 TPS`, exact `35.13 TPS`, and
speed `35.56 TPS`, with strict hashes still matching.

The 5-run confirmation of the best target-first setting still missed the active
parity gate. `/tmp/ds4-mtp-exact-target-first-5run-20260510-0826.csv` measured
baseline `36.05 TPS`, disabled `35.57 TPS`, exact `35.59 TPS`, and speed
`34.51 TPS`; strict hashes matched. This is the best exact/no-drift N=2 result
in the latest cluster and barely beats the MTP-loaded disabled lane, but it is
still only `0.987x` baseline.

After promotion, the no-env q4 oracle still passed, but the no-env 5-run gate
remained below parity: `/tmp/ds4-mtp-promoted-target-first-5run-20260510-0836.csv`
measured baseline `36.09 TPS`, disabled `35.60 TPS`, exact `35.52 TPS`, and
speed `34.53 TPS`, with strict hashes identical. In other words, the promoted
N=2 path is now essentially at the MTP-loaded disabled overhead floor, but not
above the plain baseline.

A later quiet-host 5-run gate with the updated benchmark summary reached the
same conclusion with a smaller gap:
`/tmp/ds4-mtp-current-default-5run-20260510-0855.csv` measured baseline
`34.99 TPS`, disabled `34.65 TPS`, and exact `34.65 TPS`. The script reported
`disabled_vs_baseline=0.990` and `exact_vs_baseline=0.990`, with both disabled
and exact hashes matching the baseline. This is the current clean checkpoint:
exact N=2 has parity with the MTP-loaded disabled lane, not with the plain
baseline.

A fresh no-env 5-run gate after the later diagnostic-only pair-prefetch code
landed reproduced the same checkpoint:
`/tmp/ds4-mtp-current-default-postpair-5run-20260510.csv` measured baseline
`34.99 TPS`, disabled `34.65 TPS`, exact `34.65 TPS`, and speed `33.47 TPS`.
Baseline, disabled, and exact shared the baseline hash; speed drifted. This
confirms the current promoted path still preserves output and still lands on
the MTP-loaded disabled floor, not at plain-baseline parity.

`DS4_MTP_EXACT_TARGET_FIRST_FUSED=1` is an opt-in target-first probe fusion. It
encodes the exact target decode and the one-token MTP state update in one Metal
command stream, then still uses the exact target top-1 token as the first
proposal. The q4 oracle passed, but the benchmark regressed:
`/tmp/ds4-mtp-exact-target-first-fused-20260510.csv` measured baseline
`36.09 TPS`, disabled `35.81 TPS`, exact `35.56 TPS`, and speed `34.54 TPS`
over a 3-run interleaved filter. Strict hashes matched. The stats run showed
why this is not promotable: the separate probe timer disappears, but fusion
runs the MTP block before the exact target-margin gate can skip it, so low
margin tokens pay unnecessary MTP work. The sample recorded `target_margin_skip=6`
and `est_net=-45.753 ms`, worse than the current unfused target-first path.

`DS4_MTP_EXACT_TARGET_FIRST_PAIR=1` is a narrower opt-in target-first
prefetch. It leaves the target decode and target-margin gate unchanged, but
when the gate allows speculation it encodes the two dependent MTP blocks
(`current token -> MTP state`, then `target_first -> second draft`) in one
command stream and skips the later second-draft MTP call. The q4 oracle passed,
but the 3-run filter still missed parity:
`/tmp/ds4-mtp-exact-target-first-pair-20260510.csv` measured baseline
`34.99 TPS`, disabled `34.64 TPS`, exact `34.54 TPS`, and speed `33.47 TPS`,
with strict hashes matching. The stats sample moved draft time into the probe
bucket (`probe_avg=2.847 ms`, `draft=0.531 ms`) and showed a small positive
local estimate (`est_net=3.302 ms`), but end-to-end TPS still regressed. This
means command submission between the two MTP blocks is not the dominant gap.

A follow-up variant let that pair-prefetch path return the second-token top-2
candidates so it could feed the existing exact second-token top-k rescue
without a later standalone MTP call. This was also reverted. The q4 oracle
passed with `DS4_MTP_EXACT_TARGET_FIRST_PAIR=1
DS4_MTP_EXACT_SECOND_TOPK=2`, but the 3-run filter
(`/tmp/ds4-mtp-target-first-pair-secondtopk2-20260510.csv`) measured baseline
`35.50 TPS`, disabled/no-open `35.39 TPS`, exact `34.61 TPS`
(`exact_vs_baseline=0.975`), and speed `33.43 TPS`. Strict hashes matched, but
the result confirms that another MTP prefetch shortcut is not the tree path; a
useful tree verifier must batch branch-local target rows.

Rechecking the previously safe router shortcut on top of the promoted
target-first path did not help. `DS4_MTP_VERIFY_FAST_ROUTER=1` measured baseline
`34.98 TPS`, disabled `34.73 TPS`, exact `34.63 TPS`, and speed `33.48 TPS` in
`/tmp/ds4-mtp-promoted-target-first-fast-router-20260510-0852.csv`; exact
produced two hashes, so the combination is not correctness-preserving.

## Tree Oracle

`DS4_MTP_TREE_ORACLE=1` is now diagnostic-only evidence collection for a
future OPT-Tree/Sequoia-style verifier. The current default depth is 3, because
that is the smallest tree shape that can plausibly beat linear N=2 without
exploding branch-local target state.

After the pivot away from linear N=2 work, the q4 tree oracle was rerun in a
dense diagnostic mode with adaptive and target-margin skips disabled:

```sh
DS4_MTP_TREE_ORACLE=1 \
DS4_MTP_TREE_BRANCH_ORACLE=1 \
DS4_MTP_TREE_ORACLE_DEPTH=3 \
DS4_MTP_NO_ADAPTIVE=1 \
DS4_MTP_NO_TARGET_MARGIN_SKIP=1 \
./ds4 -m "$BASE_Q4" --mtp "$MTP_Q4" --mtp-draft 2 \
  --temp 0 --nothink -n 96 -p "$PROMPT"
```

The three-prompt Studio q4 refresh
(`/tmp/ds4-tree-oracle-pivot-dense-20260510-171202`) sampled 164 oracle
positions with zero failures. Weighted averages across the prompts were:

| Tree policy | Average contained path length | Average candidate nodes | Contained tokens / node | Full depth-3 path |
| --- | ---: | ---: | ---: | ---: |
| full top-2 / `2x2x2` | 1.894 | 14.0 | 0.135 | 59/164 (36.0%) |
| static `4x2x2` | 2.032 | 28.0 | 0.073 | 64/164 (39.0%) |
| static `4x4x2` | 2.139 | 52.0 | 0.041 | 69/164 (42.1%) |
| root-margin `root>=2:2x2x2_else4x2x2` | 2.032 | 20.9 | 0.097 | 64/164 (39.0%) |
| target-path dynamic `depth>=2:2_else4` | 2.196 | 38.6 | 0.057 | 82/164 (50.0%) |
| branch-local dynamic `depth>=2:2_else4` | 2.196 | 41.4 | 0.053 | 82/164 (50.0%) |

This is the current pivot read: tree containment is real, but the dense all-step
ceiling is more modest than the earlier sparse/speculation-gated samples.
Static `4x2x2` is implementable with the current 16-row scratch layout, but its
average contained path length is only slightly above two tokens in the dense
oracle. The only policy that materially improves depth-3 containment is
`depth>=2:2_else4`, and the branch-local measurement confirms the node cost is
about 41 candidates per oracle step. That is enough to keep tree verification
on the table, but not enough to justify broad kernel work blindly; the next
gate should be a selective oracle that reports containment only for online
positions where a cheap confidence policy would actually choose to speculate.

The first depth-3 q4 run
(`/tmp/ds4-mtp-tree-oracle-shapes-20260510-134116.{out,err}`) measured 103
oracle steps with no failures. The exact greedy path containment was:

| MTP tree | Average contained path length | Average candidate nodes |
| ---: | ---: | ---: |
| full top-1 | 1.26 | 3.0 |
| full top-2 | 1.86 | 13.9 |
| full top-4 | 2.36 | 83.4 |
| full top-8 | 2.61 | 579.0 |

Full top-4 and top-8 improve containment, but the candidate-node cost is too
large to justify an implementation by itself. The useful next question is
whether small budgeted trees capture most of that improvement. A three-prompt
diagnostic suite
(`/tmp/ds4-mtp-tree-oracle-shapes-suite-20260510-134206-*.err`) gave these
weighted averages across 319 oracle steps:

| Shape | Average contained path length | Average candidate nodes | Read |
| ---: | ---: | ---: | --- |
| `2x2x2` | 2.00 | 14 | Same as full top-2. Efficient, but limited. |
| `4x2x1` | 2.09 | 20 | Better first-token coverage, weak depth-3. |
| `4x2x2` | 2.22 | 28 | Best small static budget so far. |
| `4x4x1` | 2.20 | 36 | Similar length for more nodes. |
| `4x4x2` | 2.33 | 52 | Higher ceiling, but weaker efficiency. |
| `8x4x1` | 2.23 | 72 | Not enough gain for the extra nodes. |

The suite also showed prompt sensitivity: the Python-code prompt reached
`2.68` average contained tokens for `4x2x2`, while the speculative-decoding
summary prompt reached only `2.03`. So a static tree may not be enough.

The oracle now also emits an offline OPT-style root-margin policy table: for
each first MTP margin bucket it reports the static shape that maximizes contained
length and the static shape that maximizes contained tokens per candidate node.
This is still diagnostic only; it does not change generation. After fixing the
branch-local MTP diagnostic to reset the MTP raw-row counter for every sibling
path, a short q4 three-prompt suite
(`/tmp/ds4-mtp-branch-raw-reset-20260510-142523-*.err`) measured 79 oracle
steps:

| Policy or shape | Average contained path length | Average candidate nodes | Contained tokens / node |
| --- | ---: | ---: | ---: |
| static `2x2x2` | 2.479 | 13.8 | 0.179 |
| static `4x2x2` | 2.479 | 27.7 | 0.089 |
| static `4x4x2` | 2.554 | 51.4 | 0.050 |
| root-margin best length | 2.554 | 19.6 | 0.130 |
| root-margin best efficiency | 2.504 | 14.3 | 0.175 |

The root-margin policy is useful evidence, but not a breakthrough: it can get
near the widest static shape's containment with fewer nodes, while the efficient
policy mostly collapses back to narrow trees. That reinforces the current
implementation order: keep `4x2x2` as the first exact target-tree verifier shape
because it fits the 16-row scratch layout, and treat wider/dynamic trees as a
later chunking problem.

That extension now reports a small set of margin-driven dynamic policies. The
diagnostic is still an offline oracle: root-margin policies are implementable
from the first MTP distribution, while `min>=...` and per-depth policies are
upper bounds until the candidate tree computes branch-local MTP margins. The
three-prompt q4 suite
(`/tmp/ds4-mtp-tree-oracle-dynamic-suite-20260510-134829-*.err`) measured:

| Dynamic policy | Average contained path length | Average candidate nodes | Read |
| --- | ---: | ---: | --- |
| `root>=2:2x2x2_else4x2x2` | 2.18 | 20.8 | Best efficiency; slightly below static `4x2x2` length. |
| `root>=5:2x2x2_root>=2:4x2x2_else4x4x2` | 2.28 | 36.8 | Higher ceiling, but node cost approaches wider trees. |
| `min>=2:2x2x2_else4x2x2` | 2.21 | 26.0 | Similar to static `4x2x2`; not directly online. |
| `min>=5:2x2x2_min>=2:4x2x2_else4x4x2` | 2.32 | 48.3 | Near `4x4x2` containment at similar cost; not directly online. |
| `depth>=2:2_else4` | 2.33 | 39.8 | Best ceiling/cost tradeoff, but needs branch-local MTP margins. |

This narrows the next diagnostic step. Static `4x2x2` remains the best simple
offline tree, but the only policy that moves the ceiling meaningfully is
per-depth confidence. Before designing target-tree kernels, the oracle should
measure branch-local dynamic node counts instead of estimating them only from
the accepted target path.

`DS4_MTP_TREE_BRANCH_ORACLE=1` adds that branch-local diagnostic for the
promising `depth>=2:2_else4` policy. It expands non-target MTP branches only to
measure the real candidate-node count; it still does not run a target tree
verifier. The first version of this diagnostic was too optimistic because it did
not reset `mtp_n_raw` before each sibling replay, allowing stale sibling raw
rows to affect later branch expansion. After fixing that reset, the q4
three-prompt suite
(`/tmp/ds4-mtp-branch-raw-reset-20260510-142523-*.err`) measured 79 oracle
steps:

| Tree policy | Average contained path length | Average candidate nodes | Full depth-3 path |
| --- | ---: | ---: | ---: |
| static `4x2x2` | 2.479 | 27.7 | 60.8% |
| static `4x4x2` | 2.554 | 51.4 | 64.6% |
| target-path estimate `depth>=2:2_else4` | 2.604 | 27.7 | 70.9% |
| branch-local `depth>=2:2_else4` | 2.604 | 30.0 | 70.9% |

This confirms the per-depth policy is not merely an artifact of using
target-path node estimates: with clean sibling-local MTP raw counters, real
branch-local node cost is still close to the estimate. It gives better
containment than `4x4x2` with fewer nodes, but it needs dynamic width support
and is only about as node-efficient as static `4x2x2`. The decision gate remains
unchanged: implement static `4x2x2` first, then revisit dynamic/chunked trees
only after exact target-tree state works.

`DS4_MTP_TREE_STATE_PLAN=1` prints the concrete q4 target-state footprint for
that design. Tree diagnostics now allocate `spec_logits_rows=16` instead of the
strict N=2 default of 2, so the efficient depth-3 shape can fit the existing
scratch rows without changing normal generation. The short q4 plan run
(`/tmp/ds4-mtp-tree-state-plan-20260510-140247.{out,err}`) reported:

| Shape | Nodes | Max active width | Fits current scratch | Active HC | Branch raw rows | Naive branch frontiers |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| `2x2x2` | 14 | 8 | yes | 1.00 MiB | 1.18 MiB | 162.97 MiB |
| `4x2x2` | 28 | 16 | yes | 2.00 MiB | 2.35 MiB | 325.94 MiB |
| `4x4x2` | 52 | 32 | no | 4.00 MiB | 4.37 MiB | 605.31 MiB |
| `depth>=2:2_else4` worst case | 84 | 64 | no | n/a | n/a | n/a |

This makes `4x2x2` the first implementable exact tree verifier target with the
current scratch layout. The per-depth dynamic policy still has the better
containment ceiling, but it needs chunking or an explicit width cap before it
can become an exact target verifier.

This makes a real exact tree verifier the only credible path toward a larger
gain. Single-path N=2 needs roughly another 10 ms per successful verifier call
to break even on this q4 setup; a tree verifier can instead target longer
accepted prefixes, but only if it has tree-aware target attention/state
handling rather than the current linear-suffix verifier.

`DS4_MTP_EXACT_TREE_REPLAY=1` is an opt-in proof-of-control diagnostic for
that path. It requests MTP top-k alternatives, follows the exact greedy target
token whenever it is contained in those alternatives, and commits the path by
ordinary exact target replay. This is hash-safe but intentionally not a fast
tree verifier. With `DS4_MTP_EXACT_TREE_TOPK=2
DS4_MTP_EXACT_TREE_DEPTH=4`, the q4 oracle passed and the 3-run interleaved
benchmark (`/tmp/ds4-mtp-exact-tree-replay-20260510-072704.{log,csv}`)
measured baseline `36.02 TPS`, disabled `35.42 TPS`, exact `35.06 TPS`, and
speed `34.39 TPS`. Baseline, disabled, and exact shared hash `cc6bd264...`;
speed drifted to `0d556e54...`.

The matching stats run confirms both sides of the tradeoff: top-2 replay
accepted longer exact prefixes (`commit_hist=1:2,2:5,3:2,4:2`) with no replay
fallback, but it spent `720.597 ms` in target replay for `727.047 ms` of
estimated saved target work. Including MTP probe/replay overhead, estimated net
was `-63.548 ms`. This proves that top-k containment can increase exact prefix
length, but also that it needs a real batched/tree target verifier before it
can be a speed path.

`DS4_MTP_EXACT_TREE_SHAPE=4x2x2` extends that replay diagnostic to the first
implementable tree shape. It uses first-token top-4, then top-2 at depths 2 and
3, and still commits by ordinary exact target replay. A paired q4 run
(`/tmp/ds4-mtp-exact-tree-shape-20260510-140727.{baseline,shape}.{out,err}`)
was hash-identical and showed the shape committing 2-3 tokens per accepted
tree step (`commit_hist=2:6,3:5`). The timing sample still spent `775.300 ms`
in target replay for `775.531 ms` of estimated saved target work, with MTP
overhead making the net `-68.772 ms`.

A compact 3-run interleaved check
(`/tmp/ds4-mtp-exact-tree-shape-bench-20260510-140759.csv`) measured baseline
`34.62 TPS` vs exact `4x2x2` replay `34.09 TPS` (`0.985x`), with identical
output hashes. This is the expected result: `4x2x2` improves contained-prefix
length, but replay is still serial target decode. The next performance gate is
therefore an exact target-tree verifier that verifies the `4x2x2` rows by depth
instead of replaying the accepted path.

A later decisive q4 retry on the speculative-decoding prompt confirmed this is
not a hidden win. `DS4_MTP_EXACT_TREE_SHAPE=4x2x2` over 3 interleaved runs
(`/tmp/ds4-decisive-tree-shape-20260510-171822.{log,csv}`) measured baseline
`36.41 TPS`, disabled/no-open `36.48 TPS`, and exact `35.57 TPS`
(`0.977x`), with baseline/disabled/exact all sharing hash
`9194cc520c2a...`. The approximate speed lane reached `38.55 TPS`, but output
drifted (`437` bytes, hash `7433deff45b6...` versus baseline `497` bytes).
`DS4_MTP_EXACT_TREE_REPLAY=1 DS4_MTP_EXACT_TREE_TOPK=2
DS4_MTP_EXACT_TREE_DEPTH=4`
(`/tmp/ds4-decisive-tree-top2-20260510-171926.{log,csv}`) was also exact but
slower: baseline `36.47 TPS`, disabled `36.43 TPS`, exact `35.44 TPS`
(`0.972x`). Verdict: existing exact tree replay does not work as a performance
path. The only worthwhile implementation remains a real depth-batched target
tree verifier; replay can be retired as a speed candidate.

The final exact-tree falsifier used the existing `4x2x2` depth-batch verifier
as the lower-bound measurement for the current implementation. It does not
commit, does not use replay for the branch path, and reports the target branch
verifier cost separately from MTP tree construction and normal serial replay:

```sh
DS4_MTP_TREE_DEPTH_BATCH_PROBE=4x2x2 \
DS4_MTP_TREE_PROBE_STEPS=5 \
DS4_MTP_NO_ADAPTIVE=1 \
DS4_MTP_NO_TARGET_MARGIN_SKIP=1 \
./ds4_test --mtp-oracle
```

Artifact: `/tmp/ds4-final-tree-lb-20260510-181233.{out,err}`. The q4 probe
remained exact (`mismatches=0`, `max_delta=0`), but the branch verifier was far
outside the useful range:

| Metric | Median |
| --- | ---: |
| target branch layer-ish verifier | `794.054 ms` |
| target branch with output head/read | `807.840 ms` |
| Metal layer execute bucket | `762.577 ms` |
| command encode | `30.480 ms` |
| output head execute | `13.447 ms` |

For the dense-oracle static `4x2x2` contained length of `2.032`, the required
round cost is about `44.7 ms` for a projected `1.25x` and `37.3 ms` for
`1.5x` on a `27.5 ms/token` baseline. The measured verifier bucket is `17.8x`
over the `1.25x` threshold and `21.3x` over the `1.5x` threshold, before adding
draft/commit overhead. This falsifies the current exact MTP/tree speed path for
this sidecar. Further exact work should stop unless a different drafter
materially improves contained length or a new verifier architecture can prove a
radically lower target-row cost before integration.

`DS4_MTP_EXACT_TREE_VERIFY=<shape>` is now the next diagnostic-only oracle for
that verifier design. It does not change the generation path. For a limited
number of oracle steps (`DS4_MTP_EXACT_TREE_VERIFY_STEPS`, default `1`), it
builds the MTP candidate tree for the selected shape, then verifies target
branches by restoring the saved prefix and serially replaying each branch path.
This is intentionally slow; its purpose is to validate branch isolation and
measure the gap between candidate nodes and naive target decode calls before
writing real tree kernels.

The first q4 checks passed without target-verify failures:

| Shape | Candidate nodes | Branch paths replayed | Serial target decodes | Target replay time | MTP branch time |
| --- | ---: | ---: | ---: | ---: | ---: |
| `2x2x2` | 14 | 14 | 34 | `983.501 ms` | `11.832 ms` |
| `4x2x2` | 28 | 28 | 68 | `1974.734 ms` | `23.979 ms` |

Artifacts:

- `2x2x2`: `/tmp/ds4-mtp-exact-tree-verify-2x2x2-20260510-143940.{out,err}`
- `4x2x2`: `/tmp/ds4-mtp-exact-tree-verify-20260510-143853.{out,err}`
- q4 regression guard: `DS4_MTP_EXACT_TREE_VERIFY=4x2x2
  DS4_MTP_EXACT_TREE_VERIFY_STEPS=1 ./ds4_test --mtp-oracle` passed.

The read is useful but sober: the diagnostic proves the branch-local restore
model can validate exact tree candidates, while the replay cost proves this
cannot become a speed path without depth-batched target rows and branch-local
raw/compressor/indexer state. It also gives the expected work multiplier for
the naive fallback: `2x2x2` doubles candidate nodes into 34 target decodes, and
`4x2x2` doubles them into 68 target decodes because ancestors are replayed for
every child/grandchild branch.

`DS4_MTP_TREE_BATCH_PLAN=1` extends the state plan with the first concrete
depth-batched target verifier blueprint. A q4 plan run
(`/tmp/ds4-mtp-tree-batch-plan-20260510-144348.{out,err}`) printed:

| Shape | Level widths | Target rows | Serial replay decodes | Depth passes | Chunked passes | Frontier double buffer | Active raw path |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| `2x2x2` | `2x4x8` | 14 | 34 | 3 | 3 | 139.69 MiB | 2.02 MiB |
| `4x2x2` | `4x8x16` | 28 | 68 | 3 | 3 | 279.38 MiB | 4.03 MiB |

This is the real engineering target. The current `4x2x2` q4 scratch layout can
cover each depth without chunking (`4`, `8`, then `16` rows), so the next
implementation should not start by growing scratch rows. It should instead add
branch-local state buffers so each depth pass can consume parent state rows and
produce child state rows without replaying ancestors. In rough terms, that
turns the diagnostic's 68 exact target decodes into 28 target rows across three
layer passes, with the cost shifted to branch-local frontier/raw state movement.

`DS4_MTP_TREE_STATE_ALLOC=<shape>` now probes that state movement directly. It
allocates the per-layer branch-local raw rows plus frontier double buffer for
the selected shape and copies the live root frontier into one or all slots. On
q4, `4x2x2` passed both allocation probes:

| Probe | Branch state | Slot copies | Allocate time | Copy time |
| --- | ---: | ---: | ---: | ---: |
| root slot only | 283.41 MiB | 1 | `0.645 ms` | `5.286 ms` |
| fill all slots | 283.41 MiB | 24 | `0.654 ms` | `7.140 ms` |

Artifacts:

- `/tmp/ds4-mtp-tree-state-alloc-20260510-144814.{out,err}`
- `/tmp/ds4-mtp-tree-state-alloc-fill-20260510-144832.{out,err}`

This removes one concern: the branch-local state footprint is not too large for
the Studio q4 target, and raw allocation/copy overhead is not the dominant
risk. The hard part remains semantic: layer kernels must read parent branch
frontiers/raw rows and write child branch state without touching the live
linear cache or mixing sibling futures.

`DS4_MTP_TREE_BRANCH_SWAP_PROBE=<shape>` is the first semantic check for that
kernel path. It binds the live graph's compressor/indexer frontier pointers and
counters to branch-local scratch slot 0, runs one exact target token, and
compares it with the normal exact decode for the same token. This first probe
still uses the live raw cache (`raw=live` in the log), so it proves frontier
pointer/counter isolation before we tackle branch-local raw attention.

The q4 prompt probe
(`/tmp/ds4-mtp-tree-branch-swap-20260510-145253.{out,err}`) passed:

| Shape | Token position | Normal top | Branch top | Logit delta | Branch-state copy | Branch eval |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| `4x2x2` | 18 | 59524 | 59524 | `0` | `4.794 ms` | `28.059 ms` |

The q4 regression guard also passed with the probe enabled:
`DS4_MTP_TREE_BRANCH_SWAP_PROBE=4x2x2
DS4_MTP_EXACT_TREE_VERIFY=4x2x2
DS4_MTP_EXACT_TREE_VERIFY_STEPS=1 ./ds4_test --mtp-oracle`. Its probe line
again reported matching tops and `delta=0` at position 24. This is the first
green light that exact layer execution can be pointed at branch-local frontiers
without changing target logits.

`DS4_MTP_TREE_RAW_SWAP_PROBE=<shape>` extends that semantic check to a
branch-owned raw cache. It still is not the final tree verifier design: the
probe copies the whole raw ring for one branch, while the real verifier should
read shared prefix raw rows plus only branch-local future rows. Its value is
semantic, not speed.

The q4 prompt probe
(`/tmp/ds4-mtp-tree-raw-swap-20260510-145743.{out,err}`) passed:

| Shape | Token position | Normal top | Branch top | Logit delta | Frontier copy | Raw copy | Branch eval |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| `4x2x2` | 18 | 59524 | 59524 | `0` | `4.636 ms` | `3.958 ms` | `27.997 ms` |

The q4 regression guard also passed with raw-swap and target-verify enabled:
`DS4_MTP_TREE_RAW_SWAP_PROBE=4x2x2
DS4_MTP_EXACT_TREE_VERIFY=4x2x2
DS4_MTP_EXACT_TREE_VERIFY_STEPS=1 ./ds4_test --mtp-oracle`. Artifact
`/tmp/ds4-mtp-tree-raw-swap-test-20260510-150013.{out,err}` reported
matching tops at position 24 (`normal_top=40131`, `branch_top=40131`,
`delta=0`), `283.41 MiB` of branch frontier state, `193.50 MiB` of copied raw
cache, and `mtp-oracle: OK`. This removes the immediate semantic blocker:
exact decode can run from branch-owned frontiers and branch-owned raw cache
without logit drift. The remaining design problem is efficient mixed raw
attention for depth-batched siblings, not single-branch correctness.

`DS4_MTP_TREE_RAW_COMPACT_PROBE=<shape>` tightens that raw-cache diagnostic. It
copies only the visible raw window into a compact per-branch ring, then runs the
same branch-local exact decode with the compact raw capacity. This is closer to
the target verifier than a full raw-ring copy, although it still copies shared
prefix rows instead of reading them from the live prefix plus branch-local
future rows.

The q4 regression guard passed with the compact raw-window probe:
`DS4_MTP_TREE_RAW_COMPACT_PROBE=4x2x2
DS4_MTP_EXACT_TREE_VERIFY=4x2x2
DS4_MTP_EXACT_TREE_VERIFY_STEPS=1 ./ds4_test --mtp-oracle`. Artifact
`/tmp/ds4-mtp-tree-raw-compact-test-20260510-150544.{out,err}` reported
matching tops at position 24 (`normal_top=40131`, `branch_top=40131`,
`delta=0`), `raw_cap=25`, `2.10 MiB` of copied compact raw cache, `1.159 ms`
raw-copy time, and `mtp-oracle: OK`. This reduces the raw-state requirement
for the single-branch check from full-ring copy to visible-window copy. The
next verifier design step is still a true mixed raw attention source for
depth-batched siblings, so shared prefix rows are not duplicated per branch.

The compact probe also passed over a depth-3 greedy target path with
`DS4_MTP_TREE_RAW_COMPACT_PROBE_DEPTH=3`. Artifact
`/tmp/ds4-mtp-tree-raw-compact-depth3-test-20260510-150836.{out,err}` reported
matching final logits after three exact target decodes (`normal_top=295`,
`branch_top=295`, `delta=0`), `raw_cap=27`, `2.27 MiB` of compact raw cache,
and `1.115 ms` raw-copy time. This proves that branch future raw rows written
into the compact ring are visible to later tokens in the same path. It still
does not solve sibling batching: divergent siblings need either a mixed
live-prefix/branch-future attention source or per-sibling compact windows.

`DS4_MTP_TREE_SLOT_PATH_PROBE=<shape>` adds the corresponding frontier
propagation check. Instead of staying bound to one branch-local slot for the
whole path, it decodes one token, captures the mutated compressor/indexer
frontier and counters into a child slot, unbinds, then rebinds the child slot
for the next token. With `DS4_MTP_TREE_SLOT_PATH_PROBE_DEPTH=3`, the q4 guard
passed: `/tmp/ds4-mtp-tree-slot-path-test-20260510-151210.{out,err}` reported
`normal_top=295`, `branch_top=295`, `delta=0`, `raw_cap=27`, `2.27 MiB` of
compact raw cache, and `raw=branch-compact-slot-path`. This proves the
frontier state needed by a branch can be promoted from parent slot to child
slot across a depth-3 exact path without logit drift. The next unproven piece
is sibling fanout: multiple children from the same parent must each get their
own child slot and branch-local future raw rows.

`DS4_MTP_TREE_SIBLING_PROBE=<shape>` checks that first sibling fanout case. It
uses the root MTP top-k for the requested shape, copies the same root frontier
into one child slot per root candidate, decodes each child with compact raw
state, and compares every child against a normal serial exact decode restored
from the same prefix. The q4 guard passed for `4x2x2`: artifact
`/tmp/ds4-mtp-tree-sibling-probe-test-20260510-151533.{out,err}` reported
`root_width=4`, `mismatches=0`, `max_delta=0`, `raw_cap=25`, `2.10 MiB` of
compact raw cache, and `raw=branch-compact-siblings`. This proves root-level
sibling slots can be exact. The remaining tree-verifier risk moves one level
deeper: children of different parents must be expanded into grandchild slots
without replaying ancestors, and then the exact target rows need to be batched
by depth instead of run serially.

`DS4_MTP_TREE_GRANDCHILD_PROBE=<shape>` exercises that next level. For
`4x2x2`, it expands the four root candidates, obtains two MTP children for
each root, decodes the root candidates into parent slots, copies each parent
slot into a grandchild slot, decodes the child token from there, and compares
all eight two-token branches against normal serial exact restore. The q4 guard
passed: `/tmp/ds4-mtp-tree-grandchild-probe-test-20260510-152001.{out,err}`
reported `root_width=4`, `child_width=2`, `branches=8`, `mismatches=0`,
`max_delta=0`, compact raw caps in the `26-55` range, and compact raw copies
from `8.73 MiB` to `18.48 MiB` as the probe position advanced. This clears the
main semantic risks for a static `4x2x2` tree: branch-owned raw windows,
parent-to-child frontier propagation, root sibling slots, and grandchild slots.
The remaining work is no longer correctness of branch-local state in serial
diagnostics; it is turning the serial branch probes into depth-batched target
rows and a prefix-k commit path.

`DS4_MTP_TREE_FULL_PROBE=<shape>` closes the static-tree semantic check for
the first target shape. For `4x2x2`, it builds all MTP candidates for a depth-3
tree, decodes the four root candidates into branch-local slots, clones compact
raw windows per root-child pair, decodes the eight child states, compacts those
pair states into parent slots, then verifies all sixteen grandchild branches
against normal serial exact restore. The q4 guard passed:
`/tmp/ds4-mtp-tree-full-probe-test-20260510-153039.{out,err}` reported
`root_width=4`, `child_width=2`, `grand_width=2`, `branches=16`,
`mismatches=0`, `max_delta=0`, `raw_cap=27`, `27.21 MiB` of compact raw
copies, and `state_bytes=283.41 MiB`. This is still a serial diagnostic
(`branch≈750 ms`, `normal≈1276 ms` in the one-shot probe), not a fast
implementation. Its value is that the exact state model now survives the full
static tree: branch-local compressor/indexer frontiers plus per-pair compact
raw windows are enough to make every depth-3 branch logit-identical to serial
restore. The next worthwhile implementation slice is therefore a depth-batched
target verifier for this same `4x2x2` shape, with exact prefix commit as the
gate before any throughput claim.

`DS4_MTP_TREE_DEPTH_BATCH_PROBE=<shape>` tried the shallowest implementation
step after the full-tree proof: keep the same branch-local states and compact
raw windows, but group all rows at a tree depth into a single command-buffer
sequence. This preserves exact per-row decode semantics and avoids one command
submission per branch token, but it still encodes rows one by one and binds
branch-local state views per row. The q4 guard passed for `4x2x2`:
`/tmp/ds4-mtp-tree-depth-batch-probe-test-20260510-153753.{out,err}` reported
`branches=16`, `mismatches=0`, `max_delta=0`, `raw_cap=27`, `raw_bytes=27.21
MiB`, and `raw=branch-compact-depth-command-batch`. The timing was a useful
negative result: branch work was about `782 ms`, slightly slower than the
serial full-tree probe's `750 ms`, while normal replay was about `1306 ms`.
The follow-up bucketed profile
(`/tmp/ds4-mtp-tree-depth-batch-profile-test-20260510-154139.{out,err}`)
showed why: over 28 branch rows in three depth calls, CPU-side view creation
was below `0.02 ms`, embedding about `5.6-5.9 ms`, bind/unbind about
`0.33 ms`, command encoding about `30-31 ms`, logits readback below `1 ms`,
and output-head execution about `13.4 ms`; the dominant bucket was Metal
execution of the exact per-row decode work at about `744-745 ms`. So
command-buffer batching and lighter branch-state binding are not enough to
reach parity or 1.25x. The next implementation would need true row-parallel
exact layer kernels or tree attention before the tree verifier is worth wiring
into generation.

An additional `DS4_METAL_DECODE_STAGE_PROFILE=1` run on the same probe
(`/tmp/ds4-mtp-tree-depth-batch-stage-profile-20260510-154437.{out,err}`)
kept the exactness guard green (`mismatches=0`, `max_delta=0`). This profiler
adds synchronization at every stage, so its absolute timings are not comparable
to normal generation, but the stage mix is still informative. Aggregated across
the profiled q4 oracle, the largest exact-decode buckets were routed MoE
(`6315 ms`), attention output (`5854 ms`), Q path (`5503 ms`), attention
(`5148 ms`), shared gate/up (`4444 ms`), attention HC pre (`4336 ms`), FFN HC
pre (`4319 ms`), compressor/indexer (`4234 ms`), router (`4197 ms`), KV path
(`4070 ms`), and shared down (`4039 ms`). Norm/post stages were much smaller
at roughly `360-378 ms`. The cost is spread across the normal decode layer
stack, so the credible implementation path is broad row-parallelization of the
exact target rows, not a narrow fix in readback, branch binding, or one small
kernel.

The first repeated tree probes exposed an important missing state component.
After adding `DS4_MTP_TREE_PROBE_STEPS=<N>`, the one-shot exactness result did
not hold across all positions: `/tmp/ds4-mtp-tree-depth-batch-repeat-20260510-155338`
showed `mismatches>0` at several starts, including positions where a speculative
path crosses a compression boundary, while
`target-verify shape=4x2x2 steps=8 failures=0` stayed green. The repeated
serial full-tree probe (`/tmp/ds4-mtp-tree-full-repeat-20260510-155511`) showed
the same pattern. The bug was not the candidate tree itself; the scratch state
carried branch-local raw rows plus compressor/indexer frontier state and
counters, but it did not preserve newly emitted compressed attention/indexer
cache rows per branch slot.

The diagnostic scratch state now stores those branch-local compressed cache
rows and restores them before decoding a branch. This raises the `4x2x2`
scratch estimate from `283.41 MiB` to `289.93 MiB`, but it closes the exactness
hole. The q4 repeat guard
(`/tmp/ds4-mtp-tree-branch-comp-cache-repeat-20260510-160006`) passed for both
paths:

- `DS4_MTP_TREE_DEPTH_BATCH_PROBE=4x2x2 DS4_MTP_TREE_PROBE_STEPS=8`:
  every repeated probe line reported `mismatches=0`, `max_delta=0`, and
  `target-verify shape=4x2x2 steps=8 failures=0`.
- `DS4_MTP_TREE_FULL_PROBE=4x2x2 DS4_MTP_TREE_PROBE_STEPS=8`: every repeated
  probe line reported `mismatches=0`, `max_delta=0`, and the same target verify
  guard stayed green.

This is still a diagnostic fix, not a speed path. Its main design consequence
is that a future exact row-parallel tree verifier must treat compressed cache
rows as branch-local state too; branch-local frontier tensors and raw windows
alone are insufficient.

A root-sibling batch-reuse probe then tested whether the existing linear suffix
row-batch verifier could be reused as a cheap target verifier for sibling
alternatives. It cannot. The q4 diagnostic
(`/tmp/ds4-mtp-tree-root-batch-reuse-detail-20260510-161054`) ran
`DS4_MTP_TREE_ROOT_BATCH_REUSE_PROBE=4x2x2
DS4_MTP_TREE_ROOT_BATCH_REUSE_PROBE_STEPS=8`; the oracle passed, but every
probe line reported `logit_mismatches=4`, and later sibling rows often changed
top-1 as well (`top_mismatches=1-3` on most repeated steps). Row 0 usually kept
the same top with only tiny logit delta because it is equivalent to the first
linear suffix token, while rows 1-3 were evaluated as future positions instead
of same-position sibling alternatives. The existing row-batch path was faster
for four rows (`batch≈82-84 ms` versus `normal≈110-125 ms`), but it is not an
exact sibling verifier. This means the tree path needs same-position,
branch-local target kernels/tree attention; `metal_graph_verify_suffix_tops()`
cannot be repurposed directly.

A target-first entry variant for exact tree replay was tried and reverted. It
allowed the tree replay diagnostic to start when the first speculative token
was already the exact target token, avoiding the previous first-token top-k
entry requirement. The q4 oracle passed with `DS4_MTP_EXACT_TREE_REPLAY=1
DS4_MTP_EXACT_TARGET_FIRST=1 DS4_MTP_EXACT_TREE_TOPK=2
DS4_MTP_EXACT_TREE_DEPTH=4`, but the 3-run filter
(`/tmp/ds4-mtp-target-first-tree-replay-20260510.csv`) measured baseline
`35.49 TPS`, disabled/no-open `35.37 TPS`, exact `34.27 TPS`
(`exact_vs_baseline=0.966`), and speed `33.41 TPS`. Strict hashes matched, but
the target-first variant was slower than the promoted N=2 default. Sequential
tree replay remains useful as a containment proof, not a speed path.

## Practical Conclusion

The current exact linear MTP path is correctness-preserving and close to
baseline, but not yet faster than baseline on q4. Further work should prioritize
an exact tree verifier with prefix-k commit support. More linear N=2 tuning
should stop unless it is needed for cleanup, because the q4 profile shows the
exact layer pass, not readback/head/upload, dominates verifier cost.
Margin/adaptive policies help avoid losing badly, but mostly by skipping
speculation rather than making speculation profitable.

The credible path toward `1.25x` is not another policy knob on the current
linear N=2 verifier. The measured verifier-scale ceiling starts becoming
interesting at depth 3, and the tree oracle shows that wider or budgeted MTP
alternatives often contain the target greedy path for more than two positions.
A realistic implementation path is:

1. Keep the promoted N=2 path as the stable strict default.
2. Treat the offline tree oracle as the gate before fast kernels: use MTP
   top-k/confidence to measure small OPT-Tree/Sequoia-style trees, starting
   with top-2/top-4 and depth 3, and check whether the greedy baseline path is
   contained often enough to justify implementation.
3. Do not optimize tree kernels until the oracle shows a promising
   accepted-length ceiling. If the oracle stays weak, stop at diagnostics.
4. If the oracle remains promising, add branch-local exact target state so
   siblings can be verified without corrupting the live raw ring, compressor
   frontier, indexer frontier, newly emitted compressed cache rows, and
   per-branch counters.
5. Only after the oracle is hash-stable, batch target rows by tree depth and
   commit the longest verified prefix. This is where the depth-3 verifier
   ceiling can translate into real generation TPS.

The next useful engineering slice should therefore be deliberately scoped:

1. Keep the promoted N=2 path as the default strict path and add any tree work
   behind a new diagnostic env flag. Do not replace the default until it passes
   the q4 oracle and a 5-run hash-identical benchmark gate.
2. Start with a diagnostic top-2/top-4 depth-3 tree oracle, then a branch
   verifier only if the oracle shows enough accepted-length ceiling. The current
   row-batch verifier (`metal_graph_verify_suffix_n2_split_head()` via
   `metal_graph_encode_layer_batch()`) is still one linear suffix with one live
   raw/compressor/indexer frontier; it cannot represent sibling target futures
   just by adding more rows.
3. Add explicit branch-local target state: at minimum per-branch raw-row
   scratch, attention compressor frontier counters/state, indexer frontier
   counters/state, and newly emitted compressed attention/indexer cache rows.
   The existing `spec_frontier_snapshot()`,
   `spec_frontier_restore()`, `spec_frontier_commit_prefix1()`, and
   `spec_frontier_commit_prefix2()` are commit/rollback tools for one path, not
   enough storage for concurrent siblings.
4. Verify siblings by tree depth, then commit the longest exact prefix through a
   prefix-k commit path. A first speed target is not `1.25x`; it is simply to
   beat the promoted N=2 exact median while keeping the baseline hash. If this
   cannot beat the current exact `34.6-34.7 TPS` cluster, the tree route is not
   ready for a larger optimization pass.

If those larger changes are out of scope, the current evidence says the
promoted N=2 exact path is the best conservative checkpoint: it preserves the
baseline stream, has useful diagnostics, and should not get more linear tuning
unless that work is needed for cleanup.

## Next Exact Tree Kernel Slice

The next implementation should be a deliberately small exact tree verifier
slice for static `4x2x2`, not a new policy experiment. The current diagnostic
harness to reuse is `DS4_MTP_TREE_DEPTH_BATCH_PROBE=4x2x2`; the hot function to
replace incrementally is `ds4_mtp_tree_decode_rows_command_batch()`. Today that
function restores a branch slot and then calls `metal_graph_encode_decode_layer()`
once per row. This is exact, but it is still serial decode work inside one
command buffer.

The first row-parallel target should verify one tree depth at a time with
same-position branch semantics:

1. Inputs per row: token id, absolute position, parent branch slot, destination
   branch slot, compact raw window, raw capacity, and branch-local compressed
   cache-row base.
2. State per row: raw ring window, attention compressor frontier state and
   counters, indexer frontier state and counters, newly emitted compressed
   attention/indexer rows, and HC residual state.
3. Output per row: updated branch slot state plus one logits row for the target
   model top-1 check.

The linear suffix verifier cannot supply this contract. Its rows are positions
`start`, `start+1`, `start+2`, ... in one shared future, while tree siblings
need rows that may share the same absolute position but have different token KV
and different branch-local future state.

A safe implementation order is:

1. Add an opt-in `DS4_MTP_TREE_ROW_KERNEL_PROBE=4x2x2` path that only replaces
   the root-sibling depth first.
2. Keep compressor/indexer updates exact, even if the first implementation
   falls back to row-local update kernels there. Do not allow the live linear
   caches to be mutated by sibling rows.
3. Add same-position raw/mixed attention row kernels that read common prefix raw
   rows plus the row's own current KV, instead of reading other sibling rows as
   previous tokens.
4. Extend the same path to depth 2 and depth 3 only after root siblings are
   full-logit identical across repeated q4 oracle positions.
5. Promote nothing until repeated q4 guards show `mismatches=0`, `max_delta=0`
   for root, grandchild, full-tree, and depth-batch probes, followed by a 5-run
   hash-identical benchmark.

The first performance gate should be modest: beat the current exact N=2 cluster
and the serial `4x2x2` replay diagnostic. Parity with baseline is the next gate.
The credible path toward `1.25x` only exists if the exact row-parallel tree
verifier can turn the observed `4x2x2`/dynamic-tree accepted-length ceiling into
real target rows per pass without mixing sibling state.

`DS4_MTP_TREE_ROW_KERNEL_PROBE=<shape>` now exists as the first gated entry
point for that work. It currently exercises only depth 0/root siblings and
prints `serial_fallback=1`: rows have the correct same-position tree contract,
but execution still routes through the exact serial row fallback. This is
intentional scaffolding for the next Metal slice. The probe compares full logits
against normal sequential decode and reports top/logit mismatches, max delta,
state/raw copy costs, and the row fallback profile. A useful q4 guard is:

```sh
DS4_MTP_TREE_ROW_KERNEL_PROBE=4x2x2 \
DS4_MTP_TREE_ROW_KERNEL_PROBE_STEPS=8 \
./ds4_test --mtp-oracle
```

The first real kernel promotion criterion is still exactness, not speed:
`top_mismatches=0`, `logit_mismatches=0`, and `max_delta=0` across repeated q4
oracle positions before extending the path to depth 1 or depth 2.

The initial q4 guard for that entry point passed:
`/tmp/ds4-mtp-tree-row-kernel-probe-20260510-162015.{out,err}` ran
`DS4_MTP_TREE_ROW_KERNEL_PROBE=4x2x2
DS4_MTP_TREE_ROW_KERNEL_PROBE_STEPS=8 ./ds4_test --mtp-oracle` on Studio.
All 8 root-sibling checks had `top_mismatches=0`, `logit_mismatches=0`, and
`max_delta=0`. The timing profile stayed in the expected diagnostic shape:
`serial_fallback=1`, one row-depth call per oracle position, root rows
`4`, and row fallback cost around `116-128 ms`, dominated by layer execution
around `108-119 ms`. This confirms the branch-local same-position contract is
ready for a real root-depth Metal row kernel, but it does not claim a speedup
yet.

`DS4_MTP_TREE_ROW_LAYER_PROFILE=1` adds a scoped row-fallback profile that
ends the command buffer around each tree row/layer and buckets only the
diagnostic row fallback, avoiding the noisy global decode-stage profiler. The
Studio q4 guard
`/tmp/ds4-mtp-tree-row-layer-profile-20260510-162843.{out,err}` remained exact
for `4x2x2` root siblings (`top_mismatches=0`, `logit_mismatches=0`,
`max_delta=0`) and reported the useful cost shape for four root rows:

| Bucket | Layer rows | Encode | Execute |
| --- | ---: | ---: | ---: |
| raw (`ratio=0`) | 8 | ~0.4 ms | ~6.6-6.8 ms |
| ratio-4 compressed | 84 | ~4.8 ms | ~74-75 ms |
| ratio-128 compressed | 80 | ~4.9 ms | ~67-68 ms |

The profiling mode itself is slower because it deliberately splits command
buffers per layer, so its absolute row time is not a throughput estimate. The
bucket share is the important result: a raw-only root-row kernel would mostly
be a correctness exercise. The performance-critical exact tree kernel must
parallelize branch-local compressed layers as well, especially ratio-4
compressor/indexer/attention and ratio-128 compressor/attention, while
preserving per-branch frontier state.

The first non-speed tree-kernel prerequisite is now in place:
`ds4_metal_rope_tail_tensor_step()` exposes the existing Metal RoPE position
stride so a future sibling-row kernel can apply RoPE with `pos_step=0` for
same-position branch alternatives. The normal public `ds4_metal_rope_tail_tensor()`
still uses `pos_step=1`, so existing decode/prefill behavior is unchanged.
Local `./ds4_test --metal-kernels` covers the new contract by comparing a
same-position batch against serial one-row RoPE calls. This does not change the
q4 TPS result; it only removes one small correctness prerequisite before a real
branch-local compressed-layer row kernel.

A narrower attempt to batch only the tree-row output head was tested and not
kept. With `DS4_MTP_TREE_ROW_BATCH_HEAD=1`, the q4 row probe kept top-1
identical but produced full-logit deltas around `1e-5` on every row
(`/tmp/ds4-mtp-tree-row-batch-head-20260510-163737.{out,err}`). Since the
tree path is gated on full-logit exactness before performance, this shortcut is
not promotable. The serial output-head path remains in the row fallback.

The row fallback now does promote one very small exact cleanup: sibling token
embedding is batched when a tree depth has more than one row. The paired q4
probe `/tmp/ds4-mtp-tree-row-embed-pair-20260510-164205.{default,batch}.{out,err}`
showed full-logit exactness stayed clean (`top_mismatches=0`,
`logit_mismatches=0`, `max_delta=0`) and reduced the four-row embedding bucket
from about `0.815-0.830 ms` to `0.223-0.234 ms`. Total row time did not move
materially because exact layer execution stayed around `108-109 ms`, so this
is only diagnostic cleanup before real compressed-layer row kernels.

An attempted raw-layer-only row batch was also tested and rejected. Root-row
probes stayed top-1 exact, but full tree depth-batch probes diverged in full
logits on every leaf even after restricting batching to distinct raw-cache
windows (`/tmp/ds4-mtp-tree-depth-raw-batch-unique-20260510-170318.{out,err}`:
`mismatches=16`, top IDs equal, max deltas up to about `2.81`). This confirms
that partial raw-layer batching is not a safe shortcut for the exact tree path:
branch-local raw/cache state and downstream compressed state must be advanced
under one coherent per-branch row contract. The experimental env path was not
kept.

`DS4_MTP_TREE_ROW_KERNEL_PLAN=1` now prints a code-owned contract for the next
exact tree-kernel slice. It defaults to `4x2x2`; pass another shape name as the
env value, or set `DS4_MTP_TREE_ROW_KERNEL_PLAN_DEPTH=N` to inspect a shallower
depth. The output makes the next implementation boundary explicit:

- shape widths, node count, scratch-row fit, active HC bytes, frontier bytes,
  and raw active-path bytes;
- per-depth row/layer buckets (`raw`, ratio-4, ratio-128), so the q4 row-probe
  cost can be mapped back to the exact layer mix;
- required row inputs and outputs: token, absolute position, parent/destination
  branch slots, branch-local raw window, compressed row base, HC/logits, raw
  row, frontier counters, and newly emitted compressed rows;
- exactness gates: no linear suffix-batch reuse, sibling KV visibility must be
  branch-local, same-position RoPE is required, batched output head remains
  disabled, and promotion still requires `top_mismatches=0`,
  `logit_mismatches=0`, and `max_delta=0`.

This is intentionally not a speed path. It is a guardrail for the next Metal
slice: replace `ds4_mtp_tree_decode_rows_command_batch()` one depth at a time,
starting with root siblings for `4x2x2`, while prioritizing the compressed
ratio-4 and ratio-128 layers that dominate the serial row probe.

## 2026-05-11 Heavy-Kernel Exact N=2 Pass

Starting from pushed checkpoint `bb55c09`, the next q4 pass focused only on
heavy matvec/MoE/compressor candidates for exact `--mtp --mtp-draft 2`. The
long-code benchmark prompt was:

> Write a complete Python module implementing a small asyncio task runner with
> retries, exponential backoff, cancellation, typed dataclasses, and
> unit-testable pure helper functions. Include clear docstrings and a short
> example.

The one kept change is an exact batch2 compressor F16 paired-projection kernel.
The verifier previously computed `attn/indexer_compressor_kv` and
`attn/indexer_compressor_gate` as two separate exact F16 row-pair matvecs.
`ds4_metal_matmul_f16_pair_rows_tensor()` now computes both projections for the
two verifier rows in one dispatch while preserving the same row-reduction order
for each matrix. It is gated to exact verifier rows and can be disabled with
`DS4_METAL_DISABLE_COMPRESSOR_F16_PAIR_ROWS2=1`.

Validation:

- Studio q4 `./ds4_test --mtp-oracle`: OK.
- Warm lower-bound A/B:
  `/tmp/ds4-compressor-pairrows-lb2-20260511-125243`
  - disabled: `batch2=42.194 ms`, `layers=40.925 ms`,
    `layer_dispatch=2059.4`
  - fused: `batch2=42.090 ms`, `layers=40.817 ms`,
    `layer_dispatch=1997.4`
- 5-run interleaved sustained Python/code benchmark:
  `/tmp/ds4-compressor-pairrows-prod-20260511-125416/results.csv`
  - baseline median: `34.18 TPS`
  - exact with compressor pair disabled: `35.13 TPS`
  - exact with compressor pair fused: `35.14 TPS`
  - all modes produced one hash, and both exact modes matched baseline output.

This is a real but tiny win: about `1.028x` baseline on the sustained code
prompt, and only `+0.01 TPS` over the previous exact path in that 5-run sample.
It does not reach the `1.05x` goal by itself.

Falsified heavy-kernel candidates in the same pass:

- Routed batch pair+SwiGLU: a device-barrier attempt stayed exact in the oracle
  but failed batch2 lower-bound (`top_mismatch=8`, `final_mismatch=3`) and was
  slower. A deeper register-sum Q4 rewrite failed the q4 oracle. Not kept.
- Q8 exact row-pair `nsg=8`: exact but slower
  (`batch2=44.756 ms` vs `42.284 ms`). Not kept.
- F16 exact row-pair `nr0=4` for all pair2 rows: exact but slower
  (`batch2=42.822 ms` vs `42.231 ms`). Not kept.
- Existing shared-down + HC fusion:
  exact but slower (`batch2=42.504 ms` vs `42.253 ms`). Kept disabled.
- Existing shared gate/up fusion:
  exact and reduced dispatches, but still slightly slower
  (`batch2=42.200 ms` vs `42.144 ms`). Kept disabled.

The practical heavy-kernel knob pass therefore leaves the branch with a small
compressor dispatch reduction, but the remaining gap is still dominated by the
large exact verifier layer pass. The next credible path to a larger exact gain
would need a deeper rewrite of the heavy kernels themselves, especially Q8 row
matvecs, raw/compressed attention, or routed MoE arithmetic. Small scheduling
changes around the existing row kernels are now mostly falsified on Studio q4.

## 2026-05-11 Q8 Pair2 Microkernel Diagnostic

The follow-up pass added an isolated `./ds4_test --metal-kernels` diagnostic for
`kernel_mul_mv_q8_0_f32_pair2_rows`. It builds synthetic Q8_0 weights and two
activation rows, compares the exact batch2 row-pair kernel against two serial
one-row Q8 matvecs, and optionally prints timing with:

```sh
DS4_METAL_Q8_PAIR2_BENCH_REPEATS=120 \
DS4_METAL_Q8_PAIR2_BENCH_IN=4096 \
DS4_METAL_Q8_PAIR2_BENCH_OUT=4096 \
./ds4_test --metal-kernels
```

Studio q4 validation:

- default `./ds4_test --metal-kernels`: OK.
- diagnostic `4096x4096`: `pair_ms=0.286`, `serial2_ms=0.528`,
  `speedup=1.847x`, `max_abs=0`.
- earlier sequential probes showed the same exactness and about `1.86x-2.20x`
  isolated speedup depending on shape.

Two deeper Q8 pair2 rewrites were tested and not kept:

- paired-reduction helper: exact and moved batch2 lower-bound slightly
  (`41.460 ms` to `41.224 ms`), but production was flat. Both 5-run sustained
  Python/code benchmarks measured exact median `36.75 TPS`, baseline median
  about `35.30 TPS`, and strict hash identity (`exact_vs_baseline=1.041`).
- `nr0=4` row-pair specialization: exact, but slower in isolated Studio
  probes (`4096x4096` `0.296 ms` vs `0.271 ms`; `7168x8192` `0.306 ms` vs
  `0.301 ms`).
- char4 Q-value vector loads with scalar add order preserved: exact in
  `./ds4_test --metal-kernels`, but the batch2 lower-bound regressed slightly
  (`41.526 ms` vs the `41.460 ms` restored control), so it was reverted without
  a production run.

The current Q8 pair2 kernel is therefore not the missing 1.05x lever. It is
already a strong isolated win over two serial Q8 rows, but production remains
limited by the full exact verifier layer stack rather than this reduction path
alone.

The same checkpoint did reach the sustained-code stop condition on a longer
256-token Python/code prompt:

```text
prompt: Write a complete Python module that reads CSV rows, groups them by user
id, computes per-user count, sum, average, min, and max, includes a small
argparse command line interface, and includes concise unit tests for the pure
helper functions.
csv: /tmp/ds4-current-code-256-prod.csv
baseline median: 34.81 TPS, hashes=1
disabled median: 34.80 TPS, hashes=1, hash_matches_baseline=1
exact median: 36.72 TPS, hashes=1, hash_matches_baseline=1
exact_vs_baseline: 1.055
speed median: 38.70 TPS, hashes=1
speed_vs_baseline: 1.112
```

This satisfies the current exact-MTP goal for sustained code generation:
`--mtp --mtp-draft 2` is repeatably above baseline over 5 interleaved Studio q4
runs with no output drift. The credible path toward a larger margin remains
heavy-layer work, especially routed MoE and attention/output kernels, but the
branch now has a hash-identical exact speedup on the target workload.

## 2026-05-11 Routed MoE Tiny Pair+SwiGLU Promotion

The radical routed-MoE pass found one real correctness bug in the opt-in tiny
batch pair+SwiGLU path. For `n_tokens <= 4`, the fused pair+SwiGLU kernel wrote
the routed `mid` activation, but the host then fell through into the legacy
clamp/SwiGLU block and overwrote `mid` without the route weight. That made
`DS4_METAL_ENABLE_ROUTED_BATCH_PAIR_SWIGLU=1` fail the strict q4 batch2 gate.

After guarding the legacy activation blocks with `!use_tiny_pair_swiglu`, the
same path became exact:

```text
artifact: /tmp/ds4-radical-q4-promoted-swiglu-lb.err
batch2-lb steps: 30
failures: 0
top_mismatch: 0
final_mismatch: 0
seq2: 55.261 ms
batch2: 41.386 ms
layer_dispatch: 1988.0
```

The q4 oracle also passed without the opt-in env after promoting the fused path
to the default tiny routed-MoE verifier path. The disable escape hatch remains:
`DS4_METAL_DISABLE_ROUTED_PAIR_SWIGLU_FUSION=1`.

The final 5-run sustained Python/code benchmark with no special env measured:

```text
csv: /tmp/ds4-radical-q4-promoted-swiglu-prod.csv
baseline median: 34.58 TPS, hashes=1
disabled median: 34.61 TPS, hashes=1, hash_matches_baseline=1
exact median: 36.61 TPS, hashes=1, hash_matches_baseline=1
exact_vs_baseline: 1.059
speed median: 38.51 TPS, hashes=1
speed_vs_baseline: 1.114
```

This is a valid default exact improvement and keeps the sustained-code exact
lane above baseline, but it is not a radical ceiling change. The routed-MoE
slice removes a repeated activation dispatch and fixes the previously drifting
fused diagnostic; the remaining gap to `1.5x` still requires a larger verifier
architecture change, such as branch-local tree rows with coherent compressed
state or a much broader verifier-layer fusion.

## 2026-05-11 Sustained-Code Tree Lower Bound

The same sustained Python/code prompt was also rerun through the existing exact
`4x2x2` depth-batched tree probe after the routed-MoE promotion:

```sh
DS4_MTP_TREE_DEPTH_BATCH_PROBE=4x2x2 \
DS4_MTP_TREE_PROBE_STEPS=5 \
DS4_MTP_NO_ADAPTIVE=1 \
DS4_MTP_NO_TARGET_MARGIN_SKIP=1 \
./ds4 ... --mtp ... --mtp-draft 2 --temp 0 --nothink -n 96
```

Artifact: `/tmp/ds4-radical-tree-code-lb.err`.

The probe stayed exact for all five sampled positions:

```text
branches: 16
mismatches: 0
max_delta: 0
branch verifier range: 768.132-805.891 ms
normal serial replay range: 1309.477-1323.608 ms
MTP tree build range: 23.392-24.091 ms
```

But the containment ceiling on this code sample was still too low for that cost:

```text
shape 4x2x2 avg_accept_len: 2.12, avg_nodes: 28.0
shape 4x4x2 avg_accept_len: 2.21, avg_nodes: 52.0
dynamic depth>=2:2_else4 avg_accept_len: 2.19, avg_nodes: 41.2
full top8 avg_accept_len: 2.60, avg_nodes: 584.0
```

This falsifies the current depth-batched tree implementation as a near-term
speed path for sustained code generation. It is exact and faster than fully
serial branch replay, but it is still orders of magnitude above the cost needed
to amortize roughly two accepted tokens. A tree route remains plausible only as
a real row-parallel verifier-layer rewrite, with branch-local compressed
attention/indexer state advanced coherently across siblings; the existing
command-batched serial row fallback should not be wired into generation.

## 2026-05-11 Shared-Down Rows Fusion Falsifier

The disabled `DS4_MTP_VERIFY_FUSED_SHARED_DOWN_HC=1` path was revisited as a
broader verifier-layer fusion candidate. The old version was exact but slow
because the rows wrapper called the one-token fused helper once per verifier row,
forcing per-row command-buffer submission. A prototype rows API encoded the same
fused shared-down+HC kernel for all verifier rows into one command buffer.

Studio q4 gates:

```text
oracle: OK
artifact: /tmp/ds4-shared-down-rows-lb.err
batch2-lb steps: 30
failures: 0
top_mismatch: 0
final_mismatch: 0
seq2: 55.251 ms
batch2: 41.634 ms
layer_dispatch: 1988.0
```

The promoted control for the same prompt was about `41.386 ms`, so the rows
rewrite moved the lower bound in the wrong direction. It was reverted without a
production benchmark. The useful conclusion is that shared-down+HC fusion is not
blocked merely by per-row command submission; the fused kernel itself is not
better than the current exact rows path on this q4 shape.

## 2026-05-11 Attention Output + HC Rows Fusion Falsifier

A broader verifier-layer fusion was tested for the attention tail: compute only
the attention low projection, then fuse the Q8 output projection with HC post
expansion for all exact verifier rows. The prototype was gated behind
`DS4_MTP_VERIFY_FUSED_ATTN_OUT_HC=1`, preserving the default path while testing
the rewrite.

Studio q4 gates:

```text
oracle: OK
artifact: /tmp/ds4-attn-out-hc-rows-lb.err
batch2-lb steps: 30
failures: 0
top_mismatch: 0
final_mismatch: 0
seq2: 55.332 ms
batch2: 42.114 ms
layer_dispatch: 1988.0
```

A same-build no-fusion control on the same prompt measured:

```text
artifact: /tmp/ds4-attn-out-hc-control-lb.err
seq2: 55.293 ms
batch2: 41.205 ms
layer_dispatch: 1988.0
```

The fused attention-output+HC path was exact but slower by about `0.9 ms` in the
batch2 lower bound, so it was reverted without a production benchmark. This
falsifies another local tail fusion as a radical ceiling change; the remaining
high-leverage options need to change the dominant routed-MoE/attention
verification economics rather than combine already-small post-projection
helpers.

## 2026-05-11 Routed MoE Radical Falsifiers

Two routed-MoE kernel-family changes were tested after the promoted row-pair
matvec/SwiGLU path.

First, `DS4_METAL_ROUTED_BATCH_FORCE_MM_ID=1` forced tiny verifier batches onto
the expert-major MM path instead of the pair matvec path. The q4 oracle stayed
exact, but the lower-bound verifier regressed badly:

```text
artifact: /tmp/ds4-force-mm-lb.err
batch2-lb steps: 30
failures: 0
top_mismatch: 0
final_mismatch: 0
seq2: 54.834 ms
batch2: 88.242 ms
layer_dispatch: 2330.5
```

Second, a prototype split routed MoE after gate/up+SwiGLU, computed the shared
expert output, then fused Q4 routed-down `sum6` with FFN HC post. This was exact
for the strict verifier and reduced dispatch count, but it did not improve
time:

```text
artifact: /tmp/ds4-routed-down-hc-lb100.err
batch2-lb steps: 100
failures: 0
top_mismatch: 0
final_mismatch: 0
seq2: 56.789 ms
batch2: 42.727 ms
layers: 41.461 ms
layer_encode: 1.537 ms
layer_execute: 39.894 ms
layer_dispatch: 1879.1
```

The same-build control on the same sustained-code prompt measured:

```text
artifact: /tmp/ds4-routed-down-hc-control-lb100.err
seq2: 56.854 ms
batch2: 42.662 ms
layers: 41.395 ms
layer_encode: 1.495 ms
layer_execute: 39.870 ms
layer_dispatch: 1922.1
```

The fused routed-down+HC path saved about `43` dispatches per batch2 verifier
but was still slightly slower. It was reverted without production benchmarking.
This is useful evidence: the remaining MoE gap is not dominated by the final
routed-down write/read plus HC-post dispatch. A future MoE rewrite would need to
change the gate/up/down math shape itself, not just fuse the tail.

## 2026-05-11 Exact Batch-Attention Fusion Falsifier

A verifier-layer attention batching prototype tried to replace the per-row
attention fallback inside exact batch2 verification with a shared batched
attention call when verifier rows had identical compressed/indexer frontiers.
The broad version was a meaningful lower-bound speed signal, but it was not
exact:

```text
env: DS4_MTP_VERIFY_EXACT_BATCH_ATTENTION=1
artifact: /tmp/ds4-batchattn-lb.err
batch2-lb steps: 80
failures: 0
top_mismatch: 0
final_mismatch: 1
seq2: 56.552 ms
batch2: 39.826 ms
layers: 38.560 ms
layer_dispatch: 1900.7
first mismatch: pos=106 row0=38272 batch_next=201 expected=3820
```

Narrowing the prototype to indexed compressed attention only restored exactness,
but removed the useful speed signal:

```text
artifact: /tmp/ds4-batchattn-indexed-lb.err
batch2-lb steps: 80
failures: 0
top_mismatch: 0
final_mismatch: 0
seq2: 56.476 ms
batch2: 42.375 ms
layers: 41.104 ms
layer_dispatch: 1929.6
```

The required production gate also showed no ceiling movement:

```text
artifact: /tmp/ds4-batchattn-indexed-prod/results.csv
baseline_median_tps: 34.99
disabled_median_tps: 35.02
exact_median_tps: 37.03
exact_vs_baseline: 1.058
hash_matches_baseline: 1
speed_median_tps: 39.02
```

The prototype was reverted. The broad version proves there is a few-millisecond
attention-layer lower-bound opportunity if raw/non-indexed batch semantics can
be made exact, but the exact indexed-only subset is too small to materially
change sustained-code TPS.

## 2026-05-11 Routed Down Expert-Parallel Falsifier

The routed MoE stage profile showed the tiny exact batch down projection was
heavier than gate/up under synchronized stage probes. A diagnostic build forced
the batch path away from the fused Q4 `sum6` down kernel and back to the generic
expert-parallel routed matvec plus expert-sum path, to test whether M3 Ultra
preferred more parallel expert rows over the serial six-expert fused kernel.

Studio q4 gates:

```text
env: DS4_METAL_DISABLE_ROUTED_DOWN_SUM6=1
oracle: OK
artifact: /tmp/ds4-downsum-disable-lb.err
batch2-lb steps: 80
failures: 0
top_mismatch: 0
final_mismatch: 0
seq2: 56.575 ms
batch2: 44.452 ms
layers: 43.189 ms
layer_dispatch: 2156.2
```

This was slower than the promoted direct `sum6` path, which stays in the
`~41-42 ms` batch2 lower-bound range with fewer dispatches. The diagnostic hook
was reverted. The result falsifies the simple expert-parallel down rewrite: a
useful routed-MoE radical rewrite would need to improve the fused down math
itself, not replace it with the older split expert path.

## 2026-05-11 Pair-SwiGLU Dead-Store Probe

The routed pair-SwiGLU kernels were also tested with the obvious dead
intermediate writes guarded off in the fused path. This remained exact, but did
not improve the verifier lower bound:

```text
artifact: /tmp/ds4-pairstore-lb.err
oracle: OK
batch2-lb steps: 80
failures: 0
top_mismatch: 0
final_mismatch: 0
seq2: 56.536 ms
batch2: 42.434 ms
layers: 41.166 ms
layer_dispatch: 1929.6
```

The q4 reason is structural: the existing Q4 pair-SwiGLU kernel gets gate/up by
calling the normal Q4 matvec helper, which writes those rows before the fused
activation can read them. Guarding only the later redundant writes is too small
to matter. A real MoE rewrite would need a new Q4 matvec primitive that returns
gate/up reductions directly to the activation without first materializing the
full gate/up rows.

## 2026-05-11 True Fused Q4 Pair-SwiGLU Falsifier

The next routed-MoE attempt replaced the Q4 pair-SwiGLU kernel's two calls into
the generic Q4 matvec helper with a single combined Q4 loop that computed gate
and up reductions side by side, then wrote only the routed `mid` activation.
This was the intended heavy-kernel version of the dead-store probe above.

Studio q4 gates:

```text
artifact: /tmp/ds4-q4truefuse-lb.err
metal-kernels: OK
oracle: OK
batch2-lb steps: 80
failures: 0
top_mismatch: 0
final_mismatch: 0
seq2: 58.357 ms
batch2: 43.387 ms
layers: 42.130 ms
layer_dispatch: 1976.9
```

The rewrite preserved exactness but was slower than the promoted Q4 pair helper
path. The likely cause is register pressure or instruction-cache pressure from
carrying both Q4 reductions in one kernel; avoiding gate/up materialization did
not compensate. The prototype was reverted. This falsifies the most direct
single-kernel routed gate/up rewrite for q4.

## 2026-05-11 Exact Batch-Attention Promotion

The broad exact batch-attention path was retried after the later verifier
kernel promotions. The strict verifier now keeps exact row-preserving state
updates, but allows the attention stage itself to use the existing raw/mixed
batch attention kernels. This is the same high-upside family that previously
showed a lower-bound win but hit a final-token mismatch. In the current branch,
the mismatch did not reproduce across the q4 oracle and two focused
lower-bound prompts.

The promoted path is enabled by default for exact verifier batches. The escape
hatch is:

```sh
DS4_METAL_DISABLE_EXACT_BATCH_ATTENTION=1
```

Studio q4 validation:

```text
oracle: OK
artifact: /tmp/ds4-batchattn-promoted-lb.err
batch2-lb steps: 65
failures: 0
top_mismatch: 0
final_mismatch: 0
seq2: 56.539 ms
batch2: 39.997 ms
layers: 38.732 ms
layer_dispatch: 1878.0
```

The same-build escape-hatch control showed the actual A/B:

```text
artifact: /tmp/ds4-batchattn-disable-lb.err
batch2-lb steps: 65
failures: 0
top_mismatch: 0
final_mismatch: 0
seq2: 56.605 ms
batch2: 42.310 ms
layers: 41.030 ms
layer_dispatch: 1921.4
```

A speculative-decoding explanation prompt, chosen because the old prototype
had failed in this area, also stayed exact over 100 lower-bound samples:

```text
artifact: /tmp/ds4-batchattn-specdecode-lb.err
batch2-lb steps: 100
failures: 0
top_mismatch: 0
final_mismatch: 0
seq2: 56.736 ms
batch2: 39.625 ms
layers: 38.360 ms
layer_dispatch: 1851.1
```

The no-env sustained Python/code production gate is the strongest exact result
so far for the current q4 branch:

```text
artifact: /tmp/ds4-batchattn-promoted-prod-20260511-211012.csv
baseline median: 34.71 TPS, hashes=1
disabled/no-open median: 34.71 TPS, hashes=1, hash_matches_baseline=1
exact median: 37.93 TPS, hashes=1, hash_matches_baseline=1
exact_vs_baseline: 1.093
speed median: 38.70 TPS
speed_vs_baseline: 1.115
```

This promotes exact batch attention as a real default verifier improvement. It
does not change the larger conclusion about `1.5x`: exact N=2 is now clearly
above baseline on sustained code generation, but the remaining gap to a 50%
gain still needs a bigger verifier architecture change or much deeper layer
fusion.
