# CLAUDE.md — the map of mira

The index to every document in this repo: what each one is, whether it is current, and
when to read it. **Start here.** If you are picking the project up after a break, or you
are an agent with no memory of the last session, this file is the entry point.

**Last updated: 2026-09-20.** Keep the *Recent work* log at the bottom current — that is
this file's second job.

---

## What mira is, right now

A local audio-understanding tool: point it at folders of samples, stems and full tracks,
it analyses them and builds a searchable, captionable index. Everything runs locally on
Apple Silicon. No server, no cloud, no GPU assumption.

**Built and in daily use.** A C++20/JUCE desktop app (`MIRA.app`) plus a `mira` CLI over
one SQLite library. As of 2026-09-19 the library holds **3,228 scanned files, 1,390
analysed**, and has produced the caption sets for a dozen trained SA3 LoRAs.

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
| [ARCHITECTURE.md](ARCHITECTURE.md) | **how mira is put together**: the two binaries and `mira_core`, the schema, the analysis pipeline, the app's windows, **the four audio paths**, the bridge to Python, the threads | current (2026-09-19) |
| [README.md](README.md) | what mira is, for someone who has never seen it | current |
| [ANALYSIS.md](ANALYSIS.md) | **what mira measures and what reaches the model.** Every caption field, what it means, how to edit it, what is deliberately not captioned | current (2026-09-15) |
| [PRD.md](PRD.md) | the design: stack, models, phases, licence reasoning, every "why this and not that" | current as design; §-numbers are cited throughout the code |
| [TASKS.md](TASKS.md) | the build checklist, phase by phase. Phases 0–5 complete, **Phase 6 in progress** | live — tick items here |
| [MIRA-GENERATE.md](MIRA-GENERATE.md) | **the generation-and-delivery workflow**: projects as folders, cues, keep-to-cue, cut/fade, export. Its own 7-phase task list | live — **phases 1–5 built**, 6–7 open (2026-09-17) |
| [MIRA-VIDEO.md](MIRA-VIDEO.md) | **scoring to picture** — a video window slaved to the transport, a locked reference track, timecode, markers, a cue sheet | live — **Phases 0–5 built and verified on screen** (2026-09-19) |
| [CANVAS.md](CANVAS.md) | **the block canvas** — the Blockhead-shaped experiment: blocks that own their generator, tracks that sum, and the `.mira` document | live — experimental, 2026-09-19 |
| [MIRA-BLOCKS.md](MIRA-BLOCKS.md) | **the block as a musical object** — tempo and key on the BLOCK not the track, blocks that follow other blocks, stretch as a take, and the block-as-sampler. Its own 5-step task list | live — **step 1 built and verified**, 2–5 open (2026-09-20) |

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
src/mira/export/      AudioWriter — the CLI's side of rendering
src/mira_ui/Source/   CanvasEngine.{h,cpp}  — the canvas MIXER (see ARCHITECTURE.md §6.3)
                      CanvasWindow.{h,cpp}  — the canvas: blocks, tracks, document, export
                      Sa3Worker{,Hub}       — the bridge to sa3_worker.py
                      Export.{h,cpp}        — the only code that renders a take to a file
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
11. **A claim verified by reading code is not verified.** "Everything outside an inpaint
    range stays bit-exact" was true of the LATENTS and false of the AUDIO -- the whole
    timeline is decoded and the source had to be encoded first -- and it sat in two
    documents for two days because nobody measured the output. -21.9 dB relative error
    after one extension, -14.5 dB after three. Convention 10 applies to reading, not just
    to arguing.
12. **State that outlives its action is a bug waiting to happen.** One Extend set an
    `initAudio` field that nothing cleared, and every generation afterwards was silently
    guided by the old block's audio -- invisible because the canvas hides that control,
    the block switch never cleared it, and the recipe never recorded it. Scope state to
    the action that wanted it, and record in the sidecar everything the request carried.

---

## Recent work

Newest first. Keep this current — it is how the next session finds the thread.

### 2026-09-20 (latest) — an SA3 extension holds tempo EXACTLY, n=2

[MIRA-BLOCKS.md](MIRA-BLOCKS.md) **step 2b — the measurement this whole plan was written to
be able to make.** Nobody knew, and there had never been an instrument to ask.

Two extension pairs, each split at the join the sidecar records, kept and new regions
measured separately:

| pair | parent | ext. KEPT | ext. NEW | stability |
|---|---|---|---|---|
| block 27, 30 s → 59 s (29 s new) | 107.14 | **107.14** | **107.14** | 1.000 |
| block 36, 30 s → 46 s (18 s new) | 142.86 | **142.86** | 69.77 | 0.795 |

- **The kept region is exactly in tempo in both** — the crossfade join confirmed rather than
  assumed.
- **Block 27's new material is exact**: 107.14 over 45 new beats, zero drift.
- **Block 36's new material is the SAME TEMPO reported an octave down**, and that is
  arithmetic: 142.86 bpm is a 0.4200 s beat, so half-time is 0.8400 s; the measurement says
  0.8600 s. The difference is **0.0200 s — exactly one frame of the beat network's 50 fps**.
  The −2.33% "drift" inside the octave is the network's own quantisation and nothing else.
  **Both pairs are consistent with the tempo being held exactly.**
- **The confidence gate caught the octave case** (0.795, refused, grid left alone) knowing
  nothing about octaves. Its first two real cases: one adopted, one refused, both correctly.
  The best argument yet for having built the gate before the snap.
- **It caught a flaw in the instrument before the instrument reported anything.** The drift
  summary would have said "tempo moves 142.9 → 69.8" — a 51% collapse, and its first answer
  would have been its first wrong answer. It folds to the octave now and says **"an octave,
  not a drift"**.
- **Needed no new analysis code.** The measured beats were already fetched, so drift across
  a take is arithmetic over what step 2 holds — the same dividend as the groove fix in
  September: store everything, derive at read time.
- Found on the way: **only 2 of 65 sidecars in a real project record `extend_from`** — and
  the provenance IS written when an extend runs, so the rest simply are not extensions. Four
  takes of growing length in one block look like a chain and are not one. Convention 12
  earning its place: without the sidecar this measurement was impossible, and with it it
  took one grep.

**This makes step 3 smaller than it was planned.** An extension that returns in tempo does
not need stretching; what it can need is its OCTAVE named correctly. So step 3 now opens
with a new item 3.0 — halve/double the block's tempo, a menu item and a keystroke, which
stretches nothing — and stretch-to becomes what it is actually for: conforming a *separate*
take to a block's tempo.

### 2026-09-20 — the block can be analysed, and four takes in ten are refused

[MIRA-BLOCKS.md](MIRA-BLOCKS.md) step 2, **complete and verified on screen by the user** —
*"yes now look correct and works"*.

**It took four passes, and every fault was found by the user looking at it.** The first
build compiled clean, launched, and was wrong in four separate ways no compiler and no
amount of re-reading would have caught. That is convention 8 demonstrated rather than
quoted, and the four are worth keeping because the next step will be tempted to repeat them:

1. **A button drawn underneath the block's name.** Where the header text may start was
   computed in TWO places, and the copy that draws the name did not know a third chip had
   been added — so `ANALYSE` was painted over and the report was simply "the button is
   hidden". `blockHeaderRow()` is one definition now. Also: a 15-px "A" beside two other
   15-px squares was the wrong control anyway. M and gain can be glyphs because you already
   know them; **a verb nobody has met has to be spelled**, and the word is now the state —
   ANALYSE / READING / MEASURED / UNSURE / FAILED.
2. **Two different claims drawn as the same mark.** An on-grid onset and a bar line were
   both full-height lines, so *"a bar starts here"* and *"a transient is here"* were
   indistinguishable — *"so what is what… iam confused"*. The rule now: **the grid is the
   only thing that spans the wave, onsets grow up from the floor, and an unmeasured grid is
   DASHED.** That last one answers *"so the bar doesn't shift according to the onset?"* in
   the picture instead of in prose. Off-grid onsets went from red to dim, because an onset
   that misses the grid is not an error, it is most of music.
3. **The one gesture every input device agrees on, spent on the wrong thing.** The plain
   wheel panned the timeline, so the canvas had **no vertical scrolling at all** — tracks
   below the window unreachable, waveform impossible to enlarge. Now wheel scrolls, shift
   pans, option zooms vertically, cmd zooms the timeline. The waveform also went from 512 to
   **128 source samples per thumbnail point** (11.6 ms → 2.9), and bar 1 became draggable by
   grabbing a bar LINE rather than only a 14-px footer where a miss starts a move.
4. **A synthetic grid, where a measured one was already in the database.** The bars were laid
   out from `beat_this_bpm` — one number for a whole take — so they drifted off the audio on
   anything not metronomic: *"the onsets are not actually aligning with the bars"*. The
   canvas now draws **the measured beats and downbeats themselves**. The browser's bar ruler
   has done this since it was written and carries a comment saying why in almost the user's
   words. **The canvas was repeating a mistake this project had already written down**, which
   is the strongest argument there is for reading the map before adding to it.

Step 1 drew the grid a block was ASKED for. This is the grid it actually GOT.

- **A canvas take is usually not in the library at all**, and `mira analyze --paths-from`
  **skips a path with no row**, then prints `nothing to analyze` and exits **0**. So without
  registering the take first the analysis would appear to run, succeed, and change nothing —
  a silent no-op wearing a success. It is registered with `upsertScannedFile` before the
  enqueue, which is also the side effect worth wanting: generated audio becomes searchable
  and captionable beside the source material instead of living only inside a project folder.
- **`--groove` is forced on**, whatever the Analyze menu is set to. Onsets and the fitted
  grid ARE the question a block is asking; a sticky session toggle deciding whether a
  feature works at all is the invisible dependency convention 6 exists for.
- **The confidence gate fires often, and that is the point.** Of 819 analysed rows carrying
  a `beat_grid_stability`, **468 clear 0.90 and 385 clear 0.95** — so the gate refuses
  roughly four takes in ten. Below it the block keeps its grid, does not become
  `tempoSource = "measured"`, and says *"measured 88.14 bpm at confidence 0.71 — below 0.90,
  so the grid is left as it was"*. **`Refused` is its own state, not a kind of `Failed`:**
  the analysis SUCCEEDED and its answer was not good enough to impose, which is the system
  working (convention 1), not something that went wrong.
- **The meter has a SECOND, different gate.** `beat_grid_stability` says the beats are
  steady; `meter_bar_spread` says whether they group into a bar the same way twice, and a
  steady grid with a drifting bar is exactly where 4 is a guess. 1.5 is the number the
  groove panel already turns red at, reused deliberately — two thresholds for one question
  would let the canvas adopt a meter another window is drawing in red. Measured over the 386
  rows with a meter that clear the stability gate: min 1.005, p25 1.075, **p50 1.336**,
  p75 1.647, max 39.4.
- **Onsets are drawn OUTSIDE the gate, and the footer now exists for them alone.** An onset
  is a measurement of the audio; whether the beat grid is trustworthy says nothing about
  whether a transient is where it is — and on a take whose grid was refused, the onsets are
  the only honest thing on screen about its timing. So a block with no tempo at all still
  gets a footer if it has onsets. One consequence, taken deliberately: **bar 1 now needs a
  tempo to be draggable**, not just a footer, or the gesture would move a number nothing
  draws.
- **Watchers, so a block cannot be stuck saying `analysing…` forever.** The analyze queue
  reports to the whole window; the canvas needs to know about ONE take. Watchers fire off
  the CLI's own `progress:` line and are **swept as failed when the queue drains without
  one** — a batch that died, or a file the decoder refused. Matched with `pathsEquivalent`
  and never with `==`: the CLI echoes the path back as the DATABASE holds it and the canvas
  asked with JUCE's bytes, which is convention 9 and the bug that cost most of a day.
- **The running state is shown where the ANSWER will appear.** The chip is 15 pixels and the
  status line is one repaint from being overwritten, so the tempo box itself reads
  `analysing…` — the place your eye is already on for a tempo, and the thing about to
  change.
- **`measurementFor()` lives with the database, not in the canvas.** The canvas has no
  database and is not getting one; the owner runs the analysis, reads `files.machine` back
  and hands the canvas a finished answer in its own vocabulary. The same line
  `loadSetting`/`saveSetting` already draw.

Checked before it was trusted rather than after: every JSON key in MIRA-BLOCKS.md §5 is
present and populated across the real library, and the two thresholds above are measured
distributions rather than round numbers (convention 2).

### 2026-09-20 — the block has a tempo, and the grid is drawn

[MIRA-BLOCKS.md](MIRA-BLOCKS.md) step 1, all six tasks. **Verified on screen by the user**,
including the round trip: a typed tempo and a dragged bar 1 both survive save and reopen.

**Musical time now lives on the BLOCK.** `Block` gained `tempo`, `meter`, `barOnePos`,
`key`, `tempoSource`, `tempoConfidence`, `followsBlockId`, `barOneIsHuman` and an empty
`slices` list. A block moves between tracks freely, so anything musical on a track is
positional — and a per-track tempo is the global grid reintroduced one level down, which is
what the canvas exists to avoid.

- **`followsBlockId` is serialised as the parent's INDEX, not its id.** Ids are handed out
  fresh on every load, so a saved id points at whatever block takes that number next time —
  the same trap `audioBlockId` is deliberately left unwritten to avoid. The index is stable
  because `toJson` writes blocks in the order `fromJson` reads them.
- **`musicFromTake()` reads the take's own sidecar, never the block's `settings`.**
  `settings` is the recipe you are about to generate WITH, so reading it would let a tempo
  you just typed describe audio made before you typed it. A take that says nothing CLEARS
  the old numbers rather than leaving a grid drawn over audio it was never about.
- **The slice list went in now, empty.** It is the one part of MIRA-BLOCKS that is
  expensive to retrofit, because it changes the document.
- **The grid is a footer, not an overlay.** Beat lines through the waveform are what makes a
  drawn grid start to feel like one you have to obey. Density degrades in steps — beats,
  then bars, then the footer itself — because a grid you cannot count is not a smaller grid,
  it is noise. `gridFooterHeight()` is ONE decision asked by both the painter and the
  waveform; two would leave a mystery empty band whenever they disagreed.
- **The tempo field was built as a typed box and rejected on sight.** A `TextEditor` popping
  up over a block is a modal moment in the middle of arranging. It is a DRAG box now, drawn
  and behaving like the gain box three pixels to its left: whole bpm, shift for 0.1,
  double-click for the recipe's answer back. The lesson is the cheap half of convention 8 —
  it compiled, it worked, and it was still the wrong control.
- **A split keeps the parent's bar numbers, for free.** Bar 1 is stored in SOURCE time, so
  the right half of a cut at bar 9 goes on saying bar 9 wherever it is dragged. The user
  asked which it should be; the answer was already built, because the source-time decision
  made it correct without code. **Bar 1 starts here** in the block menu is the other answer.
- **Measured before building, not after:** of 45 sidecars in a real project, 33 carry `BPM:`
  and 23 carry `Keyscale:`. So a quarter of takes legitimately draw no grid — which is why
  a block with no tempo still shows a faint dash in its tempo box rather than nothing at
  all. Those are exactly the blocks someone needs to be able to set a tempo on.

### 2026-09-19 — timecode, several reels, and the spotting notes

[MIRA-VIDEO.md](MIRA-VIDEO.md) Phases 3, 4 and 5. **Built and confirmed working by the
user** — they drive the UI checks now, which is how three real faults surfaced in this
session within seconds of each feature landing.

**Markers took three passes, and every fault was invisible to the compiler:**
`paintMarkers` was written and NEVER CALLED (the edit that should have added the call was
made conditional on the call already being there — *assert that an edit applied*, the same
lesson as the canvas entry below, now twice); `Cmd-M` is **Minimise** on macOS, so the
window shrank to the dock and no marker appeared; and then the labels collided with the
ruler's time ticks, which is what drawing them on the ruler was always going to do. They
have their own row now, are draggable by the whole label, and have a list window with
go-to — the third thing this canvas has had to give a row of its own, after the picture
and the reference.

- **Timecode** ([Timecode.h](src/mira_ui/Source/Timecode.h), header-only and UI-free). The
  ruler, the transport clock and every block header read the same way. **The two rates are
  separate**: a 23.976 file is numbered in 24 fps timecode — the frame COUNT comes from the
  real rate, the display divides by the NOMINAL one, and collapsing them drifts 3.6 seconds
  an hour against the editor's clock. Drop-frame is offered only where it exists.
  Checked against SMPTE facts rather than against itself, 9 of 9; **two of the first run's
  failures were wrong expectations of mine, not wrong code**, which is worth recording
  because a test trusted more than the thing it tests is how a correct implementation gets
  "fixed" into a broken one.
- **Several reels on the one video track**, with TWO `VideoComponent`s: the one you watch
  and one parked on the next clip's first frame, swapped at the boundary. A jump into a reel
  that was not pre-loaded loads there and then and **says so** — knowing when the pre-load
  missed is how six seconds of lead gets tuned rather than guessed. The gap between clips is
  black, not the last frame held.
- **`audioBlockId` is deliberately not serialised.** Ids are handed out fresh on every load,
  so a saved one would point at whatever block took that number next time; the clip-to-
  reference link is rebuilt from the geometry.
- **Markers, and `Cmd-shift-M`: a block from the playhead to the next marker.** That is the
  whole gesture of scoring to picture — the start and the out-point are what you know, and
  the length follows. Snapping is by PIXELS, not seconds: a snap a second wide zoomed out
  and a frame wide zoomed in is one nobody can predict.
- **A cue sheet**, in timecode, sorted by start — the canvas's own list is in the order
  blocks were made, and an out-of-order cue sheet cannot be read against picture.

Also this session: **tracks reorder** (drag a header, or `Cmd-↑`/`Cmd-↓`) with ONE
permutation applied to every per-lane list, and **per-track height with a padlock**, the
reference locked by default — one concept, since a lane with a height of its own is exactly
a lane the zoom leaves alone.

### 2026-09-19 — the reference track, and two bugs the UI found

[MIRA-VIDEO.md](MIRA-VIDEO.md) Phase 2. The film's audio arrives as a block on a reserved
REFERENCE lane: locked to its picture, excluded from every export, and still carrying a
fader, a mute and a meter, because scoring to picture means riding the reference under the
cue constantly.

- **Two routes, and which one ran is always said.** An `.mp4` is read straight from the
  file (44.1 kHz, 2 ch, 93.0 s). A `.mov` — which Phase 0.1 measured JUCE refusing 4 times
  out of 4 — goes through **AVAssetReader**, written to `reference/<name>.wav` beside the
  project: 0.3 s for 51.9 s of 48 kHz stereo. They cost wildly different amounts and a user
  who cannot tell them apart cannot explain why one cut opened at once and another took
  three minutes.
- **The extractor's first version read nothing, silently.** Non-interleaved float:
  `canAddOutput` said yes, `startReading` said yes, and every `copyNextSampleBuffer`
  returned nothing while the status never went to Failed. `AVAssetReaderAudioMixOutput`
  wants interleaved. The error now carries buffer count, frame count and reader status —
  "produced no audio" sent the first round looking at the wrong half.
- **The exclusion is measured, not asserted.** Two audio tracks plus a 51.9 s reference
  exported exactly two files, and the note said `(reference track excluded)`.
- **A project was being opened TWICE at launch** — `showCanvasWindow` bound
  `ui_settings.current_project`, then the launch window opened its own choice, and the
  second pass threw away everything the first built. Invisible while the work was cheap;
  with a film attached the trace read `reference: read straight from finalucut.mp4` twice,
  which on a 40-minute reel is the expensive half of opening a project, done for nothing.
- **The same double-load silently deleted the reference lane**, because the "same film,
  do not reload the picture" guard is about the PICTURE — which is expensive to reopen —
  and the reference is rebuilt state. Whether it is missing is a different question from
  whether the film changed.
- **`MIRA_TRACE_VIDEO=1`** prints every canvas note to stderr (the `MIRA_TRACE_ROWS`
  precedent). Both bugs above were found with it, after the status line — one line anything
  can overwrite — had silently dropped the message that would have explained them.

### 2026-09-19 (later still) — picture, and a spike that measured the wrong thing twice

[MIRA-VIDEO.md](MIRA-VIDEO.md) Phase 0 and Phase 1. `spike/07_video_sync/` answered the
three questions that decide the shape of the rest, and all three changed the plan.

- **It is an offset, not a drift.** 700 seconds of muted `VideoComponent` against samples
  the audio device actually consumed: start latency **−290.3 ms, constant**, worst drift
  from it **8.1 ms** (0.20 frames at 25), and the worst value was reached in the first 30
  seconds and never grew. So sync is a one-time seek, and the rate-nudge of the plan is a
  safety net rather than the mechanism.
- **`getVideoDuration()` returns 0.00** — for the whole run, after polling five seconds,
  while the same player was plainly playing. The clip's length comes from our own
  `AVURLAsset` query ([VideoNative.mm](src/mira_ui/Source/VideoNative.mm)) instead.
- **`.mp4` reads, `.mov` does not.** 4/4 and 0/4, despite `.mov` being in
  `CoreAudioFormat`'s own advertised extension list and `afinfo` opening every one. Phase
  2.1 must try the direct read, fall back to `AVAssetReader`, and **say which route ran**.
- **385× realtime on the internal SSD, 12× on an external drive** — 6 s against 200 s for a
  40-minute film. Progress and a cache are not optional.

**Two mistakes in the spike itself, both mine, both the same shape as convention 10.** The
first version waited for `getProportionComplete()` to reach 1.0; it sits at 0.9999 forever,
so it measured its own 120 s timeout instead of a read that had finished in two seconds.
The second extrapolated the constant −292 ms offset into "12,635 ms per hour of drift" —
a plausible number, confidently stated, measuring the wrong quantity. Offset and drift are
different problems and the spike now reports them separately.

Phase 1 is built **and seen running**: a floating always-on-top `VideoWindow` with no
native controls, `Canvas ▸ Open Video...`, a PICTURE track above the tracks, stop-parks and
scrub-follows, and the window's geometry in `ui_settings` per project. A 93 s mp4 measured
**306.9 ms of start latency on this machine** (the spike's laptop said 290.3) and reported
**93.0 s at 25.0 fps** from AVFoundation, against `getVideoDuration()`'s 0.00. The clip
round-tripped through the `.mira`'s new `video` array. Still owed: the Done-when, which is a
full 40-minute reel — 93 seconds proves it works, not that it holds.

**And opening the UI found a bug that had nothing to do with video: the whole Canvas menu
had been dead since the day it was added.** Play, Fit, Save Canvas — every item greyed out
with a canvas plainly open, because the macOS menu bar bakes each item's enabled state in
when the menu is BUILT and nothing told it the canvas had opened. `onMenuStateChanged`
already existed for exactly this, carrying a comment about the identical failure in the
Tags/Segments/View menus; `showCanvasWindow()` never called it. **The user saw it in the
first five seconds of looking at the menu, in a menu I had shipped, tested and documented
without once opening.** That is convention 8, stated as cheaply as it will ever be stated.

### 2026-09-19 (later) — the canvas becomes the project, and ARCHITECTURE.md

[ARCHITECTURE.md](ARCHITECTURE.md) written: the two binaries over one `mira_core`, the
schema and the three rules it encodes, the analysis pipeline stage by stage, the app's
windows and their lifetimes, **the four separate paths audio takes** (analysis, take
preview, the canvas mixer, export), the bridge to Python, and the thread map. It is the
document to read before changing anything structural.

The canvas is now the project. `File ▸ New/Open` opens it, a `.mira` document is what Open
asks for, and the old project window survives as the take pool.

- **Open Project asked for a FOLDER**, so the canvas took the first `.mira` it found inside
  -- or, finding none, wrote an empty one over the folder. Saved blocks did not come back.
- **Recents were stored and correct** but only the tray and the launch window showed them.
  `File ▸ Open Recent` now does.
- **A Canvas menu in the macOS menu bar** -- "the osx toolbar" meant the MENU bar. The
  title-bar toolbar experiment lasted one commit: buttons across a full-size content view
  leave nowhere to grab the window.
- **No block, no generate pane.** A generator over an empty canvas is a Generate button
  that cannot say where the audio would go.
- **Export**, through the player rather than by copying takes -- one mixer for the speakers
  and the file. Stems render `0 -> end` so they line up at zero in a DAW.
- **Clean up unused takes**, to the Trash, only inside this canvas's block folders,
  compared with `pathsEquivalent`.
- **Block gain and rename in the header**, an on-block progress bar, per-track slabs,
  track select/delete, per-tab panel widths, key and tempo from the prompt.
- **Cmd-S did nothing** whenever focus was in the prompt field -- the document shortcuts
  were on the view, which only receives keys when the canvas itself has focus. They belong
  to the window.

### 2026-09-19 — four generation bugs, all found by measuring instead of reading

The user reported the generator "taking a different prompt" and an `acr` block throwing
koan-ish drums. Reading the code said the request was correct, and it was — the sidecars
prove the prompt and the LoRA that left mira were exactly the ones on screen. What the
code could not say is what the OUTPUT did.

- **One Extend was guiding every generation after it.** `generateExtension`/`generateRemix`
  set `initAudio` and nothing ever put it back, so every later generation — another block,
  a brand-new block — silently carried `init_audio` pointing at the old block's take.
  Invisible three times over: `panelOnly` hides the AUDIO IN section on the canvas,
  `applySettings` never cleared it, and the sidecar never recorded it. Now scoped to the
  action, cleared on block switch, and **recorded in the recipe**.
- **The recipe could not reproduce its own audio.** Two takes with byte-identical recipes
  correlate **0.50**; a third regenerated from its own sidecar correlates **0.003** (8.84
  onsets/s against the recipe's 0.05). That gap is what located the bug above — a recipe
  that does not reproduce means the request held something the recipe does not record.
- **"Outside the range stays bit-exact" was false.** Verified by reading `sa3_mlx.py`,
  never measured. The mask preserves latents; the timeline is decoded from latents and the
  source was encoded first, so the kept region takes a lossy round trip — −21.9 dB relative
  error after one extension, **−14.5 dB after three**. mira now writes the original samples
  back outside the range with a 30 ms equal-power crossfade. Kept region is bit-exact after.
- **The trigger is in the LoRA filename**, so it stopped being something to remember: the
  prompt builder's trigger row is filled from the loaded slots, and `generate()` reports a
  mismatch in both directions. That row also decides which corpus every other field draws
  from, so a wrong trigger offers words the loaded LoRA never saw.
- **`extendSelection` returned silently** in three cases, so "extend does not work" had no
  way to become a reason (convention 6). After an extension lands the block is full again,
  which is exactly when the next extend silently refused.

Measured and **ruled out** on the way: worker LoRA state. It does leave residue on a cached
DiT — the fp16 clear is not the exact inverse `_reconcile_lora` claims, and a warm worker's
second generation of a gated config differs from its first — but at −55 to −66 dBFS, corr
0.9997 or better. Real, logged, far too small to hear. Reporting it as the cause before
measuring its size was the mistake to avoid repeating.

### 2026-09-19 — the block canvas

A second, separate window ([CANVAS.md](CANVAS.md)) where a **block owns its generator**:
interior a private take folder, exterior one piece of audio on a track. Only the chosen
take reaches the timeline, which removes the clipping risk of stacked alternates by
construction rather than by remembering to mute things. Tracks sum, with fader, mute, solo
and a meter each. A project is a **`.mira` file**, not a folder — the ambiguity of "is this
a project or the folder containing one" is what let a parent folder open as a project and
list the real one inside it as a cue.

Three audio bugs worth keeping, all from the same misunderstanding of
`BufferingAudioSource` — that it, not the source, owns the read position:

- **every voice silently skipped**: it fills its buffer in chunks far larger than one device
  block (44,100 at a time) and the scratch buffer was `blockSize + 8`. Near silence, with
  the odd small block getting through. The skip is a `jassert` now, not a `continue`.
- **looping restarted every chunk**: it calls `setNextReadPosition(P)` with LINEAR positions
  before every read, so an internal rewind at the out point meant every chunk restarted at
  the in point. Looping is a pure mapping now, as `AudioFormatReaderSource` does it.
- **the meter kept counting after a stop**: it pre-fills whether or not the transport runs.

And one process failure worth more than any of them: **I committed code that did not
compile.** An edit that was meant to add a declaration matched nothing and reported success
anyway, and the build that should have caught it ran `cmake --build build` from a directory
with no build folder — the command failed, a grep for "error:" found none, and I called it
built. Assert that an edit applied; check the exit status, not the output.

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
  **Corrected 2026-09-19:** this entry also claimed everything outside the range stays
  bit-exact. It does not — see the 2026-09-19 entry below.
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

### Next — [MIRA-BLOCKS.md](MIRA-BLOCKS.md) step 3, smaller than it was written

**Steps 1, 2, 2a and 2b are done and verified (2026-09-20).**

2b's answer changes step 3. An SA3 extension holds tempo exactly, so it does not need
stretching — what it can need is its **octave** named right. So step 3 now opens with:

- **3.0 (new, first): halve / double the block's tempo.** A menu item and a keystroke,
  `tempoSource` unchanged. Costs nothing, stretches nothing, and it is the fix for the one
  real failure 2b found.
- Then 3.1–3.7 as written — with stretch-to understood as conforming a *separate* take to a
  block's tempo (step 4's child case), not as repairing an extension, which is not what
  MIRA-BLOCKS §6 assumed when it was written.

---

**Verify the canvas on screen.** Roughly a dozen commits on 2026-09-19 have not been seen
running -- the tabs, the master strip, the file list, `[`/`]`, track select/delete, the
title-bar colour, the Canvas menu, Open Recent, the block gain and rename, the on-block
progress bar, cleanup and export -- **and now the whole of MIRA-VIDEO Phase 1**: open a
film, play, stop, scrub, close and reopen the project, and watch the picture against the
playhead over a long reel (MIRA-VIDEO.md task 1.7). Convention 8 says compiling is not verifying, and this is
the largest unverified stack this project has carried.



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
