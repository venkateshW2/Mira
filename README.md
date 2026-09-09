# mira

Local audio understanding and similarity search for a producer's drive.

Point it at folders of samples, stems and full tracks; it analyses them and builds a
searchable index — tempo, key, loudness, instruments, moods, genre, vocal presence, with
confidences. Sononym-like in purpose, Sonic Visualiser in spirit. Everything runs locally
on Apple Silicon.

A rewrite of [drive-audio-analyzer](https://github.com/venkateshW2/drive-audio-analyzer)
and [2w12-backend](https://github.com/venkateshW2/2w12-backend), local-first: no server,
no cloud, no GPU assumption.

**Status: design. Nothing implemented yet.** See [PRD.md](PRD.md).

---

## Layout

```
mira/
├── PRD.md          ← the design: stack, models, phases, measured benchmarks
├── NOTES.md        ← research notes (SA3 caption format, setup state, gotchas)
├── bench/          ← reproducible benchmarks
│   └── mir_bench.py
└── sa3-studio/     ← SA3 + underfit LoRA training rig (a consumer of mira's output)
```

`sa3-studio/` is deliberately a subfolder, not the point. mira produces analysis
documents; LoRA caption generation is one downstream consumer among several, and is
explicitly out of scope for v1.

## Why

Two problems with the previous generation, both found by reading the old code:

1. **Speed came from not doing the work.** The prior implementation limited analysis to
   10/5/3 frames per file and reported whole-file results — 3,300× "faster" because it
   analysed a fraction of a second. mira records `coverage` on every result.
2. **Silent fallbacks hid broken models.** A failed classifier fell back to a heuristic
   returning `1.0`, indistinguishable from a real answer. mira returns `null` plus a
   recorded error, never a substitute.

## Measured, on an M1 Pro (16 GB)

| | |
|---|---|
| Full analysis pass, 60 s track | **≈ 2.2 s (≈ 27× realtime)** |
| 40-minute file | ≈ 90 s |
| Similarity search, 500k tracks | **46 ms**, brute force — no vector DB needed |

Reproduce with [`bench/mir_bench.py`](bench/mir_bench.py).

## Stack

- **Essentia** (C++, arm64 wheels) — MIR + music classification heads
- **ONNX Runtime** — general-audio models Essentia doesn't ship (CED / AudioSet)
- **NumPy + SQLite** — index and search; no FAISS, no vector database
- Python 3.12, `uv`

See [PRD.md §7](PRD.md) for the full stack and why each choice was made.
