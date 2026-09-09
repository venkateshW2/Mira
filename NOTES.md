# SA3 LoRA Studio — working notes

Status as of 2026-09-09. Companion to SETUP.md (which has errors — see below).

---

## 1. Current state: setup is DONE and verified

`$STUDIO` = this folder (`/Users/justmac/w2app/SA-3-LoraStudi_Underfit`), **not** `~/sa3-studio`.
Internal APFS, not cloud-synced. `env.sh` self-locates, so the folder can be renamed/moved.

```
SA-3-LoraStudi_Underfit/          ← $STUDIO
├── env.sh                        ← self-locating; source before any command
├── SETUP.md  NOTES.md  start-studio.sh  check-studio.sh
├── stable-audio-3/               ← 779434a (2026-09-01)
│   └── optimized/mlx/
│       ├── .venv/                ← Python 3.11.15, mlx 0.32.2, Metal OK
│       └── models/mlx/*.npz      ← 11 GB of weights
├── underfit/                     ← e336b3d (2026-08-27)
│   ├── .venv/                    ← Python 3.10.19, torch 2.7.1
│   └── state/                    ← runs/checkpoints land here
├── .hf/hub/  .uvcache/  datasets/  loras/
```

`./check-studio.sh` → 7/7 present. Studio ≈ 13 GB.

**Smoke test passed** — real audio generated, not just a clean install:
```
6.25s wall → 5.0s audio → 0.80× realtime → peak RAM 1.62 GB
```
Matches the repo's published sm-music figure exactly.

### Launch
```bash
cd /Users/justmac/w2app/SA-3-LoraStudi_Underfit
./start-studio.sh          # → http://localhost:8787, occupies the terminal
```

---

## 2. Weights on disk (all byte-verified against the HF manifest)

Repo is `stabilityai/stable-audio-3-optimized`, **public, NOT gated** — no token, no
licence click. Files live under `MLX/` there, and locally in
`stable-audio-3/optimized/mlx/models/mlx/`.

| File | GB | Purpose |
|---|---|---|
| `dit_sm-music_f16.npz` | 0.92 | sm-music inference (ARC) |
| `dit_sm-music-base_f16.npz` | 0.92 | **sm-music LoRA training** |
| `same_s_decoder_f32.npz` | 0.22 | SAME-S codec |
| `same_s_encoder_f32.npz` | 0.21 | SAME-S codec |
| `dit_medium_f16.npz` | 2.91 | medium inference (ARC) |
| `dit_medium-base_f16.npz` | 2.91 | **medium LoRA training** |
| `same_l_decoder_f32.npz` | 1.70 | SAME-L codec |
| `same_l_encoder_f32.npz` | 1.70 | SAME-L codec |
| `t5gemma_f16.npz` | 0.57 | text encoder (shared) |

Bundle status: `medium 4/4`, `sm-music 4/4`, `sm-sfx 3/4` (sfx skipped on purpose —
only `dit_sm-sfx-base` / `dit_sm-sfx` missing; SAME-S is already here).

Training uses the **`-base`** checkpoints, not ARC. The installer's bundle picker does
NOT include them — they were downloaded separately. Don't lose them.

`ensure_local()` returns early if the file exists, so nothing re-downloads.

---

## 3. Corrections to SETUP.md (it is wrong in five places)

1. **Repo name** — `stable-audio-3-optimized`, not `stable-audio-3-small-music`.
2. **No HF login needed** — repo is public. Step 3 is optional; a token only lifts the
   anonymous ~50 GB/day CDN limit.
3. **Python is 3.11** (MLX) and **3.10** (underfit), not 3.10 everywhere.
4. **Sizes** — sm-music is ~2.8 GB incl. base weights, not "~7 GB".
5. **"medium is a Studio job" is overcautious.** Repo benchmarks are from an M1 **8 GB**:
   medium peaks 3.8 GB (10 s) to 5.2 GB (120 s); README says "8 GB M1 is fine for
   everything." The flash-attn/CUDA warning in Troubleshooting is about the **torch**
   backend and is irrelevant on MLX.
   *Unverified:* medium **training** RAM is not published anywhere. Inference numbers
   don't answer it.

Also: `./install.sh --no-setup` still pulls torch 2.7.1 + torchaudio (~1.5 GB) as base
deps of the underfit package. Unused on MLX, harmless, but SETUP.md implies otherwise.

---

## 4. Captioning — the actual findings (main open work)

### The `.txt` files people share are captions, not MIR dumps
A raw feature table (`tempo=128.3, spectral_centroid=...`) is the wrong vocabulary and
trains worse. Two formats are valid:

**(a) Key-value tags** — what the SA3 base was trained on, and what the "columns" in
those community files are:
```
Genre: techno, BPM: 140, Mood: dark, Key: D minor
```

**(b) Prose + numbers** — SA3's own `reprompt.py` `Music` preset template:
```
Genre/Style with main instruments, supporting layers, and rhythm/percussion
creating mood/energy. BPM: X. Length: Y seconds
```
Its rule 8: *"Combine all elements into one natural, fluid sentence. Avoid semicolons."*

### How underfit builds prompts
`underfit/dataset_processing/prompt_templates.py`:
- `_build_tag_prompt` → `", ".join(f"{Label}: {value}")` — comma-joined key-value.
- Four prompt sources with balance %  summing to 100: **tags / paths / fixed / trigger**.
- `trigger_pct` prepends the trigger token to N% of prompts.
- Shuffle randomises field order per step (prevents memorising comma order).

**Built-in display names** (`_TAG_DISPLAY`):
```
title→Title  artist→Artist  album→Album  genre→Genre  label→Label
date→Year    composer→Composer  bpm→BPM   prompt→Prompt
```
`key`, `mood`, `instruments`, `style` are NOT built in — but they still work:
`_TAG_DISPLAY.get(key, key)` falls back to the raw key as the label.

> **Rule:** use lowercase `bpm` and `date` to get the auto-labels; **capitalise custom
> fields** (`Key`, `Mood`, `Instruments`, `Style`) so they render consistently.

### Use JSON sidecars, not .txt
`.txt` collapses to one opaque `prompt` field. JSON keeps fields separate, and
`tag_keys = pc.get("tag_keys", _ALL_TAG_KEYS)` means **underfit can toggle which keys
enter the prompt per run** — caption once, then experiment with subsets without
re-captioning. This is what makes machine-draft → human-edit work.

Sidecar locations read automatically: `{stem}.json` beside the audio, or
`<parent>/json/{stem}.json`. (`.txt` equivalent: same dir or `<parent>/txt/`.)

Note: MLX `pre_encode_mlx.py` **deliberately omits embedded ID3/Vorbis tags** — sidecars
only. Since underfit drives the MLX trainer, don't rely on embedded tags.

### Neither repo ships any of this
Verified: **zero** yt-dlp, **zero** MIR in underfit or stable-audio-3.
`dataset_processing/autotagger.py` is a **filename parser** — it parses artist/title/
album/year/genre from folder and file names. It listens to nothing.
The official SA3 LoRA doc says only "~20–50 clips, more is better" and assumes paired
text already exists. Every dataset-prep pipeline in the wild is community-built.

### Community pipeline (what was observed)
`yt-dlp` → wav → **audio-LM writes prose + MIR fills BPM/key** → sidecars → underfit
scan → pre-encode → train. Captioning takes minutes; the "couple of hours" is training.

Tools in use:
- Prose: Qwen2.5-Omni / Qwen2-Audio, LP-MusicCaps, NVIDIA Music Flamingo
- `ace-lora-trainer` bundles Qwen2.5-Omni + BPM/key/time-sig/duration extraction
- Hard MIR: Essentia (`RhythmExtractor2013`, `multifeature` vs `degara`), or librosa

---

## 5. Planned tool: `mir_caption.py` (NOT YET BUILT — waiting on reference repo)

Design agreed:
1. Walk a folder, write `{stem}.json` beside each audio file.
2. Machine fields: `bpm`, `Key`, `duration`, + confidence values.
3. Human stubs: `Genre`, `Mood`, `Instruments`, `Style` — left empty to fill in.
4. `--dry-run` by default, prints a table; `--force` to overwrite.
5. **Re-runs update only machine fields, never clobber human edits.** This is what makes
   the edit loop safe.
6. librosa for BPM + key (clean arm64 install; verify Essentia on Apple Silicon before
   committing to it). Goes in the MLX venv — already has numpy + soundfile.
7. Audio-LM for prose fields slots in later behind the same interface.

**Caveat to surface in the tool:** key detection is unreliable and tempo estimators make
double/half-time errors constantly. A wrong `BPM: 140` on a 70 BPM track teaches the
model something false — hence the confidence output and the human-edit step.

---

## 6. Open decisions

- **`sm-music` vs `medium`** — decides `same-s` vs `same-l` pre-encoding. Latents are
  **not interchangeable**; switching means re-encoding the whole dataset.
  Recommendation: first run on sm-music (0.80× realtime, 1.6 GB) to learn the loop, then
  re-encode for medium. Both weight sets are already on disk.
- **Dataset** — own tracks. README: 10 min floor, 30+ better, one *coherent* style
  ("mixed bags train into mush"). Dashboard allows tick/untick after scan, so curate there.
- **Trigger phrase** — pick something distinctive that isn't real English.
- Reference repo the user saw (columns: tempo/key/mood/instruments/genre/style) — link
  pending; match its field names when it arrives.

### Training settings (from SETUP.md / README)
DoRA-rows, rank 16, ~10000 steps (elbow ≈ creatively-underfit sweet spot), demo every
1000. Single-style set → Fixed text = trigger phrase, 100%.
Inference: LoRA strength 0.6–0.8, skip first denoising step (keeps base song structure).

---

## 7. Machine gotchas learned the hard way

- **exFAT kills uv.** `/Volumes/v`, `/Volumes/V.Sound`, `/Volumes/T7 Shield 1` are all
  exFAT (via macOS 15 `fskit`). No hardlinks, no reflinks, no symlinks — verified live
  (`os error 45` = ENOTSUP). uv falls back to full per-file copies; one dependency
  resolution took **1291 s**. Deleting 4.5 GB of small files took ~12 min.
  **Never put a venv, HF cache, or live checkpoint on exFAT.** Transfer medium only.
  Relevant to SA3: `weights.py` does `target.symlink_to(cached)` — that would fail
  outright on exFAT, not merely crawl.
- **VoiceStudio** was fixed by editing
  `~/Library/Application Support/com.debpalash.omnivoice-studio/config.json`:
  `env_dir`/`data_dir` → `~/VoiceStudio` (internal), `models_dir` → external.
  `setup_complete: true` is what locks the wizard out. Backup kept alongside.
- **Quitting VoiceStudio orphans Python processes** (`parent=1`) that ignore SIGTERM.
  Check with `pgrep -f "VoiceStudio/project"` before a training run.
- **16 GB is tight.** VoiceStudio logged `<4GB free RAM`. Don't run it during training.
  Free RAM after killing it: ~5.8 GB.
- **GUI apps don't see `/opt/homebrew/bin`** — VoiceStudio failed to find ffmpeg 9.0.1
  and burned minutes trying to install its own. CLI installs don't have this problem.
