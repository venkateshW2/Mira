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
- [x] Provenance recorded per file: mira version, Essentia version, model
      names/versions, analysis timestamp (§6) — mira's "version" is its git commit hash
      (`git rev-parse --short HEAD` at CMake configure time; no release versioning
      exists yet), since that's the only thing that actually identifies which build
      produced a row. Essentia's version comes from its own committed `version.h`
      (`ESSENTIA_VERSION`/`ESSENTIA_GIT_SHA`). Model "versions" are just the file paths
      for `beat_this.onnx`/`model.onnx` (basic pitch) — neither publishes a version
      string of its own

**Scanner + router**
- [x] `mira scan <dir>...` — walk, hash (sha256 via CommonCrypto), record mtime/size,
      upsert rows, no analysis yet (§8) — `src/mira/scan/Scanner.cpp`, skips re-hashing
      when mtime is unchanged
- [x] Skip macOS AppleDouble sidecar files (`._Foo.wav`) — found on a real exFAT
      stem-delivery drive (first real-world test of the whole pipeline): every real
      `.wav` had a same-named `._` resource-fork sidecar next to it, which passed the
      extension check and got scanned as audio, only to fail to decode at `analyze`
      time. `isAppleDoubleSidecar` in `Scanner.cpp`; reported separately in scan output
      from "non-audio files skipped" so it's clear what's being excluded and why
- [x] `mira scan <dir> --as stem` — route 3, declaration always overrides detection (§8,
      §12.3) — always excluded from routing, verified by smoke test
- [ ] Content-type router (partial — see note): one-shot / loop / track / stem, via duration + onset density +
      loop-point heuristics (§5) — `src/mira/analyze/Router.cpp`, `mira analyze`. **Only
      the duration cut is implemented** (≤3s one_shot, ≤30s loop, else track — a
      documented first-pass guess, not measured on real material). Onset rate is
      computed and stored in `machine` but doesn't move the boundary yet; the loop-point
      heuristic (matching start/end for a tight loop) isn't implemented at all. Revisit
      once there's real material to tune against.
- [x] Stem detection route 1: filename/folder pattern (opportunistic) (§12.3) —
      `looksLikeStemPath` in `main.cpp`, deliberately narrow (just "stem"/"stems" as a
      case-insensitive path substring, matching the PRD's own examples) rather than
      guessing instrument-role keywords, which would risk false positives the PRD didn't
      ask for. Stays `content_type_source='router'`, never `'declared'`
- [x] Stem detection route 2: sibling-set detection — the general case (§12.3). **Known
      limitation:** grouping only happens within one `analyze` run's batch, not across
      the whole library — a folder analyzed in two separate runs won't be grouped
      correctly. Fine for now, worth fixing before Phase 2 relies on `group_id`.
- [x] Nullable `group_id` for cue grouping across sibling stems (§6)

**Active-region detection**
- [x] Frame-energy gate finding non-silent spans, no model (§5) —
      `src/mira/analyze/ActiveRegions.cpp`. RMS-per-frame vs a -60dB threshold, gaps
      under 300ms bridged so natural micro-pauses don't fragment a span — first-pass
      thresholds, not measured on real material, same caveat as the router's
      (documented in the file). Verified against a real silence/tone/silence fixture:
      correct span count and ~0.5 active_ratio.
- [x] Always runs for anything routed as `stem`, regardless of duration (§5) — including
      *declared* stems, which required widening `mira analyze` to process them too
      (previously excluded entirely, since routing skips them — but routing and
      active-region detection turned out to be separate concerns the code was
      conflating; declared stems still need the latter)
- [x] Runs for everything else when duration > 5 minutes (§5)
- [x] `active_ratio` + span list stored per file (§6)
- [x] All downstream descriptors/MIR/embedding operate only over active spans (§5) —
      `extractActiveAudio` in `ActiveRegions.cpp` concatenates the samples within each
      span; `main.cpp` swaps DSP/rhythm/key/chords/transcription onto that sliced audio
      whenever active-region detection ran. Verified on the same file analyzed two ways
      (as a stem, restricted; as an ordinary file, not) — spectral centroid differs
      between the two runs, proving the restriction actually changes what gets measured,
      not just that the code compiles. One caveat worth naming: splicing spans together
      creates an artificial discontinuity at each boundary that could in principle read
      as a spurious transient to tempo/beat tracking — accepted since the PRD asks for
      this regardless and it mainly matters on multi-span, rhythmically-dense material,
      which is rare for the mostly-silent-stem case this exists for. (embedding: n/a —
      Phase 2, not built yet)

**DSP descriptors (all content types)**
- [x] Duration, sample rate, channels (§5) — `src/mira/analyze/AudioLoader.cpp` now loads
      at the file's *native* sample rate (no forced resample) and reports real
      `numberChannels`; found and fixed an output-type bug along the way (`bit_rate` is
      `int`, not `Real` — Essentia throws a type-check exception on mismatch, another
      instance of the Phase 0 lesson that these errors surface at unexpected points)
- [x] Integrated LUFS, loudness range, true peak (`LoudnessEBUR128`, `TruePeakDetector`)
      (§5) — `src/mira/analyze/Descriptors.cpp`
- [x] Crest factor (§5) — verified exactly against a pure sine wave's theoretical
      peak/mean(|x|) of π/2 (smoke test)
- [x] Spectral centroid (brightness), spectral flatness (noisiness) (§5) — frame-averaged
      over Windowing(hann)→Spectrum→Centroid/Flatness. Verified: a 440Hz sine's measured
      centroid lands at ~459Hz (smoke test)
- [x] Harmonicity (§5) — `computeHarmonicity` in `Descriptors.cpp`: frame-averaged
      Windowing→Spectrum→{PitchYinFFT, SpectralPeaks}→HarmonicPeaks→Inharmonicity,
      stored as `1 - inharmonicity` (1=purely harmonic, 0=inharmonic/noisy), averaged
      only over frames with `pitchConfidence` above a threshold — an unpitched frame has
      no harmonic series to measure. `harmonicity_frame_count==0` means "not
      measurable" (e.g. a rhythm stem), distinct from a real 0.0 score, so callers can't
      misread "couldn't measure" as "confirmed inharmonic". Verified: a 220Hz sine gets
      harmonicity 1.0 (128 confident frames); white noise gets 0/0 (no confident frames
      at all, not a false "totally inharmonic" reading). This also closed a documented
      honesty gap: `shouldRunKeyDetection` (key + chords gate) now uses this real signal
      instead of the spectral-flatness proxy it shipped with
- [x] DSP performance fix (§5) — prompted by comparing mira against the user's own
      `FluCoMaAnalyser` (a hand-written single-STFT-pass analyser in a separate sampler
      project) after a real-song profiling run showed DSP taking ~7.4s. Two things found:
      (1) centroid/flatness/harmonicity each ran their own Windowing→Spectrum frame loop
      over the same audio — consolidated into one shared `computeSpectralAverages()` pass
      (real but modest: ~120ms saved on the 5:08 test song). (2) `TruePeakDetector` was
      the actual dominant cost at 5.5s/75% of DSP time, from Essentia's default 4x
      oversampling — found only after two wrong hypotheses (the redundant-pass theory,
      then a harmonicity-chain A/B test) were disproven by direct `std::chrono`
      instrumentation around each DSP sub-stage. Dropped `oversamplingFactor` 4→2
      (`truePeak->configure(..., "oversamplingFactor", 2)`), verified linear: 5543ms→2790ms.
      No smoke test asserts an exact `true_peak_db`, so the slightly coarser oversampling
      is safe. Combined effect on the same real song: DSP ~7.4s → ~4.7s
- [x] MFCC (13 coefficients) and chroma/HPCP (12 bins), frame-averaged (§5, similarity) —
      needed for the similarity-search goal (comparable to a "2D corpus" style timbre/
      pitch-class comparison), and previously entirely missing despite being asked about
      directly. Added inside the same shared `computeSpectralAverages()` pass rather than
      a fourth frame loop: MFCC runs unconditionally per frame off the existing magnitude
      spectrum; chroma reuses the same `SpectralPeaks` output the harmonicity chain
      already computed, but unconditionally (not gated on pitch confidence — chroma is
      meaningful on polyphonic/noisy material, unlike the monophonic harmonicity chain).
      Verified correct, not just non-crashing: a 440Hz (A4) sine's chroma vector peaks
      exactly at the A bin (index 0, matching HPCP's default `referenceFrequency`=440).
      Cost: ~110ms added to DSP on the 5:08 real song (4.60s→4.71s) — near-free because it
      shares the pass
- [x] Onset rate (§5) — was already computed by the router; unchanged
- [x] Attack time (§5) — whole-file `Envelope` → `LogAttackTime`. Only meaningful for a
      single dominant transient (one-shots); on a multi-onset track/loop the number is
      real but not very informative — documented as a known limitation, not fixed
- [x] Loudness-on-stems recorded but never used to flag quiet/thin/faulty (§5) —
      `mira inspect` now exists and displays loudness, and deliberately does not flag a
      stem's absolute LUFS as low/faulty the way it flags `active_ratio < 0.5` as
      "mostly silent" — those are different things (a stem mixed relative to its cue can
      correctly sit at -38 LUFS integrated, PRD §5) and inspect keeps them separate
- [x] DSP descriptors restricted to active regions only, on stems/long tracks (§5) — see
      "All downstream descriptors/MIR/embedding operate only over active spans" above,
      same change

**MIR (loops, tracks, stems — over active regions only)**
- [x] `RhythmExtractor2013` (multifeature) tempo estimate (§5) —
      `src/mira/analyze/Mir.cpp`. Gated: only runs for loop/track/stem, never one_shot
      (verified — a 0.5s clip gets no rhythm section at all), matching PRD §5's "tempo on
      a 300ms kick is wasted work"
- [x] `beat_this_cpp` tempo + downbeat estimate, wired into the app build (§5) — the
      `beat_this_api` shared library now `add_subdirectory`'d straight into `mira`'s
      CMake build, not just standalone in the spike. On the flamenco fixture: essentia
      117.8 BPM vs beat_this 82.8 BPM — a real, non-octave disagreement (ratio 1.42, not
      near 1/2/0.5), exactly the kind of case §14.1 says to surface rather than resolve
- [x] Dual-estimator disagreement stored as the tempo confidence signal (§5, §14.1) —
      stored as `bpm_ratio` (essentia/beat_this); resolving *what* counts as agreement
      vs. a real disagreement is explicitly deferred to a later phase (§12.6)
- [x] Revised: both estimators no longer run by default (§5, §14.1 amended) — prompted by
      comparing mira's rhythm stage against the user's own sampler project (a
      hand-written, Goertzel/RMS-flux based key+tempo detector that runs in milliseconds
      by design) and a question of which estimator is actually worth its cost. Profiled
      each in isolation on the 5:08 real song: `beat_this_cpp` 9.2s vs
      `RhythmExtractor2013` 2.7s (+ `BeatsLoudness`) — `beat_this_cpp` is ~3.4x more
      expensive but also the more accurate one (the only estimator that gives downbeats;
      a neural model trained specifically for beat/downbeat tracking, generally stronger
      than a classical multifeature extractor on syncopated/complex material). Made
      `beat_this_cpp` the default, primary estimator; `RhythmExtractor2013` (+
      `BeatsLoudness`, + `bpm_ratio`) is now opt-in via `--recheck-tempo`, for comparison
      against `beat_this_cpp` rather than a default-on second opinion — same
      "store-everything, don't resolve automatically" spirit as before, just no longer
      unconditional. Default rhythm-stage cost on the same song: ~12.1s → ~9.4s.
      `Danceability` (0.13s) stays unconditional — cheap and independent of which tempo
      estimator runs. `mira inspect`'s disagreement warning only fires when
      `--recheck-tempo` was used (bpm_ratio stays 0 otherwise, by design, not a bug)
- [x] Tempo stability metric (§5, §14.1) — real-file testing on a 14:20 through-composed
      score mix ("multiple tempos and stuff") showed exactly the failure mode a single
      whole-file BPM can't represent: `beat_this_bpm` 105.4 (a defensible average) while
      manually windowing the beat array in 60s chunks showed the piece actually running
      60→175 BPM across sections. Added `computeTempoStability()` in `Mir.cpp`: local BPM
      in 60s windows over `beatThisBeats`, then stddev/range across those windows, stored
      as `tempo_stability_bpm_stddev`/`tempo_range_bpm`/`tempo_window_count`/
      `tempo_unstable` (first-pass, unmeasured threshold: stddev > 10 BPM). Requires ≥3
      windows (~3 min of material) before judging anything — 2 windows on a short cue
      produced a spurious instability flag in testing (noise from beat-tracking edge
      effects, not real drift), so `tempo_window_count` stays 0 below that, meaning
      "insufficient duration to judge," not "stable." Verified on the real score mix:
      stddev 29.25 BPM, range 114.56 BPM over 14 windows, correctly flagged
- [x] Fixed a real `mira inspect` display bug found while verifying the above: numeric
      rhythm fields (`essentia_bpm`, `bpm_ratio`) are always serialized in `machine` JSON
      (default 0.0 when unmeasured, e.g. `--recheck-tempo` wasn't used), so
      `json_extract`'s "does this field exist" is always true even at 0 — the old
      presence check (`if (auto x = jsonExtractDouble(...))`, true for any non-null
      value including 0.0) showed a fabricated "essentia: 0.00 BPM" and a spurious
      "⚠ estimators disagree (ratio 0.000)" on every file analyzed *without*
      `--recheck-tempo`. Fixed by gating on `*value > 0.0` (a real BPM), not just
      optional-has-value — caught by inspecting a real file analyzed both ways back to
      back, not by the smoke suite (which always paired `--recheck-tempo` with the
      `inspect` test, masking this)
- [x] `BeatsLoudness`, `Danceability` (§5)
- [x] Full beat array stored, not just the BPM scalar (§6) — `essentia_beat_ticks`,
      `beat_this_beats`, `beat_this_downbeats` all stored in full in `machine`
- [x] Vendor + wire in `libKeyFinder` for key detection, gated on harmonic content
      (§5, §12b) — `src/mira/analyze/Key.cpp`. Originally gated on spectral flatness as
      a proxy (no harmonicity descriptor existed yet); now gated on the real harmonicity
      descriptor once that landed (DSP section above) — threshold on the harmonicity
      score is still a documented, unmeasured first guess, but the *signal* is the real
      one PRD §12b asks for, not a proxy. Verified: white noise (harmonicity 0, 0
      confident frames) gets no key section; flamenco.wav gets one ("F minor")
- [x] Vendor + wire in `Chordino`/`NNLS-Chroma` for chord sequence, gated on harmonic
      content (§5, §12b) — `src/mira/analyze/Chords.cpp`. Harder integration than key
      detection: Chordino is a `Vamp::Plugin` (its native interface, not something with a
      simple function call), and it declares `FrequencyDomain` input, so it needs
      `vamp-hostsdk`'s `PluginInputDomainAdapter` (FFT framing) and
      `PluginBufferingAdapter` (block-size negotiation) in front of it — essentia's
      vendored vamp SDK copy only has the plugin-side headers, not these host-side
      adapters, so a second, complete `vamp-plugin-sdk` clone was needed. Two real bugs
      found and fixed along the way: upstream's `CMakeLists.txt` has the same
      option-guard-doesn't-actually-guard-the-block bug seen in `beat_this_cpp`'s and
      essentia's builds now three times; and a double-free crash (SIGSEGV) in mira's own
      code — Vamp's `PluginWrapper` destructor deletes the plugin it wraps, so wrapping
      each layer (`Chordino`, `PluginInputDomainAdapter`, `PluginBufferingAdapter`) in
      its own `unique_ptr` deleted the inner ones twice. Fixed by only owning the
      outermost adapter. Verified on flamenco.wav: 10 chord segments (Cm, F/G, C#, C,
      Eb7/G...), gated off correctly for white noise same as key detection
- [x] Basic Pitch (`nmp.onnx`) note transcription via ONNX Runtime (§5, §12b) —
      `src/mira/analyze/Transcription.cpp` + `BasicPitchOrt.cpp` + `BasicPitchNotes.cpp`.
      Confirmed the exact 230,444-byte model PRD §12b names is committed in-tree in
      github.com/sevagh/basicpitch.cpp, not fetched separately. Unlike `beat_this_cpp`
      and `nnls-chroma`, this one's note-*decoding* logic (peak-picking, the "melodia
      trick", Gaussian-windowed pitch-bend estimation) was adapted rather than built
      unmodified — reimplementing Basic Pitch's published post-processing algorithm from
      scratch here would have carried real correctness risk with no easy way to verify
      against ground truth, so it's ported from that repo's `midi_notes.cpp` (MIT)
      instead, trimmed to note-event output (the upstream file's MIDI-serialization half
      needs `libremidi`, which mira doesn't vendor — not needed since notes are stored
      directly in `machine`, not as MIDI files). Also had to resample to the model's
      fixed 22050 Hz internally (via Essentia's `Resample`, already linked) since
      `AudioLoader` preserves native sample rate. Gated the same as rhythm (skipped for
      one-shots) but **not** on harmonic content like key/chords — deliberately, since a
      sparse/empty transcription on noisy material is itself informative. Verified on
      flamenco.wav: 80 notes, MIDI pitches in a sane guitar range; white noise still
      gets a (near-empty, 1-note) transcription rather than being gated out entirely
- [x] Camelot/Open Key notation lookup table (§12b) — two independent direct lookups
      from libKeyFinder's 24-key enum via the circle of fifths, rather than converting
      one system to the other by a numeric offset formula. This sidesteps the exact
      ambiguity the PRD flags ("sources disagree on the exact Camelot<->Open Key numeric
      offset") instead of resolving it — worth the openkeyscan-analyzer cross-check the
      PRD suggests before treating this as ground truth, which hasn't been done yet

**CLI**
- [x] `mira analyze` — idempotent and resumable via `analyzed_at IS NULL` + `--force`,
      `--limit N`, `--content-type <type>` (§8). `--resume` is implicit (re-running
      without `--force` already only processes unanalyzed rows) rather than a separate
      flag — `--content-type` without `--force` is a hard error, since every unrouted
      row is `content_type='unknown'` and the filter would silently match nothing
- [x] `--chords` and `--transcribe`, opt-in (off by default) — added after the first
      real-world test (a 5:08 song, not a short fixture) showed `mira analyze` taking
      39s per file. `--verbose` (also added) broke that down by stage: Chordino
      (chords) was 15.0s/39% of total, Basic Pitch (transcription) 3.7s/10%, vs. 0.4s
      for key alone — both are well past what "BPM/key/loudness across a drive" (the
      Phase 1 headline goal, PRD §9) needs, so they're opt-in now. Default `analyze`
      dropped from 39.0s to 20.6s on the same file (~15x realtime up from ~7.9x).
      Rhythm (Essentia multifeature + the `beat_this_cpp` neural net) turned out to be
      the single biggest cost even in default mode — 58% of the 20.6s — left on by
      default anyway since it's core to the Phase 1 goal, but a candidate for a future
      optimization pass if 20.6s/song is still too slow for real use
- [x] `mira inspect <file|id>` — human-readable report, surfaces low-confidence
      tempo/key rather than hiding it, reports `active_ratio` (§8) — `runInspect` in
      `main.cpp`, reads `machine` via SQLite's `json_extract`/`json_array_length` rather
      than a C++ JSON parser (none vendored). Flags tempo disagreement (`bpm_ratio` far
      from 1.0) inline rather than hiding it, matches PRD's example exactly. Handles
      "not yet analyzed" and "not found" without crashing

**Fixtures + tests**
- [ ] Fixture clips covering one-shot / loop / track / stem, incl. a mostly-silent stem
      and non-tonal material (§9 Phase 0 skeleton, carried into Phase 1) — only
      `fixtures/flamenco.wav` is checked in; the rest of this section's coverage comes
      from `tests/smoke_test.sh` synthesizing fixtures on the fly with `ffmpeg` (sine
      tones, silence, white noise) rather than committing more binary files
- [x] Content-type router regression tests — informally, via `tests/smoke_test.sh`
      (72 assertions as of this commit), not a dedicated test binary
- [ ] Numerical-parity tests for the DSP/MIR path, extending `lab/`'s harness pattern —
      `lab/`'s harness only covers the Phase 0 ONNX embedding path so far, not any of
      Phase 1's DSP/MIR work

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
- [ ] `mira analyze` keeps every candidate file's full decoded audio buffer in memory for
      the whole run (so sibling-set grouping can see all durations before deciding
      anything). Fine at today's scale; revisit — probably a duration-only first pass,
      re-decoding for the real analysis — before running this over a real drive-sized
      batch
