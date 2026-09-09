# SETUP — SA3 LoRA Studio (underfit) on M1 Pro, one self-contained folder

**Target machine:** Apple M1 Pro, 16 GB RAM, macOS 15.5, ~100 GB free internal.
**Goal:** run the underfit dashboard + SA3 MLX trainer to train and test a LoRA,
with **everything living inside one folder** so the whole thing is portable
(copy the folder = move the setup).
**Model for this machine:** `sa3-sm-music` (small, 433M) — the right fit for 16 GB RAM.
Save `sa3-medium` for the Mac Studio.

> This doc is written to be run top-to-bottom, including with Claude Code. Each step
> is a labeled block. Stop and read the CHECK lines — they tell you what "success"
> looks like before moving on.

---

## Ground rules (why it's built this way)

- **One folder, internal disk.** We create `~/sa3-studio/` and put *everything* under it:
  both repos, the Python venvs, the downloaded model packs, datasets, and trained LoRAs.
  Nothing scattered in system caches where we can't move it.
- **Do NOT reuse the Stable Audio DAW plugin's model.** That plugin (VST/AU) is the
  commercial product with its own bundled model — not the open SA3 checkpoints the
  trainer needs. This setup is separate and leaves the plugin untouched.
- **exFAT work drives stay out of it.** Everything is on internal APFS. The exFAT SSDs
  are not in the critical path (they're unreliable for venvs/live checkpoints).
- **Python 3.10 via uv.** The system python is 3.13 (too new). Never use it here —
  `uv` fetches and pins 3.10 automatically inside the folder. Don't `pip install`
  anything globally.
- **Portability = copy the folder.** Because HF caches and venvs normally live in
  `~`, we redirect them INTO the folder (Step 1) so the folder is truly self-contained.

---

## Prerequisites (already confirmed present on your machine)

git ✅  curl ✅  uv ✅  brew ✅  ffmpeg ✅ — nothing to install.

One-time, do this in a browser before Step 4 or downloads 401:
- Go to https://huggingface.co/stabilityai/stable-audio-3-small-music and click
  **"Agree and access repository."** (Free, instant. The SA3 repos share one license.)
- Then create a HF access token: https://huggingface.co/settings/tokens (role: read).
  You'll paste it in Step 3.

---

## Step 0 — Free-space sanity check

```bash
df -h / | awk 'NR==1 || NR==2 {print $4" free"}'
```
**CHECK:** you want ≥ ~30 GB free for the small setup (pack ~7 GB + T5/codec + venvs +
datasets + checkpoints). You said ~100 GB free — fine.

---

## Step 1 — Make the one folder and pin caches inside it

This is the key move for portability: everything (including HF model downloads and
uv's Python) goes under `~/sa3-studio/`.

```bash
export STUDIO="$HOME/sa3-studio"
mkdir -p "$STUDIO"
cd "$STUDIO"

# Redirect HF + uv caches INTO the folder so the folder is self-contained
mkdir -p "$STUDIO/.hf" "$STUDIO/.uvcache" "$STUDIO/datasets" "$STUDIO/loras"
cat > "$STUDIO/env.sh" <<'EOF'
# source this before any command in this project
export STUDIO="$HOME/sa3-studio"          # <-- edit if you move/rename the folder
export HF_HOME="$STUDIO/.hf"               # HuggingFace models download here
export HUGGINGFACE_HUB_CACHE="$STUDIO/.hf/hub"
export UV_CACHE_DIR="$STUDIO/.uvcache"     # uv package cache here
EOF
source "$STUDIO/env.sh"
echo "STUDIO=$STUDIO ; HF_HOME=$HF_HOME"
```
**CHECK:** `echo $STUDIO` prints `/Users/<you>/sa3-studio`. From now on, **always**
`source ~/sa3-studio/env.sh` in a fresh terminal before running project commands.

---

## Step 2 — Clone both repos INTO the folder (as siblings)

underfit needs stable-audio-3 as a sibling.

```bash
cd "$STUDIO"
git clone https://github.com/dada-bots/underfit
git clone https://github.com/Stability-AI/stable-audio-3
ls -1 "$STUDIO"
```
**CHECK:** `ls $STUDIO` shows both `underfit/` and `stable-audio-3/`.

---

## Step 3 — Authenticate to Hugging Face (for gated model download)

```bash
source "$STUDIO/env.sh"
# Installs the HF CLI into a throwaway uv env and logs in; token stored under HF_HOME
uv tool run --from huggingface_hub huggingface-cli login
# paste your read token when prompted
```
**CHECK:** it prints "Login successful". (Token is saved under `$STUDIO/.hf`, so it
travels with the folder too.)

---

## Step 4 — Install the SA3 MLX runtime (its own venv + MLX weight packs)

This sets up the Apple-Silicon (Metal/MLX) engine underfit will drive.

```bash
source "$STUDIO/env.sh"
cd "$STUDIO/stable-audio-3/optimized/mlx"
./install.sh
cd "$STUDIO"
```
**CHECK:** the script finishes without error and creates a venv under
`stable-audio-3/optimized/mlx/`. If it 401s, your HF login/license-agree step didn't
take — redo Step 3 and the browser "Agree" click.

> NOTE: this may pull the small MLX packs (base + ARC + codec + T5). If it tries to
> grab medium, you can skip that for now — we only need `sa3-sm-music` on this machine.

---

## Step 5 — Install the underfit dashboard (skip the torch wizard)

On MLX we don't use the torch backend, so `--no-setup` skips that.

```bash
source "$STUDIO/env.sh"
cd "$STUDIO/underfit"
./install.sh --no-setup
cd "$STUDIO"
```
**CHECK:** finishes without error; creates underfit's venv + `state/` dir.

---

## Step 6 — Point underfit's state at the folder (already there — just confirm)

underfit writes runs/checkpoints to `underfit/state/`, already inside `$STUDIO`. Good.
Optionally symlink your datasets/loras dirs so they're tidy:

```bash
# optional tidiness — safe to skip
ln -sf "$STUDIO/datasets" "$STUDIO/underfit/datasets" 2>/dev/null || true
```
**CHECK:** `ls $STUDIO/underfit/state` exists (created by install).

---

## Step 7 — Launch the dashboard (MLX engine)

```bash
source "$STUDIO/env.sh"
cd "$STUDIO/underfit"
UNDERFIT_ENGINE=mlx UNDERFIT_MLX_ROOT="$STUDIO/stable-audio-3/optimized/mlx" ./run.sh
```
**CHECK:** open http://localhost:8787 in a browser — the dashboard loads, and the
**Engine** dropdown in *New Finetune* shows **MLX**. If a model isn't present it offers
a one-click download — choose **sa3-sm-music**, not medium.

Leave this terminal running; the dashboard serves from it.

---

## Step 8 — First LoRA (your learning run)

In the dashboard:
1. **+ Dataset** → paste a path to a folder of ~20–40 min of ONE coherent style
   (one instrument / one mood from your score library). Tick/untick files after scan.
   Let it pre-encode (torch-free on MLX).
2. **+ Finetune** → Model `sa3-sm-music`, LoRA type **DoRA-rows**, rank **16**,
   steps ~**10000** (elbow ≈ creatively-underfit sweet spot), demo every ~1000.
   For a single-style set: *Configure prompts* → Fixed text = a trigger phrase, 100%.
3. **Launch.** Watch the loss curve; listen to the demo MP3s as they render.
4. Stop anytime — checkpoints land in `underfit/state/runs/<id>/`. Download the
   `.safetensors` (also copy it to `$STUDIO/loras/` to keep it with the folder).

---

## Step 9 — Test inference with your LoRA

From the dashboard, click **[Launch]** on a checkpoint → a Gradio link. Try:
- **LoRA strength 0.6–0.8** ("in the style of" without regurgitation).
- **Skip first denoising step** (keeps song structure from the base).
- **audio2audio** — drop a stem in, hear it restyled.
- **inpainting** — regenerate a region.

---

## Moving to another machine (the payoff)

Because everything is under `~/sa3-studio/` with caches redirected inside it:

**Simplest (same-arch Mac, e.g. the Studio):**
```bash
# on old machine
rsync -a --progress ~/sa3-studio/ /Volumes/<exfat-ssd>/sa3-studio/
# on new machine
rsync -a --progress /Volumes/<exfat-ssd>/sa3-studio/ ~/sa3-studio/
cd ~/sa3-studio && source env.sh
# venvs may be arch/path-specific — rebuild them cleanly on the new machine:
( cd stable-audio-3/optimized/mlx && ./install.sh )
( cd underfit && ./install.sh --no-setup )
```
The **models, datasets, LoRAs, and HF token travel in the folder** (no re-download);
only the two venvs get rebuilt (`uv` makes this fast from the lockfiles).

> The exFAT SSD is fine as a *transfer medium* for copying the folder between Macs.
> Just don't *run* the project from exFAT — run it from internal disk on each machine.

**On the Mac Studio** you can additionally pull the **sa3-medium** pack (more RAM/disk
there): in the dashboard's model download, choose `sa3-medium`.

---

## Everyday use (after setup)

```bash
source ~/sa3-studio/env.sh
cd ~/sa3-studio/underfit
UNDERFIT_ENGINE=mlx UNDERFIT_MLX_ROOT="$STUDIO/stable-audio-3/optimized/mlx" ./run.sh
# -> http://localhost:8787
```

---

## Troubleshooting

- **401 / gated errors on download:** you didn't accept the license in-browser or the
  HF token isn't set. Redo Step 3 + the "Agree and access" click.
- **`sa3-medium` demos glitch / static:** medium needs flash-attn on CUDA — not your
  case (you're on small/MLX). Stick to `sa3-sm-music`.
- **Wrong Python picked up:** never run project commands without `source env.sh`, and
  never `pip install` globally. Let `uv` own Python 3.10 inside the folder.
- **Out of RAM on a run:** you're on 16 GB — keep to `sa3-sm-music`, batch size 1,
  shorter latent length (~12–47s). Medium is a Studio job.
- **Dashboard unreachable:** confirm the `run.sh` terminal is still running; it serves
  the site. Don't close it.

---

## What is NOT in this setup (on purpose)

- The Stable Audio **DAW plugin** — untouched, unrelated, stays where it is.
- The **exFAT work SSDs** — used only (optionally) to copy the folder between machines.
- Any **cloud / external GPU** — everything runs locally on the Mac.
