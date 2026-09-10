# TASKS — mira build checklist

Working checklist derived from [PRD.md](PRD.md)'s phases (§9) and CLI surface (§8).
Check items off as they're done; each links back to the PRD section that specifies it, so
context isn't lost between sessions. Detailed day-by-day log for Phase 0 lives in
[spike/README.md](spike/README.md) — this file tracks the whole project at a coarser
grain.

Work generally goes top to bottom within a phase, but nothing here is rigidly ordered
beyond that — re-sequence as needed.

---

## Phase 0 — week-1 spike ✅ COMPLETE (2026-09-10)

- [x] Day 1-2: Essentia C++ builds arm64 `--no-tensorflow` static, links from an external
      CMake project (PRD §9, §16.1)
- [x] Day 3: ONNX embedding pipeline (mel frontend + `discogs-effnet-bsdynamic-1.onnx`)
      matches Python reference to ~1e-4 — achieved exact match (PRD §9, §16.3)
- [x] Day 4: JUCE 9 drag-out builds and the OS-level drag itself works, manually
      confirmed (PRD §9, §2d, §16.6)
- [x] Day 5: `beat_this_cpp` runs end-to-end on arm64 (PRD §9, §16.5)
- [x] Day 5: SQLite amalgamation + `sqlite-vec`, statically linked, 10k × 1280-dim
      vectors, correct exact KNN (PRD §9, §16.8)
- [x] Day 5: `AudioThumbnailCache` survives a relaunch (PRD §9, §13)
- [x] Repo skeleton: CMake project, `lab/`, `fixtures/`, `models/`, `vendor/` fetch script

---

## Phase 1 — describe (PRD §9 Phase 1, §5, §6)

No neural yet. Exit: BPM/key/loudness across a drive, via `mira inspect`.

**Storage**
- [x] SQLite schema: one row per file — path, sha256, mtime, content type, descriptors/tags
      (JSON columns), provenance (§6) — `src/mira/db/Database.cpp`
- [x] `machine`/`human` split enforced: re-analysis rewrites `machine` wholesale, never
      touches `human` (§6) — schema has both columns; `upsertScannedFile` never writes
      either (scan is index-only, analyzer will own `machine`)
- [x] Vendor + wire in `SQLiteCpp` (§7) — against our own SQLite 3.53.4 amalgamation, not
      SQLiteCpp's bundled copy or Apple's `libsqlite3` (`src/CMakeLists.txt`)
- [ ] Provenance recorded per file: mira version, Essentia version, model
      names/versions, analysis timestamp (§6) — column exists, analyzer will populate it

**Scanner + router**
- [x] `mira scan <dir>...` — walk, hash (sha256 via CommonCrypto), record mtime/size,
      upsert rows, no analysis yet (§8) — `src/mira/scan/Scanner.cpp`, skips re-hashing
      when mtime is unchanged
- [x] `mira scan <dir> --as stem` — route 3, declaration always overrides detection (§8,
      §12.3) — always excluded from routing, verified by smoke test
- [ ] Content-type router (partial — see note): one-shot / loop / track / stem, via duration + onset density +
      loop-point heuristics (§5) — `src/mira/analyze/Router.cpp`, `mira analyze`. **Only
      the duration cut is implemented** (≤3s one_shot, ≤30s loop, else track — a
      documented first-pass guess, not measured on real material). Onset rate is
      computed and stored in `machine` but doesn't move the boundary yet; the loop-point
      heuristic (matching start/end for a tight loop) isn't implemented at all. Revisit
      once there's real material to tune against.
- [ ] Stem detection route 1: filename/folder pattern (opportunistic) (§12.3)
- [x] Stem detection route 2: sibling-set detection — the general case (§12.3). **Known
      limitation:** grouping only happens within one `analyze` run's batch, not across
      the whole library — a folder analyzed in two separate runs won't be grouped
      correctly. Fine for now, worth fixing before Phase 2 relies on `group_id`.
- [x] Nullable `group_id` for cue grouping across sibling stems (§6)

**Active-region detection**
- [ ] Frame-energy gate finding non-silent spans, no model (§5)
- [ ] Always runs for anything routed as `stem`, regardless of duration (§5)
- [ ] Runs for everything else when duration > 5 minutes (§5)
- [ ] `active_ratio` + span list stored per file (§6)
- [ ] All downstream descriptors/MIR/embedding operate only over active spans (§5)

**DSP descriptors (all content types)**
- [ ] Duration, sample rate, channels (§5)
- [ ] Integrated LUFS, loudness range, true peak (`LoudnessEBUR128`) (§5)
- [ ] Crest factor (§5)
- [ ] Spectral centroid (brightness), spectral flatness (noisiness) (§5)
- [ ] Harmonicity, onset rate, attack time (§5)
- [ ] Loudness-on-stems recorded but never used to flag quiet/thin/faulty (§5)

**MIR (loops, tracks, stems — over active regions only)**
- [ ] `RhythmExtractor2013` (multifeature) tempo estimate (§5)
- [ ] `beat_this_cpp` tempo + downbeat estimate, wired into the app build (spike proved
      it runs standalone — integrate properly) (§5)
- [ ] Dual-estimator disagreement stored as the tempo confidence signal (§5, §14.1)
- [ ] `BeatsLoudness`, `Danceability` (§5)
- [ ] Full beat array stored, not just the BPM scalar (§6)
- [ ] Vendor + wire in `libKeyFinder` for key detection, gated on harmonic content
      (§5, §12b)
- [ ] Vendor + wire in `Chordino`/`NNLS-Chroma` for chord sequence, gated on harmonic
      content (§5, §12b)
- [ ] Basic Pitch (`nmp.onnx`) note transcription via ONNX Runtime (§5, §12b)
- [ ] Camelot/Open Key notation lookup table (verify numeric offset against
      openkeyscan-analyzer first) (§12b)

**CLI**
- [ ] `mira analyze` — idempotent, resumable (skip unchanged sha256 + model version),
      `--limit`, `--content-type`, `--force`, `--resume` (§8)
- [ ] `mira inspect <file|id>` — human-readable report, surfaces low-confidence
      tempo/key rather than hiding it, reports `active_ratio` (§8)

**Fixtures + tests**
- [ ] Fixture clips covering one-shot / loop / track / stem, incl. a mostly-silent stem
      and non-tonal material (§9 Phase 0 skeleton, carried into Phase 1)
- [ ] Content-type router regression tests
- [ ] Numerical-parity tests for the DSP/MIR path, extending `lab/`'s harness pattern

---

## Phase 2 — classify + search (PRD §9 Phase 2, §2c, §5)

Exit: the Sononym-parity milestone.

- [ ] `discogs-effnet-bs64` embedding pass wired into the pipeline (mandatory input to
      every head below, and the similarity vector) (§2c, §5)
- [ ] CED-small content gate (ONNX) — "is this even music?" before running music heads
      (§2c)
- [ ] `mtg_jamendo_instrument-discogs-effnet-1` head (40 classes) (§2c, §5)
- [ ] `mtg_jamendo_moodtheme-discogs-effnet-1` head (56 classes) — already downloaded in
      Phase 0 spikes (§2c, §5)
- [ ] `genre_discogs400-discogs-effnet-1` head (400 classes) — needs `tf2onnx`
      conversion in `lab/` first, no ONNX published (§2c, §16.3)
- [ ] `voice_instrumental-discogs-effnet-1` head — needs `tf2onnx` conversion, no ONNX
      at all published (§2c, §16.3)
- [ ] `danceability-discogs-effnet-1` head (§2c, §5)
- [ ] Label normalisation as versioned YAML data files, not code (§5)
- [ ] Label normalisation regression tests; both `raw` and `label` stored (§5)
- [ ] Embedding store in `sqlite-vec` (float32[1280] per file) wired into the schema
      (§6, spike already proved the mechanics)
- [ ] `mira similar <file|id>` — `--by overall|timbre|rhythm|spectrum`, `--n`,
      `--filter`; accepts an external file not in the library (§8)
- [ ] `mira search "--filter" ...` — expressions over tags/descriptors (§8)
- [ ] `mira models --download | --list` (§8)
- [ ] `mira stats` — library composition, coverage, unmapped labels (§8)

---

## Phase 3 — SA3 captioning (PRD §9 Phase 3, §11, §15, NOTES.md §4-5)

Ordered after all DSP/MIR/classification are complete, so the renderer never gets
designed around a missing field.

- [ ] Field-dict + prose renderer architecture: one analysis document, N renderers (§11)
- [ ] SA3 key-value tag renderer
- [ ] SA3 prose renderer, 256-token / 45-word ceiling
- [ ] `seconds_total` field, trigger-token injection
- [ ] Folder-level human defaults
- [ ] Machine pre-fills genre/mood/instruments as an editable draft; human field always
      wins on conflict (§6, §11)
- [ ] Sidecar JSON export (`--emit-sidecars`) (§6)
- [ ] Confidence-gated field omission at render time — the renderer omits low-confidence
      fields rather than asserting them (§12.6)

---

## Phase 4 — quality (PRD §9 Phase 4)

- [ ] Embedding A/B: `discogs-effnet` vs CLAP vs MuQ-MuLan, measured on the real library
- [ ] Per-dimension similarity (timbre / rhythm / spectrum), not Sononym's fixed five
      (§12 Q5)
- [ ] Per-head confidence calibration → sets the Phase 3 render-gate thresholds
- [ ] Segment-level analysis replacing whole-track averaging

---

## Phase 5 — surfaces + multi-target captioning (PRD §9 Phase 5, §13)

- [ ] JUCE UI shell: one native window, same process as analysis (§13)
- [ ] `TableListBox` file list with `paintCell` only, at drive scale
- [ ] `AudioThumbnail` + `AudioThumbnailCache` waveform display (persistence already
      proven in Phase 0 spike)
- [ ] Playback: `AudioDeviceManager` → `AudioSourcePlayer` → `AudioTransportSource` →
      `AudioFormatReaderSource`
- [ ] Drag-out wired into the real app (mechanics already proven in Phase 0 spike)
- [ ] Filter bar (stackable chips — SonikSearch-inspired, NOTES.md UI research)
- [ ] Per-field source-of-truth display (Analysis/Filename/Manual — SonikSearch-inspired,
      NOTES.md UI research)
- [ ] ACE-Step JSON renderer
- [ ] Remaining two caption registers
- [ ] Segment slicing UI/export

---

## Not scheduled / open questions to revisit

- [ ] DAW-project browsing (Tuva-inspired, NOTES.md UI research) — natural extension
      once the index exists, not committed to a phase yet
- [ ] CLAP zero-shot escape hatch for open-vocabulary labelling (§2c) — Phase 3+,
      adds a `torch` dependency
- [ ] Korzeniowski-CNN key detector as an ONNX upgrade path over `libKeyFinder` (§12b)
- [ ] BTC chord model as an ONNX upgrade path over `Chordino` (§12b)
