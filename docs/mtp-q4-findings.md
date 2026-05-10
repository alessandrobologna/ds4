# MTP q4 Findings on studio.local

Date: 2026-05-10

Host: `studio.local` (M3 Ultra, q4 base model)

Model paths:

- Base: `/Users/studio/git/antirez/ds4/ds4flash.gguf`
- MTP: `/Users/studio/git/antirez/ds4/gguf/DeepSeek-V4-Flash-MTP-Q4K-Q8_0-F32.gguf`

Prompt:

```text
Write a concise technical explanation of how Redis Streams support consumer groups, pending entries, and message acknowledgement.
```

## Current Result

Exact row-preserving N=2 verification is hash-identical, but still slower than
baseline. The best single-path exact policy tested so far is confidence-gated
N=2 with adaptive cooldown:

```sh
DS4_MTP_ADAPTIVE=1 DS4_MTP_ADAPTIVE_SKIP=10 \
  tools/mtp_benchmark.sh --runs 8 --tokens 128 --draft 2 --margin 4 ...
```

Rotated interleaved medians:

| Mode | Median generation TPS | Output |
| --- | ---: | --- |
| baseline | 34.81 | baseline hash |
| MTP loaded, disabled | 34.275 | identical |
| exact MTP | 34.14 | identical |
| `--mtp-speed` | 34.46 | drifted |

So the current exact path is close to the MTP-loaded disabled path, but it has
not reached baseline parity over repeated interleaved runs.

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

## Tree Oracle

`DS4_MTP_TREE_ORACLE=1 DS4_MTP_TREE_ORACLE_DEPTH=6` shows the target greedy path
often appears in wider MTP alternatives:

| MTP alternatives | Average contained path length |
| ---: | ---: |
| top-1 | 1.33 |
| top-2 | 2.00 |
| top-4 | 2.69 |
| top-8 | 3.37 |

This makes a real exact tree verifier the credible path toward a larger gain.
Single-path N=2 needs roughly another 10 ms per successful verifier call to
break even on this q4 setup; a tree verifier can instead target longer accepted
prefixes, but requires tree-aware target attention/state handling rather than
the current linear-suffix verifier.

## Practical Conclusion

The current exact linear MTP path is correctness-preserving and close to
baseline, but not yet faster than baseline on q4. Further work should prioritize
an exact tree verifier with prefix-k commit support, or a substantial N=2 kernel
cut. Margin/adaptive policies help avoid losing badly, but mostly by skipping
speculation rather than making speculation profitable.
