# Packaging SA3 into mira — embed Python, or port to C++?

Findings from the 2026-09-13 session. Everything under "Measured" was checked against the
source and the files on disk on that date. Everything under "Options" is judgement with
**rough** effort figures — not measurements.

Companion to [SA3-INFERENCE-AND-TRAINING.md](SA3-INFERENCE-AND-TRAINING.md), which proposed
the C++ port as Tier 2. **This document corrects two of that document's numbers** (see §5)
and reaches a different recommendation.

---

## The question this answers

> mira should bundle SA3 inference so it ships as one app, with a real generate window and
> drag-out to the DAW. Do we port the model to C++, or embed a Python runtime?

Short answer: **embed Python.** The port does not deliver the packaging win it appears to,
because Python is required locally for training prep regardless.

---

## Measured

### 1. Weights dominate the bundle; the runtime does not

Medium inference (`medium` DiT + `same-l` decoder + T5Gemma):

| Component | Size |
|---|---|
| `dit_medium_f16.npz` | 2.91 GB |
| `same_l_decoder_f32.npz` | 1.70 GB |
| `t5gemma_f16.npz` | 0.57 GB |
| **Weights subtotal** | **4.8 GB** |
| `libmlx.dylib` 21 MB + `mlx.metallib` 136 MB | **157 MB** |
| Python interpreter + numpy + stdlib | **~90 MB** |

MLX ships its Metal kernels as a 136 MB `.metallib` whether it is driven from C++ or from
Python. **The C++ port removes ~90 MB from a ~5 GB payload — under 2%.**

Packaging size is therefore *not* an argument for the port.

### 2. The port does not remove Python from mira

The working pipeline is: mira captions → **MLX pre-encode on the Mac** → LoRA training on a
remote GPU. That middle step is `scripts/pre_encode_mlx.py` — MLX Python, run locally.

To actually delete Python, the encode path would have to be ported too:

| File | Lines |
|---|---|
| `models/defs/audio_encoding.py` | 287 |
| `models/defs/same_l_encoder.py` | 146 |
| `models/defs/latent_dataset.py` | 388 |
| **Encode subtotal** | **~820** |

on top of the inference port. Porting inference alone still leaves a Python dependency for
every training run — so it buys nothing for packaging.

**Corollary: embedding Python serves the caption → encode → train path and the inference
path with one mechanism.** That is the main reason to prefer it.

### 3. Model load is 44 s, and it is per-process

Measured locally, medium + same-l, 30 s of audio, 8 steps:

```
DiT load    44.4 s
sample       4.6 s   (575 ms/step)
decode       2.6 s
total       53.2 s wall     peak RAM 5.68 GB
```

A design that shells out per generation pays 44 s on **every** click. This is the single
biggest determinant of whether the mira window feels better or worse than the gradio page.

### 4. MLX's C++ API is real, but its loader does not read `.npz`

`libmlx.dylib`, headers and a CMake package all ship inside the MLX wheel. `include/mlx/io.h`
declares `load` (`.npy`), `load_safetensors` and `load_gguf` — **not `.npz`**, which is the
format every SA3 weight file uses. A one-time conversion is required before any C++ work
begins.

### 5. Corrections to SA3-INFERENCE-AND-TRAINING.md

That document's Tier 2 estimate is for the **sm-music** path. The model actually in use is
**medium**, which resolves to different files (`sa3_mlx.py:83-99`):

| | doc (sm-music) | actual (medium) |
|---|---|---|
| DiT | `dit_mlx.py` 369 | `dit_mlx_medium.py` **485** |
| Decoder | `same_s_decoder.py` 294 | `same_l_decoder.py` **345** |
| T5Gemma | `t5gemma_mlx.py` 313 | 313 |
| Pipeline | `sa3_pipeline.py` 207 | 207 |
| LoRA merge | *"can be skipped"* | `lora_merge.py` **823 — cannot be skipped** |
| **Total** | **1,180** | **2,173** |

The second correction matters more than the first. The doc says `lora_merge.py` can be
skipped by merging the LoRA into the base weights offline. **That removes the runtime
strength slider and multi-slot A/B** — the features that distinguish this from Stability's
commercial plugin, which has no LoRA support at all. Runtime LoRA is the product; it has to
be ported.

---

## Options

### A. Shell out to a venv on disk (status quo) — works, fragile

What `generate.sh` does today. Zero packaging work.

- Depends on a venv at a path, which **has already broken once**: `uv run` re-synced
  underfit's venv, downgraded safetensors to 0.7.0, and killed training at model build
  (see COLAB-TRAINING.md).
- Not distributable to anyone else.

### B. Embed a relocatable Python in the app bundle ⭐ recommended

A standalone CPython (e.g. astral's `python-build-standalone`) plus MLX and the SA3 scripts,
shipped read-only inside `mira.app`.

- **Rough effort: days.**
- Kills the whole fragility class in §A — nothing to activate, nothing to re-resolve.
- Covers **both** inference and the pre-encode step (§2).
- **Keeps mira on upstream.** Stability ships a fix, you drop the script in. A port forks
  mira from their codebase permanently and makes us the maintainer of 2,173 lines of
  someone else's numerics.
- Reversible: the process seam is identical for a future C++ port, so the mira UI is built
  once either way.

**Required design element — a persistent worker.** Do not spawn a process per generation
(§3). Run one long-lived Python process that loads the model once and accepts JSON commands
on stdin, returning output paths. ~100 lines. Turns 44 s-every-click into 44 s once, then
~10 s per generation.

**~~Main risk: code signing and notarization.~~ Tested 2026-09-13 — it works.**
[spike/06_embedded_python](../spike/06_embedded_python/) embeds python-build-standalone 3.11
+ MLX in a 235 MB `.app`, signs it with the hardened runtime, and runs MLX on the GPU
(`Device(gpu, 0)`) as a child process. It needs **no entitlements at all** — not `allow-jit`,
and library validation does **not** have to be disabled, because same-identity signing
satisfies it. `codesign --verify --deep --strict` passes, and the bundle still runs and
verifies after being copied to a different path.

One trap found: a bundled interpreter writes `__pycache__/*.pyc` into `Contents/Resources`
on first run, **breaking its own code signature** — the app works once, then fails
verification and would be rejected by Gatekeeper after shipping. Fixed with
`PYTHONDONTWRITEBYTECODE=1`; the spike's `build.sh` guards against regressions.

Still untested: real Developer ID signing, `notarytool submit`/`stapler staple`, and
Gatekeeper acceptance of a quarantined download. `security find-identity` reports 0 valid
identities on this machine, so **an Apple Developer account ($99/yr) is a prerequisite** for
shipping to anyone else. These are submission steps, not design risks.

Other costs: ~250 MB added to the bundle (immaterial beside 4.8 GB of weights); debugging
across a process boundary is worse than in-process; and it does not solve distribution —
4.8 GB of weights still needs a first-run downloader whichever option is chosen.

### C. Port inference to C++ against libmlx — not now

- **Rough effort: a month, realistically more**, for 2,173 lines (§5).
- **No speed gain.** MLX runs the same Metal kernels from either language.
- **~2% bundle gain** (§1), and **does not remove the Python dependency** (§2).
- Failure mode is silent: a numerical port that is wrong does not crash, it sounds subtly
  off. Stability shipped `parity_forward_torch.py` / `parity_forward_mlx.py` to prove their
  own torch→MLX port; an equivalent harness would be mandatory here.
- Revisit only if upstream stops moving, or if local training is ruled out permanently.

---

## Recommendation

1. ~~**Spike notarization** of embedded Python + MLX — the only genuine unknown.~~
   **Done 2026-09-13, PASSED** — [spike/06_embedded_python](../spike/06_embedded_python/).
2. **Write the persistent worker** (~100 lines), the seam both options share.
3. **Build the mira generate window** against it: prompt prefilled from the selected file's
   caption, LoRA slots with strength, seed, seconds, steps, `--init-audio` from the
   selection, and JUCE drag-out to the DAW.
   - Every control gets help text. `sa3_gradio.py` has **zero** `info=` parameters, which is
     why the existing page is opaque to anyone who did not build it.
   - Drag-out is the capability a browser structurally cannot provide, and is the reason to
     build this rather than restyle the gradio page.
4. **Then** `mira train --target sa3`, which reuses the same worker for pre-encoding.
5. **Only then** reconsider option C.

## Open items

- ~~Notarization of a bundled interpreter is unverified.~~ Signing and hardened-runtime
  execution verified 2026-09-13 (spike 06). The notarisation *submission* remains unverified
  and is blocked on an Apple Developer account, not on any technical question.
- The 4.8 GB weight download has no design yet. It is the real barrier to "one app someone
  else can install", and neither B nor C addresses it.
- ~~`SA3-INFERENCE-AND-TRAINING.md` still carries the superseded figures corrected in §5.~~
  Patched 2026-09-13: its Tier 2 table now shows the medium figures, with the sm-music
  originals kept in a collapsed block for the record.
