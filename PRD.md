# PRD — drive-audio-analyzer (v2)

**Status:** draft for review. Nothing built.
**Date:** 2026-09-09
**Machine:** Apple M1 Pro, 16 GB, macOS 15.5. Fully local. Slow is acceptable.
**Scope:** Part 1 only — analysis, classification, similarity search.
Caption generation for LoRA training is **out of scope for v1** and lands as Phase 4
(§9, §11). It is the eventual destination, and not only for Stable Audio 3 — the analysis
document must be renderable into the caption format of any trainable base model.

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
- Works across **one-shots, loops, full tracks and delivery stems** (different paths, one
  document)
- Similarity search over a personal-scale library with no server, no cloud
- Everything local on Apple Silicon; slow is fine, wrong is not
- Output is a plain, versioned JSON document — greppable, scriptable, diffable

### Non-goals (v1)
- Real-time / plugin-hosted analysis
- Training our own models
- A GUI (CLI + library API; a UI can come later on top of the same index)
- Caption rendering for generative models (Phase 4)
- **Lyric transcription** — see §15. Puts lyric-bearing material out of reach for the
  song-model family; instrumental content is unaffected
- **Local prose captioning** — no music captioner has a working on-device port for this
  machine (§14.3). Prose comes from an offline batch job, if at all

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
- **Source-separated, instrument-weighted similarity reaches 90.4%** — a large jump over
  whole-track cosine, and it enables per-instrument "importance sliders" (drum-similarity
  vs guitar-similarity).
- *Caveat the authors state plainly:* results are on Slakh (synthetic multitrack) and may
  not generalise to real recordings.

> **Terminology — two different things called "stem".** The paper above means
> *source separation*: running Demucs over a finished mix to manufacture isolated
> instrument tracks. **mira does not do this, in any phase.** In this project a
> **stem** always means a *delivery submix* printed from a scoring session for the mix
> engineer — a strings stem, a rhythm stem, a vocal stem. It already exists as a file.
> We only ever analyse what is on the drive. See §12.3.

Two design consequences:

1. **One embedding will not serve everything.** `discogs-effnet` is trained on
   Discogs *music* — good for tracks and loops, and it is also the required input for every
   Essentia classifier head. It is unlikely to be good at one-shot drum hits, where
   timbral/spectral descriptors and a general-audio model matter more. Plan for
   **multiple embedding spaces**, selected by content type, and A/B them on real files.
2. **Source separation is out of scope.** It is the literature's biggest quality lever, but
   it is expensive, unvalidated outside synthetic multitrack, and irrelevant to this
   library: the user's stems are delivery submixes of whole sections (all strings, all
   rhythm), not isolated sources, so they neither require separation nor serve as
   ground truth for it. Per-dimension similarity is pursued instead, in Phase 3.

---

## 5. Architecture

```
scan ──► route by content type ──► active regions ──► analyse ──► store ──► query
      (one-shot / loop / track / stem)        │            │        │
                                              │            │        ├── SQLite: metadata,
                                              │            │        │   tags, descriptors
                                              │            │        └── .npy memmap:
                                              │            │            embedding matrix
                                              │            │
                    ┌─────────────────────────┴────────────┴──────────┐
                    │ A. DSP descriptors   (always, cheap)            │
                    │ B. MIR               (loops / tracks / stems)   │
                    │ C. Neural embedding + heads (music content)     │
                    └─────────────────────────────────────────────────┘
```

**Content-type router** decides the analysis path across **four classes — one-shot, loop,
track, stem**. Duration, onset density and loop-point heuristics separate one-shot / loop /
track. Stems are identified by the three routes in §12.3 (declaration, sibling-set
detection, or filename pattern) rather than by signal heuristics, because a delivery stem is
indistinguishable from a sparse track on duration alone. Running key detection on a 300 ms
kick is wasted work and produces confident nonsense — routing prevents that.

### Active-region detection — required, not an optimisation
An energy gate over frames finds the **non-silent spans** of a file before any analysis
runs; descriptors, MIR and the embedding then see only those spans. `active_ratio` and the
span list are stored.

This exists because of delivery stems. A strings stem is timeline-aligned to a whole cue, so
a three-minute stem may hold forty seconds of strings and one hundred and forty of digital
black. Without gating:

| Stage | Failure on a mostly-silent file |
|---|---|
| `discogs-effnet` embedding | averages over mostly silence — the similarity vector points at "quiet", not "strings" |
| LRA, crest factor | meaningless. (R128 gating already protects *integrated* LUFS) |
| `RhythmExtractor2013` | a sparse pad with three entries yields a confident BPM from nothing — §2b Finding 2's exact failure class |
| CED content gate | scores silence against music over the full duration and gates the file out of music analysis entirely |

**When it runs:** always for anything routed as a **stem**, regardless of duration — a stem
for a 90-second cue is still mostly silence. For every other content type, when duration
exceeds **5 minutes**. It is a frame-energy gate with no model, so the cost is negligible
either way; it also pays off on ordinary tracks with long intros, tails and gaps.

**Loudness on stems is not a judgement.** A stem is mixed *relative to the cue*, so a
correct strings stem can sit at −38 LUFS integrated. Absolute loudness is recorded but
never used to flag a stem as quiet, thin or faulty.

**A. DSP descriptors** (all content) — duration, sample rate, channels, integrated LUFS,
loudness range, true peak, crest factor, spectral centroid (brightness), spectral flatness
(noisiness), harmonicity, onset rate, attack time. Cheap, interpretable, and the backbone
of one-shot similarity.

**B. MIR** (loops, tracks, stems — over active regions only) — **two tempo estimators run
in parallel and their disagreement is the confidence signal** (§14.1): `RhythmExtractor2013`
(multifeature) and **Beat This!** (ONNX). Beat This also yields **downbeats**, which
Essentia does not give well and which unlock bar-aligned slicing and time signature.
`RhythmExtractor2013` for BPM + beats +
confidence; `TuningFrequency` → `KeyExtractor` for tuning-corrected key with profile
choice; `BeatsLoudness`; `Danceability`. **Key is gated on harmonic content** and never run
blindly — a rhythm stem has no meaningful key, and a bass-led stem correlates strongly but
misleadingly (§12b).

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
- **Active regions** — `active_ratio` plus the span list, per file. Every descriptor is
  understood to be computed over those spans.
- **Beat positions** — the full beat array from `RhythmExtractor2013`, not just the BPM
  scalar. Cheap to store and required to draw beat markers in the UI (§13); also lets a
  human see *why* a tempo estimate is wrong rather than just that it is.
- **Stem grouping** — a nullable `group_id` linking the stems of one cue. Not a correctness
  requirement (a strings stem and a rhythm stem are genuinely dissimilar, so siblings do not
  flood `similar`); it exists so you can ask "what cue is this from" and so `similar` can
  return one hit per cue instead of six pieces of the same one.
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
| Beat/downbeat tracking | **Beat This! prebuilt ONNX** (§14.1) | 2024-SOTA beats *and downbeats*, MIT weights, ~10–83 MB, **no torch** — runs under the onnxruntime already present |
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
mira scan <dir> --as stem     # declare a folder as delivery stems (§12.3 route 3)
mira analyze [--limit N] [--content-type track|loop|oneshot|stem] [--force] [--resume]
mira similar <file|id> [--by overall|timbre|rhythm|spectrum] [--n 20] [--filter "bpm>120"]
mira search  "--filter" expressions over tags/descriptors
mira inspect <file|id>        # human-readable report, flags low-confidence fields
mira models  --download | --list
mira stats                    # library composition, coverage, unmapped labels
```

- `analyze` is idempotent and resumable — skips files whose `sha256` **and** model versions
  are unchanged
- `similar` accepts an external file not in the library (analyse-then-query)
- `inspect` surfaces low-confidence tempo/key rather than hiding it, and reports
  `active_ratio` so a mostly-silent stem is visibly mostly silent

---

## 9. Phases

**Phase 0 — skeleton.** Venv, model download, SQLite schema, pydantic models, CLI stubs,
fixture clips covering one-shot / loop / track.

**Phase 1 — describe.** Scanner, four-class content-type router, **active-region
detection**, DSP descriptors, MIR, `inspect`. No neural yet. Already useful:
BPM/key/loudness across a drive. Active-region detection lands here rather than later
because every descriptor downstream depends on it.

**Phase 2 — classify + search.** discogs-effnet embedding, five heads, normalisation
mappings + tests, embedding store, `similar`, `search`. **This is the Sononym-parity
milestone.**

**Phase 3 — SA3 captioning.** The first renderer: SA3 key-value tags + prose under the
256-token cap, `seconds_total`, trigger token, folder-level human defaults, sidecar export
(§15). Ordered here deliberately — **all DSP, MIR and classification complete first**, so the
renderer has every field it will ever have and is never designed around a missing one.
Machine fills BPM/duration/active regions; the classifier pre-fills genre/mood/instruments as
an editable draft; the human field always wins (§6).

**Phase 4 — quality.** Embedding A/B (discogs-effnet vs CLAP vs MuQ-MuLan) measured on
*your* library; per-dimension similarity (timbre/rhythm/spectrum); per-head confidence
calibration (which sets the render-gate thresholds of §12.6); segment-level analysis instead
of whole-track averages.

*(Source separation was a previous Phase 4 and has been removed — see §4.)*

**Phase 5 — surfaces + multi-target captioning.** The UI (§13) over the same index, and the
remaining renderers: ACE-Step JSON, three caption registers, segment slicing.

---

## 10. Risks

| Risk | Severity | Mitigation |
|---|---|---|
| Pinned Essentia **dev** build vanishes from PyPI | high | vendor the wheel, record hash |
| discogs-effnet poor on one-shots | high | content-type routing; DSP descriptors carry one-shot similarity; Phase 3 A/B |
| Tempo double/half-time errors | med | store confidence; `inspect` flags; never silently trust |
| Key unreliable, esp. non-tonal material | med | store `strength`; expose profile; route away from one-shots |
| Whole-track averaging blurs long/varied tracks | med | Phase 3 segmentation |
| Mostly-silent delivery stems poison descriptors and embeddings | **high** | active-region detection in Phase 1; `active_ratio` stored and surfaced |
| Sibling-set stem detection misfires on unrelated same-length files | low | manual `--as stem` declaration always overrides; content type is stored and editable |
| Octave / half-time tempo errors reach a caption and teach a false mapping | **high** | dual estimator (§14.1); 2×/0.5× disagreement flagged; renderer omits low-confidence BPM (§12.6) |
| Essentia is dormant — no 2025/26 releases, no `effnet-discogs` update | med | plan around it; ONNX is the second runtime for anything newer (Beat This, DCLAP) |
| DCLAP is AGPL-3.0 and its retrieval quality vs full CLAP is unpublished | low | optional index behind a flag; fine privately, resolve before any distribution |
| Taxonomy labels unusable raw | med | normalisation layer + regression tests; keep `raw` |
| TF inference CPU-only, slow on long tracks | low | acceptable; cache aggressively; resumable |
| Model licences (MTG-Jamendo may be CC BY-NC-SA) | **open** | verify before any non-private use |
| Slakh-based similarity results may not generalise | n/a | source separation removed from scope (§4); no longer a risk we carry |

---

## 11. Deferred: caption generation (out of scope for v1, now Phase 3)

Research already done and retained in `NOTES.md` §4 — SA3's training-time prompt
augmentation, the 45-word / 256-token ceilings, the instrumental bias, and which fields are
in-vocabulary (BPM, genre, moods, instruments). When we return to it, captioning becomes a
**renderer over the same analysis document** — no re-analysis, no changes to Part 1. That
constraint is the design's main test.

**Not SA3-only.** SA3 is the trainer already set up (`sa3-studio/`, MLX, `underfit`), but it
must not be the thing the schema is shaped around. Other open-weights base models permit
local inference and LoRA training, and they do not agree on caption format — some expect
comma-joined key-value tags, some free prose, some a fixed vocabulary, with differing token
ceilings. The design requirement is therefore: **one analysis document, N renderers.** Which
fields are universally useful versus model-specific is an open research item (§12.7).

---

## 12. Open questions

1. ~~Name~~ — **`mira`**, decided.
2. ~~Repo~~ — **own repo**, decided: `/Users/justmac/w2app/mira`, SA3 studio nested at
   `sa3-studio/`.
3. ~~Library scale~~ / ~~stems~~ — **unbounded, and stems are a first-class content type.**
   Decided; see below.

   **Scale.** No fixed count; content is full tracks, delivery stems, loops and samples.
   Analysis must therefore be **resumable and incremental**, because a run may cover any
   number of files and will be interrupted.

   **What a stem is here.** A **delivery submix printed from a scoring session for the mix
   engineer** — a strings stem, a rhythm stem, a vocal stem. Not a source-separated
   instrument track; mira never separates anything (§4). Analytically it is:
   full cue length and timeline-aligned; **mostly silent**; a submix of a whole section
   rather than a single instrument; and mixed *relative to the cue*, so its absolute
   loudness carries no judgement about it.

   **Consequences for the build.** A fourth router class; **active-region detection as a
   mandatory pipeline stage** (§5) rather than an optimisation; key gated on harmonic
   content; loudness recorded but never used to judge a stem; and a nullable `group_id`
   for cue grouping (§6).

   **How a stem is identified — three routes, all supported:**
   1. **Filename or folder pattern**, where one happens to exist (`CUE_03_STRINGS.wav`, a
      `STEMS/` folder). Cheapest and exact, but **the user's naming differs every time**, so
      this is opportunistic only.
   2. **Sibling-set detection** — N files of identical length in one folder, optionally
      summing to another file of the same length. This is the **general case** and the one
      to build well.
   3. **Declaration** — `mira scan ./cue03/stems --as stem`. Always correct; overrides
      detection.
4. ~~Key profile~~ — **`edma` default, configurable.** Explained in §12b.
5. ~~Similarity dimensions~~ — **derive our own**, decided. Not Sononym's five. Dimensions
   come from what our embeddings and descriptors actually separate on real material
   (Phase 3), rather than inheriting another tool's taxonomy.
6. ~~Confidence thresholds~~ — **store everything; gate at render time.** Resolved by
   research (§14.2). The store keeps top-k with raw scores and never thresholds — §3c's
   correct-but-`0.298` instrument stays. Thresholds exist in exactly one place: the
   **caption renderer omits a field whose confidence is below threshold**, because a
   caption asserting "85 BPM" over a 170 BPM track teaches the LoRA a false mapping,
   and hallucinated captions are measurably worse than absent fields. Per-head threshold
   *values* still need calibration on real files, but the architectural question is
   settled: uncertainty is preserved in the index and resolved at the boundary.
7. ~~Caption-format portability~~ — **resolved: field dict + prose, never a frozen
   caption string.** Two render targets, SA3 and ACE-Step, bracket the design space.
   See §15.
8. ~~UI~~ — **deliberately small: one screen, read + human-field write, no job runner.**
   Decided in §13. Visual design still to settle.

### 12b. Key profiles — what the choice means

Key detection builds a pitch-class distribution for the audio (chroma / HPCP — how much
energy sits on each of the 12 notes), then correlates it against a **template profile** for
every candidate key and picks the best match. The *profile* is that template: 12 numbers
saying how prominent each scale degree should be in a given key.

Essentia ships four, derived from different evidence:

| Profile | Derived from | Suits |
|---|---|---|
| `temperley` | music-theoretic weighting | classical, notated music |
| `krumhansl` | 1980s listener probe-tone experiments | general tonal music |
| `edma` | **electronic dance music corpora** | electronic, loop-based, bass-led material |
| `bgate` | gating variant that discards low-confidence frames | sparse or noisy material |

They disagree often — most where the tonic is implied by a bass line rather than stated
harmonically, which is common in electronic and cinematic work. `edma` was built for that
case, so it is the default; the profile is exposed per run and **recorded in provenance**,
so results stay comparable when it changes.

Stems sharpen the point: a **rhythm stem has no meaningful key**, and a bass-led stem
correlates strongly but misleadingly. Key must be **gated on harmonic content**, never run
blindly — the same discipline as §2b Finding 3.

---

## 13. UI — deliberately small

**The call: one screen, read-mostly, no job runner.** The UI exists to do the one thing the
CLI genuinely cannot — **let a human look at a waveform and type a caption** — and nothing
else. Everything that has a working terminal answer stays in the terminal.

### What was cut, and why

| Cut | Cost avoided | Where it lives instead |
|---|---|---|
| **Triggering analysis from the UI** | the whole job runner: queue, persisted job state, cancel, SSE progress, and **single-writer locking** against a concurrently running CLI. Easily the largest item in the original scope, and it exists only to move a command from the terminal to a button | `mira analyze` in a terminal |
| **Spectrogram** | decoding an entire 40-minute stem client-side to draw it. Hundreds of MB of browser memory for something the waveform + shaded active regions already answer | nothing; revisit if a real need appears |
| **Folder tree** | duplicating a filesystem browser you already have in Finder | a filter box over the index — which is more useful anyway, since the index knows BPM and key and Finder doesn't |
| **Similarity browsing** | a second interaction model (query → result set → re-query) layered on the file list | `mira similar` |

That leaves a **read + human-field-write** surface. The API has no endpoint that starts work
or touches a `machine` field.

### What it is

```
┌─ mira ──────────────────────────────────────────────────────────┐
│ filter: [ bpm>115 and type=stem            ]      34 of 12,481  │
├─────────────────────────────────────────────────────────────────┤
│ ✓ 01_STRINGS   119*  Cmin  −38.2  act 0.29  stem                │
│   02_RHYTHM    120    —    −31.0  act 0.44  stem                │
│   03_BRASS     119*  Cmin  −35.7  act 0.18  stem                │
├─────────────────────────────────────────────────────────────────┤
│ ▁▁▃█▇▅▃▁▁▁▁▁▁▁▁▁▁▁▂▆█▆▃▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁  ▶ 0:12 / 3:00     │
│ ░░████████░░░░░░░░░████████████░░░░░░░░░░░░  ← active regions   │
│ ¦   ¦   ¦   ¦   ¦   ¦   ¦   ¦   ¦   ¦   ¦    ← beats/downbeats  │
├─────────────────────────────────────────────────────────────────┤
│ BPM 119.9 ⚠ estimators disagree 2×   key Cmin 0.81  edma        │
│ folder defaults ▾  Genre [cinematic orchestral                ] │
│                    Mood  [dark, tense                         ] │
│                    Inst  [strings, brass, timpani             ] │
│ this file          Inst  [+ solo cello                        ] │
│ trigger [vnkxstr]  SA3 preview: 218/256 tokens                  │
│ "vnkxstr cinematic orchestral, strings, brass, timpani, dark…"  │
│                                          [export 3 sidecars]    │
└─────────────────────────────────────────────────────────────────┘
```

Four things, in order of why they justify a UI at all:

1. **Waveform + transport + beat markers + shaded active regions.** Impossible in a
   terminal, and the fastest way to see that a tempo estimate is wrong or that a stem is
   mostly silence.
2. **Editable human fields with folder-level defaults and per-file override.** The actual
   captioning workflow (§15): label a cue folder once, override the exceptions.
3. **Live SA3 caption preview with a token count**, since the 256-token cap truncates
   silently and a person should see how close they are.
4. **A filter box and a virtualised list**, because an unbounded library cannot mount every
   row and folders are not how you find things.

### Stack

| Concern | Choice |
|---|---|
| Framework | React + TypeScript, Vite — build-time Node only; the running tool needs Python and a browser |
| Server | FastAPI, same process and venv, serving the built bundle + a JSON API |
| Waveform, transport, beat markers, active regions | **wavesurfer.js v7** + `regions` plugin (no `spectrogram` plugin) |
| Long lists | TanStack Virtual |
| Server state | TanStack Query |
| Job progress | **none** — there are no jobs |

### The one thing that cannot be cut

**Browsers cannot decode much of a producer's drive** — 32-bit float WAV, AIFF and some FLAC
variants will not play natively. The audio endpoint needs HTTP range for what plays and an
**ffmpeg transcode fallback** for what does not. ffmpeg is already a dependency (§7). Without
this the waveform pane is empty on exactly the files that matter most.

### Write scope

- **`human` fields** — genre, mood, instruments, keywords, trigger, folder defaults
- **content-type override** — marking a folder as stems (§12.3 route 3)
- **nothing else.** No `machine` field is writable; no endpoint starts analysis. The §6 split
  is enforced at the API boundary, not by convention

### If the job runner is wanted later

It is additive, not a rewrite: `analyze` already needs resumable job state for
`--resume` (§8), so the persistence exists regardless. Adding a start button and an SSE
progress stream on top is a contained change — and single-writer locking becomes a real
problem to solve at that point, not before.

### Where it sits in the phases

The UI is **Phase 5** and does not move: it is a consumer of the index, and building it
before the schema is settled means designing a view over data that has no shape yet.

---

## 14. Research findings — MIR and captioning (2026-09-09)

Two deep-research passes. Everything here was fetched from primary sources; items the
research could not verify are marked and must not be treated as settled.

### 14.1 Beat This! — the one clear upgrade, and it costs no new runtime

`RhythmExtractor2013` is not embarrassing. On the DeepRhythm benchmark (953 tracks):

| Method | Acc1 (±2%) | Acc2 (harmonics OK) | s/track |
|---|---|---|---|
| Essentia multifeature | 87.9% | **97.5%** | 2.72 |
| Essentia `degara` | 86.5% | 97.2% | 1.38 |
| `TempoCNN` (deeptemp) | 84.8% | 97.7% | 1.21 |
| librosa | 66.8% | 75.1% | 0.48 |

Read the Acc1→Acc2 gap: **~10 points of Essentia's error is octave/half-time errors**, not
noise. That is precisely the error class that poisons a caption, and precisely what a drive
of half-time hip-hop and 170 BPM DnB will produce.

**Adopt [Beat This!](https://github.com/CPJKU/beat_this) (ISMIR 2024, still SOTA) via the
prebuilt ONNX** committed in [beat_this_cpp](https://github.com/mosynthkey/beat_this_cpp) —
MIT, ~10–83 MB, deps are **onnxruntime only**. No torch. It also gives **downbeats**, which
Essentia does not, unlocking bar-aligned slicing, time signature and loop boundaries.

**Do not pick a winner between them.** Beat This's documented failure mode
([SMC Blind Spot](https://arxiv.org/html/2605.12287v1)) is *confident-but-wrong* activations
on expressively-timed music; Essentia's is octave errors. **Run both; treat disagreement —
especially a 2× / 0.5× ratio — as the confidence signal.** The disagreement is worth more
than either estimate alone, and it is §2b Finding 2 implemented rather than asserted.

*Unverified:* no direct Beat This vs `RhythmExtractor2013` tempo-Acc1 table exists. **Measure
it on 50 of our own files** — the project sessions carry true tempo, which is better ground
truth than any published benchmark.

### 14.2 Caption quality → LoRA quality — five findings that shape the renderer

1. **Metadata → LLM rendering is a validated architecture, not a shortcut.**
   [arXiv:2602.03023](https://arxiv.org/html/2602.03023) decouples exactly as mira does:
   predict structured metadata, then render it as language. **Parity with end-to-end
   captioners at 46.3% of the GPU hours**, **>20% gain from prompt refinement alone**, and
   metadata imputation recovering up to 33% on individual fields. This is external
   validation of the whole design: **the DB is the source of truth; captioning is a cheap
   re-runnable pass over the DB, never over the audio.** Restyling costs a re-render, not a
   re-analysis — and retargeting to a 2027 base model is a new template.
2. **Confidence-gate every rendered field.** Hallucinated captions measurably degrade
   training ([TAC](https://arxiv.org/html/2602.15766v1) ties clip-level summarisation to
   higher hallucination). Combined with 14.1's octave errors: **omit BPM/key from the caption
   when confidence is low.** Costs nothing, and it resolves §12.6.
3. **Prose beats tag templates — but only in the register the base model was trained on.**
   [arXiv:2605.21433](https://arxiv.org/html/2605.21433): LLM prose 0.943 val loss vs
   template 0.968, a gain the authors say "exceeds any single training technique" they
   tested. *Their own caveat:* eval prompts were also LLM-generated, so part of it is
   train/test text alignment. The transferable rule is therefore **match the target model's
   caption register**, not "prose is better".
4. **Emit multiple variants at randomised lengths.**
   [arXiv:2506.16679](https://arxiv.org/html/2506.16679v1): dense captions improve text
   alignment but *reduce* aesthetic quality and diversity — a real problem for something as
   capacity-limited as a LoRA — and **randomising caption length eliminates the trade-off.**
   [SonicCaps](https://arxiv.org/html/2609.02343) (~24 captions/clip, 4 registers) confirms
   multiple granularities compose. Note their lengths are **short**: main captions average
   **11.2 words**, not paragraphs. So emit three registers — `tags` (~3–4 terms), `short`
   (~10 words), `long` (~40 words, one fact per clause) — and let the trainer sample.
   **Do not do persona-based captioning: it measurably hurt prompt-following.**
5. **Write captions as independently droppable clauses.** Trainers apply sentence-level
   masking and conditioning dropout as a matter of course. *Unverified:* the exact
   5–20% / 20–50% figures are field convention, not a proven optimum for music LoRAs — but
   the structural implication holds regardless.

### 14.3 There is no local music captioner for this machine, and that is an absence of ports

Checked both on-device runtimes directly:

- **llama.cpp** audio-capable GGUFs are Ultravox, Voxtral, Qwen3-ASR — **all speech/ASR.
  Zero music captioners.**
- **mlx-vlm**'s only documented working audio model is `gemma-3n-E2B-it-4bit`.
- The Qwen3-Omni MLX port is **17.2 GB** (over total unified memory) **and text-only** — the
  audio encoder still needs torch.

Every captioner worth using is 7–8B+ with a torch-only audio encoder, and the two best
music-specific ones ([Music Flamingo](https://huggingface.co/nvidia/music-flamingo-2601-hf),
[AF-Next](https://huggingface.co/nvidia/audio-flamingo-next-captioner-hf)) are **NVIDIA
OneWay Noncommercial** on A100-class hardware.

**Consequence: local prose captioning is not a v1 feature, and mira should not pretend
otherwise.** The route that fits: run
[`ACE-Step/acestep-captioner`](https://huggingface.co/ACE-Step/acestep-captioner) — **7B,
MIT**, and the model that actually labelled ACE-Step 1.5's own training data, so its register
matches the base model — as an **occasional offline batch job on borrowed GPU time**. Its
prose lands in the DB as one more field, and mira's renderer fuses it with mira's *measured*
BPM/key. That is exactly the 2602.03023 architecture, and it plays to the division of labour:
**the captioner guesses, mira measures.**

### 14.4 Segmentation, not timestamps

The research has moved decisively to temporal grounding (FUTGA → FUTGA-MIR → TAC →
MusTBench, which finds current audio LLMs struggle badly at precise temporal alignment). **The
trainers have not.** ACE-Step's LoRA dataset is one caption per file; MusicGen's trainer is
`segment_000.wav` / `segment_000.txt` pairs. YuE's `msa` field is the sole exception (§15).

So temporal richness reaches trainers **as segmentation**: slice long content into
**30–120 s segments** (ACE-Step's documented range) and caption each segment whole. Active
regions (§5) are what makes the slicing correct rather than arbitrary — and for a
mostly-silent delivery stem, whole-file captioning produces a caption describing silence or
hallucinating content. **Never caption a stem whole-file.**

Dataset scale, from ACE-Step's tutorial: **20 samples minimum for basic style capture,
50–100 for robust generalisation**, WAV/FLAC ≥44.1 kHz. Consistent with SA3's "~20–50 clips".

### 14.5 Similarity — a torch-free CLAP exists, with a licence catch

[AudioMuse-AI-DCLAP](https://github.com/NeptuneHub/AudioMuse-AI-DCLAP) is a **distilled
LAION-CLAP** (audio tower ~80M → ~7M params) shipped as ONNX alongside the unmodified CLAP
text tower. **512-dim shared text/audio space, onnxruntime + librosa + numpy, no torch.**

That buys something `discogs-effnet` structurally cannot: **natural-language search over the
drive** — "find my dusty broken-tape piano loops". Adopt as an **additional index behind a
flag**, not a replacement, because (a) **AGPL-3.0** — fine for a private tool, a problem if
mira is ever distributed, and (b) **no published retrieval metrics vs full CLAP**, so the
distillation is trusted blind.

**Essentia is stable-to-dormant** — no releases in 2025 or 2026, no update to
`effnet-discogs`. Plan around it rather than expecting upstream improvement. One correction
in our favour: `KeyExtractor` *has* been updated (new profiles, detuning correction, spectral
whitening) and its upstream default is now **`bgate`**, not `edma`. §12.4 keeps `edma` for
electronic/bass-led material, which is the right call for this library, but the divergence
from upstream should be recorded in provenance — as §12.4 already requires.

### 14.6 Deferred, with reasons

| Candidate | Verdict |
|---|---|
| **SongFormer** (structure, ACC 0.807 vs All-in-One 0.740) | **v1.5 at the earliest.** Head is trivial (4 layers, 512 dim) but needs **two SSL backbones (MuQ + MusicFM), torch-only**. Worth a spike: can MuQ/MusicFM be ONNX-exported? *Nobody has tried, as far as the research could tell.* Structure labels would be transformative for captioning long tracks and finding loop points — this is the one place to eventually pay the torch tax |
| **MuQ-MuLan** (similarity, MTAT ROC-AUC 79.3) | only if DCLAP disappoints. Torch-only, no ONNX, and MAEB's ranking of it is confusing enough to re-read first |
| **KeyMyna** (key, CC-BY) | **no.** 72% GiantSteps is a low ceiling, torch-only, and the delta over Essentia's updated `bgate` + whitening is unverified. Bad trade |
| **DeepRhythm** (tempo, 95.9% Acc1) | **no.** AGPL-3.0 + torch + nnAudio, and Beat This gives downbeats too |
| **LP-MusicCaps** | **no.** CC-BY-NC, torch, 2023-quality output. Superseded |

*Research could not verify:* MAEB's leaderboard (extracted numbers were self-contradictory);
KeyMyna's margin over updated Essentia; whether MuQ/MusicFM export to ONNX; trigger-token
efficacy in audio LoRAs (ACE-Step's own docs say "limited effect", and no study exists either
way); and a search-surfaced claim that LoRA is 15–30% worse than full fine-tuning for audio,
unconfirmed in the Audiobox PDF.

---

## 15. Caption schema — one document, N renderers

### The rule: never emit a frozen caption string

Every trainer permutes and drops fields **itself**, at training time:

- `underfit` shuffles and subsamples its tag list every step
- MusicGen's `augment_music_info_description()` shuffles `meta_pairs`, with `drop_desc_p` /
  `drop_other_p`
- ACE-Step's `genre_ratio` swaps `caption` for `genre` on a percentage of samples

A pre-rendered sentence destroys all three augmentations. mira therefore emits a
**normalised field dict plus short prose**, and each trainer gets a thin **field assembler**
— not a string formatter. There are five incompatible join syntaxes in the wild:
`"Label: v, Label: v"` (SA3) · `"key: v. bpm: v."` (MusicGen, period-joined) ·
`"v, v"` (ACE-Step, LeVo) · `"v,v"` (HeartMuLa, no space) · `"v v v"` (YuE, space-delimited).

### Canonical fields

Every key maps 1:1 onto at least one real trainer's field name, so renderers stay trivial.

```
caption            # prose, three registers (§14.2.4): tags / short ~10w / long ~40w
genre[]            # universal
instruments[]      # universal
moods[]            # universal (MusicGen's plural key name)
keywords[]         # production / recording / era / texture
bpm                # number + confidence
keyscale           # "D major" (ACE-Step's name; MusicGen calls it `key`) + strength
timesignature      # from Beat This downbeats (§14.1)
duration           # seconds
is_instrumental    # bool
language
lyrics             # not produced by mira — see below
segments[]         # {start, end, label}
trigger            # + placement
```

| Field | Status across trainers |
|---|---|
| genre, instruments, moods, prose, duration, vocal presence | **universal** — every model consumes them under some name |
| **bpm** | near-universal semantically, **five renderings syntactically**: structured JSON (ACE-Step, MusicGen), a tag (`underfit`), prose (`"140 BPM"` SA3, `"the bpm is 140"` LeVo), absent entirely (YuE, HeartMuLa) |
| **keyscale** | **only three pipelines** — ACE-Step `keyscale`, MusicGen `key`, Mustango `prompt_key`. **Not in SA3's documented tag vocabulary nor in `underfit`'s `_TAG_DISPLAY`.** Emit it; expect it dropped for over half the targets |
| **loudness** | **no model in the survey has a loudness field, in any form.** MusicGen just normalises output to −14 LUFS at write time. Loudness is a **curation and filtering signal for mira, never a caption field** — which is consistent with §5's rule that a stem's loudness is not a judgement |
| **segments** | first-class in **exactly one** training pipeline (YuE's `msa`). Others take structure only as `[Verse]` markers inside lyrics. So active regions stay an **internal correctness device** (§5), not a caption feature |
| timesignature | ACE-Step only |
| chords | Mustango and JASCO only — both non-commercial. Not worth building for |
| **lyrics** | required or central for ACE-Step, YuE, HeartMuLa, LeVo, Muse, DiffRhythm. **mira does not transcribe anything** — see the non-goal below |

### Two render targets, chosen to bracket the space

| Target | Format | Why this one |
|---|---|---|
| **Stable Audio 3** via `underfit` | key-value tags + prose inside a **hard 256-token** T5Gemma prompt, plus numeric `seconds_total` (0–384) | the only thing actually trainable on this machine (§14.3 / below) |
| **ACE-Step 1.5** | structured JSON — `caption`, `bpm`, `keyscale`, `timesignature`, `language`, `is_instrumental` | **MIT-licensed**, commercially unambiguous, official LoRA *and* LoKr/DoRA trainers, and the one pipeline taking BPM/key as structured fields |

Render cleanly into both and MusicGen, LeVo, HeartMuLa and YuE become different join
characters over subsets of the same dict.

**SA3's 256-token ceiling truncates silently.** The renderer needs a **ranked field order,
truncating from the tail**: `trigger → TrackType/VocalType → Genre → Instruments → BPM →
prose → production/era keywords`. (MMAudio is worse: CLIP's 77 tokens.)

**BPM is the sharp edge.** SA3 has *no numeric BPM input* — it must go in the prose string.
ACE-Step wants `"bpm": 140` as JSON and assembles conditioning itself, so putting
`BPM: 140` in its `caption` **double-conditions**. Same measurement, two incompatible
destinations. This is why the field dict, not the string, is the stored artefact.

### What is actually trainable on this machine

| | Trainable on M1 Pro / 16 GB? | Licence |
|---|---|---|
| **Stable Audio 3** via `underfit` (MLX) | **Yes** — small comfortably, medium at the documented floor | Stability Community — commercial OK under $1M revenue |
| ACE-Step 1.5 | **No.** MPS training OOMs (~45 GB needed); LoRA does not apply under the MLX DiT | **MIT** — cleanest in the field |
| MusicGen / MAGNeT / JASCO | CUDA | **CC-BY-NC** — non-commercial |
| YuE | 24–80 GB | Apache-2.0 |
| DiffRhythm 2, HeartMuLa, LeVo | ship no trainer at all | mixed |
| Suno / Udio | closed, no weights | — |

So SA3 is the only real near-term target, and ACE-Step's schema is **cheap insurance** —
emitted now, trainable elsewhere later, and MIT.

*Research could not verify:* whether SA3's training captions ever contained a `Key:` field
(the prompting guide lists `TrackType`, `VocalType`, `Genre`, `Instruments`, `Format` and
nothing about key) — **treat key as prose-only and possibly out-of-distribution for SA3**.
LeVo's exact licence also remains unconfirmed (HF card 401s; reported variously as Tencent
custom non-commercial and NOASSERTION).

### New non-goal: lyrics

mira does not transcribe. That places **ACE-Step, YuE, HeartMuLa, LeVo, Muse and DiffRhythm
out of reach for lyric-bearing material** regardless of caption quality — they are reachable
only for instrumental content. Stated as a boundary rather than left as a surprise. Adding
transcription is a different project.

