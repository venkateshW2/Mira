# mira

Local audio understanding and similarity search for a producer's drive.

Point it at folders of samples, stems and full tracks; it analyses them and builds a
searchable index — tempo, key, loudness, instruments, moods, genre, groove, vocal
presence, all with confidences — and renders those into training captions. Sononym-like
in purpose, Sonic Visualiser in spirit. Everything runs locally on Apple Silicon: no
server, no cloud, no GPU assumption.

A rewrite of [drive-audio-analyzer](https://github.com/venkateshW2/drive-audio-analyzer)
and [2w12-backend](https://github.com/venkateshW2/2w12-backend), local-first.

**Status: built and in daily use.** A C++20/JUCE desktop app plus a CLI over one SQLite
library. New here? Read [CLAUDE.md](CLAUDE.md) — it maps every document in the repo.

---

## Build

```bash
cmake -B build -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build build -j8
```

- App: `build/src/mira_ui/mira_ui_artefacts/RelWithDebInfo/mira.app`
- CLI: `build/src/mira`
- Library: `~/.mira/library.db`

```bash
mira scan ~/Music/samples
mira analyze --groove --recheck-tempo
mira caption <file> --trigger amt
mira similar --text "brass swell"
```

## Layout

```
mira/
├── CLAUDE.md       ← the map: every document, what it is, whether it's current
├── ANALYSIS.md     ← what mira measures, what it means, how to change it
├── PRD.md          ← the design: stack, models, phases, every "why this and not that"
├── TASKS.md        ← the build checklist, phase by phase
├── CAPTION-TAGGING.md ← the human-only folder vocabularies
├── src/mira/       ← the CLI and the analysis library
├── src/mira_ui/    ← the JUCE app
├── taxonomy/       ← label normalisation
├── lab/            ← offline Python for model conversion; never shipped
├── spike/          ← Phase 0 proofs, one CMake project per risky assumption
└── sa3-studio/     ← SA3 + underfit LoRA training rig (a consumer of mira's output)
```

`sa3-studio/` is deliberately a subfolder, not the point. mira produces analysis
documents; LoRA caption generation is one downstream consumer among several.

## Why

Two problems with the previous generation, both found by reading the old code:

1. **Speed came from not doing the work.** The prior implementation limited analysis to
   10/5/3 frames per file and reported whole-file results — 3,300× "faster" because it
   analysed a fraction of a second. mira records `coverage` on every result.
2. **Silent fallbacks hid broken models.** A failed classifier fell back to a heuristic
   returning `1.0`, indistinguishable from a real answer. mira returns `null` plus a
   recorded error, never a substitute.

Both became standing rules — see [CLAUDE.md](CLAUDE.md) for the rest.

## Measured, on an M1 Pro (16 GB)

| | |
|---|---|
| Full analysis pass, 60 s track | **≈ 2.2 s (≈ 27× realtime)** |
| 40-minute file | ≈ 90 s |
| Similarity search, 500k tracks | **46 ms**, brute force — no vector DB needed |

Reproduce with [`bench/mir_bench.py`](bench/mir_bench.py).

## Stack

- **C++20 + JUCE** — the app and the CLI; one static `mira_core` shared between them
- **Essentia** (arm64, `--no-tensorflow` static) — MIR and music classification heads
- **ONNX Runtime** — the models Essentia doesn't ship (EffNet/Discogs, CED, CLAP, beat_this)
- **SQLite + sqlite-vec**, statically linked — index and exact brute-force KNN, no FAISS
- **Python** only offline, in `lab/`, for model conversion and parity checks

See [PRD.md §7](PRD.md) for why each choice was made.

## Licence

**AGPL-3.0.** A deliberate choice, not a default — it is what makes Essentia (AGPL-3.0)
and JUCE's free tier (AGPLv3) usable together with no fee and no revenue cap.
See [PRD.md §12 Q9](PRD.md).
