# Training SA3 LoRAs on Colab Pro

Written 2026-09-13. Operational companion to [RUNBOOK-dune-lora.md](RUNBOOK-dune-lora.md)
(the local run) and [SA3-INFERENCE-AND-TRAINING.md](SA3-INFERENCE-AND-TRAINING.md) (the
options landscape).

**Why Colab at all:** a 16 GB M1 Pro cannot train `sa3-medium` — measured, not guessed
(see "What 16 GB measured" below). Colab also frees the laptop, which is the music
production workhorse.

---

## ✅ Account — settled 2026-09-13

**All three are on `w2@2w12.one`:** the Colab Pro subscription, the `colab-cli`
authorisation, and the Drive that mounts on the VM. Confirmed working —
`colab sessions` authenticates and responds.

| | |
|---|---|
| **Google account for everything SA3** | **`w2@2w12.one`** |
| Colab Notebooks folder | `Colab Notebooks` |
| ...folder ID | `1dl3xCSXfz4q8MLoVYx7AmgFW2ecD6WHt` |
| ...link | https://drive.google.com/drive/folders/1dl3xCSXfz4q8MLoVYx7AmgFW2ecD6WHt |
| CLI credentials | `~/.config/colab-cli/sessions.json` |

Note this is **not** the profile email on this machine (`rnd@frizzon.co`) — if you ever
see an auth failure or an empty Drive, that mismatch is the first thing to check.

## The big correction: Colab has an official CLI *and* MCP

Earlier in this project I claimed Colab was browser-only with no CLI or MCP. **That was
wrong.** Google ships both:

- **CLI** — https://github.com/googlecolab/google-colab-cli (`google-colab-cli` on PyPI)
- **MCP** — https://github.com/googlecolab/colab-mcp

They serve different purposes:

| | Use it for |
|---|---|
| **CLI** ⭐ | Headless automation from a terminal. An agent can drive it via shell. **This is the one that matters here.** |
| MCP | Bridging a local agent to a Colab session *in the browser* — in-notebook interactive assistance |

The CLI's own README states it is "designed to support seamless developer productivity,
headless automation, and AI agent integrations." macOS and Linux only.

### Status on this machine

```
uv tool install google-colab-cli    # DONE — v0.6.0 installed
colab sessions                     # BLOCKED — needs OAuth in a browser
```

**Your one manual step:** run any `colab` command in your own terminal, open the URL it
prints, approve, and paste the code back into *that* terminal. Credentials then persist at
`~/.config/colab-cli/sessions.json` and the CLI can be driven from scripts (and by Claude
via shell) from then on.

---

## What the CLI actually gives you

This is a much better story than the notebook-in-a-browser workflow, and it removes ngrok
entirely.

| Command | What it does |
|---|---|
| `colab new -s NAME --gpu L4 [--high-mem]` | Provision a GPU VM (T4, L4, G4, A100, H100) |
| `colab upload -s NAME LOCAL REMOTE` | Push a file straight to the VM — **no Drive round-trip** |
| `colab install -s NAME PKG...` | Install deps with `uv` |
| `colab exec -s NAME -f script.py` | Run a local script on the VM (file is sent for you) |
| `colab ssh -s NAME` | Real SSH shell over WebSocket; works as an OpenSSH `ProxyCommand` |
| `colab console -s NAME` | Raw interactive TTY (tmux) — watch training live |
| `colab download -s NAME REMOTE LOCAL` | Pull checkpoints back |
| `colab drivemount -s NAME` | Mount Drive at `/content/drive` |
| `colab status` / `colab sessions` | Hardware, machine shape, state |
| `colab stop -s NAME` | Tear down the VM **(this is what stops the spend)** |
| `colab pay` | Manage compute units |

Two features that directly solve problems we hit:

- **Automatic keep-alive daemon** — a background process prevents idle VM termination
  "without requiring open browser tabs." This is the answer to Colab killing long runs.
- **`--high-mem`** — requests a high-RAM machine shape. Requires Colab Pro. Ignored on L4
  and TPU (single shape only).

### `colab upload` changes the plan

The earlier plan was: zip latents → drag to Drive → mount Drive on the VM. With
`colab upload` you push the 125 MB zip straight onto the VM filesystem. Drive becomes
optional, useful mainly for persistence across sessions.

---

## Compute units — the thing to actually watch

Colab Pro is **100 compute units/month**, not unlimited time. Units expire after 90 days
and more can be bought. **Burn rate scales with GPU tier** — an A100 drains units far
faster than an L4 or T4.

*Unverified:* exact units/hour per GPU. Check `colab status` and the subscription page as
you go, and **start on the smallest GPU that fits medium** rather than defaulting to A100.

Rough shape of the budget: a 10,000-step medium run takes ~6 h on the M1 and should be
well under 2 h on a decent GPU. The constraint is units, not wall-clock.

---

## The workflow

```bash
# 0. ONE TIME, in your own terminal — browser OAuth
colab sessions          # follow the URL, approve, paste the code

# 1. provision
colab new -s sa3 --gpu L4          # add --high-mem if medium needs it

# 2. push the dataset (125 MB, ~1 min)
colab upload -s sa3 \
  /Users/justmac/w2app/mira/sa3-studio/dune-ost-latents-same-l.zip \
  /content/dune-latents.zip

# 3. set up underfit + SA3 on the VM
colab ssh -s sa3
#   (on the VM)
#   unzip /content/dune-latents.zip -d /content/latents
#   git clone https://github.com/dada-bots/underfit
#   git clone https://github.com/Stability-AI/stable-audio-3
#   ...install, download the sa3-medium pack

# 4. watch it train
colab console -s sa3

# 5. retrieve the LoRA (~36 MB)
colab download -s sa3 <run>/checkpoints/<name>.safetensors ./dune-zvq.safetensors

# 6. STOP THE VM — this is what ends the spend
colab stop -s sa3
```

Then run inference **locally** — medium inference fits comfortably on the M1 (~5.2 GB peak
at 120 s per the repo's own 8 GB M1 benchmarks), and `lora_merge.py` reads underfit/PEFT
torch-format safetensors directly. Train remote, generate local.

---

## Ready to go

| Asset | Where |
|---|---|
| Latents + captions (125 MB, 78 files) | `sa3-studio/dune-ost-latents-same-l.zip` |
| Source audio (2.7 GB, fallback if latents don't load) | `sa3-studio/datasets/dune-ost/` |
| Trigger token | `zvq` |
| Prompt config | tags 100%, prepend `zvq` 80%, pills OFF for `prompt` / `trigger` / `length_seconds` |

---

## What 16 GB measured (why we're here)

| Config | Result |
|---|---|
| medium @ 2048 crop | OOM |
| sm-music @ 1300, no grad-checkpoint | OOM |
| sm-music @ 1300 **+ `--grad-checkpoint`** | works — **10.99 GB peak**, 2.2 s/step, ~6.1 h for 10k steps |

Note `--grad-checkpoint` is **CLI-only** — underfit's dashboard never passes it (no
reference in `mlx_engine.py`, `server.py` or `index.html`). Same for the torch trainer's
`--base_precision bf16`. The UI hides the memory levers.

---

## Session log — 2026-09-13 (first Colab session, L4)

Everything below was done and verified on a live L4. The headline: **the whole
mira → MLX pre-encode → Colab torch pipeline works end to end.**

### ✅ Verified: MLX latents load in the torch trainer

This was the one assumption the plan rested on. `stable_audio_3.data.dataset.PreEncodedDataset`
reads the MLX-produced `.npy` + `.json` pairs directly:

```
Found 38 files
DATASET LEN: 38
ITEM TYPE: tuple
  0  torch.Tensor  torch.Size([256, 1300])   <- the requested crop
  1  dict
```

And mira's captions survive intact into the torch metadata:

```
prompt      = zvq, TrackType: Music, VocalType: Instrumental, Electronic: Ambient featuring ...
trigger     = zvq
genre       = Electronic: Ambient
moods       = film, relaxing, epic
instruments = synthesizer, piano, violin, flute, harp, strings, keyboard, cello
bpm = 61 | keyscale = E minor | seconds_total = 60.07 | seconds_start = 0
```

Plus the trainer's own `latent_crop_start`, `padding_mask`, `latent_shape`.

**Consequence: no re-encoding on the VM, and the 2.7 GB audio upload is unnecessary.**
The 125 MB latents zip is the only dataset artifact that needs to move.

### Gotchas that cost real time (not in anyone's docs)

**1. `colab-cli` from PyPI is broken — install from git.**
`pyproject.toml` pins a Google fork via `[tool.uv.sources]`:
```
jupyter-kernel-client = { git = "https://github.com/googlecolab/jupyter-kernel-client.git" }
```
A PyPI install ignores that source override and pulls upstream `jupyter-kernel-client==1.0.2`,
which has `JupyterKernelClient` but not `KernelClient` — every `colab exec` dies with
`AttributeError`. Fix:
```bash
uv tool install --force git+https://github.com/googlecolab/google-colab-cli.git
```
That yields colab-cli 0.7.0 + jupyter-kernel-client 0.8.0 (the fork), and `exec` works.

**2. `colab upload` has a size limit — chunk anything big.**
125 MB failed instantly with `SSLError(SSLEOFError)`. 1 MB and 20 MB succeed. Also, the
remote directory must already exist or the upload fails with no useful message. Working
recipe:
```bash
split -b 20m dune-ost-latents-same-l.zip chunk_
echo 'import os; os.makedirs("/content/chunks", exist_ok=True)' | colab exec -s sa3
for f in chunk_*; do colab upload -s sa3 "$f" "/content/chunks/$(basename $f)"; done
# then cat the parts back together on the VM and CHECK THE MD5 against local
```
The reassembled md5 matched local exactly — always verify, a silent truncation here would
poison training.

**3. `colab exec` times out on long jobs — the VM keeps working, the client gives up.**
A 14 GB download died with `TimeoutError: Timeout waiting for reply` while the VM happily
continued. Run anything long detached and poll:
```python
subprocess.Popen("cd /content/underfit && nohup <cmd> > /content/x.log 2>&1 &", shell=True)
```
Then poll with `pgrep -fc` until it hits 0.

**4. HF gating is split — base is open, ARC is gated.**
This contradicts `NOTES.md` §3 point 2 ("No HF login needed"), which was correct only for
`stabilityai/stable-audio-3-optimized` (the MLX repo the local weights came from).

| Repo | Gated? | Used for |
|---|---|---|
| `stable-audio-3-medium-base` | **no** | LoRA training |
| `stable-audio-3-medium` (ARC) | **yes** | demo rendering / fast inference |

A valid `HF_TOKEN` is not enough for ARC — the account must click **"Agree and access
repository"** at https://huggingface.co/stabilityai/stable-audio-3-medium. One click
unlocks all three SA3 ARC repos. Without it: `GatedRepoError: 403`.

**Never paste the token into a chat.** Set it from your own terminal:
```bash
read -rs HF_TOKEN
echo "import os; os.environ['HF_TOKEN']='$HF_TOKEN'; print('set')" | colab exec -s sa3
```

### VM setup sequence that worked

```bash
colab new -s sa3 --gpu L4                      # L4, 23034 MiB VRAM, python 3.13.15
# upload latents in 20 MB chunks, reassemble, verify md5, unzip -> /content/latents
git clone --depth 1 https://github.com/dada-bots/underfit        /content/underfit
git clone --depth 1 https://github.com/Stability-AI/stable-audio-3 /content/stable-audio-3
cd /content/underfit && ./install.sh --no-setup                  # torch 2.7.1+cu128
uv run python -m underfit.cli.setup --backend sa3     --backend-path /content/stable-audio-3 --models sa3-medium   # needs HF_TOKEN + ARC agree
```

`flash_attn` is absent on the VM and the backend logs that it is disabling Flash Attention.
Harmless so far — noted in case it affects medium demo quality (SETUP.md's troubleshooting
mentions medium demos glitching without flash-attn on CUDA).

---

## Session log — 2026-09-13 (part 2: notebook + dashboard on L4)

Switched from the CLI to the notebook route. Training reached the GPU, but only after
six undocumented obstacles. All of these will recur on any new dataset.

### 1. `uv run` silently downgrades safetensors and breaks training

The notebook launched the dashboard with `uv run python dashboard/server.py`. **`uv run`
re-syncs the venv to underfit's lockfile**, which pins `safetensors==0.7.0` — undoing the
`0.8.0` that the SA3 install put there. Training then dies at model build:

```
ImportError: safetensors>=0.8.0 is required ... but found safetensors==0.7.0
```

underfit's own `run.sh` uses `exec .venv/bin/python dashboard/server.py` for exactly this
reason. **Always launch the dashboard with `.venv/bin/python`, never `uv run`.**

### 2. The venv has no `pip`

`python -m pip install` → `No module named pip`. It is a uv-built venv. Use:
```bash
uv pip install --python /content/underfit/.venv/bin/python 'safetensors>=0.8.0'
```

### 3. Importing pre-encoded latents leaves the dataset stuck on `error`

`_validate_datasets_on_startup()` (server.py:2380) marks any `ready` dataset as `error` if
`latent_dir/details.json` is missing. The import builds a symlinked shadow of the `.npy`
files but **does not link `details.json`** — so the dataset flips back to `error` on every
dashboard restart, and patching `status` in `datasets.json` appears to work and then reverts.

Fix: copy `details.json` from the source latents dir into the shadow `latent_dir`.
`pre_encode_mlx.py` writes it (codec, sample_rate, max_duration, count), so it already exists.

### 4. An imported dataset has no tag pills — and that silently destroys your captions

**The most dangerous one.** The tag UI discovers keys by walking an *audio* `input_dir`
(server.py:3617); `_scan_audio_tags` iterates audio files and only uses `.json` as a
stem-keyed lookup. An imported latents dataset has no audio, so it reports
*"No files with tags found"*, no pills render, and the UI posts `tag_keys: []`.

Downstream, `_build_tag_prompt` iterates an empty list and returns `""` — so every training
prompt collapses to just the trigger. Training runs happily and looks fine. **Every caption
is discarded.**

Workaround: build a directory of the real `.json` sidecars beside placeholder `.wav` files
(underfit requires >= 4096 bytes, `AUDIO_EXTS`), point the dataset's `input_dir` at it, and
delete `<ds_id>_tags.json` from the datasets dir. Training never reads those files — it reads
latents; this only feeds tag discovery.

### 5. Patch `datasets.json` only while the dashboard is stopped

A running server holds the registry in memory and writes it back, clobbering file edits.
Order: `pkill -f dashboard/server.py` → edit → restart.

### 6. Clone Settings does NOT preserve tag pills

It carries model, dataset, crop, LoRA config, trigger, balance and demos — but the tag-key
selection resets to **everything on**. Re-check that screen on every cloned run.

### 7. Latent sidecars carry bookkeeping keys that must be switched off

Because the pills come from the *latent* `.json` (pre-encode output) rather than mira's
original sidecar, extra keys appear — plus six more from `details.json`. Left on, prompts
train on absolute file paths and sample counts:

```
zvq, Src_relpath: Arrakeen .wav, Relpath: Arrakeen .npy, Audio_samples: 6043649,
Path: /Users/justmac/w2app/mira/..., Prompt: zvq, TrackType: Music, ...
```

**Leave ON (7):** `TrackType` `VocalType` `genre` `instruments` `moods` `bpm` `keyscale`
**Turn OFF (15):** `audio_samples` `length_seconds` `path` `prompt` `relpath` `seconds_total`
`src_relpath` `trigger` `audio_dir` `codec` `count` `max_duration` `max_samples` `pad_modulo`
`sample_rate`

### 8. Measured speed — medium, L4

```
Step 99, Epoch 2: 2.29s/it, loss=0.649, lr=3.94e-05, grad_norm=0.022, lora_magnitude=2288
```

**2.29 s/step at batch 1** → ~6.4 h for 10,000 steps, plus ~30 min of demo rounds.
VRAM was ~6.9 GB of 22.5 GB, i.e. the GPU was two-thirds idle — batch 1 wastes an L4.
Retried at **batch 4 / 2500 steps** (same 10,000 samples seen) to test the throughput gain.

Also note: `flash_attn` is not installed; attention falls back to FlexAttention (compiled).
Fine at batch 1; flash-attn's gain is largest at batch > 1, so it may matter more now.

### Misc

- The Colab port-proxy window goes stale — the dashboard stops refreshing and buttons
  (including KILL) do nothing. Kill from a notebook cell (`pkill -f lora_train`) and restart
  the dashboard to get a fresh link.
- tqdm redraws in place, so a live training log *looks* frozen. It isn't.

---

## Open items — none of these are verified

1. **Do the MLX-made latents load in the torch trainer?** Same SAME-L encoder weights and
   the same `.npy` + `.json` format, so they should — but untested. Fallback: upload the
   2.7 GB of audio and re-encode on the VM (fast on a GPU).
2. **Does `--high-mem` matter for medium?** Unknown until tried.
3. **Which GPU tier is cheapest that still fits medium?** Start low, escalate.
4. **Reaching underfit's dashboard.** `colab ssh` acts as an OpenSSH `ProxyCommand`, so a
   port-forward to the dashboard should work — untested. `colab console` gives a TTY, which
   may be enough to watch training without the web UI at all.
5. **Which Google account.** See the top of this document. Settle it first.
