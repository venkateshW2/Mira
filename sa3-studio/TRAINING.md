# TRAINING — the SA3 LoRA pipeline, end to end

**Entry point for this whole area.** Written 2026-09-13 after the first working run;
rewritten 2026-09-14 when the platform moved to JarvisLabs and the first run's real
config was recovered. Read this first; the other docs are detail.

| Doc | What it is |
|---|---|
| **TRAINING.md** (this) | the map — platform, setup, settings, costs |
| [COLAB-TRAINING.md](COLAB-TRAINING.md) | every Colab bug found, with code references. Colab is gone, the trainer bugs are not |
| [COLAB-CLI-TUTORIAL.md](COLAB-CLI-TUTORIAL.md) | driving Colab from a terminal — historical |
| [RUNBOOK-dune-lora.md](RUNBOOK-dune-lora.md) | the original local-Mac run |
| [SA3-INFERENCE-AND-TRAINING.md](SA3-INFERENCE-AND-TRAINING.md) | mira/underfit/SA3 architecture options |
| [PACKAGING.md](PACKAGING.md) | shipping SA3 inside mira — embed Python vs port to C++ |

---

## 1. Status — the pipeline works

**Proven on 2026-09-13.** A LoRA trained from mira's captions responds to prompts:

- 38 Dune OST tracks → mira captions → MLX pre-encode (Mac) → torch training → local MLX inference
- Different prompts gave different, appropriate audio — so the individual tag dials
  work, not just the trigger token.
- Best checkpoint: **`dune-zvq-medium-03-step=500-epoch=49.safetensors`** (36.5 MB),
  from the **batch-4** run.

### The batch-size trap — read this before anything else

Two Dune runs exist and they are not comparable. The filenames give it away on a
38-track set:

| run | checkpoint | steps/epoch implied | batch |
|---|---|---|---|
| **run 1** (`dune-zvq-medium-03`) | `step=500-epoch=49` | 500/50 = 10 = ceil(38/4) | **4** |
| run 2 (`zvq`) | `step=500-epoch=13`, `1000/26`, `1500/39` | 38 | **1** |

Run 2's step-1500 checkpoint is *worse* than run 1's step-500 — not because it
overfit, but because it was **undertrained**. Count samples, not steps:

- run 1 @ step 500 = 500 × 4 = **2,000 samples** (≈50 epochs)
- run 2 @ step 1500 = 1500 × 1 = **1,500 samples** (39 epochs)

Run 1's "early" checkpoint had seen more data in a third of the steps.
**Train at batch 4, and read epochs, not steps.**

### The dataset

| | |
|---|---|
| Source | Dune OST, 38 tracks, 4.21 h |
| Captions | mira, `--trigger zvq --emit-sidecar` |
| Latents | SAME-L (sa3-medium), 125 MB zip, captions embedded |
| Trigger | `zvq` |

Three more sets are built and uploaded: Mad Max `xyr` (52), Dark Knight `qsk` (28),
Batman v Superman `vzx` (13).

---

## 2. Where to run it

> **Train remote. Generate local.**

Medium *inference* needs only ~5 GB and runs fine on the 16 GB Mac (repo benchmarks:
3.8 GB at 10 s, 5.2 GB at 120 s, on an **8 GB** M1). Only *training* doesn't fit.
And `lora_merge.py` reads underfit/PEFT torch safetensors directly — no conversion —
so a remotely-trained LoRA drops straight into local MLX inference.

| | Mac M1 Pro 16 GB | JarvisLabs A30 |
|---|---|---|
| **Train sm-music** | ✅ w/ `--grad-checkpoint` | ✅ |
| **Train medium** | ❌ OOM | ✅ |
| **Inference (any)** | ✅ comfortable | ✅ |
| Storage survives | ✅ | ✅ `/home` survives pause |
| Cost | ₹0 | ₹39.53/hr running, ₹467/mo parked |

**Colab Pro was cancelled on 2026-09-13** after two runs were lost to runtime
recycling (480 and 980 steps). Its trainer-level findings still apply and live in
[COLAB-TRAINING.md](COLAB-TRAINING.md) — especially §7, the tag-pill trap.

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

**Batch 4 × 2500 steps = batch 1 × 10,000 steps.** Same samples seen. Leaving steps at
10,000 with batch 4 would be 4× the training, not faster.

### VRAM — batch size barely moves it

underfit's own estimator (`_estimateTrainingVram`) and the `sa3-medium` registry give
the real shape:

```
activation_mb = act_per_latent_mb x seq_len x batch     # 0.005 x 2048 x 4 = 41 MB
total ~= base_fp16_mb + lora_mb + activation_mb         # 6670 + ~36 + 41 ~= 6.7 GB
```

**Model weights dominate; activations are noise.** Batch 4 costs ~31 MB more than
batch 1, so the measured 6.9 GB at batch 1 and the UI's ~7.0 GB at batch 4 agree.

A 16 GB card is therefore fine for medium at 2048, at any batch size that fits the
step-time budget. An earlier revision of this doc claimed batch 4 needed 15-17 GB —
that was an assumption that activations scale into gigabytes, and it is wrong.

On JarvisLabs this changes nothing in practice: the A30 24 GB is already the cheapest
GPU on offer (Rs 38.88/hr), so there is no smaller, cheaper tier to drop to.

---

## 3. JarvisLabs — the platform

**Chosen 2026-09-14** on India pricing: A30 24 GB at ₹38.88/hr against ~₹62.8/hr for
an equivalent 24 GB card from a USD-billed provider. INR billing with a GST invoice
(input credit if registered), no forex markup, and an India region — the 363 MB latent
upload took **20 seconds**.

### The machine

```
jl create --gpu A30 --region IN2 --storage 50 --http-ports 8787 -n sa3
```

| | |
|---|---|
| GPU | A30, **24 GB VRAM** (`nvidia-smi`: 24576 MiB) |
| RAM / vCPU | 64 GB / 16 — the pricing page's 112 GB is wrong, `jl gpus` is right |
| Region | IN2 |
| Type | container, `pytorch` template (not `--vm`) |
| Storage | 50 GB custom tier · **cannot be reduced after creation** |
| Ports | 8787. **8889, 6006, 7007 and 22 are reserved** by JarvisLabs |
| Cost | ₹38.88/hr GPU + ₹0.648/hr storage = **₹39.53/hr**, and ₹467/month parked |

**Why A30 over L4** — same 24 GB and cheaper (L4 is ₹41.31), and A30's HBM2 bandwidth
is ~3× L4's. Training at 2048 latent seq is bandwidth-bound. L4's 124 GB RAM is
irrelevant here.

**Why on-demand, not spot** — spot is ₹27.54 and saves ~₹34 across the whole job. An
interruption mid-run is the exact Colab failure this move was meant to end.

### `/home` is the only persistent path

`/root` and everything else is **wiped on pause or destroy**. That is why the entire
install — repos, venv, HF cache, datasets — lives under `/home/workspace`, and why
system packages can't be relied on.

### Setup, once

```bash
# local, once
uv tool install jarvislabs      # provides `jl`
jl setup                        # writes the API token to config
jl ssh-key add ~/.ssh/id_ed25519.pub -n <name>
```

> ⚠️ **Register the SSH key BEFORE `jl create`.** Keys are injected at boot only. An
> instance created first comes up with `ssh_command: ""` and is unreachable — it has to
> be destroyed and recreated. This cost one instance on 2026-09-14.

Then create the instance and run the setup script on it:

```bash
scp -F /dev/null -i ~/.ssh/id_ed25519 jarvis-setup.sh jarvis-start.sh root@<ip>:/root/
ssh -F /dev/null -i ~/.ssh/id_ed25519 root@<ip> 'umask 077; cat > /root/.hf_token' \
    < ~/.cache/huggingface/token
ssh -F /dev/null -i ~/.ssh/id_ed25519 root@<ip> \
    'nohup bash /root/run-setup.sh > /home/workspace/setup.log 2>&1 &'
```

> ⚠️ **`jl exec` and `jl ssh` fail on this Mac** — the local `~/.ssh/config` sets a
> `RemoteCommand`, and ssh then refuses with *"Cannot execute command-line and remote
> command."* Use plain `ssh -F /dev/null` as above.

The HF token goes to a 0600 file rather than an env var on the command line, so it
never lands in `ps` output. `/root` is wiped on pause, which is the right place for it.

### The two scripts

- **[`jarvis-setup.sh`](jarvis-setup.sh)** — one time. Checks VRAM ≥ 20 GB and bails
  early rather than OOM-ing hours in; installs uv, clones underfit + stable-audio-3,
  runs `install.sh --no-setup`, pins safetensors, pulls sa3-medium. Everything under
  `/home/workspace`.
- **[`jarvis-start.sh`](jarvis-start.sh)** — every `jl resume`. Reinstalls ffmpeg
  (apt lives outside `/home`, so a pause eats it — it is *optional*, without it demos
  fall back to WAV), then launches the dashboard on 8787.

Two landmines both scripts encode:

1. **Pin safetensors AFTER `install.sh`.** `uv run` re-syncs the venv to underfit's
   lockfile and downgrades safetensors to 0.7.0, which breaks transformers at model build.
2. **Launch the dashboard with `.venv/bin/python`, never `uv run`** — same reason.

### Measured, 2026-09-14

| | |
|---|---|
| Setup wall-clock | under 10 min, including the 24 GB weight pull |
| Disk after setup | **31 G used, 19 G free** of 50 G |
| Latent upload | 363 MB in **20 s** (~18 MB/s) |
| Dashboard | HTTP 200 on the port-8787 endpoint |

19 GB of headroom is enough for checkpoints at 36.5 MB each, but not for another
film's latents without checking `df` first.

### Datasets — one per LoRA, and the import trap

**underfit registers the folder you point it at as a single dataset.** Unzipping all
four films under one parent got them imported as one 131-file set (52+28+13+38) — the
dashboard showed exactly one dataset named `datasets`. Three separate LoRAs need three
separate dataset records.

Import each film's folder individually. The dashboard's Import button does this; the
API does it without clicking:

```bash
curl -s -X POST http://127.0.0.1:8787/api/datasets/import \
  -H 'Content-Type: application/json' \
  -d '{"path":"/home/workspace/datasets/MadMAx","name":"madmax-xyr",
       "mode":"preencoded_import","model":"sa3-medium"}'
```

`mode` is `preencoded_import` for latents made elsewhere (mira's MLX pre-encode), which
**symlinks** a shadow under `state/datasets/<name>/latents/<model>/` — no extra disk.
`underfit_native_import` registers in place; `bare_import` is for raw latents with no
sidecars. Delete a bad record with `POST /api/datasets/<id>/delete`.

Registered on this box:

| dataset | files |
|---|---|
| `madmax-xyr` | 52 |
| `dune-zvq` | 38 |
| `darkknight-qsk` | 28 |
| `batman-vzx` | 13 |

Check `num_files` per dataset after importing. If one set shows the sum of all of them,
you pointed at the parent.

### Pre-encoded latents have no tag pills — and that silently discards every caption

**The most dangerous trap in the whole pipeline.** The tag UI discovers keys by walking
the dataset's `input_dir` for *audio* (`server.py` `_scan_audio_tags`); `.json` files are
only used as a stem-keyed lookup against audio it finds. A latents-only dataset has no
audio, so the screen reads *"No files with tags found"*, no pills render, and the modal
posts `tag_keys: []`. Downstream `_build_tag_prompt` iterates that empty list and returns
`""` — **every prompt collapses to the trigger token and all of mira's captions are
thrown away.** Training runs, checkpoints appear, demos render. Nothing looks wrong.

Three things fix it, and all three are needed:

1. **Placeholder `.wav` files beside the sidecars.** underfit needs a recognised
   `AUDIO_EXTS` suffix and **≥ 4096 bytes**; it never reads them for training, which
   reads latents. 8 KB of silence per track is enough — 93 placeholders cost ~800 KB.
   Do **not** upload the real audio for this: 4.6 GB was uploaded on 2026-09-14 before
   this doc was re-read, then replaced with placeholders for identical results.

   ```bash
   python3 - <<'EOF'
   import wave
   from pathlib import Path
   SIL = b"\x00\x00" * 4096          # 8 KB of PCM
   for stem in Path("/home/workspace/datasets/MadMAx").glob("*.npy"):
       with wave.open(str(stem.with_suffix(".wav")), "wb") as o:
           o.setnchannels(1); o.setsampwidth(2); o.setframerate(44100)
           o.writeframes(SIL)
   EOF
   ```

2. **Delete the stale tag cache.** `state/datasets/<ds_id>_tags.json` is written on first
   view. If it was written while the scan returned 0 files it keeps serving empty results
   forever, no matter what you add. `rm state/datasets/*_tags.json`, then reload.

3. **Copy `details.json` into the shadow `latent_dir`.** The import symlinks the `.npy`
   files but not `details.json`, so the dataset flips back to `error` on every dashboard
   restart — and patching `status` in `datasets.json` appears to work, then reverts.

Verify before launching: the `/files` endpoint should report `total_files` equal to the
dataset size and the payload should carry tag keys.

```bash
curl -s http://127.0.0.1:8787/api/datasets/<id>/files \
  | python3 -c 'import json,sys; d=json.load(sys.stdin); print(d["total_files"], len({k for f in d["files"] if f.get("tags") for k in f["tags"]}))'
```

### Daily use

```bash
jl pause 505855      # stops GPU billing, /home survives, storage still bills
jl resume 505855     # then: bash /home/workspace/jarvis-start.sh
jl destroy 505855    # permanent — do this once the LoRAs are downloaded
```

Storage bills at ₹467/month whether the instance runs or is paused. For gaps of
months, `destroy` and re-run setup (~10 min) beats parking.

---

## 4. Settings that work

### Finetune

| Field | Value |
|---|---|
| Base model | SA3-medium |
| Latent seq length | **2048** (190 s) |
| Crop mode | Random |
| LoRA type | DoRA-rows |
| Rank / Alpha | 16 / = rank |
| LR | 1e-4 |
| Batch size | **4** ← the field that ruined run 2 |
| Max steps | **2500** |
| Ckpt / Demo every | **250** |

`--checkpoint-every 250`, not 500: two Colab recycles cost 480 and 980 steps. A resumed
run also needs `--lr-scheduler none`, because the LR schedule otherwise restarts from
the bottom of its warmup ramp.

**Why 2048, not medium's native 4096:** 4096 = 380 s, but most cues are shorter, so ~29%
of every step would be padding. 2048 fits 27 of 38 tracks fully.

| Latent seq | Duration | Files that fill it | Real audio per crop |
|---|---|---|---|
| 4096 | 380 s | 14/38 | 71% |
| **2048** | **190 s** | **27/38** | **91%** |
| 1300 | 121 s | 34/38 | 98% |

### Steps per film — a flat step count means different things

The good Dune checkpoint sat at **~50 epochs**. At batch 4 that is a different step
count for every set:

| film | files | steps/epoch | ~50 epochs |
|---|---|---|---|
| Mad Max `xyr` | 52 | 13 | **~650** |
| Dune `zvq` | 38 | 10 | **~500** |
| Dark Knight `qsk` | 28 | 7 | **~350** |
| Batman `vzx` | 13 | 4 | **~200** |

Checkpoint every 250 and **select the checkpoint whose `epoch=` is nearest 50** — the
filename already carries it. Treat ~50 as the centre of a bracket, not a law: 50 epochs
over 13 Batman files is not the exposure 50 over 38 Dune files was.

### Dataset Text Prompts — the screen that silently ruins runs

| Setting | Value |
|---|---|
| Use fixed prompt | ❌ OFF |
| Prepend to prompt | ✅ **the trigger** (`xyr`), 80% — not the dataset name |
| Use tags | ✅ ON |
| shuffle | ✅ ON |
| Balance bar | **Tags 100%** |

**ON — 10:** `TrackType` `VocalType` `genre` `instruments` `moods` `bpm` `keyscale`
`dynamics` `rhythm` `texture`

**OFF — 8:** `audio_samples` `length_seconds` `path` `prompt` `relpath` `seconds_total`
`src_relpath` `trigger`

`dynamics`, `rhythm` and `texture` are newer mira fields — present in the 2026-09-14
Mad Max / Dark Knight / Batman exports, absent from the older Dune set. Turning them on
is a deliberate difference from the run-1 Dune config; they are real musical descriptors,
and these are new LoRAs rather than a Dune reproduction.

Earlier revisions listed 15 OFF keys. Seven of those (`audio_dir` `codec` `count`
`max_duration` `max_samples` `pad_modulo` `sample_rate`) come from `details.json`, whose
stem matches no track, so they never reach the pills. The live set is 18 keys.

Three traps on this one screen:

1. **"Use fixed prompt" is a competing source, not a modifier.** Left on at 50%, half your
   steps train on the literal text `Genre: dune-ost` instead of real captions.
2. **Pills come from the latent sidecars + `details.json`**, not mira's clean caption. Left
   on, prompts train on absolute file paths and sample counts.
3. **Clone Settings does NOT preserve tag pills** — they reset to all-on. Redo this screen
   on every cloned run.

**Preview check:** musical words only. No `/Users/...`, no `.npy`, no `6043649`.

### Demos

Preset **Four**, all **ARC**, **CFG 1**, **Steps 8**, **2048**, different seeds. Three
trigger prompts of varying specificity plus **one empty** — the unconditional control. If
that one starts sounding like your dataset, the LoRA is bleeding into the base model.

Never Base demos (need ~50 steps each) and never Steps 2 (renders mush, looks like a broken LoRA).

### Two strategies — and we have been running a hybrid

underfit's own README recommends **20,000 steps** (*"a reasonable LoRA lands around 10k —
that's where it creatively underfits: still varied on new prompts, not yet memorising"*).
That is 769 epochs on the 52-track Mad Max set, against the ~50 this doc targets. The gap
is not a contradiction — it is a different strategy, and it hinges on **latent length**:

> **"Latent length is the underrated knob.** Lowering it to ~47 s or ~12 s (with
> random_crop on) is often the cleanest way to learn a style *without* memorisation. The
> model only ever sees patterns at that timescale and never sees full songs, so it can't
> memorise structure."

| | long-window (what we ran) | short-crop (underfit's advice) |
|---|---|---|
| Latent seq | 2048 (190 s) | 512 (~48 s) or 128 (~12 s) |
| Steps | ~500-750 | 10,000 |
| Sees | whole cues | fragments only |
| Learns | structure **and** style | style, texture, orchestration |
| Fails by | memorising cues early | never learning structure |

2048 was chosen to minimise padding — 27 of 38 Dune tracks fit *whole*. That is precisely
what lets it memorise, and it is the likeliest reason the Dune elbow appeared as early as
~50 epochs. **For score-ness — orchestration, texture, harmonic language — the short-crop
path is the better theoretical fit**, because you want the vocabulary, not the cues.

Neither has been measured against the other. That comparison is the open question.

### Batch size is a creative parameter, not just throughput

> *"batch_size=1 learns something different (focuses on one song at a time, sharper
> imprint) from batch_size=4 (averages gradients across songs, smoother fit)."*

So the Dune run 2 was **undertrained at equal step count**, not categorically wrong.
Batch 4 remains right for a smooth style LoRA; batch 1 is a legitimate different character.

### Measured on the A30 (2026-09-14)

**3.17 s/step** at 2048 / batch 4 — **2.1x faster than Colab's L4** (6.74 s/step). A
2500-step run is ~2.2 h ~= Rs 87. Short-crop runs should be materially faster per step;
read the real rate off the log in the first minute rather than trusting an estimate.

### The LR warmup is ~1000 steps long — short runs are mostly warmup

Read from a live run's config (`state/runs/<id>_model.json`):

```
AdamW lr=1e-4, betas=[0.9, 0.95], weight_decay=0.01
InverseLR: inv_gamma=1000000, power=0.5, warmup=0.995
```

`InverseLR` gives `lr = base * (1 - 0.995^(step+1)) * (1 + step/inv_gamma)^-0.5`. With
`inv_gamma` at 1e6 the decay term is ~1.0 for any run under 10k steps, so **LR is
effectively `1e-4 x (1 - 0.995^step)` — pure warmup, then flat.** Verified against the
2026-09-14 Mad Max log: step 227 -> 6.811e-5, step 468 -> 9.047e-5, both exact.

| step | % of target LR |
|---|---|
| 138 | 50% |
| 250 | **72%** |
| 459 | 90% |
| 919 | 99% |

**The first 250 steps average only ~43% of full LR.** That is a large part of why an early
checkpoint looks undertrained — it is not just fewer steps, it is weaker ones.

Consequences:

- A **2500-step run spends ~40% of itself warming up.** A 10,000-step run spends 10%.
  This is an independent argument for longer runs, separate from the crop-length one.
- **No decay to design around.** After warmup the LR sits at 1e-4 essentially forever, so
  long runs keep learning at full rate rather than annealing to nothing.
- A **resumed** run restarts `last_epoch` at 0 and re-walks the whole warmup ramp from
  zero — which is why resuming needs `--lr-scheduler none`.

### Why the loss curve looks flat — and what to read instead

Total training loss bounces without trend (0.48-0.73 across steps 325-468 on Mad Max,
no direction). **This is normal and not a fault.** Diffusion loss is dominated by which
noise level got sampled that step, so it drowns out the learning signal. Read instead:

- **Loss by Noise Level** — the per-band panel. High-noise bands trending down is the
  style being learned. The raw data is in `demos/loss_by_timestep.bin`.
- **`lora_magnitude`** — should drift steadily off its start. On Mad Max it moves
  monotonically 2287.84 -> 2287.57 over 143 steps: small in relative terms, but consistent,
  which is the point. Flat would be the alarm.
- **The demos.** Still the only real test.

### Run 2 — the short-crop experiment, ready to enter

Same Mad Max dataset, same pills, same trigger. Only these differ:

| Field | Run 1 (baseline) | **Run 2 (short-crop)** |
|---|---|---|
| Run name | `xyr` | `xyr-short` |
| Latent seq length | 2048 (190 s) | **512 (~48 s)** |
| Max steps | 2500 | **10000** |
| Ckpt every | 250 | **1000** |
| Demo every | 250 | **1000** |
| Batch size | 4 | 4 |
| Everything else | | unchanged |

Plus **a fifth demo the baseline lacks** — see below. Do not use Clone Settings without
redoing the tag pills; it resets them to all-on.

---

## 5. Reading a run

| Signal | Meaning |
|---|---|
| **Demos** | 🔴 the real test. Everything else is supporting evidence |
| Loss by Noise Level | 🟡 high-noise bands trending down = the style is being learned |
| LoRA magnitude | 🟡 drifts off start = learning. Flat = not. Spiking = LR too high |
| Smoothed loss | ⚪ nearly useless — dominated by which noise level got sampled |
| Unconditional demo | should NOT sound like your dataset |

### The stop signals underfit's author actually uses

1. **The loss elbow** — *"where the loss stops being initially flat and begins to drop
   tends to be the most creatively underfit checkpoint."* Note this is where loss **starts
   falling**, not where it flattens. Earlier revisions of this doc dismissed the loss curve
   as useless; that applies to the smoothed value drifting, not to locating the elbow.

2. **The CFG crossover — the clearest signal, and our demos cannot show it.**
   *"Base RF demos (CFG~7) light up first… Then CFG=7 over-cooks and CFG=1 takes over. If
   CFG=1 sounds good and CFG=7 doesn't, that's a sign the LoRA has internalised the style."*

   Every demo in the run-1 config is **ARC at CFG 1**, so only half that signal is visible.
   The "never Base demos" rule in this doc was a Colab compute-unit economy, and Colab is
   gone. **Add a fifth demo: Base, RF, CFG 7, ~50 steps**, reusing one of the ARC prompts.

3. **ARC lags then wins** — *"ARC demos take a few thousand more steps to catch up to base
   RF, but final quality is usually better."* An all-ARC demo set therefore looks worse
   than reality early, which biases you toward over-training.

4. **Conditional -> unconditional crossover** — when the empty-prompt demo starts sounding
   like the dataset, the style has been absorbed into the base.

**A memorised checkpoint is not a failed one.** Lower **LoRA strength to 0.6-0.8** at
inference, or use **LoRA interval (skip first step)** so the base model sets song structure
and the LoRA only supplies style — that alone prevents most regurgitation. Memorised
checkpoints also hit harder for audio2audio style transfer. Keep every checkpoint.

**The best checkpoint is often not the last one** — but make sure you are comparing
equal training, not equal step numbers. On 38 tracks at batch 4, 2500 steps is ~250
epochs; random crop mitigates repetition but the elbow came at ~50. Keep every
checkpoint and compare at the same prompt and seed.

---

## 6. Local inference — the payoff

Runs on the Mac, no cloud, no hourly rate.

**UI** (LoRA strength sliders, seed, steps, audio2audio, inpainting):

```bash
cd sa3-studio/stable-audio-3/optimized/mlx
.venv/bin/python scripts/sa3_gradio.py --dit medium \
  --lora ~/Downloads/500chekpoint-1run/dune-zvq-medium-03-step=500-epoch=49.safetensors \
  --default-seconds 120 --default-steps 8 --no-share
```

**CLI:**

```bash
.venv/bin/python scripts/sa3_mlx.py --dit medium \
  --lora ~/Downloads/500chekpoint-1run/dune-zvq-medium-03-step=500-epoch=49.safetensors \
  --lora-strength 0.7 \
  --prompt 'zvq, TrackType: Music, Moods: dark, epic, Instruments: strings, low brass' \
  --seconds 120 --out dune-test.wav
```

Try `--lora-strength` 0.5 / 0.7 / 0.9 to hear how hard the style is applied.

Prefer mira's own generate window over these scripts where it covers the job — it
supports 3 LoRA slots, and it is the thing being built.

---

## 7. Next

1. **Train the three film LoRAs** at batch 4, stopping each near ~50 epochs.
2. **Compare checkpoints by epoch**, at the same prompt and seed.
3. **Dune vs Dark Knight** is the widest contrast available (mean onset_rate 0.90 vs
   2.25) — the pair for one clean inference experiment. Batman is the weak set: 13
   files, 1.2 h, and 11 of 13 in one rhythm bucket.
4. **Then tune the captions.** The pipeline is proven; the next gains are in what mira
   writes, not in the training setup.

Untried idea: instead of one LoRA per film, train them together with a shared
score-ness token plus a per-film token, so every new film reinforces the shared one and
styles can be blended at inference.

### Open items

- `keyscale` came back "D major" on 13 of 38 — key detection is the weakest mira field.
  Consider dropping that pill and re-comparing.
- 8 tracks were cut at the 600 s pre-encode cap (~39 min, ~15% of the set). `--max-duration 1200`
  would recover it, at untested encoder RAM.
- mira's content-type router called two full cues `stem`; their sidecars were hand-corrected.
  Re-running `mira caption --emit-sidecar` on those two undoes the fix. mira has no
  "declare this a track" command — worth adding.
- `flash_attn` is not installed; attention falls back to FlexAttention (compiled). Its
  gain is largest at batch > 1, so it may be worth installing now that batch 4 is standard.
