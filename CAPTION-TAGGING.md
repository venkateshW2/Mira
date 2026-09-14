# Folder-level tagging — a caption vocabulary for style LoRAs

**Status: design, not built.** Written 2026-09-14 after analysing five film scores in
mira and measuring what actually separates them.

The question this answers: *Lord of the Rings and Dune are both film score, but one is
fantasy epic and the other sci-fi. How does a caption say that?*

Related: [PRD.md](PRD.md) §11/§15 (captioning as a renderer over the analysis document),
[src/mira/caption/CaptionFields.h](src/mira/caption/CaptionFields.h),
[sa3-studio/TRAINING.md](sa3-studio/TRAINING.md).

---

## 1. What mira already separates

Measured across 187 analysed files, five scores:

| | LOTR | Dune | MadMax | DarkKnight | BvS |
|---|---|---|---|---|---|
| electronic share | **0.06** | 0.43 | 0.32 | 0.26 | 0.19 |
| top instruments | violin, cello, orchestra, flute, doublebass, **harp** | **synthesizer 0.44** | synth + violin | synth + violin | — |
| genre head | **Classical**—Neo-Romantic / Romantic / Modern | **Electronic**—Ambient / Drone / Dark Ambient | split | split | — |
| onset rate | 0.73 | 0.95 | 1.73 | 2.25 | 1.37 |
| spectral flatness | **0.023** | 0.038 | 0.051 | 0.056 | 0.039 |
| loudness range LU | 16.1 | 15.3 | 13.8 | 17.8 | 17.5 |
| mood unique to it | **adventure** | **space, meditative** | trailer | trailer | — |

**LOTR is 56/56 acoustic** — not one file above 40% electronic. It is cleanly separable
from everything else in the library, and the genre head already calls it Neo-Romantic
while calling Dune Dark Ambient.

**So the difference is already measured.** What is missing is not analysis, it is a
caption vocabulary that states the difference as a *dial you can turn at inference*
rather than leaving it implicit across genre and instrument lists.

## 2. What does NOT work — measured, not assumed

**Averaged chroma cannot support harmony captions.** The store keeps one 12-bin
pitch-class average per file. Over a three-minute cue that moves through keys, it
averages flat:

```
LOTR chroma, sorted, normalised:  0.123 0.122 0.106 0.103 0.090 ... 0.053 0.038
perfectly flat would be:          0.083 in every bin
```

Standard mode templates (Ionian, Dorian, Phrygian, Lydian, Mixolydian, Aeolian, Locrian,
harmonic minor, whole-tone, octatonic) applied to it return musically absurd answers —
Locrian for 38 of 56 Howard Shore cues. Diatonic fit lands at 0.64-0.69 and chromaticism
at 0.955-0.976 for **every** film, i.e. no separation at all.

The theory is sound; the stored data cannot carry it. Real mode detection needs the chord
track or per-frame chroma. **`chords` exists only for files analysed after chordino landed**
(LOTR 56/56; Dune, MadMax, DarkKnight, BvS all 0). Absent re-analysis, harmonic language
must be asserted by a human, not measured.

This is the same discipline `CaptionFields.h` used when it rejected `harmonicity`
(Dune 0.939 vs others 0.944 — no separation) and `spectral_centroid` (r=0.91 with
flatness). A field that does not discriminate is prompt noise.

## 3. The core rule

> **Never ask a human for something the machine already knows.**

| Machine measures — no dropdown | Human chooses — dropdown |
|---|---|
| `palette` · acoustic / hybrid / electronic | **material** |
| `rhythm` `dynamics` `texture` (already built) | **world** |
| `feel` · jitter, drift, swing | **harmonic** |
| bpm, key, instruments, genre, moods | **signature** |

Four dropdowns, set **once per folder**, applying to every file inside. LOTR's 56 files
are one row.

## 4. New measured fields

### `palette` — acoustic | hybrid | electronic

From the instrument head: electronic share = sum(electronic labels) / (electronic +
acoustic). Measured means: LOTR 0.06, BvS 0.19, DarkKnight 0.26, MadMax 0.32, Dune 0.43.

**Honest limit:** the axis isolates LOTR crisply (56/56 below 0.4) but Dune, MadMax and
DarkKnight overlap heavily — only 4 of 38 Dune files clear 0.6. One sharp bucket, one
soft boundary. Pick thresholds from the full library distribution before shipping, not
from these five.

### `feel` — from `rhythm.beat_this_beats`

The beat-tick timeline is already stored for every analysed file. From inter-beat
intervals (IBI):

| derived | formula | reads as |
|---|---|---|
| jitter | `stdev(IBI) / mean(IBI)` | quantised ↔ loose ↔ glitched |
| swing | `max(odd,even) / min(odd,even)` on alternating IBI | straight ↔ shuffled |
| drift | `abs(mean(first half) - mean(second half)) / mean` | metronomic ↔ free-time |

Sanity-checked on the five scores: **swing 1.003-1.006 across all of them**, which is the
correct answer — orchestral score has no swing. Jitter 0.15-0.26 correctly reads as
rubato. The axis is silent on score and will light up on beat-based material.

**Honest limit:** beat ticks are *beats*. Producer swing usually lives at the 8th or 16th
level, between the beats, and is invisible here. Catching it needs onset-to-grid offsets.

## 5. The four human dropdowns

Fixed lists. Never free text — consistency is what the LoRA learns from, and a vocabulary
that drifts teaches nothing.

### Material

`score` · `song` · `beat` · `sound-design` · `live-set`

Sets how everything else is read. LOTR `score`, NIN `song`, a producer's tracks `beat`.

### World — pick 1-2

*Film:* fantasy · sci-fi · noir · heist · chase · horror · western · war ·
post-apocalyptic · superhero · survival
*Music:* club · basement · arena · lo-fi · industrial · psychedelic · spiritual

LOTR `fantasy` · Dune `sci-fi` · Last of Us `survival` · NIN `industrial` ·
Need for Speed `chase` · noir film `noir`

### Harmonic language — pick 1

This is where musical knowledge does the work no classifier can.

| value | the shape it names |
|---|---|
| `heroic` | major, Mixolydian, Lydian, open fifths, horn calls |
| `lament` | minor, descending bass, suspensions |
| `menace` | Phrygian, tritones, semitone clusters |
| `alien` | whole-tone, octatonic, drones, no clear tonic |
| `static-drone` | one harmony held; texture carries the piece |
| `modal-folk` | Dorian/Aeolian folk modes, modal cadences |
| `blues-pentatonic` | blue notes, pentatonic riffs |
| `jazz-extended` | 7ths/9ths/13ths, tritone substitution |
| `atonal` | no functional centre by design |

LOTR `heroic` · Dune `alien` · DarkKnight `menace` · noir `jazz-extended` · NIN `atonal`

### Signature — pick 0-1

Names a *person's* habit rather than a genre. The slot that makes an individual
producer's style trainable. A library-wide list that grows as artists are added:

`glitch-swing` · `boom-bap` · `broken-beat` · `wall-of-noise` · `wide-rubato`

## 6. Trigger tokens

- One auto-generated three-letter token per folder: `lrt`, `los`, `nin`.
- **One shared token on everything with `material: score`** — e.g. `scr`.

At inference: `scr` alone is generic score-ness that every new film reinforces;
`scr lrt` is Middle-earth; `scr lrt zvq` blends two across the generate window's three
LoRA slots. This is the shared-token idea that has been sitting untried; the per-film
tokens already exist (`zvq` `xyr` `qsk` `vzx`).

## 7. Where it plugs in

- **Storage:** the `folder_defaults` table already exists — `folder_path TEXT UNIQUE`,
  `human TEXT DEFAULT '{}'`. It has **0 rows**. This design is its first use.
- **Merge:** folder `human` JSON merges under per-file `human`, which already wins over
  `machine`. Per-file edits keep overriding the folder default.
- **Fields:** `world`, `harmonic`, `signature` land in `CaptionFields::keywords`, which
  the header already documents as existing "purely so a person can hand-label what
  genre/instrument/mood classifiers can't" and which "only ever comes from `human`".
  `material`, `palette` and `feel` want their own typed fields alongside
  `rhythm`/`dynamics`/`texture`.
- **UI:** four dropdowns on the folder row. No per-file tagging.

## 8. Open questions

1. **Thresholds for `palette` and `feel`** — must be measured across the whole library
   first, the way `rhythm`/`dynamics`/`texture` were over 171 files. Do not guess.
2. **Does the vocabulary survive contact with actual music?** NIN is the deliberate test:
   everything here assumes score-vs-song is the axis that matters.
3. **Sub-beat swing** needs onset-to-grid offsets. Worth it only if `signature` proves
   useful on real beat material first.
4. **Re-analysis for chords** stays deferred. If it ever happens, `harmonic` could move
   from asserted to measured — chord-change rate, vocabulary size, no-chord share and
   coloured-chord share all separated cleanly on the one film that has them
   (LOTR: 21.4 changes/min, 31.9 distinct chords, 3% no-chord, 41% minor, 27% coloured).

## 9. The honest limit

Dropdowns 2-4 are assertions, not measurements. The quality of the LoRA's dials is the
quality of the tagging discipline behind them. That is acceptable — the model learns
whatever mapping it is fed, so consistency matters more than correctness. It is the same
reason the genre head's wrong "Electronic: Ambient" label stays switched on for Mad Max:
at inference you prompt with the vocabulary you trained with.
