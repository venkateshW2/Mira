# SA3 — training and inference from inside mira

Findings from the 2026-09-12 session. Everything in "Verified facts" was checked against
the source on disk and is cited. Everything in "Options" is a proposal with **rough**
effort estimates — those are judgement, not measurements.

Companion to [RUNBOOK-dune-lora.md](RUNBOOK-dune-lora.md), which is the actual step-by-step
for the first training run.

---

## The question this answers

> Can mira train and run SA3 LoRAs directly, without underfit's dashboard?

Short answer: **yes to both, and less is needed than expected.** The parts that look like
they'd need building mostly already exist.

---

## Verified facts

### 1. Three separate pieces of software

| Piece | Owner | Licence | Role |
|---|---|---|---|
| **mira** | you | — | listens to audio, writes captions |
| **stable-audio-3** | Stability AI | MIT | the actual trainer *and* the sampler |
| **underfit** | Dadabots | MIT | a web dashboard that runs the trainer for you |

Both third-party licences are MIT — no obstacle to building on either, provided the
copyright notices are retained.

### 2. On the MLX path, underfit contributes **zero code to a training run**

`lora_train_mlx.py` imports nothing from underfit — see its import block
(`stable-audio-3/optimized/mlx/scripts/lora_train_mlx.py:52-95`). Everything it needs
lives in `stable-audio-3/optimized/mlx/models/defs/`: the LoRA/DoRA implementation, the
latent dataset, demos, the training loop.

Stability even **ported underfit's prompt-template logic themselves**, credited in the
docstring of `models/defs/latent_dataset.py:9`:

> `build_prompt` ports underfit's dataset_processing/prompt_templates.py

underfit's actual runtime role is to build two command lines
(`underfit/underfit/backends/mlx_engine.py:533`, `build_encode_cmd`) and serve a UI.

**Consequence:** the attribution owed for a training path that calls `lora_train_mlx.py`
directly is mostly **Stability's**, not Dadabots'. underfit's genuine contribution is the
prompt-balancing design and the dashboard itself.

### 3. The dashboard is large, and mostly not mira's domain

| File | Lines |
|---|---|
| `dashboard/server.py` | 7,992 |
| `dashboard/index.html` | 9,063 |
| **Total** | **17,055** |

~30 API endpoints. Most of it is outside mira's concerns: VRAM estimation, loss-by-timestep
charts, checkpoint browsing, demo playback, gradio proxying, model downloads, run
adoption/cloning, log streaming, rare-token lookup, LoRA seed validation.

### 4. LoRA inference already works from the command line

`scripts/sa3_mlx.py` already accepts `--lora` (repeatable) and `--lora-strength`
(`sa3_mlx.py:395`, `:410`), alongside `--init-audio` (audio2audio), `--inpaint-range`,
`--seconds`, `--steps`, `--cfg`, `--apg`, `--seed`, `--out`.

**Nothing needs building to get LoRA inference.** Only to put it behind a button.

This is the feature the commercial SA3 DAW plugin does not have.

### 5. MLX ships a C++ API

Found in the MLX venv:
- `.venv/lib/python3.11/site-packages/mlx/lib/libmlx.dylib`
- `.venv/lib/python3.11/site-packages/mlx/include/mlx/mlx.h`

So a native C++ SA3 implementation is genuinely feasible — link `libmlx`, no Python at
runtime. Caveat: MLX's C++ loader (`include/mlx/io.h:24-41`) reads `.npy`, `.safetensors`
and `.gguf` — **not `.npz`**, which is what the SA3 weights are. A one-time conversion
script fixes that (an `.npz` is just a zip of `.npy` files).

### 6. mira is designed to be trainer-agnostic

`PRD.md:695`:

> **Not SA3-only.** SA3 is the trainer already set up (`sa3-studio/`, MLX, `underfit`), but
> it **must not be the thing the schema is shaped around.** Other open-weights base models
> permit local inference and LoRA training, and they do not agree on caption format [...]
> The design requirement is therefore: **one analysis document, N renderers.**

mira is meant to also feed ACE-Step, MusicGen, Mustango and YuE. Anything built here should
keep that door open — which is why option B is `mira train --target sa3`, not `mira train`.

---

## Options — training

### A. Use underfit's dashboard (status quo)
Zero work. Works today. Gives loss curves, demo playback, checkpoint management for free.
**This is what the first Dune run uses.**

### B. `mira train --target sa3 <collection>` ⭐ agreed direction
mira selects files from its own library, writes the captions, and runs Stability's two
scripts directly (`pre_encode_mlx.py`, then `lora_train_mlx.py`). underfit drops out of the
runtime path entirely; keep its dashboard open for monitoring if you want charts.

- **Rough effort: ~1 day.** It is subprocess plumbing, not new maths.
- **The real win:** it removes the filesystem seam. Today mira writes JSON sidecars to disk
  and *hopes* a Python scanner finds them. This makes it one direct handoff.
- `--target sa3` today, `--target ace-step` later — stays consistent with `PRD.md:695`.
- Scope is explicitly **a button that trains a folder into a LoRA**. Not a dashboard.

### C. Rebuild the dashboard inside mira — rejected
17,055 lines, in C++/JUCE, most of it outside mira's domain, and it would weld mira to SA3
against `PRD.md:695`. Weeks to months for no capability gain.

---

## Options — inference

### Tier 1 — mira button runs `sa3_mlx.py` ⭐ recommended
Click generate in mira → shells out → WAV returns into mira's library → auto-analyzed →
drag into the DAW.

- **Rough effort: ~1 day**, and it shares its subprocess code with option B, so doing both
  together is cheaper than doing either alone.
- Gives you LoRA strength, audio2audio and inpainting immediately — all already flags on
  `sa3_mlx.py`.
- Catch: requires Python + the MLX venv present. Fine locally, awkward to ship to others.

### Tier 2 — native C++ inference inside mira
Port the MLX Python model definitions to C++ against `libmlx`.

What sm-music inference needs:

| File | Lines |
|---|---|
| `models/defs/dit_mlx.py` | 369 |
| `models/defs/t5gemma_mlx.py` | 313 |
| `models/defs/same_s_decoder.py` | 294 |
| `models/defs/sa3_pipeline.py` | 207 |
| **Total** | **~1,180** |

`models/defs/lora_merge.py` (823 lines) can be skipped by merging the LoRA into the base
weights offline, once, in Python.

- **Rough effort: a month, realistically more.**
- **The real risk is silent wrongness.** A numerical port doesn't crash when it's wrong —
  it just sounds subtly off. Stability shipped `scripts/parity_forward_torch.py` and
  `scripts/parity_forward_mlx.py` to prove their own torch→MLX port matched; an
  MLX-Python→C++ port needs the same harness. Precedent exists in this repo: the Phase 0
  spike matched the ONNX embedding pipeline to ~1e-4.
- **It would NOT be faster.** MLX runs the same Metal kernels either way. The gain is
  "ships as one app, no Python dependency" — a packaging win, not a capability or speed
  win.

### Tier 3 — a VST/AU plugin with LoRA support
The actual gap in the commercial SA3 plugin.

- **Rough effort: months.** A different product.
- mira already vendors JUCE 9, and the Phase 0 spike proved JUCE drag-out works, so the
  foundation is there.
- Generation runs ~0.8× realtime (sm-music: 5.0 s audio in 6.25 s wall, `NOTES.md` §1), so
  this is a **render** plugin, not a play-live one.
- 1.7 GB of weights per plugin instance is a real design problem, not a detail.

---

## Recommendation

1. **Tomorrow:** run option A and Tier-0 inference (underfit dashboard, as per the runbook).
   The open question is whether mira's captions actually train well — learn that before
   building tooling around them.
2. **Then:** B + Tier 1 together, since they share their plumbing. That is the whole
   "one button → LoRA → generate with it" loop, with underfit kept only for monitoring.
3. **Only if mira becomes something other people install:** Tier 2.
4. **Only after the captions are proven:** Tier 3.
