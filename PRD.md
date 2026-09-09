# PRD — drive-audio-analyzer (v2)

**Status:** draft for review. Nothing built.
**Date:** 2026-09-09
**Machine:** Apple M1 Pro, 16 GB, macOS 15.5. Fully local. Slow is acceptable.
**Scope:** Part 1 only — analysis, classification, similarity search.
Caption generation for LoRA training is explicitly **out of scope** (see §11).

**Lineage:** a local-first rewrite of
[drive-audio-analyzer](https://github.com/venkateshW2/drive-audio-analyzer) (Google Drive)
and [2w12-backend](https://github.com/venkateshW2/2w12-backend) (FastAPI + CUDA server).
Local drives, no server, no GPU assumption. Sononym-like in purpose, Sonic Visualiser in
spirit. Prior-implementation lessons in §2b — they are the main input to this design.

---

## 1. What this is

A local tool that listens to every audio file you own and builds a searchable
understanding of it — think [Sononym](https://www.sononym.net/), but open, scriptable,
and extending to full tracks rather than mainly samples.

Two capabilities, one analysis pass:

1. **Describe** — tempo, key, loudness, brightness, instruments, moods, genre, vocal
   presence, with confidences.
2. **Find** — "show me more like this", plus filtering on any extracted field.

It is a standalone tool with standalone value: finding a sound on your drives, auditing a
sample library, filtering a score collection. That it will *later* feed LoRA captioning is
a consequence of doing this well, not a requirement shaping it.

### Goals
- One analysis pass per file, cached, incremental, resumable
- Works on both **one-shots/loops** and **full tracks** (different paths, one document)
- Similarity search over a personal-scale library with no server, no cloud
- Everything local on Apple Silicon; slow is fine, wrong is not
- Output is a plain, versioned JSON document — greppable, scriptable, diffable

### Non-goals (v1)
- Real-time / plugin-hosted analysis
- Training our own models
- A GUI (CLI + library API; a UI can come later on top of the same index)
- Caption rendering for generative models

---

## 2. RTNeural — researched, and it's the wrong tool here

Worth settling since it prompted this round.

[RTNeural](https://github.com/jatinchowdhury18/RTNeural) (Jatin Chowdhury,
[paper](https://arxiv.org/pdf/2106.03037)) is a **C++ inference engine for hard real-time
audio** — built so a neural net can run inside a plugin's audio callback without
allocating or blocking. Supported layers: Dense, GRU, LSTM, Conv1D, Conv2D, BatchNorm1D/2D;
activations tanh, ReLU, Sigmoid, SoftMax, ELu, PReLU.

Three reasons it doesn't belong in this project:

1. **It has no models.** It's an engine that runs weights *you* trained. Adopting it means
   training our own classifiers — a research project, not a tool.
2. **That layer set cannot express the models we want.** EfficientNet (discogs-effnet)
   needs depthwise-separable convolutions and squeeze-excite blocks; transformer models
   need attention. Neither is representable.
3. **We have no real-time constraint.** RTNeural's entire value is bounded worst-case
   latency inside a 5 ms audio buffer. We are batch-processing a drive overnight. We'd pay
   the constraint and get none of the benefit.

If the reference pipeline uses it, that strongly implies a small custom classifier running
inside a plugin or live context. Different problem.

**We still get C++ speed** — Essentia is a C++ library with Python bindings, and its
TensorFlow predictors run compiled graphs. No C++ to write, no real-time straitjacket.

---

## 2b. Lessons from 2w12-backend (read 2026-09-09)

The prior implementation is the most valuable input we have. Four findings drive v2.

### Finding 1 — the "3,316x speedup" was analysis not happening

`ESSENTIA_BUG_FIX_JULY_12_2025.md` reports 0.0045 s for a 13 s file, "2,879x faster than
audio". `core/essentia_wrapper.py` shows why:

```
hop_size = max(1, len(y) // 10);  max_frames = 10     # spectral
hop_size = max(1, len(y) // 5);   max_frames = 5      # energy
                                  max_frames = 3      # harmonic
```

Ten frames spread across a whole file is a sample of roughly a few hundred milliseconds,
reported as a whole-file result. The speed was real; the analysis was not. This is the
single most important thing to not repeat.

**v2 rule:** every result carries `coverage` — what fraction of the file was actually
analysed. Analysis is honest about cost. A slow correct answer beats a fast fabricated one.

### Finding 2 — silent fallbacks masked broken models

From `TEST_FINDINGS_JULY_12_2025.md`: the danceability model hit a `cppPool` error and fell
back to an energy heuristic that returned **1.0**; CREPE reported key with confidence
**exactly 1.0**. Meanwhile `essentia_models.py` lists `voice_instrumental` and a YAMNet head
as *empty files, disabled*, and VGGish disabled on GraphDef errors. A caller could not tell
a model result from a heuristic guess.

**v2 rule:** no silent fallbacks. A failed model yields `null` plus a recorded error, never
a substituted heuristic. Confidence of exactly 1.0 is treated as a bug signal.

### Finding 3 — CREPE was doing a job it cannot do

`"pitch_detection": "models/Crepe Large Model.pb"` was wired to key detection. CREPE is a
**monophonic f0 tracker**. It estimates the pitch of one note at a time; it has no concept
of key, and on polyphonic music or field recordings it tracks whichever partial dominates.
That is very likely the root of the pitch problems on longer files and field recordings.

**v2 rule:** key comes from `TuningFrequency` → `KeyExtractor` (chroma/HPCP based, designed
for the job). CREPE stays available, but only for genuinely monophonic content, and is
never rendered as "key".

Related: the genre head `Genre Discogs 400` requires `discogs-effnet` **embeddings** as
input, and no embedding model appears in that model list — another likely silent failure.

### Finding 4 — the chunking plan was solving the wrong problem

`AGGRESSIVE_CHUNKING_PLAN.md` proposes 10 s chunks × 8 parallel processes to hit realtime.
But 10 s chunks destroy exactly the long-window evidence tempo and key need, and
whole-file averaging over 21 chunks blurs a varied track.

**v2 rule:** chunk for **musical structure**, not for scheduler convenience. Segment on
detected boundaries, analyse segments, and keep per-segment results *plus* a whole-file
summary. Parallelism happens across **files**, which is embarrassingly parallel and needs
no chunking at all.

### Also carried over
- The Essentia `random_device` init failure was worked around with `HOME=/tmp`, which is
  invasive. Pin the known-good wheel instead and only patch `TMPDIR` if it recurs.
- `pyacoustid` + `musicbrainzngs` fingerprinting is genuinely useful for commercial tracks
  and worth keeping as an optional enrichment — but it is useless for a producer's own
  stems and bounces, so it must never be on the critical path.
- The old stack carried librosa + madmom + audioflux + TensorFlow + torch simultaneously.
  v2 uses Essentia as the single DSP/inference stack.

---

## 2c. Which models actually do the classifying

Direct answer to "are there open weights?" — **yes, and they are the Essentia model zoo**
(`.pb` graphs, `essentia.upf.edu/models/`). RTNeural is an *engine with no models*, so it
cannot answer this question; these can.

**Core set — one embedding pass feeds every head:**

| Model | Role | Classes |
|---|---|---|
| `discogs-effnet-bs64-1` | **embedding — mandatory input to all heads below, and the similarity vector** | — |
| `mtg_jamendo_instrument-discogs-effnet-1` | instruments | 40 |
| `mtg_jamendo_moodtheme-discogs-effnet-1` | moods + themes | 56 |
| `genre_discogs400-discogs-effnet-1` | genre/style | 400 |
| `voice_instrumental-discogs-effnet-1` | vocal presence | 2 |
| `danceability-discogs-effnet-1` | danceability | 2 |

**Content gating — essential for a producer's drive** (field recordings, stems, noise):

| Model | Runtime | Role | Classes |
|---|---|---|---|
| **`CED-small`** (`sherpa-onnx-ced-small-audio-tagging`) | **ONNX** | general audio events — "is this even music?" | **527** |
| `audioset-yamnet` / `audioset-vggish` | Essentia | lighter/weaker alternatives | 521 / — |
| `Perch` | — | *only* if bioacoustics matter; beats AudioSet models on bird tasks | — |

**CED-small is the chosen gate** — 83 MB, ONNX, benchmarked at **236× realtime** (§3b), so
it is cheap enough to run on every file. Alternatives considered and rejected for now:
BEATs and AST (higher AudioSet mAP, heavier), PANNs (fixed 10 s @ 32 kHz input),
EfficientAT (worth revisiting if CPU cost ever matters), YAMNet (lighter but weaker).

This is the piece the old repo lacked. Music heads must only run on music. A field
recording pushed through a genre classifier returns confident nonsense — the same failure
class as CREPE-as-key.

**Why not one model for everything:** measured on the same track (§3c), CED returns
`Music .674, Musical instrument .127` while `mtg_jamendo_instrument` returns
`orchestra .298, violin .283, cello .272, piano .248`. General-audio models are coarse on
instruments; music models are blind outside music. Both branches are required.

**Zero-shot escape hatch — CLAP.** The fixed 40/56/400 taxonomies cannot describe a
specific field recording, texture, or anything outside their vocabulary. CLAP scores
arbitrary text labels against audio with no training. Adds a `torch` dependency, so it is
Phase 3, but it is the only route to open-vocabulary labelling.

**Licence status — resolve before any non-private use:** MTG-Jamendo heads and MuQ weights
are likely non-commercial (CC BY-NC-SA / CC-BY-NC 4.0). CED, AudioSet and Essentia's own
algorithms are more permissive. Unverified; private use is unaffected.

**Non-neural, and better than a model for these:**

| Task | Algorithm |
|---|---|
| Tempo/beats | `RhythmExtractor2013` (multifeature) — also `TempoCNN` (`deeptemp-k4/k16`, already in the old repo) as a cross-check |
| Key | `TuningFrequency` → `KeyExtractor` (profiles: `temperley`/`krumhansl`/`edma`/`bgate`) |
| Loudness | `LoudnessEBUR128` |
| Monophonic f0 | `PredominantPitchMelodia`, or CREPE — gated to monophonic content only |

**Optional later:** `genre_discogs519` (MAEST transformer, better, slower); `msd-musicnn`
as a second opinion; CLAP / MuQ-MuLan as alternative similarity spaces (§4).

### On the speed you observed
20 tracks × 3–4 min ≈ 70 min of audio "in a few seconds" is not achievable with honest
whole-file neural analysis on CPU. The plausible explanations are GPU inference, per-file
rather than total timing, or the same frame-limiting shortcut as Finding 1. Worth
establishing which before treating it as a target — otherwise we would be optimising toward
a number that may not represent real work.

---

## 3. Feasibility — verified on this machine

**Essentia on Apple Silicon** (the load-bearing assumption). Tested 2026-09-09:

```
uv venv --python 3.12
uv pip install essentia-tensorflow==2.1b6.dev1389   → wheel, no compilation

essentia 2.1-beta6-dev — all present:
  RhythmExtractor2013  KeyExtractor  TuningFrequency  PredominantPitchMelodia
  LoudnessEBUR128  Danceability  BeatsLoudness  HPCP
  TensorflowPredictEffnetDiscogs  TensorflowPredictMusiCNN
  TensorflowPredictVGGish  TensorflowPredict2D
```

**Version-critical** — from the PyPI API:

| Release | Python with macOS arm64 wheels | min macOS |
|---|---|---|
| `2.1b6.dev1177` | cp38–cp312 | 11.0 |
| **`2.1b6.dev1389`** | **cp39–cp313** | **15.0** ← pin this, on Python 3.12 |
| `2.1b6.dev1438` (latest) | cp314 only | 15.0 |

Public web sources still say "no macOS wheels, must compile from source." That is
out of date; the wheels exist and install in ~60 s.

**Similarity index — measured, not assumed.** Brute-force cosine over 512-dim float32:

| Library size | top-20 query | RAM |
|---|---|---|
| 10,000 | **0.8 ms** | 20 MB |
| 100,000 | **9.1 ms** | 195 MB |
| 500,000 | **46.2 ms** | 977 MB |

→ **No ANN index. No FAISS, no hnswlib, no vector database.** A memory-mapped `.npy`
matrix and a NumPy dot product (BLAS via Accelerate) is interactive well past any personal
library. This removes a dependency, a build risk, and an entire class of index-corruption
bugs. Revisit only past ~2 M files.

### 3b. Full-pipeline benchmark — measured, 60 s track, M1 Pro CPU

| Stage | Time | Realtime factor |
|---|---|---|
| load @44.1k mono | 0.04 s | 1664× |
| `RhythmExtractor2013` (multifeature) | 0.62 s | 97× |
| `KeyExtractor` | 0.03 s | 2068× |
| `LoudnessEBUR128` | 0.26 s | 230× |
| load @16k mono | 0.32 s | 186× |
| **`discogs-effnet` embedding** | **0.91 s** | **66×** |
| instrument head (40 classes) | 0.04 s | 1468× |
| CED log-mel (numpy) | 0.04 s | 1644× |
| **CED-small inference (ONNX, CPU)** | **0.25 s** | **236×** |
| **Total** | **≈ 2.2 s** | **≈ 27×** |

**Target of 5× realtime is exceeded by 5–6×.** A 40-minute file completes in ~90 s, not
8 minutes. Conclusions:

- **The embedding is the only meaningful cost** (0.91 s of 2.2 s). Heads are ~0.04 s each,
  so instrument + mood + genre + voice + danceability together add roughly nothing. Run
  them all.
- **CED is nearly free at 236×** — cheap enough to run on every file as the content gate.
- **CoreML EP gave no benefit** (0.26 s vs 0.25 s CPU). Not worth the complexity; CPU only.
- **ONNX Runtime works cleanly on arm64** — a legitimate second runtime alongside Essentia.

### 3c. Label sanity check — the complementarity, with data

Same 60 s track (SA3-generated from *"cinematic orchestral, strings, piano, timpani, brass,
dark and tense"*):

| Model | Top outputs |
|---|---|
| `mtg_jamendo_instrument` | orchestra .298, violin .283, cello .272, piano .248, flute .219, strings .189, doublebass .174 |
| `CED` (AudioSet) | Music .674, Musical instrument .127, Keyboard .086, Theme music .060 |

MIR on the same file: **BPM 119.9 (conf 1.46 of ~5.3), key C minor (strength 0.81),
−19.5 LUFS.**

Two things this establishes:

1. **CED answers "what kind of audio", discogs-effnet answers "which instruments".**
   Neither substitutes for the other — this is why both branches exist (§2c).
2. **Probabilities are not calibrated.** The correct top instrument scores only `0.298`.
   Ranking is reliable; absolute values are not usable as fixed thresholds. Confirms the
   open question in §12.6 — thresholds must be tuned per head on real material.

Note the tempo confidence of 1.46 is *appropriately low* for orchestral material with no
strong beat. Surfacing that is the behaviour §2b Finding 2 demands.

---

## 4. What "similarity" actually requires — research findings

Sononym exposes five similarity dimensions (Overall, Spectrum, Timbre, Pitch, Amplitude)
plus gradings like brightness, harmonicity, noisiness, and classifies loop vs one-shot and
category (kick, snare…). It is strongest on short one-shots with little harmonic content.

From the 2026 literature on
[perceptually-aligned music similarity](https://arxiv.org/html/2601.19109):

- **LAION-CLAP** and **MuQ-MuLan**, both 512-dim, score ~71.9% and ~72.4% perceptual
  agreement with plain cosine similarity — competitive with supervised baselines.
- **Stem-separated, instrument-weighted similarity reaches 90.4%** — a large jump over
  whole-track cosine, and it enables per-instrument "importance sliders" (drum-similarity
  vs guitar-similarity).
- *Caveat the authors state plainly:* results are on Slakh (synthetic multitrack) and may
  not generalise to real recordings.

Two design consequences:

1. **One embedding will not serve everything.** `discogs-effnet` is trained on
   Discogs *music* — good for tracks and loops, and it is also the required input for every
   Essentia classifier head. It is unlikely to be good at one-shot drum hits, where
   timbral/spectral descriptors and a general-audio model matter more. Plan for
   **multiple embedding spaces**, selected by content type, and A/B them on real files.
2. **Stem separation is the biggest known quality lever** for track similarity — and it is
   expensive. Deferred to Phase 4, designed for but not built in v1.

---

## 5. Architecture

```
scan ──► route by content type ──► analyse ──► store ──► query
          (one-shot / loop / track)      │        │
                                         │        ├── SQLite: metadata, tags, descriptors
                                         │        └── .npy memmap: embedding matrix
                                         │
              ┌──────────────────────────┴───────────────────────┐
              │ A. DSP descriptors   (always, cheap)             │
              │ B. MIR               (loops + tracks)            │
              │ C. Neural embedding + heads (music content)      │
              └──────────────────────────────────────────────────┘
```

**Content-type router** decides the analysis path. Duration, onset density, and loop-point
heuristics separate one-shot / loop / full track. Running key detection on a 300 ms kick is
wasted work and produces confident nonsense — routing prevents that.

**A. DSP descriptors** (all content) — duration, sample rate, channels, integrated LUFS,
loudness range, true peak, crest factor, spectral centroid (brightness), spectral flatness
(noisiness), harmonicity, onset rate, attack time. Cheap, interpretable, and the backbone
of one-shot similarity.

**B. MIR** (loops + tracks) — `RhythmExtractor2013` (multifeature) for BPM + beats +
confidence; `TuningFrequency` → `KeyExtractor` for tuning-corrected key with profile
choice; `BeatsLoudness`; `Danceability`.

**C. Neural** (music content) — one `discogs-effnet-bs64` pass produces the embedding,
which is reused for **both** similarity search **and** as input to every classifier head.
This is the key efficiency: the expensive step runs once and serves two features.

| Head | Classes | Verified label examples |
|---|---|---|
| `mtg_jamendo_instrument-discogs-effnet-1` | 40 | `accordion`, `acousticbassguitar`, `classicalguitar`, `drummachine` |
| `mtg_jamendo_moodtheme-discogs-effnet-1` | 56 | `calm`, `dark`, `deep` … also `advertising`, `corporate`, `christmas` |
| `genre_discogs400-discogs-effnet-1` | 400 | `Blues---Boogie Woogie`, `Brass & Military---Brass Band` |
| `voice_instrumental-discogs-effnet-1` | 2 | `voice`, `instrumental` |
| `danceability-discogs-effnet-1` | 2 | — |

(Class counts and labels read from each model's own metadata JSON, not from docs.)

### Label normalisation is a real component
The raw taxonomies are not presentable: `acousticbassguitar` has no spaces,
`Blues---Boogie Woogie` uses a `---` separator, and "moodtheme" mixes genuine moods
(`dark`, `calm`) with sync-licensing categories (`advertising`, `documentary`,
`christmas`). Normalisation lives in **versioned YAML data files, not code**, so it can be
fixed without a release. Both `raw` and `label` are stored, so a mapping fix never requires
re-analysing audio.

---

## 6. Storage

- **SQLite** — one row per file: path, sha256, mtime, content type, descriptors, tags
  (JSON columns), provenance. Gives free filtering, sorting, and joins, plus safe
  concurrent reads.
- **`embeddings.npy`** — memory-mapped `float32 [N, D]`, row index stored in SQLite.
  Loads lazily; the benchmark above is the whole search engine.
- **Per-file `.json` sidecar** (optional, `--emit-sidecars`) — for portability and for the
  human-edit workflow.

**`machine` / `human` split** in every document: re-analysis rewrites `machine` wholesale
and never reads or writes `human`. Human edits always win on conflict. This is what makes
repeated re-analysis safe as models and mappings improve.

**Provenance** — mira version, Essentia version, model names/versions, analysis timestamp.
Lets a later model upgrade identify exactly which files need re-analysis.

---

## 7. Tech stack

| Concern | Choice | Why |
|---|---|---|
| Language | Python **3.12** | only version with current Essentia arm64 wheels + stable ecosystem |
| Env | `uv`, own venv | isolated from MLX (3.11) and underfit (3.10) venvs |
| MIR + music models | `essentia-tensorflow==2.1b6.dev1389` | C++ core, verified arm64, MIR + music heads in one dep |
| General-audio models | `onnxruntime` (1.29, arm64) | runs CED/AudioSet models Essentia doesn't ship; **CPU EP only — CoreML measured no faster** |
| Decode | `ffmpeg` (present) + `soundfile` | anything → canonical PCM |
| Vectors | `numpy` + memmap | benchmarked sufficient to 500k |
| Index/meta | `sqlite3` (stdlib) | zero deps, transactional, queryable |
| Schema | `pydantic` v2 | versioned, validated |
| CLI | `typer` | already in the underfit stack |
| Mappings | PyYAML | human-editable, versioned |
| Tests | `pytest` + fixture clips | normalisation needs regression tests |

Deliberately **not** in v1: torch, librosa, madmom, FAISS, a vector DB. Essentia covers
DSP, MIR and inference; a second DSP stack means two answers to the same question.

---

## 8. CLI surface

```bash
mira scan <dir>...            [--jobs N] [--follow-symlinks]     # index files, no analysis
mira analyze [--limit N] [--content-type track|loop|oneshot] [--force] [--resume]
mira similar <file|id> [--by overall|timbre|rhythm|spectrum] [--n 20] [--filter "bpm>120"]
mira search  "--filter" expressions over tags/descriptors
mira inspect <file|id>        # human-readable report, flags low-confidence fields
mira models  --download | --list
mira stats                    # library composition, coverage, unmapped labels
```

- `analyze` is idempotent and resumable — skips files whose `sha256` **and** model versions
  are unchanged
- `similar` accepts an external file not in the library (analyse-then-query)
- `inspect` surfaces low-confidence tempo/key rather than hiding it

---

## 9. Phases

**Phase 0 — skeleton.** Venv, model download, SQLite schema, pydantic models, CLI stubs,
fixture clips covering one-shot / loop / track.

**Phase 1 — describe.** Scanner, content-type router, DSP descriptors, MIR, `inspect`.
No neural yet. Already useful: BPM/key/loudness across a drive.

**Phase 2 — classify + search.** discogs-effnet embedding, five heads, normalisation
mappings + tests, embedding store, `similar`, `search`. **This is the Sononym-parity
milestone.**

**Phase 3 — quality.** Embedding A/B (discogs-effnet vs CLAP vs MuQ-MuLan) measured on
*your* library; per-dimension similarity (timbre/rhythm/spectrum); confidence calibration;
segment-level analysis instead of whole-track averages.

**Phase 4 — stem-aware similarity.** Demucs separation + per-instrument weighting, per the
90.4% result. Expensive; only if Phase 3 shows whole-track similarity is the limit.

**Phase 5 — surfaces.** Optional local web UI over the same index. Caption rendering
(§11) plugs in here as one consumer among several.

---

## 10. Risks

| Risk | Severity | Mitigation |
|---|---|---|
| Pinned Essentia **dev** build vanishes from PyPI | high | vendor the wheel, record hash |
| discogs-effnet poor on one-shots | high | content-type routing; DSP descriptors carry one-shot similarity; Phase 3 A/B |
| Tempo double/half-time errors | med | store confidence; `inspect` flags; never silently trust |
| Key unreliable, esp. non-tonal material | med | store `strength`; expose profile; route away from one-shots |
| Whole-track averaging blurs long/varied tracks | med | Phase 3 segmentation |
| Taxonomy labels unusable raw | med | normalisation layer + regression tests; keep `raw` |
| TF inference CPU-only, slow on long tracks | low | acceptable; cache aggressively; resumable |
| Model licences (MTG-Jamendo may be CC BY-NC-SA) | **open** | verify before any non-private use |
| Slakh-based similarity results may not generalise | med | validate on real files before investing in Phase 4 |

---

## 11. Deferred: caption generation (explicitly out of scope)

Research already done and retained in `NOTES.md` §4 — SA3's training-time prompt
augmentation, the 45-word / 256-token ceilings, the instrumental bias, and which fields are
in-vocabulary (BPM, genre, moods, instruments). When we return to it, captioning becomes a
**renderer over the same analysis document** — no re-analysis, no changes to Part 1. That
constraint is the design's main test.

---

## 12. Open questions

1. **Name** — `mira` is a placeholder.
2. **Repo** — own repo, or sibling of `underfit/`? Leaning own; it is not SA3-specific.
3. **Library scale** — roughly how many files, and what mix of samples vs full tracks?
   Changes routing priorities and the Phase 3 A/B.
4. **Key profile** — `temperley` / `krumhansl` / `edma` / `bgate`; `edma` is usually best
   for electronic material. Needs an A/B on real tracks.
5. **Similarity dimensions** — mirror Sononym's five, or derive our own from what the
   embeddings actually separate?
6. **Confidence thresholds** — what `p` admits a tag? Tune empirically, don't guess.
