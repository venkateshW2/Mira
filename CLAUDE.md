# CLAUDE.md — the map of mira

The index to every document in this repo: what each one is, whether it is current, and
when to read it. **Start here.** If you are picking the project up after a break, or you
are an agent with no memory of the last session, this file is the entry point.

**Last updated: 2026-09-16 (late).** Keep the *Recent work* log at the bottom current — that is
this file's second job.

---

## What mira is, right now

A local audio-understanding tool: point it at folders of samples, stems and full tracks,
it analyses them and builds a searchable, captionable index. Everything runs locally on
Apple Silicon. No server, no cloud, no GPU assumption.

**Built and in daily use.** A C++20/JUCE desktop app (`MIRA.app`) plus a `mira` CLI over
one SQLite library. As of 2026-09-15 the library holds **2,257 scanned files, 666
analysed**, and has produced the caption sets for seven trained SA3 LoRAs.

`sa3-studio/` is a downstream consumer, not the point — it is the LoRA training rig that
eats mira's captions.

---

## Build and run

```bash
cmake -B build -DCMAKE_BUILD_TYPE=RelWithDebInfo     # ONNXRUNTIME_ROOT defaults to vendor/
cmake --build build -j8                              # both targets
cmake --build build --target mira_ui -j8             # app only
cmake --build build --target mira -j8                # CLI only
```

- App: `build/src/mira_ui/mira_ui_artefacts/RelWithDebInfo/mira.app`
- CLI: `build/src/mira`
- Library: `~/.mira/library.db` (SQLite + sqlite-vec, WAL)

**A running MIRA holds the old binary.** After a rebuild it must be relaunched to pick up
changes — check `pgrep -f "MacOS/MIRA"` before assuming a change did not work.

---

## The documents

### Read these first

| doc | what it is | state |
|---|---|---|
| **CLAUDE.md** | this file — the map, and the running log of recent work | current |
| [README.md](README.md) | what mira is, for someone who has never seen it | current |
| [ANALYSIS.md](ANALYSIS.md) | **what mira measures and what reaches the model.** Every caption field, what it means, how to edit it, what is deliberately not captioned | current (2026-09-15) |
| [PRD.md](PRD.md) | the design: stack, models, phases, licence reasoning, every "why this and not that" | current as design; §-numbers are cited throughout the code |
| [TASKS.md](TASKS.md) | the build checklist, phase by phase. Phases 0–5 complete, **Phase 6 in progress** | live — tick items here |

### Captioning and training

| doc | what it is | state |
|---|---|---|
| [CAPTION-TAGGING.md](CAPTION-TAGGING.md) | the human-only folder vocabularies (Material, World, Harmonic language, Signature) and why they are fixed lists | current (built 2026-09-14) |
| [sa3-studio/TRAINING.md](sa3-studio/TRAINING.md) | **the training reference.** Dataset prep, the underfit dashboard, LoRA settings, and §7 Learnings — exposures-per-cue, what over- and under-fitting look like | current, actively extended |
| [sa3-studio/SETUP.md](sa3-studio/SETUP.md) | standing up underfit + the MLX trainer locally, one portable folder | current |
| [sa3-studio/COLAB-TRAINING.md](sa3-studio/COLAB-TRAINING.md) | training on a rented GPU — what was run and why | current |
| [sa3-studio/COLAB-CLI-TUTORIAL.md](sa3-studio/COLAB-CLI-TUTORIAL.md) | Colab from the terminal, for someone who has never used it | current |
| [sa3-studio/GENERATE-UI.md](sa3-studio/GENERATE-UI.md) | design note for the Generate window; records what shipped and the cfg measurement | current (2026-09-14) |

### Research and decisions — dated snapshots

These are **session records**, not living documents. They are accurate as of their date
and should be read that way; do not "update" them, write a new one.

| doc | what it is |
|---|---|
| [NOTES.md](NOTES.md) | UI research (SonikSearch, Tuva), SA3 caption format findings, underfit tag-key behaviour |
| [sa3-studio/PACKAGING.md](sa3-studio/PACKAGING.md) | embed Python or port SA3 to C++? Measured 2026-09-13 |
| [sa3-studio/SA3-INFERENCE-AND-TRAINING.md](sa3-studio/SA3-INFERENCE-AND-TRAINING.md) | how SA3 training and inference work, verified against source, 2026-09-12 |
| [sa3-studio/RUNBOOK-dune-lora.md](sa3-studio/RUNBOOK-dune-lora.md) | the first LoRA run, start to finish. Historical — `zvq` has since trained |
| [spike/README.md](spike/README.md) | Phase 0: the six risky assumptions, each proved in a standalone CMake project |

### Small and local

| doc | what it is |
|---|---|
| [lab/README.md](lab/README.md) | the offline Python env for model conversion and parity checks. Never shipped, never called at runtime |
| [tests/README.md](tests/README.md), [fixtures/README.md](fixtures/README.md), [models/README.md](models/README.md) | one-liners for their folders |

---

## Where the code lives

```
src/mira/analyze/     the analysis stages — Essentia, ONNX heads, router, descriptors
src/mira/caption/     CaptionFields (measurement → words) + Sa3Renderer (words → prompt)
src/mira/db/          Database — the one SQLite surface
src/mira/main.cpp     the CLI: scan / analyze / caption / tag / similar / search / stats
src/mira_ui/Source/   the JUCE app
scripts/              retag-latents.py and friends
taxonomy/             *.yaml label normalisation
sa3-studio/           the LoRA training rig (underfit + stable-audio-3), plus latents/
```

**`mira_core`** is the shared static library — `Database`, `CueDetection`, `Groove`,
`CaptionFields`, `Sa3Renderer`. Anything there is pure arithmetic over stored JSON: no
audio, no models. That is what lets the UI run it inline instead of shelling out, and it
is why the CLI and the UI can never disagree about what a caption says.

---

## Conventions that are load-bearing

These are not style preferences. Each one exists because breaking it caused a real bug.

1. **Omit rather than guess.** Every caption field has a confidence gate; a field that
   cannot be supported is left out. Asserting "128 BPM" over a piece that runs 60–175 is
   the exact failure PRD §12.6 warns about. An omitted field is the system working.
2. **Measure the corpus, then set thresholds. Never guess first.** Every bucket boundary
   in `CaptionFields.h` is a measured p33/p66 with the distribution in the comment above
   it. This is also how the flat-groove bug was caught.
3. **Store everything, caption selectively.** Raw numbers go in `files.machine`; only
   some become words. `onset_times` being stored is what made the groove fix free —
   no re-analysis of 94 files.
4. **A caption word is only useful if it varies across the corpus.** A word every file
   carries is absorbed into the trigger and cannot be turned up or off at generation
   time. See [ANALYSIS.md §3](ANALYSIS.md).
5. **`human` always outranks machine** (PRD §11), and a re-analysis never overwrites it.
6. **Never silently fall back.** A failed model returns null plus a recorded error, never
   a substitute value. The previous generation's heuristic returning `1.0` was
   indistinguishable from a real answer.
7. **Reject a second measurement of the same axis.** `spectral_centroid` (r=0.91 with
   flatness), tempo drift (r=0.79 with jitter) and `flux_stddev` (r=0.71 with `flux_mean`)
   are all measured and all deliberately uncaptioned.
8. **Open the UI before claiming it works.** Compiling is not verifying. This has been got
   wrong more than once.

---

## Recent work

Newest first. Keep this current — it is how the next session finds the thread.

### 2026-09-16 — meter shipped, and a confidently wrong grid fixed

- **Meter detection built and surfaced** ([TASKS.md Phase 7](TASKS.md) tasks 1, 2, 2b).
  Ported from the collaborator's tool, parity-verified on identical inputs, and now shown:
  a read-only Meter row in File Details, green meter bars on the waveform, and bar spread
  in the groove panel — red past 1.5, where the measurement says the beats drifted.
- **`bar_spread` is mira's first confidence signal about its own beat grid.** Its absence
  is how the flat-groove bug survived.
- **Fixed a grid that was confidently wrong.** A 100 BPM loop (filename is ground truth)
  reported 149.4 BPM at 2.33x strength. The onsets were right and both estimators were
  right; `pickBeatOctave` only offered integer multiples, the fit had landed on 2/3 of the
  beat, and nothing could reach 99.9 — so it fell through to a tempo prior and labelled
  the guess "onsets". 23% of the library was getting its grid that way. Added 1.5, 2/3 and
  0.75; an uncorroborated grid now derives nothing. Strength could not have caught this:
  concentration was HIGHER at the wrong subdivision than at the true beat.
- **`kMeterMinBeats` was too permissive** — a 16-beat loop "detected" a bar of 6 from 2.7
  repetitions. The peak search now requires whole cycles.

### 2026-09-16 — meter and bar detection assessed

- Evaluated a collaborator's bar-detection tool (`~/Downloads/deploy_onnx_image`). It is
  **the same Beat This model mira already runs**, plus madmom's DBN, plus an original
  meter/bar layer. Written up as **[TASKS.md Phase 7](TASKS.md)**.
- **The meter layer needs no madmom** — `detect_meter(feats, env, beats, ...)` takes beats
  as an argument, so it runs on any grid, including mira's fitted one. That is the whole
  plan: take the separable half, skip the expensive half.
- **madmom is not a C++ library** (it was assumed to be): 37 `.py`, 4 Cython `.so`,
  69 `.pkl`. No linkable API; a port means reimplementing the Viterbi and state spaces.
- Meter detection verified working on NIN: La Mer -> 6 (spread 1.02x, correct),
  The Frail -> self-flagged uneven rather than guessing. mira cannot do this at all today.
- On Two Fingers its grid loses to mira's fitted grid 8/8 by neutral onset-phase
  concentration — but that is the hardest case and the corpus the fitter was built for.
  It says nothing about ordinary or odd-meter music.
- Also measured and rejected this session: raising `kCaptionGenreThreshold` (the
  `Experimental` catch-all has the HIGHEST median score, 0.178, so a higher bar kills the
  specifics first), and `mtg_jamendo_genre` as a genre replacement (`idm` never once
  clears 0.10 on 94 Amon Tobin files; `electronic` fires on 94/94). Jamendo is still worth
  a narrow supplement for `hiphop`/`soundtrack`/`triphop`/`classical`, which Discogs-400
  demonstrably misses.

### 2026-09-15 evening — first Amon Tobin run

- **`amt` launched** on JarvisLabs (A30, 217.18.55.220): 94 files, 20,000 steps, batch 4,
  512 latent crop. That is **833 exposures per cue**, mid-window against TRAINING.md
  §7.1's 700-900 target, and the same short-crop shape as `xyr-short` — the best LoRA to
  date. ~6h50m at 1.23 s/it.
- **The first run trained on `groove`/`swing`/`low_end`/`motion`.** 17 tag keys, with
  `prompt` and `trigger` deliberately off: the prepend already supplies the trigger at
  80%, and `prompt` duplicated every other field and dragged `Length:` back in.
- **Dataset prep**: all 94 source sidecars verified current before the encode consumed
  them, stand-ins written, tags verified to render by running underfit's own tag reader
  on the box rather than assuming.
- **Found a dashboard trap**: three of eight datasets were flagged `status: error` purely
  because `details.json` was missing from their shadow latent dir — including the fresh
  Amon Tobin import, which would have flipped on the next restart. Documented in
  [sa3-studio/TRAINING.md](sa3-studio/TRAINING.md); all eight now validate clean.
- Open: the BPM **octave** is inconsistent across the Two Fingers catalogue (six tracks
  read 159 where seven read 79 — the same grid, a different multiple called "the beat").
  The grid is right on all 94; only the convention varies. Left as-is rather than
  guessing, and worth a measured fix later.

### 2026-09-15 — groove, and the flat-histogram bug

- **Found and fixed a measurement that returned the same answer for every file.** Swing
  read 0.54–0.57 and syncopation 68–72% for everything in the library, orchestral cues
  scoring identically to programmed halftime beats. The cause was the grid, not the
  onsets: against `beat_this_beats` the onset phase histogram was flat on **all 94** Amon
  Tobin / Two Fingers files (median peak/uniform 1.17, ceiling 1.36).
- **`mira::analyzeGroove`** (`src/mira/analyze/Groove.h`, in `mira_core`) fits the beat
  period from the onsets themselves by maximising circular phase concentration, and uses
  the stored BPM scalars only to pick the octave. Median peak/uniform 1.93, max 6.93; it
  wins on 94 of 94. No re-analysis needed — `onset_times` were already stored.
- **BPM corrected.** It now prefers the fitted grid where the onsets lock.
  `essentia_bpm` agrees with that grid 82% of the time, `beat_this_bpm` 13%. Two Fingers
  is a ~79.5 BPM catalogue `beat_this` reported as 86.5, 90.8, 104.8, 127.0, 130.2.
- **Four new caption fields**: `groove`, `swing`, `low_end`, `motion`. Thresholds from the
  94-file corpus. `pocket` and `syncopation` measured and deliberately not captioned —
  reasons in [ANALYSIS.md §5](ANALYSIS.md).
- **Waveform view** draws onsets, the fitted grid beside `beat_this`'s, and the phase
  histogram, so the measurement is checkable by eye rather than trusted.
- **Editable** in File Details (with vocabulary dropdowns) and via
  `mira tag --groove/--swing/--low-end/--motion`.
- [ANALYSIS.md](ANALYSIS.md) written; this file created.

### Earlier

- **2026-09-14** — folder-level tagging ([CAPTION-TAGGING.md](CAPTION-TAGGING.md));
  `palette` and `timing` caption fields; the Generate window's cfg/negative-prompt work
  and the prompt builder ([sa3-studio/GENERATE-UI.md](sa3-studio/GENERATE-UI.md)).
- **2026-09-12/13** — SA3 training stood up end to end; first LoRAs trained (`zvq` Dune,
  `xyr` Mad Max), then `dkt`, `lrt`, `lou`, `nin`. Learnings in
  [sa3-studio/TRAINING.md §7](sa3-studio/TRAINING.md).
- **2026-09-10** — Phase 0 spikes all passed; Phase 2 complete.

---

## ⛔ Start here next session

**The tempo detection is wrong and confidently wrong** — the user's finding, on tracks
whose tempo they know. Everything built on it (groove, swing, the fitted grid, meter) is
suspect until settled. **Get their ground-truth tempos before measuring anything**, and do
not defend the pipeline with internal consistency checks: every test on 2026-09-16
compared one estimator to another or to a grid derived from the same onsets, which can
agree and be wrong together. Full note at [TASKS.md Phase 7](TASKS.md).

## Open threads

- Calibrate `groove`/`swing` thresholds against a second beat corpus — they are set from
  one artist, which is one corpus more than the old numbers had but still only one.
- Re-run the film-score sets with `--groove --recheck-tempo`; their BPMs were derived
  from `beat_this` and have never been checked the way the Amon Tobin set now has.
- `sub_ratio` may want an 8192-point pass (5.4 Hz bins) if 21.5 Hz bins smear.
- Higher-step re-runs of `lou` and `dune`.
- Decide the BPM octave convention for halftime material — see the 2026-09-15 evening
  entry. Affects captions, not grids, and needs a measurement rather than a preference.
- TASKS.md Phase 6 — 29 items open; **Phase 7** (meter, bar lines, grid confidence) newly
  opened 2026-09-16.
