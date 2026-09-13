# TRAINING — the SA3 LoRA pipeline, end to end

**Entry point for this whole area.** Written 2026-09-13, after the first working run.
Read this first; the other docs are detail.

| Doc | What it is |
|---|---|
| **TRAINING.md** (this) | the map — platforms, costs, decisions, status |
| [COLAB-TRAINING.md](COLAB-TRAINING.md) | every Colab bug found, with code references |
| [COLAB-CLI-TUTORIAL.md](COLAB-CLI-TUTORIAL.md) | driving Colab from a terminal |
| [RUNBOOK-dune-lora.md](RUNBOOK-dune-lora.md) | the original local-Mac run |
| [SA3-INFERENCE-AND-TRAINING.md](SA3-INFERENCE-AND-TRAINING.md) | mira/underfit/SA3 architecture options |

---

## 1. Status — the pipeline works

**Proven on 2026-09-13.** A LoRA trained from mira's captions responds to prompts:

- 38 Dune OST tracks → mira captions → MLX pre-encode (Mac) → torch training (Colab L4)
- At **step 500**, demos matched their prompts. Different prompts gave different,
  appropriate audio — so the individual tag dials work, not just the trigger token.
- Checkpoint: `dune-zvq-medium-03-step=500-epoch=49.safetensors` (36.5 MB)

**The open question this answered:** do mira's captions train a usable LoRA? Yes.

**Still unknown:** what a *fully* trained run sounds like (2500 steps), and where the
overfitting elbow is on a 38-track set.

### The dataset

| | |
|---|---|
| Source | Dune OST, 38 tracks, 4.21 h |
| Captions | mira, `--trigger zvq --emit-sidecar` |
| Latents | SAME-L (sa3-medium), 125 MB zip, captions embedded |
| Trigger | `zvq` |
| On Drive | `Colab Notebooks/dune-ost-latents-same-l.zip` |

---

## 2. Where to run it

Three places, each with a real role. **This is the key decision table.**

| | Mac M1 Pro 16 GB | Colab Pro | RunPod |
|---|---|---|---|
| **Train sm-music** | ✅ w/ `--grad-checkpoint` | ✅ | ✅ |
| **Train medium** | ❌ OOM | ✅ | ✅ |
| **Inference (any)** | ✅ comfortable | ✅ | ✅ |
| Storage survives | ✅ | ❌ wiped | ✅ network volume |
| Cost | £0 | $11.79/mo, 100 units | ~$0.07/GB/mo + hourly |
| Setup pain | done | done | one script |

### The architecture that fell out of this

> **Train remote. Generate local.**

Medium *inference* needs only ~5 GB and runs fine on the 16 GB Mac
(repo benchmarks: 3.8 GB at 10 s, 5.2 GB at 120 s, on an **8 GB** M1). Only *training*
doesn't fit. And `lora_merge.py` reads underfit/PEFT torch safetensors directly — no
conversion — so a Colab/RunPod-trained LoRA drops straight into local MLX inference.

### What 16 GB actually measured

| Config | Result |
|---|---|
| medium @ 2048 crop | OOM |
| sm-music @ 1300, no grad-checkpoint | OOM |
| **sm-music @ 1300 + `--grad-checkpoint`** | works — **10.99 GB peak**, 2.29 s/step |

`NOTES.md` §3 said medium training RAM was unpublished. Now measured: **it does not fit.**

Note `--grad-checkpoint` is **CLI-only** — underfit's dashboard never passes it. Same for
the torch trainer's `--base_precision bf16`. The UI hides the memory levers.

### Measured speed — medium, 2048 crop, L4

| Batch | s/step | per sample | 10,000 samples |
|---|---|---|---|
| 1 | 2.29 | 2.29 s | ~6.4 h |
| **4** | **6.74** | **1.69 s** | **~4.7 h** |

Batch 4 is ~26% faster per sample — real but less than hoped. VRAM at batch 1 was 6.9 GB
of 22.5, so the GPU was badly underfed; it was less underfed than that suggested.

**Batch 4 x 2500 steps = batch 1 x 10,000 steps.** Same samples seen. Leaving steps at
10,000 with batch 4 would be 4x the training, not faster.

---

## 3. Colab Pro — what it is and isn't

$11.79/mo, **100 compute units**, not unlimited hours. Units burn faster on bigger GPUs;
start on L4, not A100.

**It does not guarantee a machine.** Pro buys faster GPUs and longer sessions. Runtimes are
still recycled — idle tab, network blip, or Google reclaiming capacity. **This cost one run
(~45 min of GPU) on day one.**

**Mitigation: [SA3-LoRA-Training-v3-autosave.ipynb](https://colab.research.google.com/drive/1WheOmsnKFlzqCw-i8voNe7mn7s1nybMQ)**
— cell 10 starts a background watcher that copies every checkpoint and demo to Drive within
60 s (only once the file size stops changing, so never a half-written file). With that, a
recycled runtime costs ~10 min of re-setup instead of hours of training.

**Use the v3 notebook. Run cell 10 before launching.**

> Colab also re-downloads the 24 GB of weights every session. That's deliberate — VM local
> disk reads at ~500 MB/s vs Drive's ~30 MB/s — but it's 5 min of every session.

---

## 4. RunPod — the persistent option

**Not chosen because it's cheaper. Chosen because storage survives.** That is the single
feature Colab lacks, and the one that bit us.

### Costs (from RunPod's pricing page, 2026-09-13 — re-check before committing)

| | |
|---|---|
| **Network Storage** (Standard, <1 TB) | **$0.07/GB/mo** |
| 50 GB volume (24 GB models + latents + checkpoints + venvs) | **$3.50/mo** |
| 24 GB GPU — L4 / A5000 / 3090 | $0.69/hr |
| 16 GB GPU — A4000 / A4500 | $0.58/hr ← **also enough**, the run peaked ~7 GB |
| 24 GB 4090 | $1.10/hr |
| 48 GB A6000 / A40 | $1.22/hr |

A 2500-step run took 4.7 h on Colab's L4; a 3090 should do it in **~3 h ≈ $2**.

> ⚠️ **Use Network Storage ($0.07/GB/mo), not Volume Disk.** Volume Disk bills **$0.20/GB/mo
> while idle** — double its running rate. That is the trap that makes "monies just fly".

**~$10/month = persistent storage + 2–3 full runs.**

### Setup — a script, not Docker

Docker means rebuilding and pushing an image on every change. A setup script run **once**
onto the network volume is simpler, and the volume is what makes it stick.

Two scripts live here:

- **`runpod-setup.sh`** — one time. uv, both repos, underfit deps, the safetensors pin, and
  the 24 GB of weights — all under `/workspace`.
- **`runpod-start.sh`** — every pod start. Dashboard on 8787 via `.venv/bin/python`.

```bash
# 1. create a Network Volume, 50 GB
# 2. deploy a pod: RunPod PyTorch template, volume mounted at /workspace, 24 GB GPU
# 3. in the pod terminal:
export HF_TOKEN=hf_...
curl -sL https://raw.githubusercontent.com/venkateshW2/Mira/main/sa3-studio/runpod-setup.sh | bash

# 4. expose HTTP port 8787 in the pod config, then every time you start a pod:
bash /workspace/runpod-start.sh
```

**Untested on RunPod** — it is today's working sequence transcribed. Expect one or two
small fixes on first run.

### Why not the others

- **Paperspace** — Pro is $8/mo but **15 GB storage**. The models alone are 24 GB. Ruled out.
- **AWS** — not cheaper, needs GPU quota tickets, bills until you stop it (EBS bills even
  when stopped), and spot instances get preempted — the exact failure we're avoiding.
- **Vast.ai** — cheapest per hour, but interruptible. Wrong for long unattended runs.
- **Kaggle** — free 30 h/week, but same ephemeral-storage problem as Colab.

---

## 5. Settings that work

### Finetune

| Field | Value |
|---|---|
| Base model | SA3-medium |
| Latent seq length | **2048** (190 s) |
| Crop mode | Random |
| LoRA type | DoRA-rows |
| Rank / Alpha | 16 / = rank |
| LR | 1e-4 |
| Batch size | **4** |
| Max steps | **2500** |
| Ckpt / Demo every | 500 / 500 |

**Why 2048, not medium's native 4096:** 4096 = 380 s, but most cues are shorter, so ~29% of
every step would be padding. 2048 fits 27 of 38 tracks fully.

| Latent seq | Duration | Files that fill it | Real audio per crop |
|---|---|---|---|
| 4096 | 380 s | 14/38 | 71% |
| **2048** | **190 s** | **27/38** | **91%** |
| 1300 | 121 s | 34/38 | 98% |

### Dataset Text Prompts — the screen that silently ruins runs

| Setting | Value |
|---|---|
| Use fixed prompt | ❌ OFF |
| Prepend to prompt | ✅ `zvq`, 80% |
| Use tags | ✅ ON |
| shuffle | ✅ ON |
| Balance bar | **Tags 100%** |

**ON — exactly 7:** `TrackType` `VocalType` `genre` `instruments` `moods` `bpm` `keyscale`

**OFF — 15:** `audio_samples` `length_seconds` `path` `prompt` `relpath` `seconds_total`
`src_relpath` `trigger` `audio_dir` `codec` `count` `max_duration` `max_samples`
`pad_modulo` `sample_rate`

Three traps on this one screen:

1. **"Use fixed prompt" is a competing source, not a modifier.** Left on at 50%, half your
   steps train on the literal text `Genre: dune-ost` instead of real captions.
2. **Pills come from the latent sidecars + `details.json`**, not mira's clean caption. Left
   on, prompts train on absolute file paths and sample counts.
3. **Clone Settings does NOT preserve tag pills** — they reset to all-on. Redo this screen
   on every cloned run.

**Preview check:** musical words only. No `/Users/...`, no `.npy`, no `6043649`.

### Demos

Preset **Four**, all **ARC**, **CFG 1**, **Steps 8**, **2048**, different seeds. Three `zvq`
prompts of varying specificity plus **one empty** — the unconditional control. If that one
starts sounding like your dataset, the LoRA is bleeding into the base model.

Never Base demos (need ~50 steps each) and never Steps 2 (renders mush, looks like a broken LoRA).

---

## 6. Reading a run

| Signal | Meaning |
|---|---|
| **Demos** | 🔴 the real test. Everything else is supporting evidence |
| Loss by Noise Level | 🟡 high-noise bands trending down = the style is being learned |
| LoRA magnitude | 🟡 drifts off start = learning. Flat = not. Spiking = LR too high |
| Smoothed loss | ⚪ nearly useless — dominated by which noise level got sampled |
| Unconditional demo | should NOT sound like your dataset |

**The best checkpoint is often not the last one.** On 38 tracks, 2500 steps is ~249 epochs;
random crop mitigates repetition but the elbow can come early. Keep all five and compare at
the same prompt and seed.

---

## 7. Local inference — the payoff

Runs on the Mac, no cloud, no units.

**UI** (LoRA strength sliders, seed, steps, audio2audio, inpainting):

```bash
cd sa3-studio/stable-audio-3/optimized/mlx
.venv/bin/python scripts/sa3_gradio.py --dit medium \
  --lora ~/Downloads/dune-zvq-medium-03-step=500-epoch=49.safetensors \
  --default-seconds 120 --default-steps 8 --no-share
```

**CLI:**

```bash
.venv/bin/python scripts/sa3_mlx.py --dit medium \
  --lora ~/Downloads/dune-zvq.safetensors --lora-strength 0.7 \
  --prompt 'zvq, TrackType: Music, Moods: dark, epic, Instruments: strings, low brass' \
  --seconds 120 --out dune-test.wav
```

Try `--lora-strength` 0.5 / 0.7 / 0.9 to hear how hard the style is applied.

---

## 8. Next

1. **Finish a full 2500-step run** — hear what fully trained sounds like, find the elbow.
2. **Compare all five checkpoints** at the same prompt and seed.
3. **Stand up RunPod** so long runs stop depending on Colab's mood.
4. **Then tune the captions.** The pipeline is proven; the next gains are in what mira
   writes, not in the training setup.

### Open items

- `keyscale` came back "D major" on 13 of 38 — key detection is the weakest mira field.
  Consider dropping that pill and re-comparing.
- 8 tracks were cut at the 600 s pre-encode cap (~39 min, ~15% of the set). `--max-duration 1200`
  would recover it, at untested encoder RAM.
- mira's content-type router called two full cues `stem`; their sidecars were hand-corrected.
  Re-running `mira caption --emit-sidecar` on those two undoes the fix. mira has no
  "declare this a track" command — worth adding.
