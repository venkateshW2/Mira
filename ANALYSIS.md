# What mira measures, and what reaches the model

A reference for mira's analysis as it stands on 2026-09-15. Two questions it answers:
**what does each field mean**, and **how do I change it**.

Companion docs: [CAPTION-TAGGING.md](CAPTION-TAGGING.md) for the human-only folder
vocabularies, [sa3-studio/TRAINING.md](sa3-studio/TRAINING.md) for what happens after the
caption is written, [PRD.md](PRD.md) for why any of it exists.

---

## 1. The shape of the pipeline

```
scan ──▶ analyze ──▶ caption ──▶ pre_encode ──▶ train
 │          │           │            │
 │          │           │            └─ writes <name>.npy  (audio → latents, GPU)
 │          │           │               plus <name>.json   (the caption, copied in)
 │          │           └─ CaptionFields: gates measurements into words
 │          └─ writes files.machine (the raw numbers) — never words
 └─ writes the row; no audio is opened
```

Two things are worth internalising, because most confusion lives here:

**Analysis stores numbers. Captions store words.** `analyze` never writes "swung" or
"heavy" — it writes `0.652` and `0.554`. The words are derived at *caption time* from
thresholds in `CaptionFields.h`. Change a threshold and every already-analyzed file gets
the new word with no re-analysis.

**Captions and latents are separable.** The `.npy` is audio only. The `.json` beside it is
the caption. So re-captioning costs nothing — `scripts/retag-latents.py` rewrites the
caption half and leaves the latents alone. Re-encode only when the *audio* changed.

---

## 2. What the trainer actually reads

This is the part that is easy to get wrong, so it is worth stating plainly.

The trainer does **not** read mira's rendered `prompt` sentence as the caption.
`underfit/dataset_processing/prompt_templates.py` builds its own:

```python
tag_keys = pc.get("tag_keys", _ALL_TAG_KEYS)
for key in tag_keys:
    parts.append(f"{label}: {val}")
```

- It walks whatever keys you ticked in the underfit dashboard.
- `prompt` is just **one more key** in that list, not the mechanism.
- `_TAG_DISPLAY.get(key, key)` falls back to the raw key name, so **any new key works
  with no training-side change at all.**

And then:

```python
if random.random() < 0.5:
    random.shuffle(parts)
else:
    parts = random.sample(parts, random.randint(1, len(parts)))
```

Half the time it trains on a **random subset** of your tags. This is caption dropout, and
it is the reason a tag becomes a dial you can turn at generation time instead of one
frozen blob — the model sees `groove: programmed` sometimes with the genre, sometimes
alone, sometimes not at all, so it learns what the word contributes on its own.

> **The trap.** A latents-only dataset shows *"No files with tags found"*, renders no
> pills, and posts `tag_keys: []` — which silently discards every caption. You need
> placeholder `.wav` files beside the sidecars. Fully documented in
> [sa3-studio/TRAINING.md](sa3-studio/TRAINING.md); do not skip it.

---

## 3. The one rule that decides whether a field is worth having

**A caption word is only useful if it varies across the corpus.**

If all 94 files say `syncopated`, the model has no counterexample. It cannot learn what
the word contributes, so the word gets absorbed into the trigger — and at generation time
typing it does nothing. You cannot turn up something that was never a variable.

So the question is never "what describes this artist". It is **"what splits this
artist's catalogue"**. Every field below was kept or dropped on that test, plus one more:
whether it duplicates an axis already captioned. Precedents already set in the codebase:

| rejected | why |
|---|---|
| `spectral_centroid` | r=0.91 with `spectral_flatness` — same axis twice |
| tempo drift | r=0.79 with the jitter `timing` already uses |
| `crest_factor` | r=0.44 with loudness range, partly redundant with `dynamics` |
| `flux_stddev` | r=0.71 with `flux_mean` — same axis twice |
| `pocket` | below measurement resolution (see §5) |
| `syncopation` | r=−0.46 with `groove`, partly the same axis |

---

## 4. The caption fields

Every field is **omitted rather than guessed** when its confidence gate fails. An omitted
field is not a bug — it is the system declining to assert something it cannot support.
That discipline is what caught the groove bug described in §6.

### Identity

| field | source | notes |
|---|---|---|
| `trigger` | you | the LoRA's name. Not stored per file — it belongs to the run, so it is passed per render (`--trigger`) |
| `TrackType` | content router | `Music` / `Instrument` (stems). One-shots get none |
| `VocalType` | voice/instrumental head | threshold 0.5 |

### Content

| field | source | gate |
|---|---|---|
| `genre` | genre head | score ≥ 0.10, top 3 |
| `instruments` | instrument head | score ≥ 0.10, top 8. Stems use a separate isolated-audio model |
| `moods` | mood/theme head | score ≥ 0.10, top 3 |
| `keywords` | **you only** | no machine equivalent — scene/vibe words mira has no analyzer for |
| `bpm` | onsets, else beat_this | see §6 |
| `keyscale` | key detection | already gated by its own harmonicity check |

### Shape — how the music behaves

| field | words | measured from | thresholds |
|---|---|---|---|
| `rhythm` | sparse / moderate / driving | `onset_rate` | 0.8, 2.0 |
| `dynamics` | compressed / moderate / wide | `loudness_range_lu` | 4.0, 13.0 |
| `texture` | tonal / mixed / noisy | `spectral_flatness` | 0.03, 0.06 |
| `palette` | acoustic / hybrid / electronic | electronic share of instrument mass | 0.15, 0.35 |
| `timing` | tight / human / loose | stdev/mean of inter-beat intervals | 0.08, 0.20 |

### Groove and sound design — added 2026-09-15

| field | words | measured from | thresholds |
|---|---|---|---|
| `groove` | organic / steady / programmed | onset phase histogram peak/uniform | 1.75, 2.65 |
| `swing` | straight / light swing / swung | position of the off-beat 8th | 0.51, 0.55 |
| `low_end` | light / balanced / heavy | 20–80 Hz share of spectral energy | 0.20, 0.34 |
| `motion` | static / shifting / morphing | mean frame-to-frame spectral flux | 0.14, 0.20 |

**`groove`** — how hard the onsets lock to a steady pulse. 1.0 means onsets fall
anywhere at all; the most rigidly programmed track in the calibration corpus reads 7.45.
This is the axis that separates programmed from played, and nothing else captures it.

**`swing`** — where the off-beat 8th actually sits inside the beat. 50% is dead straight,
66.7% is full triplet swing. Anything above ~54% is audible. This could not be measured
before: `beat_this_beats` holds *beats*, so an 8th-note swing is invisible to it no matter
how the arithmetic is done.

**`low_end`** — how much of the spectrum is sub-bass. Correlates with nothing already
captioned (r=+0.12 with `texture`'s flatness, −0.28 with crest), which is what a new
field has to prove.

**`motion`** — how much the *timbre itself* keeps changing, frame to frame. Low means the
sound sits still (a held pad, a steady loop); high means it is constantly being reshaped
(filters sweeping, textures mutating). It is the closest single number to what "sound
design" means on beat-driven electronic material: `texture`, `rhythm` and `palette` all
describe a sound *frozen at one instant*, and none of them says the sound does not hold
still. The weakest of the four (r=+0.42 with flatness, −0.55 with crest) and the first to
drop if four turn out to be too many.

---

## 5. Why some things are measured but never captioned

Stored in `files.machine`, deliberately absent from every caption:

- **`pocket`** (mean signed offset from the nearest 16th). The middle two thirds of the
  calibration corpus spans −2.4 to +2.0 ms, while the onset detector quantises at
  ~11.6 ms (Essentia `OnsetRate`, hop 512 @ 44.1 kHz). **Two thirds of the files sit
  inside a single quantisation step** — the field would be measuring rounding, not feel.
  Only the ±30 ms tails are real. Revisit if onsets ever get finer.
- **`flux_stddev`** — r=0.71 with `flux_mean`. The same axis twice.
- **`syncopation`** — r=−0.46 with `groove`. Add only if `groove` alone does not respond.
- **`onset_times`** — the raw evidence, kept so grids can be re-derived later without
  re-analysis. That property is what made the §6 fix free.

Measuring without captioning is the point of PRD §12.6's "store everything, tune
thresholds later". The corpus decides the thresholds; the thresholds are never guessed
first.

---

## 6. The BPM correction

`bpm` now prefers the tempo of the grid the **onsets** lock to, when they lock at all.

Measured over the 94-file Amon Tobin / Two Fingers corpus, against the grid found by
maximising onset phase concentration:

| estimator | agrees with the fitted grid |
|---|---|
| `essentia_bpm` | 77 / 94 (82%) |
| `beat_this_bpm` | 12 / 94 (13%) |

`beat_this` remains the default everywhere else — it is the better estimator on songs,
which is what it was chosen for — but it is unreliable on programmed electronic material.
Two Fingers is a ~79.5 BPM catalogue it reported as 86.5, 90.8, 104.8, 127.0 and 130.2
across one record.

**A wrong BPM is worse than no BPM**: it teaches a false association rather than simply
omitting one.

The same measurement also exposed why the groove fields could not be built earlier. Using
`beat_this_beats` as the grid, the onset phase histogram was **flat on all 94 files**
(median peak/uniform 1.17, ceiling 1.36) — so swing read 0.54–0.57 for everything, an
orchestral title cue scoring identically to a programmed halftime beat. Fitting the grid
to the onsets instead gives a median of 1.93 and a maximum of 6.93, and wins on **94 of
94**. Files analyzed without `--groove` have no `onset_times` and keep the old behaviour
exactly.

---

## 7. How to change anything

Everything mira measures is a **proposal**. `human` always outranks it (PRD §11), and
nothing you write is ever overwritten by a re-analysis.

### In the UI — File Details

Each row shows three things:

```
Groove                                    [ -- measured -- ▾ ]
analyzed: 4.60× grid lock at 79.5 bpm        ← what the numbers actually said
┌──────────────────────────────────────┐
│ programmed                           │     ← the effective value, editable
└──────────────────────────────────────┘
```

The reference line shows the **raw number**, not the word repeated — so a surprising word
is accountable. The dropdown offers the vocabulary; the text box stays typeable.

- **Save** writes only the rows you actually changed.
- **Revert** clears human overrides and returns to pure machine output.
- Multi-select works: rows collapse to "same across all N files" or list the distinct values.

### On the command line

```bash
mira tag <file> --groove programmed --swing "light swing" \
                --low-end heavy --motion shifting
mira tag <file> --bpm 79 --keywords "glitch, broken"
mira tag <file> --clear            # back to pure machine output
mira tag-folder <dir> --material … --world …   # folder-wide defaults
```

Per-file tags beat folder defaults; both beat the machine value.

### Vocabulary: fixed lists, offered not enforced

The closed-vocabulary rows have dropdowns. This follows the rule
[CAPTION-TAGGING.md](CAPTION-TAGGING.md) already set for folder tags:

> Fixed lists, never free text: a LoRA learns whatever mapping it is fed, so a vocabulary
> that drifts between folders teaches the model nothing.

Concretely: `swung` / `swing` / `shuffled` are **three different tokens**, each learned
from a third of the examples. And a word invented for one file appears once in 94 — far
too few exposures to be learned at all.

But the box stays typeable, because the alternative is that the only way to record a
genuinely new distinction is to not record it. **Use the list unless you mean to add an
axis** — and if you do, add it to every file it applies to, not one.

For open-ended words with no machine equivalent, use `keywords`. That is what it is for.

### Per-track, not per-folder

Groove and swing belong on the **file**, never in the folder-tag dialog. A folder tag
puts one value on all 94 files, which by §3 makes the word useless. The split:

| | goes where |
|---|---|
| Material, World, Harmonic language, Signature | folder — constant, the artist's identity |
| Groove, Swing, Low end, Motion, BPM | per file — they vary, which is what makes them controls |

Across the calibration corpus: `groove` lands 34% / 32% / 15% across its three buckets
(19% omitted), `swing` 27% / 27% / 28%, and **all nine** groove × swing combinations
occur. That spread is the whole reason these are worth captioning.

---

## 8. Optional analysis stages

Off by default; each costs something.

| flag | adds | cost |
|---|---|---|
| `--groove` | `onset_times`, so groove/swing/BPM can be fitted | free (Essentia already produces them); ~12 KB JSON per 5-min track |
| `--recheck-tempo` | Essentia's own BPM alongside beat_this | small |
| `--dclap` | second embedding space, for `similar --text` | 27% of analysis time |
| `--chords` | chord changes | 15.0s on a 5:08 song |
| `--transcribe` | note events | 3.7s on a 5:08 song |

**Run `--groove` and `--recheck-tempo` on anything beat-driven.** Between them they are
what makes the groove fields and the corrected BPM possible, and they cost almost nothing.

---

## 9. Seeing it

The waveform view draws the groove measurement so it can be checked by eye rather than
trusted:

- **Onset lane** under the peaks — on-grid ticks tall and bright, off-grid short and dim.
  That split is the syncopation reading, drawn.
- **Fitted grid** in amber, next to `beat_this`'s bar grid in teal. They disagree on most
  beat-driven material, and seeing both is what makes the disagreement checkable.
- **Phase histogram**, top-right: the whole file folded onto one beat, against a fixed
  4.0× ceiling so tracks are comparable. A flat picture means no grid — which is exactly
  the null result that hid the original bug.

All three toggle from the Lanes menu.

---

## 10. Working order

1. `scan` the folder
2. `analyze` with `--groove --recheck-tempo` for beat material
3. Look at the waveform — does the fitted grid sit on the audio?
4. Read the captions in File Details; fix what is wrong, leave what is right
5. `caption --emit-sidecar`, or re-encode
6. Tick the tag pills in underfit — **check they rendered**, see §2
7. Train

Step 4 is the one people skip. The machine value is a first draft.
