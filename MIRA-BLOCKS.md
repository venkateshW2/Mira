# MIRA-BLOCKS.md — the block as a musical object

**Opened 2026-09-20. Step 1 built and verified the same day; steps 2–5 are still plan.**
When something lands, tick its task and move the reasoning into the past tense — the same
discipline [MIRA-VIDEO.md](MIRA-VIDEO.md) follows.

Read first, in this order:

- **[CLAUDE.md](CLAUDE.md)** — the map of the repo, and the twelve conventions. Several of
  them decide things in this document outright: 1 (omit rather than guess), 5 (`human`
  outranks machine), 6 (never silently fall back), 8 (open the UI before claiming it works).
- **[ARCHITECTURE.md](ARCHITECTURE.md) §6** — the four audio paths. §6.3 (the canvas mixer)
  is why time-stretch has to be a file and not an effect; §6.4 is export.
- **[CANVAS.md](CANVAS.md)** — what a block is today: a folder of takes with one chosen,
  sitting on a track that sums.
- **[ANALYSIS.md](ANALYSIS.md)** — what mira measures, and what it deliberately does not.

---

## 1. What this is for

**A block should know what it is musically, and blocks should be able to follow each other.**

A generated take arrives at whatever tempo and key SA3 gave it, which is near what you asked
for and not equal to it. Today nothing in the canvas knows that number, so every generation
after the first is you retyping tempo and key into a prompt and hoping.

The aim is not a grid to obey. It is, in the user's words:

> while composing I don't want tempo to be a constraint, just put things together — this is
> more like a helper tool, and we can build on top of it a lot of things. If the onsets
> align with transients, split it at the transient and rearrange the whole take. **We
> basically make each block like a sampler.**

So the guiding rule for everything below:

> ### The grid is drawn, never enforced.
> Nothing snaps unless you ask it to. Tempo is scaffolding that helps you place, stretch and
> slice — it is never a thing you must satisfy before you can put audio somewhere.

---

## 2. Why the tempo belongs to the block and not to the track

The first version of this plan put a tempo and meter header on each **track**. It was wrong,
and the reason is worth keeping.

**A block moves between tracks freely.** Anything musical attached to a track is therefore
*positional*: drag a block one row down and its meaning changes. That is a bug built into
the design. Worse, a per-track tempo is still "declare a tempo, conform to it" — the global
grid reintroduced one level down, which is exactly what the canvas exists to avoid
([CANVAS.md §1](CANVAS.md)).

Three models, and only one is unoccupied:

| | where musical time lives |
|---|---|
| every DAW | the **session** — one tempo map, everything conforms to it |
| Blockhead | **nothing** — free placement, no grid at all |
| **mira** | the **block**, and blocks can be **related to each other** |

That third one is the differentiator. The grid is not a property of the project or of the
position — it is a property of *a relationship between two pieces of audio*. There is no
project tempo to be wrong about, and every grid can name where it came from.

**A track stays what it is today: a container that sums audio, with a fader, a mute, a solo
and a meter. No musical information at all.**

---

## 3. The decisions, made

Taken before any code, because each changes the shape of the rest.

| | decision | why |
|---|---|---|
| **Where tempo lives** | on the **block** | a block moves between tracks; a track-level tempo would change a block's meaning when you drag it |
| **Where the grid comes from, first** | the **prompt** | `keyAndTempoOf()` already parses `Keyscale` and `BPM` out of the prompt and prints them in the block header. Drawing a grid from them is free and usually close. Analysis becomes a *correction*, not a prerequisite |
| **What "follows block N" means** | a **source for a number**, not a second system | tempo is an editable property; the parent link only fills the field. "Change this block to 92" and "match block 1" are the same button |
| **Time-stretch** | a **new take in the block's folder** | `renderRange` is random-access, a stretcher is sequential (§6). As a file it is just another take: Choose Take A/Bs it, Cleanup sweeps it, the original never moves |
| **Stretch quality threshold** | **none — your ears decide** | the conform is non-destructive and A/B is one click, so a number measured on someone else's material adds nothing. The ratio is *shown* (`+9.7%`); it is not a gate |
| **Bar 1 anchor** | stored in **source time**, not timeline time | moving the block, dropping it on another track, trimming its left edge and appending an extension then all leave the phase where it was |
| **A block has ONE tempo** | yes | an extension that drifts is a fault to surface, not a second tempo to support. A tempo map inside a block is the complexity this whole design cut out |
| **Stretch library** | **signalsmith-stretch** | MIT, header-only, already used and trusted by the author. Not yet vendored — one line in `scripts/fetch-vendor.sh` |

And the rule that ties the mechanics together:

> ### Every operation that changes a block's audio writes a new take into its folder.
> generate · extend · stretch · slice-and-bounce — all of them.
> **The folder is the history, Choose Take is the undo, Cleanup is the bin.**
> That is already how the canvas works; nothing new is needed to make it hold.

---

## 4. The flow: escalating cost, stop at the first thing that works

This is the whole feature in one sequence. Each step is more expensive than the last and
most sessions never reach the bottom.

```
 free      generate a take           tempo + key are already in the prompt
                                     -> draw a grid from them, immediately

 cheap     the grid looks off        drag it: move bar 1 onto the downbeat by eye
                                     type over the tempo if the number is wrong

 costly    still off                 click ANALYSE
                                     -> onsets, beats, downbeats, tempo, meter, key,
                                        chords, and a confidence in the grid
                                     -> the grid snaps to the measurement
                                     -> THAT becomes the block's tempo

 the loop  a new block               "follows block 1"
                                     -> its tempo and key go into the prompt
                                     -> generate; the take arrives close
                                     -> one button: STRETCH TO 87.3
```

Nothing above is automatic except the first line. A grid drawn from the prompt costs nothing
and can be ignored; everything after it is a gesture you make.

---

## 5. What the analysis already gives us

**None of this needs new analysis code.** `enqueueAnalyze` in `Main.cpp` already shells out
to the `mira` CLI (`mira analyze --db <db> --paths-from <list>`), which always re-analyses
regardless of `analyzed_at`. Pointing it at a block's take and reading the row back is the
whole of step 2.

What comes back, with the exact JSON keys in `files.machine`:

| what | key | note |
|---|---|---|
| beats | `$.rhythm.beat_this_beats` | Beat This + a madmom-compatible DBN |
| downbeats | `$.rhythm.beat_this_downbeats` | **bar 1 comes from here** |
| tempo | `$.rhythm.beat_this_bpm`, `$.rhythm.beat_fitted_bpm_raw` | `fitBeatPeriod` resolves the period to under a millisecond |
| **confidence** | `$.rhythm.beat_grid_stability` | 0.99–1.00 on correct grids, 0.64–0.85 on wrong ones, measured over six ground-truth tracks |
| onsets | `$.onset_times` | `mira::detectOnsets`, hop 256 — **the slice points for step 5** |
| meter | `$.rhythm.meter`, `$.rhythm.meter_bar_spread` | 4, 6, 3…; spread past 1.5 means the beats drifted |
| key | `$.key.key`, `$.key.camelot` | libKeyFinder, gated on harmonicity — **real, not prompt-derived** |
| chords | `nnls-chroma` via the analysis pipeline | not needed for tempo; see §11 |
| notes | `basicpitch.cpp` | vendored; not needed for any step here, see §11 |

**Analysing a block's take writes it into the library.** That is a side effect worth wanting:
your generated audio becomes searchable and captionable beside your source material, instead
of living only inside a project folder.

### The confidence gate

`beat_grid_stability` decides whether a measurement is allowed to overwrite anything.

Below threshold, the block **says so and leaves your grid alone**. It does not quietly
impose a measured tempo it is not sure about. This is convention 1 (omit rather than guess)
and convention 6 (never silently fall back) applied to time.

It is also forward-looking rather than backward: the beat stack is being improved
independently, and the gate is how you will know the new algorithm is actually better rather
than having to trust that it is.

---

## 6. Time-stretch is a file, not an effect

The canvas mixer renders through `CanvasAudioSource::renderRange(info, from, numSamples)`
([ARCHITECTURE.md §6.3](ARCHITECTURE.md)). That call is **random-access**: the transport
seeks, the loop rewinds, and `renderOffline` walks arbitrary ranges for export.

A time-stretcher is **sequential with internal state**. You cannot jump to sample N of a
stretched stream without resetting it. Stretching inside the voice would break looping,
scrubbing and export together, and would put a phase vocoder in the audio callback.

So a stretch renders to disk:

```
block 4/
  dun-s26-20260918-224421.wav         the take, untouched
  dun-s26-20260918-224421.json        its recipe
  dun-s26-20260918-224421@87.3.wav    conformed to 87.3 BPM
  dun-s26-20260918-224421@87.3.json   what it was stretched from, and by how much
```

Everything about this already works: the block **is** a folder of alternatives, Choose Take
switches between them, `promptCleanup()` moves unused ones to the Trash, and the sidecar
records the operation the way a recipe records a generation (convention 12 — record in the
sidecar everything the request carried).

**For large ratios, slicing beats stretching.** A 20% stretch smears percussive material; the
same 20% done by moving slices is transparent. That is step 5, and it is the better answer
to a big tempo change than any stretch quality setting would have been.

---

## 7. Extend, with a grid

[Extend](MIRA-GENERATE.md) already works: drag the block's tail out past its audio, and the
gap is the range SA3 fills. Only the last `kExtendContextSeconds` (30 s) go to the model, and
the result is joined back with a 30 ms equal-power crossfade so the kept region stays
bit-exact. Three things change once a block has a grid.

**The ask gets better.** "Extend 8 bars at 87.3" is 21.99 s. The tail snaps to bar lines when
you want it to and does not when you do not.

**The extension may not come back in tempo.** SA3 has 30 s of context so it *tends* to
continue in tempo, but nothing guarantees it — and **nobody currently knows how well it
holds**, because there has never been an instrument to ask. Step 2 is that instrument:
extend, analyse both halves, compare. Worth measuring early.

When it has drifted, the block **says so** and offers the button that already exists —
*stretch to 87.3* — applied to the extension rather than the whole take. That is one small
generalisation: **stretch-to operates on a range**, and an extension is exactly a range.

**Slices conflict, and extend bounces first.** Once a block has been sliced and rearranged,
"extend the audio" has no obvious referent. The answer is to render the block's current
audible result to a new take and extend that. The original stays in the folder. This is the
same rule as everything else: an operation that changes a block's audio writes a take.

---

## 8. The document

`Block` ([CanvasEngine.h](src/mira_ui/Source/CanvasEngine.h)) gains a musical half. Written
into the same `.mira`, and a document without it opens exactly as it does today — additive,
the way the `video` array was.

```cpp
struct Block {
    // ... file, lane, start, length, sourceOffset, contentSeconds, gain, fades, name, id ...

    // ---- musical time (MIRA-BLOCKS.md) --------------------------------------------
    double tempo = 0.0;          // BPM. 0 = unknown; from the prompt, typed, or measured
    int    meter = 4;            // beats per bar
    // Bar 1, in SOURCE time (seconds into the file), NOT timeline time. That is what
    // makes moving the block, dropping it on another track, trimming its left edge and
    // appending an extension all leave the phase where it was.
    double barOnePos = 0.0;
    juce::String key;            // "C minor", from the prompt or measured
    // Where `tempo` came from, because a grid that cannot say that is a grid you cannot
    // argue with: "prompt" | "typed" | "measured" | "block 1"
    juce::String tempoSource;
    double tempoConfidence = 0.0;// $.rhythm.beat_grid_stability, 0 when not measured
    juce::int64 followsBlockId = 0;   // 0 = independent
    // Whether the hand-set bar 1 survives the next analysis. Convention 5: `human`
    // outranks machine, so a nudge you made by ear is not overwritten by a model.
    bool barOneIsHuman = false;
};
```

### The slice list, written in from the start

Step 5 turns a block's interior into a list of regions. That is the one part of this which
is **expensive to retrofit**, because it changes the document. So the shape goes in now, even
though nothing writes more than one entry until step 5:

```cpp
struct Slice {
    double sourceStart = 0.0;    // into the file
    double sourceLength = 0.0;
    double placeAt = 0.0;        // seconds from the block's start
    double gainDb = 0.0;
    bool   muted = false;
};
// Empty means "the whole of the file from sourceOffset", which is every block today.
std::vector<Slice> slices;
```

A block with no slices behaves exactly as it does now. That is what keeps step 5 from being a
rewrite.

---

## 9. Tasks

### Step 1 — the grid, free — **DONE 2026-09-20, verified on screen**

- [x] **1.1** `Block` gained `tempo`, `meter`, `barOnePos`, `key`, `tempoSource`,
      `tempoConfidence`, `followsBlockId`, `barOneIsHuman`, and the empty `slices` list.
      Serialised into the `.mira`; a document without them opens unchanged.
      **The musical group is written only when a block has any of it**, so a project whose
      blocks never learned a tempo grows no new keys — additive the way `video` was.
      **`followsBlockId` is written as the parent's INDEX, not its id**: ids are handed out
      fresh on every load, so a saved id points at whatever block takes that number next
      time. That is the trap `audioBlockId` is left unwritten to avoid; here the index is
      stable because `toJson` writes the blocks in the order `fromJson` reads them, and it
      is remapped after the load loop. A self-link or a missing parent is dropped.
- [x] **1.2** `musicFromTake()`, called from the end of `setFileOn` — the one place every
      route a take can arrive by already goes through (generation adopted, take chosen,
      file dropped, block split, duplicated, document loaded). It reads the take's **own
      `.json` sidecar**, not the block's `settings`: `settings` is the recipe you are about
      to generate *with*, so reading it here would let a tempo you just typed describe
      audio made before you typed it. A BPM outside 20–400 is not a tempo and is dropped;
      a take that says nothing **clears** the old numbers rather than leaving a grid drawn
      over audio it was never about (convention 12).
      Measured before it was built: of 45 sidecars in one real project, **33 carry `BPM:`
      and 23 carry `Keyscale:`** — so roughly a quarter of takes legitimately draw no grid.
- [x] **1.3** A **footer** along the bottom of the block: beat ticks, heavier bar lines,
      bar numbers once a bar is 26 px wide. A footer and **not an overlay across the
      waveform** — the waveform is what you read to find a transient by eye, and beat lines
      through it are what makes a drawn grid start to feel like one you must obey.
      `gridFooterHeight()` is ONE decision asked by both the painter and the waveform,
      because a waveform that makes room for a footer the density guard then refuses to
      draw leaves a mystery empty band. Density degrades in steps: below ~5 px per beat the
      beats go, then the bars, then the footer. Below 62 px of block height there is no
      footer and the header line is the whole summary — the padlock's rule: a control that
      does not fit is replaced by words, not shrunk. Bars before bar 1 are drawn but not
      numbered, as the waveform's bars ruler already does.
- [x] **1.4** **Drag the footer** to move bar 1, `barOneIsHuman = true`. Its own verb
      (`Drag::BarOne`) rather than a modifier on Move, because the two are opposite
      intentions: moving a block says "this audio belongs later", moving its grid says
      "this audio was always in phase, I had the downbeat wrong". Trim keeps the 7 px at
      each end — trim has only those, the grid has the whole strip. The delta goes straight
      into `barOnePos` because it is source time, which is exactly why trimming the left
      edge afterwards cannot break the phase. It moves the dragged block ONLY, never the
      selection: where the downbeat falls is a fact about one piece of audio.
- [x] **1.5** The tempo is a **drag box in the header, not a typed field.** The first
      version opened a `TextEditor` over the block and the user rejected it on sight — a
      field that pops up is a modal moment in the middle of arranging. It now behaves like
      the gain box three pixels to its left, and is drawn like it: up is faster, 0.25 bpm
      per pixel, landing on whole bpm, **shift for 0.1** — which is the 87.3 case this
      whole feature exists for. `tempoSource = "typed"`, and `tempoConfidence` back to 0,
      because a stale confidence would let step 3 allow or refuse a stretch on the strength
      of a number that no longer describes anything.
      **Double-click puts the recipe's answer back**, bar 1 included — the counterpart of
      double-clicking gain for unity, and the only way back from a dragged number worth
      having an exact route to. A block whose recipe had no BPM still shows the box, faint,
      with a dash in it: drawing nothing would be honest about the tempo and silent about
      the gesture, and those are precisely the blocks someone needs to set one on.
- [x] **1.6** Verified on screen by the user: the grid draws, the bar numbers slide as the
      footer is dragged, and a typed tempo and a moved bar 1 both survive save and reopen.

**Also built, answering a question the user asked while looking at it:** a split **keeps the
parent's bar numbers**. Bar 1 is in source time, so the right-hand half of a cut at bar 9
goes on saying bar 9 wherever it is then dragged — the right default, because you split it
to move that section and you talk about it by where it came from. The other answer is one
item rather than an argument: **Bar 1 starts here** in the block menu. Nothing had to be
written to make the split correct; the source-time decision made it correct for free.

**Still owed at the end of step 1:** there is no way to *type* a meter — the grid is 4/4
until step 2 measures one.

**Done when** a generated block draws a usable grid with no analysis at all, and you can put
bar 1 where your ear says it goes. ✔

### Step 2 — Analyse — **built 2026-09-20, NOT YET SEEN ON SCREEN**

2.1–2.5 are written and the app builds and launches. **2.6 is open**, and until it is done
this is the same state MIRA-VIDEO Phase 1 was in when the whole Canvas menu turned out to
have been dead since the day it shipped. Compiling is not verifying.

- [x] **2.1** An **A** chip in the block header, third after M and the gain box, plus
      **Analyse take** in the block menu — both landing in `analyseSelection()`, so they
      cannot disagree about what Analyse means. Four states carried by the chip's FILL and
      not by its letter (running / measured / refused / failed), because a chip that
      changes its letter is a chip you have to learn to read. **While it runs the TEMPO BOX
      says `analysing…`**: that is where your eye already is for a tempo and it is the thing
      about to change, so the running state is reported where the answer will appear rather
      than only on a 15-pixel square and a status line one repaint can overwrite.
      **A canvas take is usually not in the library at all**, and `mira analyze
      --paths-from` SKIPS a path with no row, then prints "nothing to analyze" and exits 0 —
      so without registering it first the analysis appears to run, succeeds, and changes
      nothing. It is registered with `upsertScannedFile` before the enqueue, which is also
      the side effect §5 wants: your generated audio becomes searchable beside your source
      material instead of living only inside a project folder.
      **`--groove` is forced on** whatever the Analyze menu happens to be set to. Onsets and
      the fitted grid ARE the question a block is asking; a session toggle silently deciding
      whether a feature works is exactly the invisible dependency convention 6 is about.
- [x] **2.2** `measurementFor()` in `Main.cpp` reads the row back into a `Measurement` —
      tempo, stability, meter, bar spread, first downbeat, key, onsets. It lives with the
      database and not in the canvas, the same line `loadSetting`/`saveSetting` already
      draw: **the canvas has no database and is not getting one.** Legacy rows are converted
      out of active time (`$.timebase != "file"`), and `human.$.key` outranks
      `machine.$.key.key` (convention 5). Chords are deliberately not read — §11.
      Verified against the real library before it was trusted: every key in §5's table is
      present and populated on 819 analysed rows.
- [x] **2.3** Tempo, meter, key and bar 1 adopted from the measurement, **each asked
      separately** whether it has earned the right to overwrite what the block believes.
      Bar 1 takes the first downbeat **unless `barOneIsHuman`**, which survives and says so
      (convention 5). One `pushUndo()` for one answer, so adopting a grid is one Cmd-Z.
- [x] **2.4** **The confidence gate, and it fires often.** Below `kGridConfidenceGate` the
      block keeps its grid, does **not** become `tempoSource = "measured"`, and says
      *"measured 88.14 bpm at confidence 0.71 — below 0.90, so the grid is left as it was"*.
      `Refused` is its own state and not a kind of `Failed`: the analysis SUCCEEDED and its
      answer was not good enough to impose, which is the system working.
      **Measured, not guessed** (convention 2): of 819 analysed rows carrying a stability,
      468 clear 0.90 and 385 clear 0.95 — so this refuses roughly four takes in ten.
      The meter has a **second, different** gate, because `beat_grid_stability` says the
      beats are steady and `meter_bar_spread` says whether they group into a bar the same
      way twice; a steady grid with a drifting bar is exactly where 4 is a guess. 1.5 is the
      number the groove panel already turns red at, reused deliberately — two thresholds for
      one question would let the canvas adopt a meter another window is drawing in red.
      Over the 386 rows that have a meter and clear the stability gate: min 1.005, p25
      1.075, **p50 1.336**, p75 1.647, max 39.4.
- [x] **2.5** The footer draws **onsets**, and **outside the gate**. An onset is a
      measurement of the audio; whether the beat grid is trustworthy says nothing about
      whether a transient is where it is — and on a take whose grid was refused the onsets
      are the only honest thing on screen about its timing. So **the footer now exists for
      onsets alone**, with no tempo at all, which is not a special case but the case that
      matters most. They are drawn from the bottom up and shorter than a beat line, so an
      onset landing on a beat reads as two marks rather than one longer one. Their density
      guard is `onsetTicksVisible()` — ONE decision asked by both `gridFooterHeight` and the
      painter, for the same reason `gridFooterHeight` itself is one.
      A consequence worth naming: **bar 1 now needs a tempo to be draggable**, not just a
      footer, or the gesture would move a number nothing draws.
**Second pass, after the first look (2026-09-20).** The user's report was *"the analyse
button is hidden"*, and it was — twice over.

- **The block's NAME was painted over the chip.** Where the header text may start was
  computed in two places, and the copy that draws the name did not know a third chip had
  been added. `blockHeaderRow()` is one definition now, asked by the name and by the tempo
  box. This is the failure `blockTagBox`'s own comment already describes, which happened to
  the line below that comment rather than the one above it.
- **A 15-pixel "A" beside two other 15-pixel squares was the wrong control anyway.** M and
  the gain box can be glyphs because you already know what they are; Analyse is the one verb
  on a block nobody has met. It is a labelled button now, and **the word is the state** —
  ANALYSE / READING / MEASURED / UNSURE / FAILED — so the thing you press and the thing it
  told you are the same control. It falls back to a single character on a narrow block.
- **The grid and the onsets are drawn OVER the waveform**, the way the browser draws them,
  at the user's explicit request. **This reverses 1.3's call** that a footer is safer than
  an overlay. That reasoning was not wrong, it was outvoted by the thing it was a guess
  about: you cannot line a transient up against a bar line you have to look away from to
  see. The rule is unchanged — nothing snaps — and what keeps it scaffolding is the
  weighting: **bars span the wave, beats only its middle third and stay faint, and the
  onsets sit on top of both**, because the onsets are the measurement and the grid is the
  interpretation.
- **The onsets are two-tier, and deliberately the browser's own numbers** (a 16th cell, 18%
  of it counts as on the grid), so the two windows cannot say different things about the
  same audio. On-grid onsets are tall and bright, off-grid ones short and warn-coloured —
  which means **a take whose grid locked onto the wrong period shows nothing tall.** That
  split is the fastest read there is of whether the tempo on the header is really the tempo
  of the music, and it is free.
- Their density guard was measured, not picked: over 830 analysed files the onset rate runs
  p10 6.82/s, **p50 9.92/s**, p90 13.73, max 15.42. At the zoom a block is normally arranged
  at (~56 px/s) that is 35.7 px per onset at the sparsest and 3.6 at the densest, so they
  draw for essentially every take at working zoom and start dropping the densest tenth only
  around 40 px/s — where they have stopped being countable anyway.

- [ ] **2.6** Verify on screen. Analyse a generated take; watch the chip and the tempo box
      while it runs; check the adopted tempo against the recipe's; find a take the gate
      refuses and read what it says; confirm a hand-dragged bar 1 survives an analysis;
      reopen the project and confirm the onsets come back without re-measuring.

Also built, because the alternative was a block stuck saying `analysing…` forever: the
analyze queue grew **watchers** — who is waiting for which file — fired from the CLI's own
`progress:` line and **swept as failed when the queue drains without one**. Matched with
`pathsEquivalent` and never with `==`: the CLI echoes the path back as the DATABASE holds
it, and the canvas asked with JUCE's bytes (convention 9, the bug that cost most of a day).

**Done when** clicking Analyse on a generated take tells you what you actually made, as
opposed to what you asked for.

### Step 2b — the measurement step 2 makes possible

- [ ] **2b.1** **Does an SA3 extension hold tempo?** Extend a block, analyse both halves,
      compare. Nobody knows, and this is the first time the project can ask. Write the answer
      into §7 with a date.

### Step 3 — stretch to

- [ ] **3.1** `signalsmith-stretch` vendored — one line in `scripts/fetch-vendor.sh`, MIT,
      header-only.
- [ ] **3.2** **Stretch to \<tempo\>** in the block menu. Target from a typed field or from
      the parent. Shows the ratio before it runs (`+9.7%`).
- [ ] **3.3** Renders to a **new take** in the block's folder, named with its tempo, with a
      sidecar recording the source take and the ratio. The original is untouched and Choose
      Take switches between them.
- [ ] **3.4** Aligns **bar 1**, not just the rate. Right tempo with the wrong phase is the
      failure nobody predicts.
- [ ] **3.5** Refuses when `tempoConfidence` is below the gate and says why.
- [ ] **3.6** Takes a **range**, so §7's "stretch just the extension" is the same code.
- [ ] **3.7** Verify by ear, and A/B against the original take.

**Done when** a block that came back at 88.1 can sit at 87.3 without anyone typing a ratio.

### Step 4 — the child

- [ ] **4.1** **Follows block N** in the block menu, stored as `followsBlockId`. Drawn on the
      block — a badge naming the parent.
- [ ] **4.2** Cycle check on link: A → B → A is refused with a reason.
- [ ] **4.3** The parent's tempo and key are written into the child's prompt **before**
      generation, so the take arrives close and the stretch is small or unnecessary.
- [ ] **4.4** When the parent's tempo changes, children are marked **stale** — never
      re-stretched behind your back.
- [ ] **4.5** Verify on screen.

**Done when** generating a second block against the first needs no retyping.

### Step 5 — the sampler

The big one, and the reason the rest exists. Only after 1–4 are real.

- [ ] **5.1** **Split at onsets** — the block's interior becomes the `slices` list, one per
      detected transient.
- [ ] **5.2** Slices can be moved, muted, deleted and repeated inside the block.
- [ ] **5.3** The mixer plays a sliced block. One voice per sounding slice, or one voice with
      a lookup — decided by measurement, not by preference.
- [ ] **5.4** **Re-place to a new tempo**: move the slices rather than stretching the audio.
      This is the transparent answer to a large tempo change.
- [ ] **5.5** **Bounce** — render the arrangement to a new take, which is what extend then
      works on (§7).
- [ ] **5.6** Verify on screen.

---

## 10. Risks, named up front

- **A wrong grid imposed confidently is the failure mode.** It has happened twice in this
  project: a 100 BPM loop that read 149.4 at high strength (2026-09-16) and a groove
  histogram that was flat on all 94 files (2026-09-15). Both were measurement bugs that
  *looked* like answers. The confidence gate is the whole defence, and it is why step 2.4 is
  a task rather than a detail.
- **The octave convention is still open.** Halftime material reads 79 or 159 depending on
  convention (CLAUDE.md, 2026-09-15 evening). On a block header that ambiguity becomes
  *visible*, which is good — you can click it and halve it — but it will be visible.
- **Stretching generated audio is lossy** and nothing here pretends otherwise. The defences
  are that it is non-destructive, that the ratio is shown, and that step 5 offers a way out.
- **Slices are a document change.** That is why the empty `slices` list goes in at step 1.
- **Scope.** Steps 1 and 2 are small and independently useful. Step 5 is a feature on its
  own and should not be started until 1–4 are in daily use.

---

## 11. Deliberately later

- **Chords as an inheritance.** "Follows block 1" currently means one tempo and one key.
  With `nnls-chroma`'s chord track it could mean *a progression* in the prompt — a much more
  interesting kind of dependency, and a natural step 6.
- **Notes.** `basicpitch.cpp` is vendored and real, but it improves neither tempo nor phase,
  so it earns nothing in steps 1–5. Where it earns its place later: MIDI out of a cue, and
  "generate a bass that follows these notes".
- **Chains.** A ← B ← C resolves to the root. Natural, but not needed until someone wants it.
- **An SFX block whose generator is a VST rather than a prompt.** `juce_audio_processors` is
  already in the build (via `juce_audio_utils`), so hosting a plugin needs no new dependency.
  A block already owns its generator, so the abstraction exists.
- **A live lane** — a generator that runs continuously against picture rather than producing
  a take. `Sa3WorkerHub` means a second backend is a worker type, not a rewrite. Held until
  the hardware is real.

---

## 12. Where to start next

**Verify step 2 on screen (2.6).** 2.1–2.5 are written, build clean and the app launches;
nothing has been watched doing it. That is the state MIRA-VIDEO Phase 1 was in when the
Canvas menu turned out to have been dead since the day it shipped — convention 8.

Then **step 2b**, the measurement step 2 makes possible: extend a block, analyse both
halves, and find out whether an SA3 extension holds tempo. Nobody knows, and until now the
project has had no instrument to ask.

Step 3 (stretch to) should not start before 2b: what a stretch is FOR depends on how far
an extension actually drifts, and 3.5 refuses a stretch on the strength of the very
confidence number step 2 has just started producing.
