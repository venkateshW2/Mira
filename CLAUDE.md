# CLAUDE.md — the map of mira

The index to every document in this repo: what each one is, whether it is current, and
when to read it. **Start here.** If you are picking the project up after a break, or you
are an agent with no memory of the last session, this file is the entry point.

**Last updated: 2026-09-17 (evening).** Keep the *Recent work* log at the bottom current — that is
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
| [MIRA-GENERATE.md](MIRA-GENERATE.md) | **the generation-and-delivery workflow**: projects as folders, cues, keep-to-cue, cut/fade, export. Its own 7-phase task list | live — **phases 1–5 built**, 6–7 open (2026-09-17) |

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
| [ACESTEP-INFERENCE-PLAN.md](ACESTEP-INFERENCE-PLAN.md) | **ACE-Step 1.5 research + a phased plan to run generation inside mira with no Python sidecar.** 2026-09-16. Proposal — nothing built. Phase 0 is a half-day listening test that can kill it |
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
                      TakeStack.h / InpaintStrip.h / LoraLibraryWindow.h — MIRA-GENERATE
src/mira/db/          + PathNormalise.{h,cpp} — NFC/NFD path matching (convention 9)
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
   wrong more than once, and again on 2026-09-17: a waveform selection that had worked
   since it was written was reported as a finished feature while being drawn as a 10%
   wash nobody could see.
9. **Never compare two file paths with `==`.** macOS returns the same filename as
   different bytes depending on which API asked -- `readdir` gives NFD, JUCE's directory
   walk gives NFC -- so `"Göransson"` discovered one way never matches the same file
   discovered the other. Go through `mira::pathsEquivalent` / `Database::findByPath`
   ([PathNormalise.h](src/mira/db/PathNormalise.h)). This hid for months and cost most of
   a day: 46 of 82 files in one folder read as unanalysed while holding 80 KB of analysis
   each, and `mira analyze` answered "nothing to analyze" for paths it had just written.
10. **Measure before explaining.** The bug above survived three rounds of plausible
    theories -- stale rows, interrupted batches, a missing refresh -- each argued from
    screenshots. One `MIRA_TRACE_ROWS` pass over the real library answered it in a minute.
    A row showing a dash means `inDatabase && analyzedAt` is false; from the outside the
    two halves look identical, so only an instrument can tell them apart.

---

## Recent work

Newest first. Keep this current — it is how the next session finds the thread.

### 2026-09-17 night — four LoRAs queued across two boxes

Ludwig, Cortini, Ryuichi and ametsub encoded, uploaded and training on two A30s
(`217.18.55.28`, `217.18.55.56`). Checkpoint and demo every 1000, DoRA-rows, rank 16,
LR 1e-4, batch 4, random crop.

| set | trigger | crop | steps | rep/win | measured |
|---|---|---|---|---|---|
| Ludwig | `lgr` | 512 | 16000 | 157 | 1.36 s/it -> 6.0 h |
| Cortini | `acr` | 512 | 14000 | 149 | 1.36 s/it -> 5.3 h |
| Ryuichi | `rsk` | 512 | 8000 | 158 | queued |
| ametsub | `ams` | 320 | 9000 | 180 | queued |

All four land within 149-180 rep/window, against `xyr-short`'s 157 -- the best LoRA to
date -- and were computed from the real per-file durations, not from a nominal length.

- **The `details.json` trap fired again, on all four.** The importer links the latents
  and writes per-file JSON but never copies `details.json` into the shadow dir. All four
  read `ready` at import and would have flipped to `error` at the next dashboard restart.
  Copied, then the dashboard was **restarted to prove it held** -- checking at import
  time proves nothing, because the broken state is identical to the good one until then.
- **Box B's `QUEUED.json` was stale**: still `nat` from 16 Sep. A `nohup ... &` inside an
  `ssh` command keeps the channel open and the call hung, so the copy never ran while the
  arming appeared to succeed. Found by reading the queued run's `name` rather than
  trusting the exit status. `setsid nohup ... </dev/null` detaches properly.
- `pgrep -f queue_next.sh` **matches the command asking the question**, so the first
  status report claimed 3 watchers where there was 1. An inflated count is exactly what
  would hide a second watcher racing the first; `pgrep -cxf` with the full argv is right.
- Dune's latents were never unzipped -- `dune-ost-latents-same-l.zip` sat beside the
  `latents/` folders it belonged in, so `zvq` could not appear in the prompt builder,
  which builds its trigger list by scanning those folders. 38 sidecars extracted.

### 2026-09-17 evening — a file whose name has an accent in it

**The bug that ate the afternoon, and the one worth remembering.** Analysis appeared not
to run: right-click Analyze did nothing, rows stayed dashed, and re-running the CLI said
`nothing to analyze` for files that plainly were not analysed.

macOS returns the same filename as two different byte sequences depending on which API
asks. mira had both in play at once:

| discovered by | bytes for `ö` | form |
|---|---|---|
| scanner (`readdir` / `std::filesystem`) | `6f cc 88` (`o` + combining) | NFD |
| file table (JUCE `RangedDirectoryIterator`) | `c3 b6` | NFC |

`files.path` is compared with `=`, so those never match. **Every file with a decomposable
character in its name was invisible to the file table's database pairing** — and because
the UI hands the CLI *JUCE's* paths, `mira analyze` could not find them either and
correctly reported nothing to do.

- **Measured, not argued.** `MIRA_TRACE_ROWS=1` (kept — `FileTable.cpp`) prints every row
  the walk could not pair with an analysed record. Against the real library: **46 of 82
  files in one folder NOT FOUND**, each holding 50–80 KB of analysis. After the fix, 0;
  48 rows recovered library-wide.
- **Fixed at the lookup, not in the data.** `Database::findByPath` tries the exact bytes
  first and the other normalisation only on a miss, so the common case costs nothing and
  both callers are fixed at once. [PathNormalise.h](src/mira/db/PathNormalise.h) wraps
  CoreFoundation; `mira_core` now links it.
- **Deliberately not a migration.** Rewriting 2,500 rows to one form fixes today and
  breaks again the moment anything writes the other; both forms are legitimate and the
  filesystem accepts either. Matching on a canonical key is the invariant.
- Three rounds of plausible wrong theories preceded the measurement — stale rows, an
  interrupted batch, a missing refresh. Conventions 9 and 10 exist because of this.

**Two real gaps found on the way, both fixed and both worth keeping:**

- `enqueueAnalyze` returned **silently** when everything asked for was already queued, and
  said nothing when it did queue. A slow job that says nothing is indistinguishable from
  one that never started.
- The file table is a snapshot, and nothing that changed the library from **outside** the
  window could tell it to look again — a `mira analyze` run in a terminal (how long runs
  survive a rebuild) left the rows stale indefinitely. Now a 2.5 s `stat()` of the
  database file, plus `File → Reload from Library`.

### 2026-09-17 — MIRA-GENERATE phases 1–5

The generation-and-delivery workflow, built from [MIRA-GENERATE.md](MIRA-GENERATE.md).
Phases 1–5 done, 6–7 planned. Nothing that already worked was changed.

- **Projects are folders** (§3.1). `File → New Project` creates a directory, registers it
  as a `ui_folder_roots` row under a new PROJECTS group, and becomes the output folder.
  The group declares category **`projects`**, not `music` as the plan first said —
  `category` is the group's *identity key*, so a PROJECTS group under `music` would never
  have been found and every project would have been filed into MUSIC, silently.
- **`ui_settings(key, value)`** added: `current_project`, `lora_names`, and the audio
  device state — which had never persisted at all, because the app has no `PropertiesFile`
  and `initialiseWithDefaultDevices` ran unconditionally at every launch.
- **Window lifetime** (§3.3): closing the browser used to quit the app outright, killing a
  running generation. It hides while a generation window is up; `Window → Library` returns.
- **Keep files a take into a cue** (Phase 3): `<project>/<cue>/{project}_{cue}_v{n}`, the
  version read from the cue folder rather than a session counter. Raw takes live in
  `<project>/takes/` — never the project root, which *is* the deliverable.
- **The take stack** (Phase 4, `TakeStack.h`): KEPT / TAKES / DISCARDED, collapsible, rows
  expanding independently. One `WaveformView` moved between rows rather than one per row —
  it owns an `AudioDeviceManager`, so a waveform per take would be an audio device per
  take. Cmd-click multi-selects for bulk keep/discard.
- **Cut and fade** (Phase 5): a `segments` row plus `segments.human` for fades and gain.
  Nothing touches the audio until export. `WaveformView` already had click-drag selection;
  it was invisible, drawn as a 10% wash with no edges.
- **`lastRecipe` was wrong the moment two takes could coexist** — Keep wrote whatever was
  generated *most recently*. It reads the take's own `.json` sidecar now.
- **The generate window is two panes** (takes left, everything else right), both scrolling,
  after a single top-down column kept laying controls out at zero height off the bottom —
  the prompt builder's clipped-fields bug for the third time in this project.
- **`Window → LoRA Library`** names checkpoints (`tar-step20000-epoch833` → `TRENT-ATTICUS`),
  adds them by symlink, removes them. Names live in `ui_settings` keyed by filename.
- **Inpainting is extension**, verified in `sa3_mlx.py`: init audio is zero-padded to the
  requested duration, so a range past the end of the audio generates a continuation. The
  strip's timeline is the duration, not the file length.
- **The LoRA step window is in sampler steps** and compared against Steps, so a slider
  running to 50 while Steps was 8 offered 42 positions that did not exist.

### 2026-09-17 — the tempo was wrong, and now it is not

The user gave six tracks with the tempo they know each one to be. Measured against that
list rather than against another estimator, the whole rhythm stack failed at once. Four
separate faults, each fixed and each re-measured against the same six.

| track | theirs | mira before | mira now |
|---|---|---|---|
| Smurf (feat) | 86 | 127.0 | **86.49** |
| Smurf (instr) | 86 | 130.2 | **86.49** |
| Marine Machines | 97 | 96.5 | **97.01** |
| Surge | 87 | 86.4 | **86.49** |
| Crunch Rhythm | 87 | 149.4 | **86.00** |
| Deep Jinx | 86 | 86.0 | **85.99** |

- **The onsets came from Essentia and inherited its tempo error.** The user said so
  outright and was right. Against their tempos the stored onsets phase-locked at
  R = 0.001-0.024 -- zero, on 599-1278 onsets per file -- and instead peaked at 158-178
  BPM on five unrelated songs. A near-constant answer across different music is an
  algorithm artefact. Everything downstream was measuring it.
  **`mira::detectOnsets`** (`src/mira/analyze/Onsets.h`) replaces `OnsetRate`: one STFT,
  log-mel, positive first difference, adaptive-threshold peak picking, hop 256 (5.8 ms,
  half `OnsetRate`'s step), centred frames. Onset phase concentration against the beat
  grid went from **flat on all 94 files (median 1.17, ceiling 1.36)** to 1.33-3.07.
- **The tempo was a MEAN of beat intervals, across two octaves.** On Smurf the DBN's beat
  list holds a 0.70 s cluster (86 BPM) and a 0.35 s cluster (172); the mean is 127, a
  tempo occurring nowhere in the song. The collaborator's tool uses the median. mira now
  does better than either: **`fitBeatPeriod`** octave-folds the intervals and
  least-squares fits one period, because the network runs at 50 fps and 87 BPM (0.690 s)
  falls exactly between the 0.68 and 0.70 bins -- no median of quantised intervals can
  ever report it. The fit resolves the period to under a millisecond.
- **`pickTempoOctave`** folds the fit to the octave nearest 120 BPM in log space. This is
  a PRIOR and is labelled one. The downbeats are no help: the DBN halves the bar along
  with the beat (Smurf reports bars of 1.42 s), so bar/4 repeats the same error. Measured,
  not assumed. Right 6/6, and it will be wrong somewhere near 70/140 -- the open
  octave-convention thread.
- **`gridStability`** is mira's first honest confidence number about its own beats: the
  share of intervals within 25% of the median. It separates the six completely --
  0.99/1.00/1.00 on the correct grids, 0.64/0.67/0.85 on the wrong ones. Straight from
  the collaborator's `grid_stability`. Its absence is how a confidently wrong tempo stood.
- **Cross-check adopted**: below 0.90 stability the minimal peak-picked grid is decoded
  too and the steadier wins (`process_audio_both` -- both postprocessors off ONE model
  pass). It fires on exactly the three bad tracks. On these three the minimal grid is also
  bad, so the DBN is kept -- but the flag is now raised either way.
- **madmom did not need porting.** `vendor/beat_this_cpp/Source/DBNPostprocessor.cpp` is
  already a madmom-compatible Viterbi, and its config and activation formula match the
  reference exactly. The failure was never the DBN; it was what mira did with its output.
- **Bars ruler in the waveform** (`WaveformView::RulerMode`): bar numbers off the detected
  downbeats, `bar.beat` once beats are legible, radio-paired with the time ruler in the
  lane menu. A pickup before the first downbeat is left unnumbered rather than given a
  bar 0. **Written and compiling; not yet seen rendering.**

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

**MIRA-GENERATE Phase 6 — export.** Render trim + fades to wav **at the take's native
44,100 Hz** (SA3 generates at 44.1 and nothing else; the playback path resamples to the
device rate through JUCE's `ResamplingAudioSource`, which is fine for auditioning and is
not a mastering SRC), delivery name from the template with tokens dropped when a field is
unsupported, export a cue or a whole project. Details in
[MIRA-GENERATE.md](MIRA-GENERATE.md) §4.

**Known and left alone:** 8 rows under `TO-TRAIN/AlessandroCortini` point at Nate Smith
files that were moved to `TO-TRAIN/natesmithdrums` and rescanned there. They are stale
rows for files that no longer exist at that path and will never analyse. Harmless.

---

## The LoRA training thread (older)

**Four LoRAs to train, and only four: `amt`, `lou`, `tron`, `trn`.** Everything else is
either already good or deliberately left alone. The decision and the sizing are in
[TASKS.md Phase 8](TASKS.md); the short version is below.

- **`nin` is NOT being redone.** Its 183 files were analysed under the old onsets and a
  mean-interval tempo, so the analysis is wrong — but the LoRA trained off it is good, and
  a good LoRA is the deliverable. Known-wrong and left alone on purpose, not an oversight.
- **`lrt` is not being retrained either.** At crop 256 it already has zero short-window
  files and a median cue of 222 s; shortening the crop costs the long orchestral arc,
  which is the point of LOTR.
- Local prep (re-analysis of all four sets) is queued and running. After it lands:
  re-measure the `groove`/`swing` tertiles, then `scripts/retag-latents.py` for the sets
  that already have latents, and `pre_encode_mlx.py` for the two that do not.

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
