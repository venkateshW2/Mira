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

## Phase 2 — classify + search (PRD §9 Phase 2, §2c, §5) ✅ COMPLETE (2026-09-10)

Exit: the Sononym-parity milestone. All items below done; two genuine memory-safety bugs
(unrelated to Phase 2's own scope — a shared-`Ort::Env` anti-pattern and a container-
overflow inherited from the Phase 0 spike) found and fixed via AddressSanitizer while
stress-testing this phase's work, not left as latent risk for Phase 3.

- [x] `discogs-effnet-bsdynamic-1` embedding pass wired into the pipeline (mandatory
      input to every head below, and the similarity vector) (§2c, §5) — the "-bs64" name
      in this line's original text was the wrong variant; `-bsdynamic` is the one PRD
      §16.3 flags as existing *only* as ONNX (MTG never ported it to TF), and the one
      spike/02_onnx_parity actually bitwise-verified. `src/mira/analyze/Embedding.cpp`
      adapts that spike's mel-frontend + patching + inference almost directly, on every
      content type (not gated like rhythm/key — embedding-based similarity is exactly the
      point of comparing one-shots). Mean-pools per-patch 1280-d embeddings into one
      file-level vector; `patch_count` stored alongside so a caller can tell "1 patch" from
      "125 patches" rather than treating them as equally confident. Cost on a real 2:06
      score cue: 1.7s
- [x] CED-small content gate (ONNX) — "is this even music?" before running music heads
      (§2c). `src/mira/analyze/ContentGate.cpp`. Real integration work beyond the Phase 0
      spikes (no spike covered this model): its feature frontend is kaldi-style fbank
      (frame_length_ms=32, dither=0, 64 mel bins, linear not log — read directly from
      sherpa-onnx's own offline-stream.cc CEDTag constructor, not guessed), not Essentia's
      TensorflowInputMusiCNN — so `vendor/kaldi-native-fbank` (Apache-2.0,
      csukuangfj/kaldi-native-fbank, the exact library sherpa-onnx itself links) and
      `vendor/kissfft` (its real-FFT backend) are now vendored too, compiled directly like
      nnls-chroma rather than add_subdirectory'd (upstream's own CMake pulls in Python
      bindings/tests by default). Two real bugs found via real-file testing, not synthetic
      fixtures alone: (1) CED-small throws an ONNX Runtime broadcast error on long
      single-shot input (a 2:06 real file failed, a 14.2s one didn't) — sherpa-onnx's own
      CLI doesn't chunk, but its test fixtures are all short, so this limit was likely
      never exercised upstream; fixed by chunking into ≤10s windows. (2) Initially
      mean-pooled probabilities across chunks, which measurably backfired: flamenco.wav
      split into a 10s chunk (0.52 on "Music") and a 4.2s tail (0.21) — the mean, 0.37,
      was *lower* than white noise's single-chunk 0.39, inverting the gate. Switched to
      max-pooling per class across chunks (a quiet/sparse section shouldn't veto an
      otherwise clearly musical file — the same reasoning active-region detection already
      applies elsewhere) — flamenco.wav now scores 0.52, a real 2:06 score cue scores
      0.79. `kContentGateMusicThreshold = 0.45` is a first-pass threshold from these real
      measurements, not a labeled dataset — explicitly flagged for revisiting
- [x] `mtg_jamendo_instrument-discogs-effnet-1` head (40 classes) (§2c, §5) —
      `src/mira/analyze/Instrument.cpp`. Same input/output contract as moodtheme
      (`model/Placeholder` [1280] → `model/Sigmoid` [40]), which prompted extracting a
      shared `runClassificationHead()` (`ClassificationHead.cpp`) out of what was
      moodtheme-only ONNX session boilerplate — three near-identical heads justified the
      abstraction (the same "don't triple the same work" lesson as the DSP spectral-pass
      consolidation earlier in Phase 1). Real validation, not just non-crashing: on
      flamenco.wav (solo classical guitar) top instruments are
      guitar/classicalguitar/acousticguitar/bass/electricguitar — correctly identifies the
      actual instrument. Gated the same as moodtheme (ContentGate's `is_music`, 3ms cost)
- [x] **Real-world finding, not in the original PRD scope**: `mtg_jamendo_instrument`
      is unreliable on *isolated stems* specifically — real testing on a 15-file delivery
      stem set (BASSS_1.wav, GTR.wav, STRINGS.wav, etc.) showed `synthesizer` dominating
      almost every stem regardless of the actual instrument, while the same head correctly
      read the full mix (piano/guitar/violin/flute/strings/voice, plausible and
      differentiated). Root cause: `discogs-effnet`'s embedding — and by extension every
      head built on it — was trained entirely on full-mix audio; an isolated stem's
      acoustic signature (huge dynamic range, long silences, none of a mix's spectral
      density) is out-of-distribution for the whole pipeline, not just the classifier.
      Researched three alternatives (IRMAS-trained wav2vec2 model, OpenMIC-2018-trained
      models, nii-yamagishilab's NSynth-pretrained-then-IRMAS-fine-tuned model) — chose
      the last: purpose-built for exactly this isolated/predominant-instrument gap
      (APSIPA 2023 paper), MIT-licensed, small (5.5MB checkpoint, ResNet34-based).
      Converted `irnet4irmas.ckpt` (PyTorch Lightning) to ONNX via
      `lab/export_irmas_instrument_onnx.py` — reconstructed the architecture from the
      published source (SincConv learnable filterbank → custom 16-64-128-channel ResNet34
      → LDE pooling (D=8) → Linear(1024,11)) since the checkpoint alone doesn't carry
      model code; `lab/conversion_sources/irmas_predominant/` holds the fetched reference
      source (gitignored, `scripts/fetch-vendor.sh` re-fetches it). Verified two ways: (1)
      graph parity — same input through the reconstructed PyTorch model and the exported
      ONNX model agree to 1.0e-5 max abs diff, confirming the reconstruction is exactly
      right, not approximately right; (2) real audio — flamenco.wav through the model's
      actual trained preprocessing (16kHz mono, 1-second windows, -12 LUFS loudness
      normalization via `pyloudnorm`, matching `IRMASDataset` exactly) surprisingly
      predicts **voice** (mean 0.79 across 14 windows) over guitar (mean 0.029, though its
      per-window max reaches 0.32). Graph correctness and real-world reliability are two
      different questions — the first is settled, the second is not.
- [x] Wired into the C++ pipeline as `src/mira/analyze/StemInstrument.cpp`, run only for
      `content_type == 'stem'` (gated on `is_music` too), stored under a separate
      `stem_instrument` JSON key alongside (not replacing) `instrument` — "store the
      disagreement" (PRD §14.1), same as tempo. Before wiring it in, re-tested the
      flamenco.wav "voice" surprise against a second, more representative real file — a
      real 37-minute "STRINGS LOW" delivery stem — which the model got right and far more
      decisively than `mtg_jamendo_instrument` did on the same file (cello 0.52 mean vs.
      `mtg_jamendo_instrument`'s weaker 0.27/0.24 violin/cello split); read the flamenco.wav
      result as likely specific to that short demo-loop fixture (unusual percussive
      rasgueado guitar technique), not a systemic failure, and proceeded on that basis.
      C++ implementation runs 1-second non-overlapping windows (matching training) over
      the already active-region-restricted audio, mean-pooled. One approximation,
      documented not hidden: the model's training used `pyloudnorm`'s exact ITU-R BS.1770
      loudness measurement to normalize each window to -12 LUFS; Essentia's
      `LoudnessEBUR128` only measures stereo, so a mono window is duplicated to L=R and
      corrected by -10·log10(2) (≈3.01 dB) for the doubled channel power, rather than
      implementing BS.1770 K-weighting from scratch in C++. Verified end-to-end on the
      same real STRINGS LOW stem through the actual C++ pipeline: cello 0.53 mean (vs.
      0.52 in the Python/pyloudnorm reference) — close enough to confirm the approximation
      holds up in practice, not just in theory. Cost: 27.8s on this 933-window (37-minute,
      active-region-restricted to ~15.5 min) stem — real but bounded by how much of a
      stem is actually active, same as every other stage here
- [x] `mtg_jamendo_moodtheme-discogs-effnet-1` head (56 classes) — already downloaded in
      Phase 0 spikes (§2c, §5). `src/mira/analyze/MoodTheme.cpp`, now built on the shared
      `runClassificationHead()` (see instrument entry above), gated only on ContentGate's
      `is_music` signal, not on router content_type at all — "Music heads must only run on
      music" (§2c), same honesty principle as the harmonicity gate for key/chords, just
      using a real content classifier instead of a DSP proxy. Known, documented
      simplification: feeds the single whole-file mean-pooled embedding through the head
      once, rather than running per-patch and averaging *sigmoid outputs* (MTG's own
      reference pipeline does the latter — averaging before vs. after a nonlinearity are
      not equivalent). Verified sensible, not just non-crashing, against two real files: a
      "Dance"-titled score cue scores highest on happy/corporate/uplifting/positive/
      energetic; label names are raw MTG-Jamendo strings for now (the versioned-YAML
      normalisation below is separately scoped, not done here)
- [x] `genre_discogs400-discogs-effnet-1` converted to ONNX and wired into the C++
      pipeline (400 classes) (§2c, §16.3) — `lab/pyproject.toml` retargeted to Python 3.11
      (tf2onnx 1.17.0 needs TF 2.13-2.15, which has no cp312 wheels — the `lab/` Python
      version was originally pinned to 3.12 for the onnxruntime parity spike, which
      doesn't actually require that specific version). Converted via
      `python -m tf2onnx.convert --graphdef ... --inputs serving_default_model_Placeholder:0
      --outputs PartitionedCall:0`. Verified against TF directly (not just "runs"): max abs
      diff 2.06e-6 on the same random 1280-d input, well inside the ~1e-4 exit criterion.
      Labels in `src/mira/analyze/GenreLabels.h` (generated, 400 raw Discogs genre/style
      strings). `src/mira/analyze/Genre.cpp` — same shared `runClassificationHead()` as
      moodtheme/instrument, gated on the content gate's `is_music`, ~2ms cost. Genre
      labels are the first with spaces/punctuation ("Blues---Boogie Woogie", "Rock---Yé-
      Yé"), unlike moodtheme/instrument's bare-word labels — `mira inspect`'s lookup needed
      double-quoting the JSON path key segment (`$.genre."Blues---Boogie Woogie"`), not just
      `$.genre.label` like the other heads. Verified sensible, not approximately: on
      flamenco.wav (a real flamenco guitar loop) the top result is literally
      "Folk, World, & Country---Flamenco" at 0.98 confidence
- [x] `voice_instrumental-discogs-effnet-1` converted to ONNX and wired into the C++
      pipeline (§2c, §16.3) — same tf2onnx pipeline, `model/Placeholder:0` →
      `model/Softmax:0`. Verified against TF: max abs diff 1.79e-7. 2 classes
      (`instrumental`, `voice`) — small enough not to need a generated label header, same
      precedent as `Danceability.cpp`'s 2-class case. `src/mira/analyze/
      VoiceInstrumental.cpp`, stored as `voice_instrumental.voice_probability`. Verified:
      flamenco.wav (solo guitar, no vocals) scores 0.00
- [x] `danceability-discogs-effnet-1` head (§2c, §5) — `src/mira/analyze/Danceability.cpp`,
      stored as `danceability_head.danceable_probability` (distinct JSON key from
      `rhythm.danceability`, Essentia's existing DSP-based `Danceability` algorithm — a
      second opinion, not a duplicate; `mira inspect` shows both, labeled). Verified
      against real files: flamenco.wav (solo guitar) scores 0.01, the real "Dance"-titled
      score cue scores 0.60 — correctly separated
- [x] Label normalisation as versioned YAML data files, not code (§5) — started with
      instruments only (`taxonomy/instrument-labels.yaml`), the head with the most
      real-world friction so far (raw `mtg_jamendo_instrument` output like `electricguitar`
      one unspaced word, plus a second model — `stem_instrument` — using an entirely
      different 3-letter IRMAS code vocabulary for the same instruments). Explicitly
      NOT a general "make labels tidy" pass: the rule, corrected mid-design after an
      early draft wrongly proposed collapsing `electricguitar`/`acousticguitar` into one
      `guitar` bucket, is that normalization only merges labels across models that name
      the *exact same* instrument (`electricguitar` ↔ IRMAS's `gel`, both → `electric
      guitar`) and never collapses genuinely distinct instruments for tidiness — electric,
      acoustic, and classical guitar stay three separate canonical terms. Canonical terms
      are chosen to read naturally in a caption sentence (Phase 3, PRD §11), not to be
      short. `src/mira/taxonomy/Taxonomy.cpp` loads it via libyaml's document API — already
      linked into `mira` transitively (`PkgConfig::YAML` was an existing Essentia
      dependency, just not previously used directly by mira's own code) — no new library
      needed. Loaded once per `analyze` run, not per file. Loading failure (missing file,
      no matching model key) degrades to raw-labels-only rather than a hard error.
      Genre (400 classes) done next, same session — `taxonomy/genre-labels.yaml`. Different
      character from the instrument file: `genre_discogs400`'s raw labels are Discogs'
      own `"Genre---Style"` hierarchy (e.g. `"Blues---Boogie Woogie"`), which is a pure
      *formatting* problem (that `---` separator is a training-data convention, not
      something a caption would say), not a taxonomy design problem — no per-label
      judgment calls needed, unlike instrument's electric/acoustic/classical guitar
      question. Generated mechanically (`"---"` → `": "`) for all 400 labels after
      confirming every one matches that exact one-separator pattern with no exceptions;
      hand-typing 400 entries would have been pure risk (typos) for zero benefit over a
      script applying one rule uniformly. Verified on flamenco.wav: raw
      `"Folk, World, & Country---Flamenco"` (0.98) → normalized
      `"Folk, World, & Country: Flamenco"` (0.98), `mira inspect`'s genre line switched to
      showing the normalized form. moodtheme (56) and the content gate's AudioSet labels
      (527) get no taxonomy file, on purpose, not as a gap: checked both full label lists
      directly and both are already clean, human-readable words/phrases as-is (`action`,
      `calm`, `energetic`; `Music`, `Speech`, `White noise`) — an identity-mapped YAML file
      for either would only double JSON size for zero actual change
- [x] Real crash found and fixed while stress-testing the above (unrelated to genre/
      taxonomy code itself, confirmed by reproducing it on a file where the taxonomy path
      never even executes): intermittent SIGSEGV in `mira analyze`, reproduced with lldb
      (crash inside libsystem_platform.dylib's memmove, corrupted pointer, backtrace
      unable to unwind further — the classic signature of memory corruption manifesting
      far from its actual cause). Two real, separate bugs, both found via AddressSanitizer
      after lldb's post-mortem trace couldn't get past frame 0:
        1. Every ONNX-backed analyzer (`Embedding`, `ContentGate`, `ClassificationHead`,
           `StemInstrument`, plus `beat_this_cpp`'s vendored `Impl`) constructed its own
           local `Ort::Env` per call. `Ort::Env` owns process-global state (thread pools,
           logging); repeated construction/destruction within one process is an ONNX
           Runtime anti-pattern, not a supported one. With up to 8 ONNX calls now
           happening per file (embedding, content gate, 5 classification heads, stem
           instrument — this crash likely existed at lower odds since `beat_this_cpp` was
           added in Phase 1, but became reliably reproducible only once Phase 2 added
           enough heads), this reliably corrupted memory. Fixed with one shared,
           process-lifetime `Ort::Env` (`src/mira/analyze/OrtEnv.h`, function-local static)
           used by all of mira's own ONNX call sites, plus a documented local patch to
           `vendor/beat_this_cpp/Source/beat_this_api.cpp` (added to
           `scripts/fetch-vendor.sh`'s patch list) giving it its own equivalent static —
           self-contained rather than reaching into mira's headers from an unrelated
           vendored library.
        2. That fix alone only reduced the crash rate, it didn't eliminate it — AddressSanitizer
           then caught the real remaining bug directly: `Embedding.cpp`'s patch-count
           formula, `nPatches = 1 + (nFrames - kPatchSize) / kPatchHopSize`, relied on C++
           integer division truncating toward zero to detect "too short for a patch" via
           `nPatches < 1`. That's wrong: `-4 / 62 == 0` in C++ (truncation toward zero, not
           floor), so a file with `nFrames` just under `kPatchSize` (128) could compute
           `nPatches == 1` instead of `0`, then read straight past the end of the `bands`
           mel-frame vector — a genuine container-overflow, confirmed by ASan pointing at
           the exact line. Inherited from `spike/02_onnx_parity/main.cpp`, whose bitwise-
           parity verification never happened to test a file at this exact boundary
           length. Fixed by guarding `nFrames < kPatchSize` explicitly before the division,
           not by relying on the division's rounding behavior. Verified with 50 clean runs
           on the exact previously-crashing file (release build) plus 20 clean runs under
           AddressSanitizer, after both fixes — 0/70, versus a reproducible ~15-20% crash
           rate before either fix
- [x] Label normalisation regression tests; both `raw` and `label` stored (§5) — both
      `instrument`/`instrument_normalized` and `stem_instrument`/`stem_instrument_normalized`
      stored side by side in `machine` JSON (raw is never replaced, only supplemented).
      Smoke test asserts the actual rule, not just "a mapping exists": electric/acoustic/
      classical guitar remain three distinct keys in the normalized output on a real file
      (flamenco.wav), each correctly renamed from its raw camelCase form with the score
      preserved exactly. Verified end-to-end on two more real files beyond the smoke
      fixture: flamenco.wav's raw `guitar`(0.25)/`classicalguitar`(0.11)/`acousticguitar`
      (0.07)/`electricguitar`(0.05) → normalized `guitar`/`classical guitar`/`acoustic
      guitar`/`electric guitar` unchanged in score; a real "STRINGS LOW" stem's raw `cel`
      (0.53) → normalized `cello` (0.53) via the IRMAS-code taxonomy branch
- [x] Embedding store in `sqlite-vec` (float32[1280] per file) wired into the schema
      (§6, spike already proved the mechanics) — `vec_embeddings` virtual table
      (`vec0(embedding float[1280])`), keyed by `files.id` as rowid, alongside `files`
      rather than a column on it (vec0 tables have their own storage format). Two real
      build issues beyond the spike, both from building this into the actual app rather
      than a standalone spike binary: (1) the spike's own `sqlite_vec` CMake target name
      collides with `MIRA_BUILD_SPIKES=ON` building spike/04_sqlite_vec in the same
      configure — renamed mira's to `mira_sqlite_vec`, left the spike untouched. (2)
      `main.cpp` includes `sqlite-vec.h` directly (to register `sqlite3_vec_init` via
      `sqlite3_auto_extension` before any connection opens, required for a static link);
      without `SQLITE_CORE` defined on the `mira` target itself (not just the
      `mira_sqlite_vec` library), that header pulls in `sqlite3ext.h`'s macro-redirected
      API instead of linking directly against `mira_sqlite3` — silent until link time,
      "undeclared identifier 'sqlite3_api'" at compile time in this case, only surfaced by
      actually building the full app target, not just the isolated library
- [x] `mira similar <file|id>` (§8) — exact brute-force KNN (PRD §3: "no ANN index"),
      verified against real files: 3 cues from the same real score cluster at distance
      ~3.3 from each other, while an unrelated solo-guitar piece sits at ~6.0-6.2 from all
      three — the embedding space is doing real, sensible clustering, not just running
      without crashing. Scoped narrower than the PRD line for now, explicitly, not
      silently: only the overall embedding (no `--by timbre|rhythm|spectrum` subsets),
      `--n`, and only files already in the library (no external-file mode yet) — `--filter`
      and those are separate, still-open work
- [x] `mira search "--filter" ...` — expressions over tags/descriptors (§8) — a small,
      fixed grammar (`src/mira/main.cpp`'s `translateSearchCondition`), not a general
      query language: comma-separated conditions ANDed, each `field OP value` (a table of
      ~11 known numeric/string fields — bpm, duration, loudness, danceable, key,
      content_type, etc.) or `tag:value` (genre/instrument/mood name). Real bug found and
      fixed while building this: the first tag-matching implementation checked "does this
      substring appear anywhere in the object's raw JSON text," which is nearly always
      true for `genre`'s 400-entry object (every label is stored regardless of score,
      most near zero) — `genre:Techno` matched a flamenco file because *some* of the 400
      genre labels happened to contain "Techno" at a near-zero score. Fixed with
      `json_each()` to check the key (substring, so a search for a genre's *style* alone
      still hits its canonical `"Genre: Style"` form) and the score (a real threshold)
      together, per entry — a smoke test regression-checks this exact failure mode
      (`genre:Techno` against flamenco.wav must return 0 matches, not a false hit)
- [x] `mira models --download | --list` (§8) — `--list` does a straight filesystem check
      against the same compile-time `MIRA_*_MODEL` paths every analyzer already uses (10
      models total, rhythm/transcription included), so it can't drift from what `analyze`
      actually loads. `--download` deliberately doesn't fetch anything itself — no
      Python/network dependency at runtime (PRD §2d) — it names `scripts/fetch-vendor.sh`
      or the relevant `lab/` conversion script for whatever's missing
- [x] `mira stats` — library composition, coverage, unmapped labels (§8) — file counts by
      content_type, analyzed vs. scanned-only, per-classification-head coverage (`json_extract`
      presence checks against `machine`, not estimated), embeddings stored (`sqlite-vec`
      row count). "Unmapped labels" is a taxonomy *completeness* check, not a per-file DB
      scan: for every raw label a model can actually produce (all 40 instrument classes,
      all 11 IRMAS codes, all 400 genre classes), does `taxonomy/*.yaml` have an entry?
      Currently reports 0/0/0 unmapped across all three, since both taxonomy files were
      generated to cover every label their source model list had at the time — this is
      the mechanism that will actually catch it if MTG or Discogs ever adds new labels a
      future model update produces that the taxonomy hasn't caught up to yet

---

## Phase 3 — SA3 captioning (PRD §9 Phase 3, §11, §15, NOTES.md §4-5)

Ordered after all DSP/MIR/classification are complete, so the renderer never gets
designed around a missing field.

- [x] Field-dict + prose renderer architecture: one analysis document, N renderers (§11)
      — `src/mira/caption/CaptionFields.{h,cpp}`. `extractCaptionFields()` builds one
      `CaptionFields` struct per file from `machine` JSON (confidence-gated: a value below
      threshold is left out of the struct entirely, per §12.6 — see the last item below),
      with `human` overrides layered on top. Renderers (`Sa3Renderer` is the first; N=1
      for now) read only this struct, never `machine` JSON directly — verified by
      construction, not just claimed: `Sa3Renderer.cpp` has zero `json_extract`/`Database`
      references
- [x] SA3 key-value tag renderer — `src/mira/caption/Sa3Renderer.cpp`'s `renderSa3Tags()`.
      Grounded directly in SA3's own source (`sa3-studio/stable-audio-3/stable_audio_3/
      interface/reprompt.py`'s `TRACK_TYPE_PREFIXES`), not the generic AudioSparx-tag
      story from the public prompting guide — the two disagree, and reprompt.py (what
      Stability's own team built to elicit good output from this exact trained model) is
      the stronger signal. Every value is a plain string, never a JSON array: reading
      underfit's actual `prompt_templates.py` found that its `_get()` silently keeps only
      a list value's first element, so a multi-value field passed as a JSON array would
      be silently truncated by underfit at dataset-load time — a real bug caught by
      reading the training code, not a style choice. Includes a `"prompt"` key holding
      the prose renderer's output verbatim, since underfit treats `"prompt"` as a
      passthrough tag key
- [x] SA3 prose renderer, 256-token / 45-word ceiling — `renderSa3Prose()`, same file.
      Shape is `TrackType: X, VocalType: Y, ` prefix (exact strings from reprompt.py,
      stems mapped onto reprompt.py's own "instrument" category, whose classifier
      definition literally says "or with words solo or stem") + a descriptive clause
      built from CaptionFields' gated genre/instrument/mood labels + trailing
      `BPM: N. Length: N seconds` (period-joined, not the AudioSparx comma-tag style).
      One-shots get no TrackType prefix and never a BPM field, matching reprompt.py's own
      One-shot template exactly. Not free prose — mira has no text-generation step, so
      this is a grammatical join of measured labels, not invented texture language
      (see NOTES: mira currently cannot honestly produce "cavernous reverb"-style
      descriptions — no reverb/space descriptor exists yet, and the closest thing,
      moodtheme, is a blunt 56-word closed vocabulary; adding real DSP signal for this is
      a separate, later, scoped decision, not done here). 45-word ceiling enforced by
      progressive truncation (drop mood clause, then extra instruments beyond the top
      one) before ever dropping BPM/Length; 256-token hard ceiling is the frozen
      T5Gemma conditioner's own tokenizer truncation (confirmed from the actual shipped
      `sa3-medium`/`sa3-sm-music` `training_template.json`, not the class default),
      approximated here by word-budget trimming since mira has no T5Gemma tokenizer at
      render time
- [x] `mira caption <file|id>` CLI (§8, by extension) — `runCaption` in `main.cpp`.
      Prints the prose form plus the flat tag set, `--trigger <token>` for the LoRA
      trigger word, `--emit-sidecar` writes `<file>.json` next to the source audio
      (`renderSa3SidecarJson()`) for underfit's dataset loader to pick up directly
      (`pre_encode.py`'s JSON-sidecar path keeps every string/int/float key it finds).
      14 new smoke-test assertions, including a regression check that no sidecar field is
      ever serialized as a JSON array
- [x] `seconds_total` field, trigger-token injection — resolved, not built: trigger-token
      injection is done (`--trigger`, above, threaded through `mira caption`/
      `export-segments` alike); `seconds_total` itself is deliberately **not** written
      into the sidecar JSON as a training field — it's a separate numeric conditioning
      channel underfit's own `pre_encode.py` computes directly off the audio file, not
      something mira's caption renderer should assert a second, possibly-drifting copy
      of. (A `length_seconds` *text* tag is emitted for the "prompt"-only training path,
      per reprompt.py's own convention of restating duration in prose alongside the
      numeric channel — SA3's own tooling double-conditions duration on purpose, unlike
      BPM.) Originally listed as open only because it was bundled with trigger-token
      injection on one checklist line
- [x] `mira tag <file|id>` CLI — the write side of `human` overrides, added after a
      real use case surfaced one during Phase 3 testing: a score composer wants to
      hand-label pieces with words no analyzer could ever produce ("funny", "quirky",
      "action", "drama" scene descriptors) so inference-time prompts using those words
      retrieve similar-feeling pieces. Until this, `human` was readable
      (`CaptionFields.cpp`) but nothing ever wrote to it — a real gap, not a deferred
      item. `Database::setHumanField` merges one key at a time via SQLite's own
      `json_set(human, ?, json(?))`, so repeated `mira tag` calls for different fields
      never clobber each other (verified: a `--keywords` call followed by a `--key` call
      both survive); `--clear` resets to `{}`. Added a new canonical field, `keywords[]`
      (PRD §15 always specified it, CaptionFields skipped it) — the *only* CaptionFields
      field with no machine-derived source at all, since mira has no analyzer for
      narrative/vibe judgment and isn't meant to grow one. Folded into the prose
      renderer's mood clause alongside moodtheme's gated labels (`Sa3Renderer.cpp`'s
      `moodClause()`), and — unlike moods, which the word-budget truncation can drop —
      keywords are never dropped by the renderer, since dropping a person's own label to
      save two words is a decision that should stay with the person. 10 new smoke-test
      assertions
- [x] Folder-level human defaults — `mira tag-folder <folder>`, `src/mira/db/Database.
      {h,cpp}`'s new `folder_defaults` table (`setFolderDefaultField`/`clearFolderDefault`/
      `findFolderDefaultsForPath`) plus `runTagFolder` in `main.cpp`. `folder_path` is a
      plain string prefix matched against `files.path` at caption-render time — not
      resolved against the filesystem, so it must be written the same way (relative vs
      absolute) the target files were actually scanned with, documented in the CLI help
      rather than silently assumed. Multiple matching ancestor folders all apply, ordered
      shortest-to-longest (`findFolderDefaultsForPath`), so a deeper folder's default
      overrides a shallower ancestor's for the same field — same "more specific wins"
      rule already established for segment-over-file, just one more level. Wired into
      `CaptionFields::extractCaptionFields` between machine-derived values and the file's
      own `human`, so precedence end to end is: machine < folder default(s) < file
      `human` < segment `human`. Verified on the real fixture: a folder-level `genre`
      shows up in the caption, then a per-file `mira tag --genre` on top of it correctly
      wins for `genre` specifically while the folder's `keywords` still comes through
      unaffected — 8 new smoke-test assertions

**Segment-level captioning — new addition, not in the original PRD** (raised by a real
score-producer workflow: a 40-minute cue delivered as a synced 16-stem set, where the
*scene* — funny, tense, dramatic — changes at particular timestamps within the file, so
no single whole-file caption can be honest about it). Two findings drove the design, both
checked against the actual shipped model configs rather than assumed:

1. This isn't only a captioning problem — SA3 has a **hard per-clip duration ceiling**
   regardless: `sample_size` in the real `training_template.json` files is 5,324,800
   samples (`sa3-sm-music`/`sa3-sm-sfx`, ≈120s) and 16,777,216 (`sa3-medium`, ≈380s) at
   44.1kHz. A 40-minute file has to be cut into pieces to be trainable at all, whether or
   not its mood ever changes.
2. SA3's conditioning is exactly two channels, confirmed from `stable_audio_3/models/
   conditioners.py` and the training configs: `prompt` (one flat T5Gemma-encoded text
   string) and `seconds_total` (one number). There is no timeline/segment structure in
   the model's own conditioning at all — a caption cannot vary within a clip. This
   matches PRD §15's own note that segments are "first-class in exactly one training
   pipeline (YuE's)... an internal correctness device, not a caption feature" for SA3.
   So the only lever is choosing *where to cut*, not how to caption within a clip.

- [x] `segments` table + `mira tag-segment` — `src/mira/db/Database.{h,cpp}`'s
      `SegmentRecord`/`createSegment`/`findSegmentsForGroup`/`findSegmentsForFile`/
      `findFilesByGroupId`, wired into `main.cpp`'s `runTagSegment`. A time-ranged
      counterpart to `human` on `files`: each row is exactly one of group_id-scoped
      (applies to every sibling stem in that cue's existing `files.group_id` at the same
      timestamps, keeping a synced stem set synced across the cut) or file_id-scoped (one
      long non-stem file segmented directly). Reuses `human`'s own JSON convention
      verbatim, just scoped to a time range
- [x] `CaptionFields::extractCaptionFieldsForSegment` — `src/mira/caption/CaptionFields.
      {h,cpp}`. Starts from the file's whole-file `CaptionFields` (genre/instruments/
      moods/bpm/key are not re-measured per segment — mira has no segment-level
      classification yet, that's Phase 4's still-unbuilt "segment-level analysis
      replacing whole-track averaging" — a documented simplification, not hidden),
      overrides `durationSeconds` to the segment's own length, then layers the segment's
      own `human` on top last, so a segment-specific tag wins over both the machine
      value and the file's untimed human override. The human-override application logic
      was refactored out of `extractCaptionFields` into a shared `applyHumanOverrides()`
      so file-level and segment-level extraction can never drift apart
- [x] `mira export-segments` + `AudioWriter` (`src/mira/export/AudioWriter.{h,cpp}`) —
      mira's first audio-*writing* path (everything before this only ever decoded, PRD
      §7). Writes 32-bit float PCM WAV directly from the samples `AudioLoader` already
      decoded — lossless, no re-encode step, no new codec dependency (a WAV header is
      ~44 bytes of arithmetic, not a library). For a group_id target, cuts every sibling
      stem at *identical* declared boundaries so the set stays in sync across the cut —
      the entire reason group_id-scoped segments exist rather than per-file ones. Each
      cut clip gets its own SA3 sidecar JSON next to it, via the same
      `extractCaptionFieldsForSegment` + `Sa3Renderer` machinery `mira caption` already
      uses. Verified end-to-end on the real flamenco.wav fixture, not just non-crashing:
      cut into a 0-7s and a 7-14s clip via two `mira tag-segment` calls, `ffprobe`
      confirms each output file is real, valid `pcm_f32le` audio at exactly the declared
      duration (not the whole 14s file duplicated), and each sidecar's `length_seconds`
      reflects the 7s cut, not the original file's 14s. 13 new smoke-test assertions
- [ ] Segment-boundary UI/authoring beyond the raw CLI (`--start`/`--end` in seconds by
      hand) — **deliberately not built in Phase 3**, not an oversight: a real "listen and
      mark" boundary workflow needs a waveform view, which needs the JUCE UI shell that
      doesn't exist yet (Phase 5). Building a CLI-only stopgap for this (guessing
      boundaries from onset detection, say) would guess at exactly the judgment call —
      *where* the scene changes — that only a person watching picture/listening can make;
      better to leave `--start`/`--end` as the honest, if tedious, interface until Phase
      5's real UI exists. TASKS.md Phase 5 already has "Segment slicing UI/export" as a
      placeholder this folds into directly — no new placeholder needed
- [x] Active-region-aware boundary validation — `activeFractionInRange()` in `main.cpp`,
      used by `export-segments`. Computes how much of a declared `[start,end)` boundary
      actually overlaps the file's own stored active spans (Phase 1's silence gate,
      previously computed but never cross-checked against anything); prints a note
      (never skips — a low fraction is real and expected for one stem in a synced set,
      e.g. brass simply not playing during a "funny" phrase, exactly the case this was
      built to surface rather than hide) when a cut is mostly silent for that particular
      file. Returns "not applicable" rather than a false 0% for any file active-region
      detection never ran on (Phase 1: only stems/declared stems/files over 5 minutes get
      it) — verified on a real synthesized silence/tone fixture, not just by inspection:
      a segment declared entirely inside a silence gap gets the note, an adjacent segment
      declared entirely inside a tone span does not, 2 new smoke-test assertions
- [x] Machine pre-fills genre/mood/instruments as an editable draft; human field always
      wins on conflict (§6, §11) — `CaptionFields.cpp`'s human-override block. No
      project-wide `human` JSON schema exists yet, so this is `CaptionFields`' own
      minimal, documented convention (`$.genre`/`$.instruments`/`$.moods`/`$.bpm`/`$.key`/
      `$.is_instrumental` at the top level of `human`, each replacing — not merging with —
      the machine-derived value when present). Folder-level defaults (the item above)
      would need to populate `human` this same way, not a new mechanism
- [x] Sidecar JSON export (`--emit-sidecars`) (§6) — see `mira caption --emit-sidecar`
      above (singular flag name; TASKS.md's plural was the working title)
- [x] Confidence-gated field omission at render time — the renderer omits low-confidence
      fields rather than asserting them (§12.6) — enforced in `CaptionFields.cpp`, not in
      the renderer: genre/instrument/mood each need a real per-label score above a
      documented first-pass threshold to appear in the struct at all; BPM is additionally
      withheld when `Mir.cpp`'s tempo-stability check flagged the file as not having one
      representative tempo. Verified on the real flamenco.wav fixture: its top moodtheme
      score (~0.02) falls under threshold, and the rendered caption correctly contains no
      mood clause at all rather than asserting "love" at 2% confidence — a smoke-test
      regression, not just a manual check

---

## Phase 4 — quality (PRD §9 Phase 4)

- [x] Embedding A/B: `discogs-effnet` vs **DCLAP** (not full CLAP or MuQ-MuLan — PRD §14.5
      recommends DCLAP specifically as the torch-free, licence-clear candidate; §14.6 defers
      MuQ-MuLan "only if DCLAP disappoints", and it didn't), measured on a real 294-file
      sample library (`/Sample Magic - Dark Pop`, mixed one-shots/loops).

      **Built:** `models/similarity-embeddings/dclap/` (AudioMuse-AI-DCLAP v1's audio
      encoder + text tower, AGPL-3.0, no licence conflict per §12 Q9). `spike/05_dclap_parity`
      reimplements the model author's exact preprocessing (48kHz, int16 quantize round-trip,
      10s/50%-overlap segmentation, a hand-built librosa-formula slaney mel filterbank —
      Essentia ships nothing that reproduces librosa's specific normalization, so this is
      generated from the closed-form formulas in librosa/filters.py directly, not guessed —
      plus libsoxr for resampling, since the Python reference's librosa.load(sr=48000)
      defaults to res_type='soxr_hq', a different algorithm than Essentia's own
      libsamplerate-backed Resample) and verified it against `lab/dclap_parity_reference.py`
      to cosine similarity >0.999 on flamenco.wav — the residual ~1e-3 is consistent with the
      two sides calling into soxr's HQ preset via different code paths, not a bug in the mel
      math. Ported into `src/mira/analyze/DclapEmbedding.{h,cpp}`, a second `vec_embeddings_
      dclap` vec0 table (512-dim, separate from discogs-effnet's 1280-dim one — the two spaces
      are never compared to each other, only independently against a query of their own kind),
      and `mira similar --embedding effnet|dclap`. Both embeddings now compute unconditionally
      on every analyzed file, same as discogs-effnet always did (PRD §4: comparing across
      content types is the whole point).

      **Measured — coverage:** discogs-effnet embedded 199/294 files (68%); DCLAP embedded
      294/294 (100%). Broken down by content type, the gap is almost entirely one-shots:
      effnet got 8/43 one-shots (19%) — its frontend needs ~2s of audio to form even one
      128-frame mel patch, and most one-shots are shorter than that, so similarity search is
      simply *unavailable* for 81% of one-shots today. DCLAP zero-pads short clips to a full
      10s segment instead of requiring a minimum length, so it never fails to produce a vector.

      **Measured — quality on one-shots:** of 6 sampled one-shots (snare, percussion, bass,
      synth, vocal — folder names as ground truth), 5 had no effnet embedding to compare at
      all. DCLAP's top-5 neighbors landed 100% within the query's own folder/category for
      every one of the 6 (snare→snares; percussion→snares/snaps/claps/percussion; synth→synths;
      vocal→vocals). The one query with both embeddings (a bass 808 hit) also went 5/5 same-
      folder for DCLAP vs 3/5 for effnet, which additionally pulled in a bass *loop* and a
      songstarter synth-bass clip — crossing the one-shot/loop boundary DCLAP kept clean.

      **Measured — quality on loops:** effnet's expected home turf (long enough to embed,
      genre-classifiable). Of 6 sampled loops (melodic synth ×2, percussion ×2, guitar, synth
      bass), DCLAP matched or beat effnet on every query and was clearly tighter on 3: a guitar
      loop where effnet returned guitar for only 2 of 5 neighbors (rest synth-stab/pad/
      percussion) vs DCLAP's 5/5 guitar; two percussion loops where effnet dragged in one
      unrelated melodic-synth or pad loop each, DCLAP staying 5/5 drum-family both times.
      Effnet also repeatedly surfaced one-shots as "similar" to loop queries, a content-type
      bleed DCLAP never exhibited. **This is a real finding against the PRD's stated
      expectation** (§4: "discogs-effnet... good for tracks and loops") — n=6 is small and
      this isn't conclusive, but nothing in this sample showed effnet's hypothesized edge on
      its own home turf.

      **Conclusion:** keep both embedding spaces (not a replacement — PRD §14.5's "additional
      index behind a flag" framing), but on this library DCLAP is never worse and often
      better, including on content effnet was expected to win. Per-dimension similarity
      (below) and any future default-embedding decision for `mira similar` should start from
      DCLAP, not assume effnet's genre-classifier space is the stronger prior.
- [x] Per-dimension similarity (timbre / rhythm / spectrum), not Sononym's fixed five
      (§12 item 5). Built from what mira already measures (Descriptors.h's DSP
      descriptors) rather than new models — matches PRD §8's spec'd CLI surface exactly:
      `mira similar <file|id> --by overall|timbre|rhythm|spectrum`, `overall` being the
      existing embedding-based similarity (Phase 4's DCLAP/effnet A/B, above).

      **Built:** `timbre` is the 13-coefficient MFCC vector, its own `vec_timbre` vec0
      table (raw L2, no extra normalization — MFCC coefficients are already comparable
      units to each other, a standard choice in the literature). `spectrum` is a small
      2-dim `[log1p(centroid)/log1p(11000), flatness]` vector — deliberately excludes
      harmonicity (its "0.0 means unmeasured" convention can't be folded into a metric
      distance without corrupting it) and chroma (no named PRD dimension to attach it to
      yet). `rhythm` has no per-file vector at all — there's nothing to vectorize beyond
      the single BPM scalar already stored, so `findSimilarByBpm` is a plain SQL
      brute-force scan over `files`, gated to exclude one-shots (tempo is never measured
      on them) and unmeasured (0/null) BPMs.

      **Measured — real finding, not the expected one:** ran `--by timbre` against the
      same snare one-shot used in the embedding A/B write-up above. DCLAP's `--by overall`
      still won decisively: 5/6 neighbors were exact drum-hit-family matches (snares +
      one clap) at distances 0.15–0.24, while `--by timbre`'s top-6 was 1/6 clean (a clap)
      with loops and a vocal one-shot mixed in at much larger, less-separated distances
      (58–96). This is the opposite of what Descriptors.h's own comment expects ("the
      backbone of one-shot similarity") — raw MFCC alone, unweighted and uncalibrated
      against real material, does not beat a learned embedding even on the content type
      it's supposed to be strongest at. `--by spectrum` and `--by rhythm` both work
      mechanically (verified: different, non-degenerate rankings; `--by rhythm` correctly
      clustered same-BPM loops at distance 0) but are intentionally coarse single-purpose
      filters (brightness/noisiness; tempo-only), not general-purpose similarity, and
      weren't expected to compete with `overall` head-to-head.

      **Conclusion:** the CLI surface and storage are real and working, but on this
      library plain DSP-descriptor dimensions don't yet improve on the embedding spaces
      built earlier in Phase 4 — they're additive/complementary filters (useful for "same
      tempo" or "same brightness" queries specifically), not a better default for general
      one-shot similarity. If per-dimension similarity needs to actually beat `overall`,
      the embedding-derived-axes approach (PCA/clustering on the effnet/DCLAP vectors
      themselves, considered and deferred when this item was scoped) is the more promising
      next direction, not further DSP feature engineering.
- [x] Per-head confidence calibration → sets the Phase 3 render-gate thresholds
      (`CaptionFields.h`'s `kCaption*Threshold` constants). Ran all four gated heads
      (genre, instrument, moodtheme, voice_instrumental) across the same real 294-file
      library used for the embedding A/B and inspected the actual score distributions —
      see `CaptionFields.h`'s updated comment for the full reasoning per head. Result:
      **no threshold value changed**, but for a reason, not by default —

      - `instrument` (0.10): confirmed well-calibrated. Across every (file, class) score
        pair, 86% sit below 0.05 with a clear tail above 0.10 — a real noise/signal gap,
        threshold sits right in it.
      - `voice_instrumental` (0.5): confirmed against real ground truth (filenames
        containing "vocal" vs not) — correctly separated 5/6 vocal-named files (scores
        0.53–0.997) from all 49 non-vocal ones except a couple of ambiguous full
        "songstarter" mixes that may genuinely contain a vocal layer despite an
        instrument-only filename. The one real miss was a heavily processed vocal
        *one-shot* scoring 0.39 (below threshold) — moving the threshold either direction
        traded that miss for new ones on the other side, so left as-is.
      - `moodtheme` (0.10): the head's own ceiling is low on loop content (max top-1 score
        observed: 0.28) — MTG-Jamendo's moodtheme model is full-song-trained, and applying
        it to short instrumental loops produces uniformly modest confidence, not a
        threshold miscalibration. Raising the bar would have cut the field to almost
        nothing (7 of 3080 raw scores clear 0.20); spot-checked labels at the current
        threshold ("dark", "space", "deep" on a *Dark Pop* pack's bass/synth loops) look
        plausible.
      - `genre` (0.10): **flagged, not fixed.** Unlike instrument, genre's score
        distribution has no clean noise/signal gap — it's continuous from ~0.04 to ~0.58 —
        and at least one spot-checked top label ("Progressive Metal" on a soft electric
        guitar loop) looked like a real miss. But there's no ground-truth genre label set
        for isolated instrumental loops to test candidate thresholds against, so picking a
        different number would be guessing dressed up as calibration. Documented as the
        one head where genre_discogs400 (trained on full tracks) applied to loops is a
        domain-mismatch problem a threshold number can't solve — a real Phase 4 finding in
        its own right, feeding directly into the per-dimension-similarity item below
        (genre may simply not be a reliable *dimension* for loop-type content at all).
- [x] Segment-level analysis replacing whole-track averaging. Closes the gap
      `extractCaptionFieldsForSegment` (Phase 3) documented in its own comment: segment
      boundaries and human tags existed, but every machine-derived field (genre,
      instruments, mood, voice) still came from the file's *whole-track* analysis, never
      re-measured for the segment's own time range.

      **Built:** `mira tag-segment` now analyzes immediately when a boundary is declared
      (not deferred to `export-segments`) — slices the affected file's(s') audio to
      `[start,end)` and reruns a *subset* of the whole-file pipeline on just that slice:
      DSP descriptors, both embeddings (effnet/DCLAP), and — gated on the content gate's
      `is_music`, identically to the whole-file loop — moodtheme/instrument/danceability/
      genre/voice_instrumental (`buildSegmentMachineJson`, main.cpp). Stored in a new
      `segment_analysis` table keyed by `(segment_id, file_id)`, not just `segment_id` —
      required because a `group_id`-scoped segment (a synced stem set) is *one* row in
      `segments` but covers *multiple* files, each with different audio in that range.
      `extractCaptionFieldsForSegment` now reads this and overrides genre/instruments/
      moods/voice_instrumental when present, falling back to the file's whole-file values
      otherwise (guarded on the segment's own `is_music`, not on the override values
      being non-empty — the two look identical from a returned-empty-list alone, but mean
      different things: "never computed for this segment" should keep the whole-file
      value, "computed and nothing cleared threshold" is a real, more-specific empty
      answer).

      **Explicitly out of scope, not an oversight:** rhythm/key/chords/transcription
      never rerun per segment (`beat_this` needs a few seconds of audio to be reliable,
      and the added per-segment runtime cost was judged not worth it for v1) — bpm/key
      always stay the file's whole-file values, even when a segment has its own machine
      JSON. `stem_instrument` also isn't rerun; `applySegmentMachine` only ever reads
      back the plain `$.instrument` path regardless of content type.

      **Verified on a real synced stem set** (a 5-stem songstarter group,
      `DKP_80_songstarter_wet_dream_Amin`), both code paths:
      - *Fallback path*: an 8s segment across all 5 stems came back `is_music=false` for
        every stem (CED-small didn't gate short slices as music) — exported captions
        correctly kept the whole-file genre ("Electronic: Experimental") and bpm/key,
        confirming the guard doesn't silently wipe good whole-file data when segment
        classification legitimately never ran.
      - *Override path*: two different segments of the *same file* (0–6s vs the full
        0–12s) came back `is_music=true` and produced genuinely different `instruments`
        fields — `"electric guitar"` vs `"bass, synthesizer, drums, piano"` — proof the
        override is really per-segment, not one fixed whole-file value leaking through
        (that could not happen if the two segments were reading the same source).

---

## Phase 5 — surfaces + multi-target captioning (PRD §9 Phase 5, §13)

### Architecture decisions (2026-09-10 planning discussion — read before touching this phase)

Settled before any Phase 5 code exists, so the reasoning doesn't have to be re-derived
next session.

**Hand-rolled native JUCE, not a webview/Electron hybrid.** Considered three options: (A)
pure native JUCE with a custom `LookAndFeel`; (B) JUCE native shell + an embedded
`WebBrowserComponent` rendering the actual UI in HTML/CSS; (C) full Electron/Tauri (the
whole app as a web app, à la Tuva's apparent React/Node/Tone.js-or-Howler stack). Went
with **A**. B and C both lose real things this project already has or needs: B still needs
a hand-built bridge for anything real-time (a playhead synced to the audio thread doesn't
map cleanly through a JSON message bridge to DOM), re-solves `TableListBox`'s drive-scale
virtualization problem in a second stack instead of using JUCE's already-chosen
`paintCell`-only answer to it, adds a whole npm/bundler dependency ecosystem next to this
repo's carefully vendored/pinned C++ one, and demotes `SoundBrowser.h`'s existing native
investment (below) to reference-only instead of directly reusable. Both B and C also carry
a real RAM cost specifically bad for this app's context: a full embedded browser engine
(WKWebView/Chromium) has a baseline memory footprint in the 150-300MB+ range, which matters
for something meant to sit open *alongside a DAW* that's already contesting RAM with sample
libraries and plugins.

**Not starting cold — verified, not assumed:**
- `vendor/JUCE` is already vendored.
- `spike/03_dragout` (Phase 0, Day 4) already proved the founding requirement — system
  drag-out via `shouldDropFilesWhenDraggedExternally`, confirmed working into Ableton,
  Logic, and Finder on macOS 15.5. The single riskiest unknown in the whole UI plan is
  already settled, not theoretical.
- That spike's own `CMakeLists.txt` already sets `JUCE_WEB_BROWSER=0` — the codebase had
  already implicitly ruled out option B before this discussion happened.
- `~/w2app/w2-audio-plugs/src/SoundBrowser.h` (a separate, existing sampler-plugin project,
  not part of this repo) has real working reference patterns worth copying from, not
  rebuilding from zero: `WaveThumb` (background-thread waveform thumbnailing, O(width)
  paint), `AnalysisWorker` (background `juce::Thread` + `callAsync` marshaling — the shape
  the progress queue below should take), `FolderListModel` (folder navigation, currently
  one-level drill-down with a `..` up-row, needs upgrading to a persistent tree).
- Relationship to that sampler plugin, explicitly decided: mira's UI is built standalone
  first, its own process, own SQLite schema. The sampler plugin's browser folds into mira
  later — not the reverse, and not scheduled yet.

**Progress queue is required, not optional polish** — a real course-correction from this
discussion, worth flagging so it isn't lost again: PRD §13's "no queue, no server" language
is about avoiding a separate server process / inter-process protocol (the shape underfit's
own Python dashboard uses), **not** about skipping an in-app progress UI. Without live
progress (files analyzed / remaining / current file / stage) during a big scan, the app
will look frozen at drive scale. Same `AnalysisWorker`-style background thread as above,
exposing counts the UI polls or receives via `callAsync` — same mechanism, not a separate
subsystem.

**Visual direction: glassmorphism, specifically chasing Apple's current "Liquid Glass"
material** (2025-era — translucent/frosted panels with a dynamic, refractive quality),
not the older ~2020 static-blur glassmorphism look. The correct native building block
available *today* is **`NSVisualEffectView`** (real vibrancy/blur, macOS 10.10+) — genuine
native translucency under arbitrary custom content. Worth being precise: this is **not**
literally the same rendering system as the newer Liquid Glass material (a distinct, newer,
more dynamic/specular API surface, exact current class unverified here — check Apple's
current docs before committing, rather than assume). `NSVisualEffectView` gets most of the
visual language and is the pragmatic, available choice; the literal newest material is
less proven for hosting arbitrary custom-painted content rather than Apple's own system
controls. JUCE has **no built-in wrapper** for `NSVisualEffectView` — using it means real
Objective-C++ (`.mm`) bridge code hosting it as a native `NSView` behind JUCE's own
rendering. Real work, but the same category `spike/03_dragout` already proved feasible for
native Cocoa drag-out, not a new kind of risk.

**Add-on JUCE modules to vendor, chosen deliberately:** Melatonin's modules (e.g.
`melatonin_blur` — real soft shadows) and JUCE's own `juce_animation` module
(easing/tweening) — both purely additive, layer under a custom `LookAndFeel` without
imposing architecture. **Considered and deliberately not using `foleys_gui_magic`** — real
and well-regarded, but its XML/style-driven declarative layout pulls toward a generic,
data-bound look, working against the hand-crafted goal here.

**Build order agreed for the first slice**, in this sequence: (1) promote
`spike/03_dragout` into a real target in `src/CMakeLists.txt`, wired to mira's actual
SQLite DB, not the spike's toy fixture; (2) base window shell + a `LookAndFeel_V4`
skeleton — palette/type tokens first, before any real panel is built on top of it; (3) the
progress queue; (4) persistent folder tree (upgrade from `SoundBrowser.h`'s drill-down
`FolderListModel`) + the file table (`paintCell`-only, per the virtualization concern
above); (5) waveform/detail panel + filter bar wired to `mira search`'s existing filter
grammar (Phase 2).

**Reference mockup** from this planning session:
https://claude.ai/code/artifact/d590444f-6b62-4ff3-8b9e-8f7ecbae00f1 — palette/type tokens
(IBM Plex Sans/Mono, warm amber accent, teal active-region color, near-black ground) are a
starting point for the real `LookAndFeel`, not necessarily final; it's a static HTML
illustration, not a spec.

### Build order step 1 — done (2026-09-11)

Promoted `spike/03_dragout` into a real target. Two structural things worth recording,
since they weren't decided in the planning session above:

- **`mira_core`** (new, `src/CMakeLists.txt`) — `Database.{h,cpp}` split into its own
  static library (SQLiteCpp + sqlite-vec, nothing else) so `mira_ui` can read the real
  library without linking the CLI's entire Essentia/ONNX/beat_this dependency graph. The
  `mira` executable now links this too, instead of compiling `Database.cpp` directly —
  one implementation, not two.
- **JUCE add_subdirectory collision** — `spike/03_dragout` and `mira_ui` both need
  `vendor/JUCE` add_subdirectory'd, and both are reachable from the top-level
  `CMakeLists.txt` in one configure; CMake errors if the same source directory is added
  twice in a single run. Fixed with an `if(NOT TARGET juce::juce_core)` guard in both —
  whichever runs first (the spike, since spikes are listed before `src/`) actually adds
  it, the other becomes a no-op. Standalone spike builds are unaffected.

`src/mira_ui/` is a real `mira_ui` app target: one `DocumentWindow`, reads
`~/.mira/library.db` via `mira_core::Database` (same default path the CLI's
`defaultDbPath()` uses), lists every scanned file as its own draggable row (a plain
`Viewport` + stacked rows for now, **not** yet the `paintCell`-virtualized `TableListBox`
— that's still a separate, open checklist item below). Drag-out is the spike's proven
mechanics, generalized from one hardcoded fixture to reading each row's own real file path
off `SourceDetails::sourceComponent` at drop time.

Verified: builds clean (`mira_core`, `mira_ui`, and the unmodified `mira` CLI all link),
launches without crashing, and — confirmed via the accessibility tree (`System Events`),
since this environment's `screencapture` doesn't actually capture the real display —
correctly showed "15 file(s) in /Users/justmac/.mira/library.db" with 15 row elements
after scanning a real 15-file folder, matching exactly. Quit cleanly, no crash.

- [x] JUCE UI shell: one native window, same process as analysis (§13) — `mira_ui`
      target above. "Same process as analysis" still means "architecturally the same
      process the way the CLI's analyze loop is" (mira_core is shared), not that
      analysis has been wired to run from inside the UI yet — that's real work still
      ahead, not claimed here.
- [x] `TableListBox` file list with `paintCell` only, at drive scale — `FileTable.{h,cpp}`
      (build-order step 4). Six columns (File/BPM/Key/Loudness/Active/Type) matching the
      reference mockup's table, `paintCell`-only per row (no per-row Components — the
      thing that actually matters at drive scale, unlike step 1's stacked-Viewport
      version). Missing values render as an em dash in `--text-faint` (mockup's `.dash`
      convention) rather than a blank cell or a false zero — genuinely distinct meanings
      (e.g. BPM `—` on a one-shot means "never measured", not "0 BPM"). Drag-out moved
      from step 1's per-row `dynamic_cast` trick to `TableListBoxModel::
      getDragSourceDescription`, JUCE's own mechanism for this — the dragged file's path
      now travels as the drag description itself, since `TableListBox` owns its row
      components and there's no app-defined row type left to cast back to.

      Verified on the 15-file snare test library, screenshotted after `mira analyze`
      actually ran (not just `scan`): BPM correctly dashed on every row (single drum hits
      have no measurable tempo — real, expected, matches PRD §5), Key populated where
      libKeyFinder found one and dashed elsewhere, Loudness populated on every row,
      Active dashed specifically on the 3 `one_shot` rows (active-region detection is
      gated to stems/declared stems/files over 5 min, PRD §5 — correctly never ran on
      those) and a real percentage (82–100%) on the 12 `stem` rows, Type showing the
      correct content type per row. Row selection confirmed too: selecting a row tints
      its background `--accent-soft` and its filename `--accent`, matching the mockup's
      `tr.sel`/`tr.sel td.name` rule exactly.
- [x] `LookAndFeel_V4` skeleton with the mockup's palette/type tokens (build-order step
      2) — `MiraLookAndFeel.{h,cpp}`. Every color is the mockup's `:root` custom property
      verbatim (`--bg`/`--surface`/`--surface-2`/`--surface-3`/`--border`/`--text`/
      `--text-dim`/`--text-faint`/`--accent`/`--active`/`--warn`/`--good`, each with its
      documented `-soft` alpha variant). Typography is IBM Plex Sans (UI text) / IBM Plex
      Mono (numeric/technical fields — BPM, Loudness, Active%, the status line — matching
      the mockup's own tabular-nums convention), fetched from `IBM/plex` (SIL OFL 1.1,
      `vendor/fonts/`, added to `scripts/fetch-vendor.sh`) and embedded via JUCE
      `BinaryData` rather than depending on the fonts being installed system-wide.
      Applied to every component step 1/4 already had (window background, labels, the
      table's header/rows/selection) — not yet to panels that don't exist yet (filter
      bar, waveform, detail grid — later build-order steps).

      **Real `NSVisualEffectView` vibrancy tried and reverted — a real finding, not just
      an attempt.** `NativeBlur.{h,mm}` wraps one in `juce::NSViewComponent`, and the
      *material itself* is confirmed working (screenshotted: genuine blurred desktop
      showing through the window). But `juce_NSViewComponent_mac.mm` attaches the native
      view via plain Cocoa `[peerView addSubview: view]` onto the window's single shared
      JUCE peer view — since every other JUCE component (labels, the table, everything)
      is pixels painted into that *same* peer view, not a separate native layer, the
      embedded native view has no JUCE z-order to interleave with: it always renders in
      front of the *entire* window's JUCE content, not behind it. Confirmed by screenshot
      — with it wired in, only the blur was visible, every other component vanished.
      Getting real vibrancy showing *through* JUCE panels needs deeper native window
      surgery (make the effect view the `NSWindow`'s actual `contentView`, render JUCE as
      a transparent subview on top of it) — parked, not attempted. `NativeBlur.{h,mm}`
      are kept, unused, as the starting point for that if it's worth doing later.

      **Fallback in place now**: `MiraLookAndFeel::paintGlassPanel` — pure JUCE painting
      (a vertical gradient between two close panel shades, a fixed page-relative radial
      highlight so every panel agrees on where the light comes from, and the mockup's own
      `0 2px 0 rgba(255,255,255,0.02) inset` hairline top edge, ported directly). No real
      blur, but no native risk either, and visually in the same family. Applied to
      `MainComponent`'s background; not yet to individual panels.
- [x] `AudioThumbnail` + `AudioThumbnailCache` waveform display — `WaveformView.{h,cpp}`,
      white waveform (not accent-coloured, per feedback), zoom in/out/reset with
      trackpad-swipe panning when zoomed, click-drag selection tint.
- [x] Playback: `AudioDeviceManager` → `AudioSourcePlayer` → `AudioTransportSource` →
      `AudioFormatReaderSource` — same `WaveformView`, real play/pause/seek, a moving
      playhead, volume + mute, Space-bar toggles play/pause app-wide (`MainComponent::
      keyPressed`).
- [x] Drag-out wired into the real app (mechanics already proven in Phase 0 spike) —
      generalized to any real file in the library, verified above
- [ ] Filter bar (stackable chips — SonikSearch-inspired, NOTES.md UI research) — the bar
      is real now (free-text filtering over the listed rows, see the leftovers section
      below), but the *chip* UI — composing several named filters that stack — is still
      not started.
- [x] Per-field source-of-truth display (Analysis/Filename/Manual) — not the
      SonikSearch-style chip UI originally sketched, but the same underlying discipline:
      `FileTable.cpp`'s BPM/Key/Genre/Instrument/Mood columns visually distinguish
      analyzed (full brightness/green), filename-derived guesses (dimmed, two confidence
      tiers), and human overrides (accent-coloured) at a glance; `FileDetailsWindow`
      shows the same distinction with the full ranked machine distribution alongside the
      editable effective value.
- [ ] ACE-Step JSON renderer
- [ ] Remaining two caption registers
- [ ] Segment slicing UI/export

### Build order steps 2–14 — done (2026-09-11, same day as step 1, one long session)

Everything below happened after build-order step 1 in one continuous session — folder
tree, real scan/analyze pipelines wired into the UI, waveform/playback, tabs, and a
file-details/edit view. Recorded in build order, not request order.

- **Native macOS chrome**: custom `LookAndFeel_V4`-painted title bar + traffic-light
  buttons (muted dot colours, not JUCE's stock saturated glyphs) replacing the native
  gray one everywhere except secondary utility windows (`AudioSettingsWindow`,
  `FileDetailsWindow` — native title bar there on purpose, see below); a real macOS menu
  bar (`MenuBarModel::setMacMainMenu`) with File → Add Folder.../Rescan and Audio → Audio
  Settings....
- **Folder tree redesign** (`FolderTreeView.{h,cpp}`) — hand-rolled `TreeViewItem`s
  instead of `juce::FileTreeComponent` (that class's `DirectoryScanner` hardcodes
  `setDirectory(f, true, true)` on every expanded subfolder, silently breaking
  "audio-only" one level deep). Folders with no audio anywhere in their subtree (Serum
  Presets etc.) are filtered out entirely, not just hidden-when-empty. Root folders
  support: mira-side-only rename (`display_name`, never touches the real folder),
  right-click "Analyze Folder", and "Remove Folder from mira..." (only ever removes the
  `ui_folder_roots` row — never deletes real files or already-recorded analysis).
- **Mira-side folder groups** (`ui_folder_groups` table) — four built-in categories with
  their own sidebar icon each: **Score Stems**, **Music Stems** (split from a single
  "Stems" after user feedback — the two behave differently: score/film stems are
  long-form and will eventually want segment markers, music stems are short per-track
  submixes), **Samples**, **Music**. Add Folder shows a real modal choice dialog for
  these (`promptForChoice`/`ChoiceDialogWindow` — `juce::AlertWindow` only reliably
  renders ~3 buttons per its own doc comment, silently dropped all 5 when tried; a custom
  stacked-button modal window replaced it). Categorizing a folder as either stem kind
  auto-declares it at scan time (`ScanOptions::declareAsStem`, same as the CLI's own
  `mira scan --as stem`) — this is what makes the stem-tuned instrument algorithm apply
  automatically instead of depending on the router's sibling-set guessing.
- **Real scan queue** — folders auto-scan the moment they're added ("scanning is mira's
  job not the user's job," no manual Scan button anywhere), multiple queued roots show
  `queued`/`scanning • N files` per-root in the sidebar, interrupted scans
  (`ui_folder_roots.scan_complete`) resume automatically at the next launch, scan errors
  prompt a real retry dialog, and the scan thread runs at `Priority::low` so it doesn't
  compete with the UI thread on large libraries.
- **Real analyze queue** — shells out to the CLI's own `mira analyze` as a subprocess
  (`AnalyzeJob`, `juce::ChildProcess`) rather than linking the ML pipeline into `mira_ui`
  — added `--paths-from <file>` to the CLI for this (newline-delimited exact paths,
  always re-analyzed regardless of `analyzed_at`) and a plain-stdout `progress: N/M path`
  line (not gated behind `--verbose`) for the UI to parse. No standalone Analyze button —
  right-click a selection (or a whole folder, from the sidebar or the file list) →
  "Analyze...". The Status column shows `queued`/`analyzing…`/`analyzed` globally, not
  scoped to whichever tab is currently open, so analyzing folder A while looking at
  folder B still shows accurate state either way.
- **File list**: real filesystem listing merged with DB status (not a DB-only query —
  clicking a never-scanned folder still shows its files), browser-style tabs (each
  remembers its own folder scope; "+" opens a blank tab without disturbing the others),
  Format/Duration/Sample Rate read straight off each file's header (no scan needed),
  filename-derived BPM/key guesses (dimmed, two confidence tiers) as a fallback before
  real analysis exists.
- **File Details window** (`FileDetailsWindow.{h,cpp}`, new) — double-click a row or
  right-click → Details...: full ranked genre/instrument/mood distributions (not just
  the table's collapsed top-1), provenance (model versions, analyzed timestamp),
  editable BPM/Key/Genre/Instrument/Mood that write through the existing `human` JSON
  override column (`Database::setHumanField`) and a Revert-to-Analyzed button
  (`clearHumanFields`) — never touches `machine`. Native title bar (tried a custom one to
  colour-match the rest of the app; it broke both the native rounded corners and title
  text centring, reverted — colour-matching a *native* title bar would need real
  Objective-C++ `NSWindow` work, not attempted).
- **Instrument-detection accuracy work** (real findings, not just UI plumbing):
  - `$.stem_instrument` is `{"window_count":N,"scores":{...}}`, not a flat label→score
    map like `$.instrument_normalized` — reading it directly produced garbage
    ("scores 0% · window_count..."). Fixed to read `$.stem_instrument_normalized` /
    `$.stem_instrument.scores`.
  - The stem-tuned model (IRMAS, 11 classes: cel/cla/flu/gac/gel/org/pia/sax/tru/vio/voi)
    has **no drums/percussion class at all** — structurally, not probabilistically, it
    can never say "drums." The full-mix model (`mtg_jamendo_instrument`) does have
    drums/percussion and stays fairly reliable at spotting them even on isolated audio,
    so it's preferred specifically when it confidently says drums/percussion; the
    stem-tuned reading wins otherwise.
  - Confirmed on a real file that **both** models can independently miss an obvious
    percussion stem (full-mix's own top guess was "synthesizer," not drums) — a
    structural model-coverage gap, not a routing bug, and not fixable by picking between
    the two existing models. Added a last-resort filename-keyword fallback
    (`filenameSaysPercussion` — kick/snare/hihat/clap/tom/perc/etc.), dimmed like the
    BPM/key filename guesses, never presented as real model output.
  - Full tracks (`content_type='track'`) now show every instrument candidate above 10%
    confidence in both the table and Details ("a mix genuinely has several instruments at
    once"); stems/samples/one-shots still collapse to a single top label ("should
    genuinely be one instrument").
- **Rounded-corners pass** — buttons and tab chips were already rounded; added rounded
  `TextEditor` fields (app-wide, via `LookAndFeel::fillTextEditorBackground`/
  `drawTextEditorOutline`) and an inset rounded selection pill in the folder tree.
  **Tried and reverted**: a transparent-peer main window with real OS-clipped rounded
  outer corners (`ComponentPeer::windowIsSemiTransparent` + `setOpaque(false)`) —
  produced a visible ghost/duplicate window outline instead of the intended curve, a real
  regression. Backed out cleanly; a correct implementation is a separate, harder task.

### Left over for next session (Phase 5) — all nine implemented

Worked through in order in a follow-up session. **Every item below compiles clean
(`mira` and `mira_ui`) and the menu-bar/filter-bar additions were confirmed live through
the accessibility API, but none of it has been looked at on screen**: `screencapture` on
this machine returns desktop-only images (the shell has no Screen Recording permission),
so the visual result of the layout, waveform-band drawing, title-bar recolouring and
rounded corners is unverified. Those four especially want a human look before they're
called done.

- [x] **SA3 caption view in File Details** — `caption/CaptionFields.cpp` and
      `caption/Sa3Renderer.cpp` turned out to have no ML dependency at all (only
      `Database.h` plus three header-only `constexpr const char*` label tables), so they
      moved into `mira_core` alongside `Database.cpp`/`Scanner.cpp` rather than needing a
      subprocess hop — same call the `Scanner.cpp` split already made. The details window
      now shows the real rendered prose and the sidecar tag set, with a live Trigger
      field (session-only, not persisted: a trigger belongs to the LoRA being trained,
      not to the file) and Copy Prose / Copy JSON. Read-only on purpose — it's a
      derivation of the editable fields above it, so the way to change it is to edit
      those and Save. Verified identical to `mira caption 105 --trigger mystyle` output
      because it is literally the same two functions, not a reimplementation.
- [x] **Multi-file batch editing** in File Details — `FileDetailsWindow` takes
      `std::vector<int64_t>` now; right-click a multi-selection → "Edit N Selected...".
      A field unanimous across the selection shows and edits that value; a field that
      differs shows a "— multiple values —" placeholder and is left alone unless typed
      into. The caption block is hidden in batch mode (there is no "caption of 40 files").
      **Found and fixed a real pre-existing bug doing this**: single-file Save wrote
      *every* non-empty field regardless of whether it changed, silently promoting
      machine-derived values into `human` overrides that then outrank all future
      re-analysis — the comment claimed it only wrote changed fields, the code didn't.
      `FieldRow::initialText` now makes that true, in both modes.
- [x] **Analyze pipeline options in the UI** — a real Analyze menu with checkmarked
      `--chords` / `--transcribe` / `--recheck-tempo` toggles plus their measured costs
      in the menu itself. Sticky for the session, not persisted (no settings file exists
      in `mira_ui` yet, and a hidden persisted toggle that triples analysis time across
      launches is worse than re-ticking it). Each queued batch snapshots the options it
      was queued with, so ticking one mid-queue doesn't rewrite what's already waiting;
      the right-click Analyze item spells out whichever are on ("Analyze 5 Selected
      (+chords)").
- [x] **Segment-marker tagging workflow for Score Stems** — the other half of what the
      Score/Music Stems split was created for. `WaveformView` draws declared segments as
      tinted regions plus a clickable labelled band, colour-split by scope (`good` for a
      group-scoped boundary covering the whole synced stem set, `accent` for one file
      only). Drag a selection → "+ Segment"; a file with a `group_id` is asked which
      scope explicitly rather than guessed at. Per-segment menu: Edit Tags (writes the
      `human` keywords/moods/genre/instruments `export-segments` renders into each clip's
      sidecar), Export Segments (shells out to the CLI's own `mira export-segments`, same
      reasoning `AnalyzeJob` has), Delete. Added `Database::setSegmentHumanField` and
      `deleteSegment` — the CLI never needed either, since a `tag-segment` run only ever
      declares; `deleteSegment` also drops the segment's `segment_analysis` rows, which
      have no `ON DELETE CASCADE` and would otherwise be unreachable orphans.
- [x] **Bottom panel markers/segments/tags row** — the placeholder dots are gone;
      `BottomPanelPlaceholder` is now `BottomPanel` with the real "+ Segment" /
      "Segments (N)" controls above. They render only for Score Stems files (a Samples or
      Music folder has no use for time-ranged captions, and a permanently dead button is
      worse than no button), and the Add control reads "Drag to select" until there
      actually is a selection rather than being silently disabled.
- [x] **Pre-generated waveform previews during Scan** — `ThumbnailStore.{h,cpp}`:
      JUCE's `AudioThumbnail` already serialises its own peaks (`saveTo`/`loadFrom`), so
      the cache design is only "where do this file's peaks live, and is that copy still
      valid" — one file per source under `~/.mira/thumbnails/`, no index and no DB table,
      because it's pure derived data. Validity is `(path, mtime, size)` hashed into the
      filename, so an edited file simply misses rather than needing stale-entry detection;
      writes go to a temp file and move into place so an interrupted pass can't leave a
      half-written entry. Runs *after* a scan finishes, not inside it (scanning decodes no
      audio at all — generating peaks inside it would be a second, much slower scan hidden
      in the first), at `Priority::background`, skipping anything already cached. A miss
      falls through to the original lazy path unchanged, so nothing on the click path
      depends on the pass having run.
- [x] **Native title-bar colour matching** — `NativeWindowChrome.{h,mm}`:
      `titlebarAppearsTransparent` + a window background colour + an explicit
      `NSAppearanceNameDarkAqua`. This is why it works where the earlier attempt didn't —
      it keeps the *native* title bar (so real rounded corners and correctly centred title
      text still come for free) and only recolours it, instead of replacing it with a
      painted one.
- [x] **Real rounded main-window corners** — retried the other way round: not JUCE's
      `windowIsSemiTransparent` transparent-peer path (that's exactly what ghosted), but a
      Core Animation `cornerRadius`/`masksToBounds` on the `NSWindow`'s own content layer
      with the window made non-opaque, so AppKit clips and shadows the rounded shape
      itself and JUCE never sees any transparency. **Unverified visually** — this is the
      item most likely to still be wrong.
- [x] Filter/search bar at the top of the window — real: live per-keystroke filtering,
      space-separated terms that must all match ("kick 140"), substring not prefix, over
      what the row actually *shows* (so a filename-derived BPM guess is as findable as an
      analyzed one), with a "3 of 412" count and Cmd+F to focus. A view over the
      already-built rows, never a re-walk of the folder. Still a plain text box, **not**
      the stackable-chip UI still listed separately above: chips are a way of *composing*
      filters, and there was nothing to compose until one filter worked.

### Review round 2 — UI fixes first, then instrument accuracy (2026-09-11)

From the first on-screen look at the leftovers work above (user screenshots). Every
finding below was checked against the real library DB before being written down. The
order is deliberate: UI first, then algorithms.

**UI — clear-cut bugs**

- [x] *(fixed: wrapped in `CharPointer_UTF8`; not yet checked on screen)* Mojibake: filter placeholder renders "bpmâ€¦", caption hint renders "above â€".
      Mine — a raw UTF-8 literal passed straight to `juce::String(const char*)`, the
      exact mistake Main.cpp's own updateScanStatusText comment warns about.
- [x] *(fixed: border and grip draw nothing, shadow invalidated; not yet checked on
      screen)* Rounded main-window corners leave a black square edge. `LookAndFeel_V4::
      drawResizableWindowBorder` paints a square outline that the rounded layer mask
      clips, and the bottom-right resize grip is painted into the clipped corner too;
      the window shadow is also never invalidated after the shape changes.
- [x] *(fixed: `isStemPath` prefix test; not yet checked on screen)* Segment controls never appeared. The SCORE STEMS group (id 1) is stored with the
      legacy pre-split category `'stems'`, but the code only accepted `'stems_score'`. Per
      review, markers belong on Music Stems too, so gate on any `stems*` category.
- [x] *(fixed: `refreshSelectedFilePanel` after each analyzed file; not yet checked on
      screen)* Bottom summary reads "no analysis yet" for an analyzed file. It's stale: analyze
      completion refreshes the list but never re-pushes the selected file's summary or
      segments to the bottom panel.

**UI — timeline lanes (data already exists, nothing draws it)**

Built 2026-09-12. Shape of the whole group, decided in review before any of it was
written: `WaveformView` grows a **lane stack** along the bottom of its waveform area —
segment band lowest (oldest, and the one that is actually clicked), then chords, then
spans, with the peaks taking whatever height is left. A lane only claims height when the
current file has data for it, the user hasn't switched it off, and the peaks would still
be left at least 40 px — so dragging the bottom panel small drops lanes one at a time
instead of crushing the waveform to nothing. Notes are deliberately *not* in that stack
(see below). `computeLanes()` is the single place the geometry is decided; paint,
hit-testing, and `getSegmentBandBounds` all read it, so a lane can never be drawn
somewhere the mouse doesn't think it is.

Two things are shared by all three lanes. `secondsToX` is factored out of the old
inline zoom arithmetic, so every lane pans and zooms with the peaks for free. And all
three reads happen on the message thread in the selection handler rather than in another
`RowBuildJob`: each is one `json_each` statement over a `machine` blob that tops out at
65 KB across the whole library — measured, not assumed — which is nowhere near the
160–640 ms folder-click cost that justified backgrounding row builds in review round 3.

- [x] *(built; not yet checked on screen)* **Auto markers from silence detection.**
      `files.active_spans` gets its own 8 px lane of teal blocks directly above the
      segment band — decided over the alternative of dimming the silent regions instead,
      because a span is the raw material a caption segment is made out of, and putting
      the two adjacent makes them read as the same object at two stages. Teal is
      `MiraLookAndFeel::active`, which the palette already documented as the
      active-region colour before anything drew active regions.
      Span widths are floored at 1 px (several real spans on the EP6 brass stem are under
      a second on a 37-minute file, and would otherwise round away to nothing and read as
      "not detected"), and hit-testing gets 3 px of slop either side for the same reason —
      a block that can be seen but not clicked is worse than one that isn't drawn.
      **Promotion**: right-clicking a span offers "+ Segment from this span", routed
      through `BottomPanel::onAddSegment` — the identical callback the +Segment button
      uses — so a promoted span still gets the synced-set/this-file-only scope question
      and the reload that follows, with no second code path. A left click selects the
      span's range instead (seeks there, arms +Segment), so the common case doesn't
      need the menu at all. It is a menu rather than an immediate write because this lane
      sits one row above the segment band: a mis-aimed click would otherwise silently
      add a row.
- [x] *(built; not yet checked on screen)* **Chord lane under the waveform.** A 14 px
      lane of blocks, one per change, each running until the next change (the last to the
      end of the file — `$.chords` stores instants, the lane draws intervals). Blocks are
      coloured by root pitch class **around the hue wheel by fifths, not by semitone**:
      progressions move by fifths far more often than by semitone, so a I–IV–V lands in
      neighbouring hues and a real key change reads as a colour jump. A no-chord ("N")
      region stays neutral rather than being given a hue it doesn't have.
      Labels only draw when the block is wider than 26 px, which is what makes the
      211-change EP6 stem a readable harmonic map at Fit zoom instead of a smear of
      clipped text — zoom in and the names fill back in. Root parsing (`chordRootPitchClass`
      in Main.cpp) accepts only `#`/`b` as the accidental, which is what keeps "Bm" (B
      minor) apart from "Bb" (B flat) — checked against the real label set in the DB,
      which includes `Bbmaj7`, `Bm`, `Bbaug`, `Em7b5/D` and `Ab/Gb`.
- [x] *(built; not yet checked on screen)* **Note-transcription overlay.** Decided in
      review: drawn *over* the peaks, not parked in a lane of its own. The files that
      have `$.notes` are short melodic loops where the peaks aren't carrying much on
      their own, and a roll reads against the audio it transcribes. Pitch maps to y
      across the file's own min/max pitch rather than all 128 MIDI notes (a bass stem
      living in one octave would otherwise be a flat line across the bottom eighth),
      padded to at least an octave so a single-note drone isn't stretched over the full
      height as if it were a melody. Amplitude drives alpha, so a quiet passing note
      doesn't shout as loudly as the melody; blocks are floored at 2 px tall.
- [x] **Synced stem set not detected — found and fixed.** The guess in the original
      note was right: grouping only ever ran across one analyze batch. `main.cpp` built
      `siblingGroups` purely from the current run's `candidates`, and `isSiblingSet`
      needs two, so "analyze this one file" could never find a set, and a set established
      by an earlier full-folder run was invisible to every later single-file re-analysis.
      Fix is the missing other half: `Database::findAnalyzedSiblingsInDir` returns
      already-analyzed rows in the same parent directory with their stored duration, and
      those count towards the set. A new run **adopts** an existing `group_id` rather
      than minting a second one for the same folder+duration, and **backfills** rows that
      predate the set being recognised (`Database::setGroupId`) — without that, the file
      analyzed alone first would keep its empty `group_id` forever while everything after
      it got the group, which is exactly the half-grouped state the EP6 folder was found
      in. Duration comes from `machine.$.duration_seconds`, the only place it is stored,
      which is why a never-analyzed sibling can't be considered at all (see the backlog
      item below).
      The directory match is `substr(path, 1, n) = dir || '/'` plus an "no further '/'"
      test, not `LIKE` — a folder name containing `%` or `_` would otherwise match far
      too much, and a nested subfolder is a different set by `siblingKey`'s own
      definition.
      **Verified on real data** (throwaway DB, three copies of `fixtures/flamenco.wav` in
      one folder): analyze #1 → no group (correct: one file is not a set); analyze #2 →
      both files grouped, the first one backfilled; analyze #3 → joins the same id, no
      second id minted. Negative cases both hold: a half-length file in the same folder
      stays ungrouped, and a same-length file one directory deeper stays ungrouped.
      Still true of the real library: the EP6 stems only get their group once a *second*
      one of them is analyzed.
- [ ] **Scan-time `duration_seconds` column** (deferred out of the fix above, decided in
      review). Grouping can only see analyzed siblings because duration is only stored by
      analysis. A `files.duration_seconds` populated by a header-only read during Scan
      would let a whole folder group correctly with nothing analyzed at all. Needs a
      header parser or a new dependency in `mira_core` (which has no decoder today —
      Essentia's `AudioLoader` decodes in full, far too expensive for 13 × 37-minute
      stems), plus a migration and a backfill pass over the existing 1,628 rows. Not
      urgent: the fix above already covers the real workflow.

### Review round 4 — timeline lanes on screen (2026-09-12)

First on-screen look at the lanes above (user screenshots of BASS_1.wav and BRASS_1.wav,
Korean_Title_Track). "the chord and transcription has a mismatch of visual timing — wave
is somewhere else and chords are somewhere else."

- [x] **The lanes were in a different TIME BASE than the waveform.** Not a drawing bug —
      a data-semantics one, and it had been sitting in `machine` since chords and
      transcription were built. PRD §5 makes active-region detection mandatory and says
      "descriptors, MIR and the embedding then see only those spans": `main.cpp` points
      `mono` at the *spliced* active audio, and chords, transcription **and rhythm** all
      run on it. So every timestamp they produced was an offset into the splice, not a
      position in the file.
      Measured on the exact file in the screenshot (id 551, BASS_1.wav): file duration
      30.0 s, `active_spans` `[[10.496, 25.3867]]` = 14.89 s of audio, last chord at
      **14.72 s**, last note ending at 14.77 s. The chords ended at the *active*
      duration, so they drew compressed into the left half of a waveform whose audio sits
      between 35% and 85% — exactly what the screenshot showed. This also means
      `mira inspect` has been printing chord times that don't point at anything.
      **Fixed in both places (decided in review):**
      - New `src/mira/analyze/ActiveSpanMap.h` — header-only and dependency-free, because
        the analyzer (the heavy `mira` target) and mira_ui (which links only `mira_core`
        and has no decoder at all) both need exactly this arithmetic and neither should
        depend on the other to get it.
      - **Analyzer**: chord times, note start/end, beats, downbeats and Essentia's beat
        ticks are mapped back to file time before serialising, and `machine` now carries
        `"timebase":"file"`. BPM is deliberately *not* converted — it comes from the mean
        interval between beats within the spliced audio, and cutting silence out doesn't
        change the tempo of what remains.
      - **mira_ui**: converts legacy rows (anything without the marker) on read, so the
        existing 108 analyzed files are right immediately without re-analyzing anything —
        chords alone measured 15 s per song, and the lanes would have stayed visibly wrong
        until then.
      An interval that straddles a splice is **split**, not stretched: mapping its two
      endpoints independently would produce one long block covering audio no analyzer ever
      saw. Both paths then clip to `active_spans`, so a freshly analyzed row and a legacy
      one draw identically and a block never sits over silence. The one place endpoints
      are mapped rather than split is `$.notes` in the analyzer's own output — `notes` is
      a record of note *events*, and turning one straddling note into two would misstate
      how many were transcribed; consumers that draw them clip instead.
      **Verified.** Re-analyzed a copy of id 551 into a throwaway DB with the fixed
      binary: `timebase=file`, chords 10.50 → 25.22, notes 10.54 → 25.27, downbeats
      11.18 → 23.98 — all inside `[10.496, 25.3867]`, where before chords ran 0 → 14.72.
      BPM unchanged at 75.56. The mapping arithmetic itself is covered by a standalone
      check (both directions, the straddling-splice split, the empty-spans identity, and
      the file-time clip) — all pass.
- [x] **Bar/beat ruler + grid** ("can we actually build a proper scale timeline like
      bar·beat time so helpful to match it as well"). A ruler strip claimed off the *top*
      of the waveform area — the first lane claimed and the last to be dropped when the
      panel gets short, since it's the axis every other lane is read against.
      Two scales share the one strip (vertical space is already contended by four lanes,
      and they never collide horizontally): m:ss ticks always, from a fixed
      0.1/0.25/0.5/1/2/5/10/15/30/60/… ladder picked so ticks stay ≥ 64 px apart — a
      generic "nice number" algorithm would happily choose 20 s or 2.5 minutes. Bar
      numbers sit on the tick line whenever the analysis found downbeats.
      The grid is drawn from **real detected downbeats** (`$.rhythm.beat_this_downbeats`,
      already stored and never used until now), not a grid laid out from the BPM scalar —
      a synthetic grid drifts against anything that isn't metronomic, which is exactly the
      through-composed material a bar ruler is most needed for. Lines run the full height
      through the peaks and every lane, which is what makes "does this chord land on that
      hit" answerable by eye. Beat lines (fainter) only appear once they're ≥ 14 px apart,
      and bar numbers thin out below 26 px, so a zoomed-out 37-minute file doesn't become a
      solid block of lines.
- [x] **Mouse handlers for zoom / scrub / move the view.**
      - **Scrub**: press and drag in the ruler. This is the one place a horizontal drag
        moves the playhead instead of selecting a range — which is the real reason the
        ruler earns its own strip rather than being painted decoration. Playback keeps
        running while scrubbing if it was running, so you hear where you land.
      - **Pan**: Alt-drag, or the middle button. Deliberately *not* plain drag: plain drag
        already means "select a range", and a selection is how segments get declared, so
        the older and more load-bearing gesture keeps the unmodified button. Drags the
        content, not the viewport — mouse right moves audio right.
      - **Zoom**: wheel/trackpad vertical, and pinch (`mouseMagnify`), both anchored on
        the cursor so the audio under the pointer doesn't move — that's what makes "zoom
        into this hit" one gesture instead of zoom-then-hunt-then-pan. Exponential, so a
        notch is the same proportional change at 1x and at 40x. Horizontal swipe still
        pans; whichever axis dominates wins, so a slightly-off swipe can't zoom by
        accident. Double-click fits (but not in the ruler, where a double-click is just
        two scrubs). The zoom buttons keep re-centring on the playhead, having no cursor
        to anchor to.
      - **Context menu**: right-click anywhere gives Zoom In/Out/Fit, "+ Segment from
        Selection" and Clear Selection when there's a selection, and the Lanes submenu.
        Everything reachable by gesture is also reachable by name — the gestures are worth
        learning but shouldn't be the only way in, and a menu is also where "what can this
        thing even do" gets answered. Right-click over a span or a segment band still
        belongs to that object, whose own actions matter more there.
      - Cursor feedback is the only other discoverability hint: an I-beam over the ruler,
        a grab hand while Alt is held.
- [x] **Lane on/off** ("also choose the view - only on off for chords and notes
      transcription and stuff"). The Lanes menu now carries Time ruler, Bar grid, Active
      spans, Chords and Notes, and is reachable both from its transport-row button and
      from the right-click menu — the button alone was easy to miss.

### Review round 5 — menus, process visibility, logs (2026-09-12)

From the first long real run: 12 score stems of 41:27 each, analyzed in one batch.

- [x] **The waveform's title bar is a menu surface.** ("the title bar of the waveform,
      can we use that section for menu — along with the right click — so edit tags, cut
      segments, zoom, lanes and also into proper menu so it available.")
      Three menus, one per kind of thing: **Tags** (what this file or segment is called),
      **Segments** (cutting it up), **View** (zoom and lanes). Each appears in **three**
      places — the header buttons, the waveform's right-click, and the macOS menu bar —
      and all three are built by the *same* functions and dispatched through one shared
      action-id space. That matters more than it looks: the failure mode with three
      hand-written copies of a menu is the one that only shows up when someone adds an item
      to two of them. `MainComponent` builds Tags and Segments (it owns the database),
      `WaveformView` builds View (it owns the zoom and lane state, ids 800–899), and
      `BottomPanel::dispatchMenuAction` routes a result from any surface to whichever owns
      it.
      "Edit Tags..." opens the **details sidebar** rather than a second dialog: that
      sidebar already *is* mira's tag editor (review round 2 replaced the pop-out window
      with it), and a parallel dialog editing the same fields is how two editors start
      disagreeing about what was saved. The macOS bar is now
      File · Analyze · Tags · Segments · View · Window, with Audio Settings moved under
      Window beside the log rather than owning a one-item top-level menu.
      **Verified**: all four new menus enumerate correctly through the accessibility API
      with the right items, separators and enable states.
- [x] **The CLI was the reason nothing could be shown.** ("in the bottom bar - show which
      file is getting analysed or the process that is going on - so we know its not hung it
      is process.")
      Root cause, not a UI gap: `mira analyze` printed `progress: N/M path` **after** the
      database write and nothing before it. With 41-minute stems that is one line every
      several minutes and total silence in between — genuinely indistinguishable from a
      hang, and mira_ui had nothing to display because nothing had been said.
      New machine-readable progress protocol on stdout, all of it:
      `decoding: <path>` (the decode pass, which runs over every candidate before any
      analysis and is itself minutes of work on a 41-minute file — worth naming rather than
      showing as a stalled "0/12"), `starting: N/M <path>`, `stage: <name>` per completed
      stage, and the existing `progress: N/M <path>`. The stage lines are gated behind a
      new `--progress-stages` flag (mira_ui always passes it) so plain CLI use isn't
      spammed; `starting:` is unconditional because it's useful to anyone.
      Deliberately separate from `--verbose`, which stays a human-readable stderr stream
      with timings — one is a diagnostic with numbers in it, the other is a readout with
      none. **Verified**: a real run prints the full ladder, `decode` →
      `router` → `active-region detection` → `DSP descriptors` → `embedding` → … →
      `auto-segments` → `progress:`.
- [x] **Status dots in the waveform's title bar.** ("i need status dots like BLue dot when
      analysing is on or there is some process goin on so we know whats goin on.")
      A dot plus a short label in the one strip that's always on screen while a file is
      selected. It **pulses** (sine, 2 Hz) rather than sitting static — a static dot says
      "a thing is true", a pulsing one says "a thing is happening", and telling those apart
      is the whole point. Teal (`MiraLookAndFeel::active`, the palette's existing
      in-progress colour) for analyzing, amber (`accent`) for scanning, nothing when idle.
      The dot's slot is reserved whether or not it's showing, so the filename doesn't shift
      sideways when a batch starts — a name that jumps reads as a glitch.
      **Not yet seen in motion** — the wiring off the (verified) CLI lines is
      straightforward, but nobody has watched it pulse through a real batch.
- [x] **Log window.** ("in the osx bar - can we have a log - so we get to see the cli logs
      and figure out.") `Window > Log...`, new `LogView.h`. Until now every line the child
      `mira` process printed was thrown away except the one `progress:` line the readout
      needed — the wrong thing to discard when a stem comes back mislabelled and what the
      analyzer actually said on the way there is the first evidence. Now **every** line
      goes to the store verbatim, recognised or not (the whole point of a log is the lines
      nobody thought to parse), alongside app-side scan/analyze events.
      A 4000-line ring buffer, not a file: a live window into the current session, and mira
      writes nothing to disk it has no reason to. Read-only editor with Follow (a toggle —
      scrolling back to read something and being yanked to the bottom by the next file is
      the single most annoying thing a log window can do), Copy and Clear.
      **Verified**: opens from the menu. Needed an explicit `toFront(true)` — a freshly
      created `DocumentWindow` otherwise comes up *behind* the main window on macOS 15.5,
      which looks exactly like the menu item having done nothing.

### NEXT — review round 7 (2026-09-12, from the user; nothing below is built yet)

The cue window exists but is not usable as a workflow yet. In priority order as given.

- [ ] **Cues and segments are confusing as presented.** ("rite now segemetns and cue are
      confusing") The two are genuinely different things and the UI never says so: a
      **segment** is file-scoped — a sample inside one stem, from that stem's own silence;
      a **cue** is group-scoped — one piece of music across the whole synced set. They share
      a table (`segments`), a colour language, and the word "Segments (N)" in the panel, so
      nothing on screen distinguishes them. This is the root of several of the items below
      and should be settled first, because it decides the naming everywhere else.
      Needs: distinct names in every surface, distinct colours, and the file list's child
      rows saying which kind each row is.
- [ ] **Editing a cue must warn that it changes every stem.** ("make sure editing the cue
      will edit all the segments of this cue warning") A group-scoped edit silently rewrites
      the boundary for all 15 stems at once — correct behaviour, invisible consequence.
      Needs a confirmation on the first destructive edit of a session at minimum, and
      permanent wording on the cue's own menu.
- [ ] **Undo.** ("can we get undo to work?") There is none anywhere in mira_ui today — not
      for cue drags, tag edits, segment deletes or folder changes. Cue editing makes this
      urgent rather than nice-to-have: one mis-drag currently rewrites two cues across every
      stem in the set with no way back. JUCE has `UndoManager`/`UndoableAction`; the natural
      shape is one undoable action per database write in `MainComponent`, since every edit
      already funnels through a small number of methods there.
- [ ] **Reaching the cue editor is not discoverable.** ("i am still confused of how to reach
      the cue editor") It is currently buried in Segments > Cue Editor..., and was greyed
      out until the menu-rebuild fix landed. Wanted: its own entry in the macOS menu bar
      ("Cues" as a top-level menu rather than a section inside Segments), **and** a direct
      way in from the UI itself — clicking a cue, or a button in the bottom panel's header
      next to Tags / Segments / View.
- [ ] **On/off for the bottom matrix.** ("need a way to not [have] the cue editor at the
      bottom, on-off view is needed") A `Stem Activity Matrix` toggle already exists in the
      Segments menu, but it evidently isn't findable — which is the same discoverability
      problem as the item above, not a missing feature. Put it where the thing it hides
      actually is.
- [ ] **Play a cue to hear it, then name it.** ("i will need to be able to play the cue to
      hear it and name it") The cue window has no transport at all today — naming a cue
      without hearing it is guesswork. Needs play/stop scoped to the selected cue, and a
      decision about *what* it plays: the stem mix if the set has one, otherwise a chosen
      stem, since mira cannot sum 15 stems live.
- [ ] **Region-marker naming, like a DAW.** (user screenshot: Ableton's "FINAL-CUT - 1"
      locator bar) A cue should be a **named labelled bar spanning its range along the top of
      the timeline**, named inline by clicking the label — not a colour band whose name is
      only reachable through a right-click menu. This is the interaction the user is asking
      for and it should replace, not supplement, the current banding.

### Review round 6b — the cue window (2026-09-12)

From the first on-screen look at the matrix (user screenshots: it renders, zoom-sharing with
the waveform works). Three asks: "how to edit", "there should be a cue range marker which we
can mark the cue", and "this is too small of a window... the cue tracking needs a full
window?"

- [x] **Cue Editor window** (`CueEditor.h`), from Segments > Cue Editor...
      Its own window rather than a tab, which was the user's other suggestion. The tab strip
      is scoped to *folders* — every tab means "show me this folder's files" — so a cue
      workspace there muddies what a tab is. And cue work wants the whole screen, often on a
      second display beside the DAW the reel came out of, which a tab inside the main window
      can never give. The Log window already set the precedent.
      Contents: toolbar (Detect Cues / mark-a-cue / Clear Untagged / zoom), its own ruler,
      the matrix at full size, and a cue list column (number, start, length, tag) that
      selects on click and opens the cue's menu on double-click.
      The bottom panel keeps its compact 11px matrix — that one answers "what is this file
      doing inside the set" while browsing; this one is for doing the cue pass.
      **Verified on screen**: opens, toolbar and ruler render, all 15 EP9 stem names legible,
      density strip present, cue list column in place.
- [x] **Rows grow to fill the window.** First version kept the lane's fixed 20px row height,
      which left two thirds of an 848px window empty — missing the entire point of having
      asked for a bigger window. Row height is now derived from the available height
      (14–40px); 15 stems fill the window, 40 is the cap before it reads as a bar chart
      rather than a timeline.
- [x] **Cue range marker** — sweep a drag across the matrix and the range stays tinted until
      "+ Cue from range" turns it into a real cue across every stem, or it is cleared. A
      hand-marked cue is `markSegmentEdited` from birth: nobody sweeps fifteen stems by
      accident, and a re-detect must never take it away. A sweep that doesn't actually move
      is treated as a click on the cue underneath, so selecting a cue can't arm "+ Cue" for
      a zero-length range.
- [x] **Ruler bar-number collision** (visible in the user's second screenshot as overlapping
      digits across the whole ruler). The thin-out test estimated bar spacing from
      `downbeats[1] - downbeats[0]`, which is wrong whenever downbeats are unevenly spread:
      STRINGS.wav opens with a long silence, so its first two downbeats are minutes apart,
      the estimate came out huge, and every downbeat got numbered. Now thins against the
      last number actually drawn, which is correct for any distribution.
- [x] **The macOS menu bar was stuck in its launch-time state.** "Cue Editor..." was
      permanently greyed out even with a grouped file selected. The native menu bakes each
      item's enabled state in when the menu is *built*, and nothing was telling it to
      rebuild — so Tags/Segments/View had been showing "nothing is selected, everything
      disabled" since launch. This affected every menu added in review round 5, not just the
      cue items. Fixed with a `menuItemsChanged()` on selection change.
      Worth noting how it was found: the AX query said the item existed, and clicking it did
      nothing silently. Checking `enabled` rather than presence is what identified it.

**Not verified on screen**: cues rendering inside the cue window, boundary dragging, and
mark-a-cue. All three are verified at the data level (24 cues detected, both edit types
surviving a re-detect) and the window itself renders, but GUI automation was too unreliable
to drive the full path. The user's library was left untouched — no cues were written to it.

### Review round 6 — cues and the stem activity matrix (2026-09-12)

Design settled with the user in discussion before any code (their instruction: "dont do
code changes lets figure out edge case and stuff"). The conclusions that shaped it:

- **A stem is a submix, not an isolated instrument.** mira assumed stem ⇒ isolated ⇒ prefer
  IRMAS. But STRINGS is twenty players, RHTM is a rhythm section, PADS is layered synths.
- **A score reel's file-level tempo is meaningless.** Confirmed from stored data: 6 of 15
  EP9 stems are flagged `tempo_unstable` with ranges of 33–58 BPM (BASSS_1 reports "91.6
  BPM" across a 57.5 BPM spread). Worse, 5 more report `stable` with `stddev 0.0` but
  `tempo_window_count = 0` — which the code's own comment says means *unjudged*, not
  stable. **11 of 15 file tempos are untrustworthy and mira already had the evidence.**
  The right scope is neither the file nor a 2-second auto-segment: it is the **cue**.
- **Detection will never be right, so editing is the product.** Most scoring is "carpeted"
  (wall-to-wall, no silence between cues), so no detector finds every boundary. Propose
  cheaply, correct fast.

- [x] **Cue detection** — new `src/mira/analyze/CueDetection.{h,cpp}`, in **mira_core**
      rather than the heavy `mira` target because it needs **no audio at all**: it is pure
      arithmetic over `files.active_spans`, which analysis already stored. mira_ui calls it
      directly and it finishes in milliseconds on a 41-minute fifteen-stem reel, instead of
      being another child process.
      Two independent signals, both free from stored data:
      1. **Silence** — where every stem is quiet, a cue ended. On EP9 this collapses 506
         raw spans into 21 regions separated by silences of up to 4:36.
      2. **Instrumentation change** — a cue change is an instrumentation change even when
         nothing goes silent. Measured: 38 moments where ≥3 stems changed state at once,
         several *inside* silence-derived regions and invisible to signal 1 (the 2:27
         region at 7:34 has internal changes at 8:51, 9:40, 9:57).
      Non-maximum suppression on the second signal, because EP9 has four separate 3-stem
      changes inside ten seconds around 20:50 — one phrase, not four cues.
      Defaults chosen by sweep against the user's own estimate of "around 21" cues:
      `minStemsChanged=3, minCueSeconds=25` → **24 cues, median 54 s, shortest 27 s**
      (vs 36 cues with 16 s slivers at 15 s, or 13 cues with a whole 4:24 region left
      unsplit at the loosest setting).
- [x] **Two bugs found by running it on real data, before any UI existed.**
      - `filenameSuggestsFullMix` matched **"FX MASTER.wav"** — an FX bus with an active
        ratio of 0.004. Cue detection took a near-silent file as its map of where the music
        is and returned **zero cues**. Fixed: a mix word with a *section* in front of it is
        a bus, not the mix ("FX MASTER", "DRUM MASTER", "VOX MIX" → not the mix; "MASTER",
        "STEM MIX", "mixdown", "Paintball-BGM-StemMix" → the mix). Verified against twelve
        real and constructed names.
      - The same false positive was about to corrupt the review-round-5 content-type fix,
        which would have relabelled FX MASTER as a `track`.
      Also added the **confirmation** half the user asked for: a candidate mix must cover
      ≥80% of the union of the stems to be believed. A real mix plays wherever any stem
      plays; FX MASTER covers 10 s against the union's 1490 s. Belt and braces — the
      filename test is now right, but taking the wrong file as the region map is
      catastrophic rather than merely wrong, since everything downstream is built on it.
- [x] **Cues reuse the existing schema unchanged.** A cue *is* a group-scoped segment:
      `segments.group_id` covers every stem in the synced set at identical timestamps, and
      `segment_analysis` is keyed on `(segment_id, file_id)` so one cue can be analyzed
      separately as heard in each stem. No migration. This only works because review round
      4's `group_id` fix made the synced set identifiable in the first place.
- [x] **Regeneration rule (user's call: "regenerate untouched, edited never overwritten").**
      New `deleteUntouchedAutoSegmentsForGroup`, mirroring the file-scoped version's
      `human = '{}'` test. A re-detect also skips proposing any cue that overlaps a kept
      one — the human's boundary wins. `markSegmentEdited` flags a *dragged* boundary as
      touched without requiring a tag, so a correction survives even if nobody names it.
      **Verified on a copy of the real library**: pass 1 created 24 cues; one was tagged
      "action" and another's boundary dragged +7 s; pass 2 regenerated 22, kept 2, total
      still 24 — both edits survived byte-exact, no duplicates, no drift.
- [x] **Stem activity matrix** — new `GroupActivityView.{h,cpp}`. The DAW-multitrack option
      was considered and rejected: fifteen 41-minute waveforms is a lot of decoding and
      vertical space for a question that doesn't need the waveform. What the question needs
      is only *where each stem is playing*, which `active_spans` already answers for free —
      so each stem is one 11 px row of blocks, fifteen of them in about the height of two
      waveform lanes. "Six stems stop and four others start here" is visible at a glance.
      A density strip along the bottom carries the same signal in one row when the panel is
      short. The matrix shares the waveform's zoom/pan (`WaveformView::onViewChanged`) so
      the two read as one timeline, not two pictures of the same reel at different scales.
      Span blocks floored at 1 px, same reasoning as the waveform's own span lane.
- [x] **Boundary editing, both ways the user asked for.**
      - **Drag** on the matrix, with snapping to the nearest stem edge within 1% of the
        visible window — at fit zoom on a 41-minute reel one pixel is ~1.7 s, and the
        moment a stem actually starts is almost always what a boundary wants to be.
        Clamped between its neighbours so a cue can't invert or collapse.
      - **Type a timecode** (m:ss or m:ss.mmm) from the cue's own menu.
      Dragging one boundary writes **two** cues — the one that starts there and the one that
      ends there — because cues in a reel are a partition, not islands. The previous cue is
      only extended when the two were actually adjacent (<1 s apart), so a boundary across
      real silence doesn't stretch the previous cue over the gap.
- [x] **Menus**: Segments menu gains a "Cues (synced set)" section — Detect Cues, Clear
      Untagged Cues, and a Stem Activity Matrix toggle, all disabled when the selected file
      isn't in a group. Right-clicking a cue gives Edit Cue Tags / Set Start Timecode /
      Play From Here / Delete Cue. Cue tags are the point (the user's framing: a cue is a
      tag identifier — "funny", "action"), and tagging one also makes it permanent.

**Not done / open**

- [ ] **Nothing here is confirmed on screen.** Cue detection, persistence and the
      edit-survival rule are verified against the real library at the data level; the
      matrix, drag-editing and the cue menus compile and are wired but nobody has looked at
      them. GUI automation proved unreliable across this whole session.
- [ ] **The user could not verify the 21/24 cue count against the real session** (the EP9
      project wasn't to hand). Until someone checks the boundaries against the actual cue
      list, the detector is plausible, not validated.
- [ ] **`mira similar` still searches whole files.** Agreed in discussion that the retrieval
      unit should be the segment/cue — "what's the point of showing a 40 min similar file".
      The blocker is narrow: `vec_embeddings` is keyed on `files.id`, one row per file.
      Segments already store their own embedding inside `segment_analysis.machine`; they
      just aren't in the searchable index.
- [ ] **Per-cue tempo and key, and a range at file level.** Agreed (user: "range"), not
      built. Score stems should show per-cue tempo/key; the file row should show the spread
      rather than a single meaningless number. `tempo_unstable` / `tempo_range_bpm` are
      already computed and still unused by the UI.
- [ ] **FX stems should skip tempo/key entirely** (user: "yes fx stems skip"). Not built.
- [ ] **Editable naming map** (user: "yes... we can keep adding to it as and when we get new
      naming schemes"). The stem-name → instrument table is still compiled into
      `Scanner.cpp`. It should be a file alongside `taxonomy/*.yaml`, with families
      (percussion: drums/rhythm/beat/groove/hi-percs/…; strings: low/mid/high strings,
      cello, double bass, viola, violin), register qualifiers as modifiers rather than
      instruments, and **multi-label output** — the user's own "DBCELLO" is double bass
      *and* cello, which a single label already gets wrong.

### Analysis quality — investigated 2026-09-12 (EP9: 15 stems × 41:27, 284 auto-segments)

**"A lot of auto-segments identified as nothing" — the content gate, not segment length.**

The guess in the original note (too short for the heads to say anything) was **wrong**, and
checking it first is what found the real cause. **115 of 284 segments (40%)** had no
instrument/genre/mood because `content_gate.is_music` was false, so the whole gated block
never ran. A rejected segment keeps `dsp`, both embeddings and `content_gate` and loses
`moodtheme`/`instrument`/`genre`/`voice_instrumental` — literally identified as nothing.

Three things came out of measuring it, all on real material:

1. **The gate was never saying "not music".** In *every* rejected segment, "Music" was
   still the **top-ranked** AudioSet label, at 0.33–0.39. It was saying "music,
   moderately", and losing to a 0.45 cutoff.
2. **The score is length-biased by construction.** `runContentGate` max-pools class probs
   across 10 s chunks — deliberately, and for a good reason at whole-file scale (a quiet
   tail shouldn't veto an obviously musical file). But more chunks can only raise a
   maximum, so the score rises with duration. Measured mean `music_score` by segment
   length: **0.399 (<5 s) → 0.496 → 0.515 → 0.563 → 0.619 (40 s+)**, pass rate 27% → 86%;
   the same audio as one 41-minute file scores 0.71–0.87. One threshold cannot serve a
   4.8 s segment and a 41-minute file. This is the deeper bug and it is *not* fixed — see
   the open item below.
3. **The gate costs 32× more than it saves.** Measured per file: gate **419 ms**; the heads
   it suppresses total **13 ms** (moodtheme 3, instrument 4, danceability 1, genre 4,
   voice/instrumental 1). It is not a performance optimisation — its only job is PRD §2c
   honesty ("music heads must only run on music").

- [x] **Threshold recalibrated 0.45 → 0.30** (`ContentGate.cpp`). The old value came from
      exactly two points and its own comment asked for "a wider set of real files before
      trusting it further" — that set now exists. Evidence on both sides:
      265 of 284 real segments pass at 0.30 (was 167), while synthetic non-music controls
      measured on this build stay rejected — **white noise 0.253** (CED ranks its own
      "White noise" class *first* there, not "Music") and an **impulse/click train at
      0.019** ("Engine"). At 0.30 the remaining rejections stop tracking length
      (3/52, 6/103, 6/56, 2/71 across the same buckets), which is the sign the cutoff is
      discriminating on content rather than duration.
      Note the earlier comment's claim that white noise scores 0.39 did not reproduce:
      measured 0.253 here. The old number is what made 0.45 look necessary.
      **Verified end-to-end**: the exact segment that was rejected at 0.386 (RHTM-2,
      684.4–700.7 s), cut out and re-analyzed with the new build, now passes with the
      *same* score and comes back with instruments (synthesizer 0.36, electric guitar 0.26,
      guitar 0.25, drums 0.24, bass 0.22), genre and moodtheme populated.
      **Whole-library regression check** — 19 files flip to music in the 0.30–0.45 band and
      every one is genuinely musical (bass loops, strings loops, vocal stems, synth layers,
      pads, reverb tails, FX MASTER). Nothing spurious crosses over.
- [x] **RHTM-2 reading "organ" — full chain found.** Three compounding causes:
      (a) `instrumentFromFilename` had no `rhtm`/`rhythm` entry, so the filename never
      overrode anything; (b) the drums carve-out in `pickPrimaryInstrumentEntries` only
      fires when drums is **rank 1** at ≥ 0.3 in the full-mix model, and RHTM-2's full-mix
      top is synthesizer 0.43 with drums 0.21 at rank 2; so (c) IRMAS answered, and IRMAS
      **has no drums class at all** — organ 0.254 is the best a model without drums can do.
      Fixed by adding `rhtm`/`rhythm` → `drums` to the hint table, per the user's
      confirmation that these are drum/percussion stems. Placed **after** the guitar
      entries on purpose: "RHYTHM GTR"/"Rhythm Guitar" is a common delivery name and is a
      guitar. **Verified**: RHTM-2_1 → drums, RHTM 1_1 → drums, RHYTHM GTR → electric
      guitar, Rhythm Guitar → electric guitar, and every pre-existing mapping unchanged.
      `WOODWIND_1.wav` deliberately gets **no** hint: the taxonomy has clarinet/flute/oboe
      individually but no "woodwind", and the hint table's contract is that its labels are
      taxonomy vocabulary so they merge with model output instead of sitting beside it.
      A hypothesis that was checked and **rejected**: "prefer the full-mix model whenever
      IRMAS's vocabulary can't express its top class". Measured across the batch, the
      full-mix model answers **synthesizer for 13 of 15** isolated stems, so that rule
      would have broken DBCELLO (IRMAS: cello 0.47) and STRINGS (violin 0.30). The
      filename remains the most reliable signal for delivery stems, which is what the
      existing design already assumes — this just closes a gap in it.
- [x] **Bug: every named stem was findable under `Instrument: drums`.** `buildRow` added a
      hard-coded `"drums"` facet whenever *any* filename hint fired — correct when the hint
      was percussion-only, wrong since review round 5 generalised it to every instrument.
      BRASSS, DBCELLO, STRINGS and the rest were all being indexed as drums. Now adds the
      hint's own label.
- [x] **Dead code**: `topIsPercussion` was computed and then `juce::ignoreUnused`'d.

**Open — the gate's remaining false negatives (not fixed, evidence recorded)**

- [ ] **`music_score` is not comparable across durations** (finding 2 above). 0.30 is
      chosen to be safe at the short end, where the bias hurts most, but the underlying
      length-dependence of max-pooling is untouched. The principled fixes, in rough order
      of appeal: have a segment **inherit its parent file's** verdict (a slice of music is
      music — also removes the 419 ms per-segment gate entirely); or normalise by chunk
      count; or skip the gate for **declared** stems, where a human already answered "is
      this music" by filing the folder as Score Stems.
- [ ] **Real stems still failing at 0.30.** From the whole-library check:
      `Paintball-BGM-StemMix.wav` 0.276 (top label "Siren"), `VOX-SOLO_1.wav` 0.227,
      `Korean_Title_Track/SOLO.wav` 0.042 ("Siren"). A BGM stem *mix* failing a music gate
      is plainly wrong. Most of the rest that still fail are drum one-shots
      (`DRUM HITS/HATS/*`, `snare/*`) — the same isolated-percussion weakness that made
      RHTM-2 read as organ, and arguably a category where "is this music" isn't a
      meaningful question to ask at all.
- [ ] **These fixes only reach existing rows on re-analysis.** Worth knowing before doing
      that the hard way: the full 1280-d discogs-effnet embedding is already stored per
      file *and* per segment (`machine.$.embedding.vector`), and five of the six gated
      heads run from that vector in ~13 ms total. A `reclassify` that re-applies the gate
      decision to a stored `music_score` and re-runs those heads from stored embeddings
      would turn hours of re-analysis into seconds. Only `stem_instrument` (IRMAS) would
      still need audio, since it runs on the signal rather than the embedding.

Lane visibility is a "Lanes" menu in the transport row rather than three separate
toggles — the lanes are mostly absent anyway (32 of 108 analyzed files have chords, 29
have notes), so three permanently-visible buttons would spend transport-row width on
something rarely touched. The menu separates *ticked* (switched on) from *enabled* (this
file has the data), so a ticked-but-greyed item answers "why is nothing showing" honestly
instead of leaving it ambiguous. Lanes are **not** gated on `isStemPath` the way the
segment controls are: a sample or a music track can have chords and a transcription too,
and drawing what was analyzed is never wrong — only the *authoring* workflow (declaring
caption segments) is stem-specific.

**UI — second screenshot pass (decisions made in review)**

- [~] *(creation + per-segment analysis done and verified on real data; search not yet)*
      **Auto-segments from silence detection (decided: option A).**
      Built: `mira analyze` turns a stem's active spans into `segments` rows with
      `source = 'auto'` (new column, migrated with a 'manual' backfill), each analyzed
      immediately through the existing `buildSegmentMachineJson`. Spans closer than 1.5 s
      merge; anything under 2 s is dropped (first-pass numbers). Re-analysis regenerates
      only untouched auto rows. Verified on id 580 (HHB VOX, 1:04): 6 raw spans →
      2 segments, 5.6 s total. Tagged segment 1, re-analyzed → still 2 rows, the tagged one
      kept, no orphaned `segment_analysis` rows. Bands in the waveform show each segment's
      own top instrument until it's tagged. Those per-segment labels inherit the
      instrument-accuracy problem below (the vocal stem's segments read "bass" first).
      Segments are searchable as of the split-filters item below. **Still open**: a stem
      only gets auto-segments when it's next analyzed, so existing stems (including the
      EP6 brass stem) have none until someone re-runs Analyze on them. Analysis creates
      segment rows from `active_spans` itself; they're editable afterwards. Each segment
      gets its own analysis (bpm/key/instrument/…) via the existing `segment_analysis`
      machinery. Segments are indexed and searchable: a BPM search that matches a
      segment surfaces it. Framing agreed in review: detected spans are *samples of the
      stem*, not cues. Cue detection and syncing is a separate, larger project to build
      on its own.
- [x] *(built; matching checked against real data, not yet on screen)* **Split
      filters.** Separate fields for key / tempo / mood / instrument / genre
      instead of one free-text box ("difficult to manoeuvre"). New `FilterBar.h`: search
      box + Key ▾ + BPM min–max + Genre ▾ + Instrument ▾ + Mood ▾ + Clear, AND-combined.
      The dropdowns list only values present in the current folder (files and segments),
      and a choice the new folder lacks resets to "Any" so no hidden filter carries over.
      Rows filter on structured values, not display text: the full label sets (human tags
      win, else top-1 plus anything ≥ 10%, the same bar the caption gates use), numeric
      BPM (analyzed, human, or an explicit `NNNbpm` token; the loose "70?" guess is
      excluded since it's often a track number), and key.
      **Segments are searched too**: a file is listed if it or any of its segments
      matches, and the File column shows "N segments" when segments matched. BPM/key-only
      filters don't count segments, since they inherit both from the file.
      Real-data check of the facet rule. HHB VOX's two segments carry "voice" (10% and 13%),
      so Instrument: voice finds it through its segments. The EP6 brass stem (1582) gets
      synthesizer/drums/bass/electric guitar/guitar/piano/acoustic guitar and **no brass
      or woodwind label at all**, so no instrument filter can find it for what it is. That
      isn't a filter bug; it's the instrument-accuracy work below, and filters get better
      when labels do.
- [x] *(built; not yet checked on screen)* **File details as a right sidebar,** not a
      pop-out window. Full height below the filter bar, draggable width (360 px min, half
      the window max). Follows the table selection (one row → that file, several → the
      batch editor); double-click / Details... opens it, Cmd+I toggles it. The pop-out
      `FileDetailsWindow` class is removed. Every rebuild is deferred and skipped when the
      selected ids haven't changed — Save/Close are buttons inside the sidebar, and
      rebuilding synchronously from their own handlers would delete them mid-click. Known
      trade-off: selecting a different row discards unsaved edits in the sidebar.
- [x] *(built; not yet checked on screen)* Bigger default window: 85% of the display's
      usable area, capped at 1680×1050 (was a fixed 920×720).
- [x] *(built; not yet checked on screen)* Main window title centred on the whole bar
      (it was centred in JUCE's lopsided post-button title space, so it drifted on
      resize), in uppercase "MIRA". PRODUCT_NAME is also "MIRA", so the menu-bar app name
      matches and the bundle is now `MIRA.app`.
- [x] *(built; not yet checked on screen)* Waveform cache catch-up. The precache only
      ran after scans that happened once it existed, so the existing library was never
      warmed. Now one background pass per root at launch; cached files are skipped.

**UI — review round 3 (user: "finish all UI first")**

- [x] *(fixed and measured; not yet confirmed by the user on screen)* **Clicks slow and
      jerky; every 6–7 clicks it seems to hang.** Result: folder clicks now block the UI
      for **0–1 ms** on every root, cold or warm (before: 160–640 ms cold). The rows are
      built on a background thread with its own DB connection (`RowBuildJob` →
      `FileTableModel::collectRows` → `setRows`), with a generation counter so a quicker
      second click discards the stale build. They're ready 14–638 ms later cold, 5–171 ms
      warm, while the window stays responsive. Analyze batches rebuild one row per
      finished file instead of the whole folder (worst click lateness 119 ms → 1 ms). Row
      clicks measured ≤ 54 ms in every condition (plain, sidebar open, playing, analyzing).
      **Remaining, not click-related**: a ~400 ms stall at every app launch ("no click
      yet" in the log). Likely the folder tree's startup audio check walking each root's
      subfolders on disk. Not chased yet.
      How it was measured, since the synthetic benches were what found both causes: The dev-only harness `MIRA_BENCH=scope` times exactly
      what a folder click does (`FileTableComponent::setScope`) per root.
      Findings so far:
      - Folder clicks: 160–640 ms cold (the 420-file sample folders, and ~300 ms for the
        V.Sound stem folders' first header reads), 5–165 ms warm.
      - Folder-tree repaint: 0–1 ms.
      - The waveform catch-up (`MIRA_NO_PRECACHE=1` to disable it) made no measurable
        difference, and it's nearly done anyway (1,592 of 1,628 files cached).
      So folder rebuilds are sluggish when cold but don't explain a periodic hang.
      `MIRA_BENCH=click` (per-row-click handler time + how late each click fired):
      - Plain clicks: ≤ 11–23 ms, never late.
      - Details sidebar open, or audio playing between clicks: ≤ 43 ms.
      - An analyze batch running: one click 119 ms late. **Cause found and fixed**: every
        file the batch finished rebuilt the *whole* list (~165 ms for 420 files) on the
        UI thread. Now only that file's row is rebuilt (`FileTableModel::refreshPath`),
        with one full refresh when the batch ends. Re-measured: max lateness 1 ms.
      - Always-on stall logger added: `~/.mira/ui-stalls.log`, every UI-thread block over
        100 ms with what was last clicked; bench runs are tagged `[bench]`. Its only
        stalls so far were cold folder opens at 370–667 ms, which is the remaining
        measured cause and the one reported ("folder clicks horribly slow"). **Fix in
        progress**: build the list's rows on a background thread with its own DB
        connection, and swap them in when ready.
      Earlier notes: Also noted while reading: every
      `Database::jsonExtract*` / `json_each` helper prepares a fresh SQLite statement per
      call (~30–40 per row in `buildRow`, more with segments), and the precache's decode
      runs on JUCE's own `thumb cache` thread at normal priority, not the job's
      background priority.
- [x] *(built; not yet checked on screen)* **Segments as child rows in the list pane** —
      parent file, then its segments as indented child rows with their own details (time
      range, length, their own instrument/genre/mood, loudness), not just bands on the
      waveform. A disclosure triangle on files that have segments (plus a faint
      "N segments" count), open/closed state remembered per file across rebuilds. Child
      rows show `↳ start – end`, "auto segment"/"segment", their own length, loudness and
      top labels (accent when hand-tagged), with BPM/key/sample rate dimmed as inherited.
      A filter that matches inside segments auto-opens the file and lists only the
      matching ones. Selecting a child row loads the parent file and selects that range on
      the waveform; double-click opens the parent's details; Analyze/Details act on the
      parent file (deduplicated). Drag-out of a segment row drags the whole file for now.
      Only stems re-analyzed since auto-segments were added have segments to show (e.g.
      HHB VOX).
- [ ] **Instrument accuracy to a basic working level** — "some places it detects vocals
      but the accuracy is around 50%". After the UI items above; the plan is in the
      section below.

**Instrument field / segments — review round 4 (from the child-rows screenshots)**

Measured on the two files in the screenshots: HHB VOX (stem) — stem model says voice 73%,
full-mix says synthesizer 44% / bass 29%, voice head 85% vocal. Bhabi-BGM-StemMix (a
*mix*, stored as a stem) — stem model voice 54%, full-mix synthesizer 56% / bass 35%,
voice head 98% vocal.

- [x] *(fixed; verified on id 580)* **A segment's instrument disagrees with its own file** ("synthesizer +5" under a
      file that says "voice +5"). Cause: `buildSegmentMachineJson` deliberately skips the
      stem-tuned model, so a segment only ever has the full-mix opinion, while the file
      row prefers the stem-tuned one. Fix: run stem_instrument per segment for stem
      parents, and apply the same preference rule the file rows use.
- [x] *(fixed and measured)* **"Why do the track *and* the segment both get analysed?"**
      Both did, and for single-segment files that was duplicate work. Measured on HHB VOX
      (1:04, 2 segments): file pass ~3.5 s (DSP 341 ms, effnet 302, DCLAP 616, content
      gate 539, stem model 590, rhythm 995, key 34), segment pass 2,723 ms — 43% of the
      run. And whole-file analysis already runs on *active audio only* (silence spliced
      out before any model sees it), so a lone segment covers the same audio the file pass
      already measured, even when it's 16% of the file's length. 26 of this library's 34
      segmented files had exactly one segment.
      Rule now: **one segment covering ≥95% of the file → no segment at all** (it would
      add a row that says nothing — the mix's own "0:00 – 1:04" row); **one shorter
      segment → keep the row, skip its analysis** (export-segments still uses it to trim
      silence, and CaptionFields already falls back to the file's document); **two or more
      → analyse each**, which is the case segments exist for.
      Verified: id 290 (one segment, whole file) → no segment; id 571 (one segment, 13%)
      → row kept, "no analysis, inherits file", run 1.9 s instead of ~4.7 s; id 580 (two
      segments) → both still analysed.
      Noticed while verifying, not caused by this: analysing *one* file of a sibling set
      alone can change its content type (id 290 came back `loop` rather than `stem`),
      because sibling-set detection only sees the files in the same run.
- [x] *(built; not yet checked on screen)* **The details panel follows the selected
      segment**, showing that segment's own analysis and tags, not the parent file's.
      Header gives the file name plus the range, length, whether analysis or a person
      made it, and the segment's own loudness. Genre/instrument/mood come from the
      segment's own analysis (stem-tuned first for a stem parent); BPM and key show the
      file's values, since per-segment analysis doesn't re-measure them, but can still be
      overridden for the segment. Save/Revert write to that segment's `human` via
      `setSegmentHumanField` / the new `clearSegmentHumanFields`, never the file's, and
      the caption block renders *that segment's* caption
      (`extractCaptionFieldsForSegment`) — the whole reason segments exist.
- [x] *(fixed; verified on id 581, now `track`)* **A mix inside a stem folder is being
      treated as a stem.** Two rules had to change, not one: the scan-time declaration
      *and* the router's path rule (`looksLikeStemPath` matches "stem" anywhere in the
      path, so every file in a `..._Stems/` folder hit it). `filenameSuggestsFullMix`
      (mix/master/bounce, filename only — "WORKING MASTER" is a real folder here) now
      excludes both, at scan time and at analyze time, so existing rows correct themselves
      on the next Analyze instead of needing a rescan. A file that stops being a stem also
      drops the auto-segments it was given as one.
      Cause: the folder category declared every file under it a stem at scan time, and the
      router then re-forced "stem" from the path anyway, so `Bhabi-BGM-StemMix.wav` was
      handed the isolated-audio model. Same "the filename is strong evidence in a stem
      pack" reasoning the percussion fallback already uses.
- [x] *(fixed; verified in both captions)* **The instrument field must carry every
      detected instrument**, not the top label. One shared rule now in
      `CaptionFields::allInstruments`, mirrored by the details panel so the two can't
      disagree: the preferred model's labels above threshold; for a stem, full-mix labels
      at 0.30+ as well (IRMAS has no drums/percussion class at all, and the high bar keeps
      full-mix noise out); plus "voice" whenever the voice head is confident — the head
      actually trained for it read 85% and 98% on the two files where both instrument
      heads said "synthesizer". Caption cap 4 → 8, and instruments now use taxonomy-
      normalized names ("electric guitar", not "electricguitar").
      Cause: a mix genuinely has many instruments at once, but CaptionFields kept at most
      4 above threshold, and the details field collapsed to top-1 for anything that wasn't
      a `track` — which is why a whole arrangement captioned as "voice stem".

**Instrument accuracy — review round 5 (filename evidence for stems)**

- [x] *(fixed; verified on id 566)* **A drum stem's segments read "electric guitar" under
      a file that correctly read "drums".** The file rows have a carve-out — IRMAS has no
      drums or percussion class at all, so when the full-mix model confidently says drums
      it wins — and segments didn't. Measured: segments scored electric guitar 0.27/0.30
      on the stem model against drums 0.37/0.38 on the full-mix one. The carve-out is now
      applied in all three places that pick instruments (table, details panel, caption).
- [x] *(built and verified)* **A stem's filename now leads its instrument list**
      (`mira::instrumentFromFilename`, shared by CLI and UI). "In case of stems the name
      of the file defines a lot of facts": whoever bounced `BRASS_1.wav` knew what was in
      it, while both models are unreliable on isolated audio. Filename only, never the
      path. The models then *add* to it rather than overriding it.
      Also: on a stem where the stem-tuned model leads, the full-mix model may now only
      contribute drums/percussion — the classes IRMAS structurally lacks, which is the
      entire reason to consult it there; everything else it said about isolated audio was
      noise. And the instrument-count bar depends on what the file *is*: a mix lists
      everything above 10%, a stem stays essentially one (30%).
      Captions before → after: `DRUMS_1` "drums, bass, guitar, percussion, synthesizer,
      piano, electric guitar" → **"drums"**; `5. HHB VOX_1` "voice, synthesizer" →
      **"voice"**; `BRASS_1` → **"brass, trumpet"**; `STRINGS_1` → **"strings, violin"**;
      `BASS_1` → **"bass, drums"**; `SFX_1` → no instrument claimed (honest).
- [ ] **Open: should a multi-segment stem skip whole-file classification entirely?**
      Single-segment stems no longer double-analyse (above). For stems with 2+ segments
      the file pass still runs its own heads. Removing it means the file's labels become
      an aggregate of its segments — and its similarity embedding would have to be
      aggregated too, which changes what Phase 4's search compares. Needs a decision.

**Instrument / analysis accuracy — after the UI work**

- [ ] Vocal loop reads "synthesizer" (id 1220). `voice_instrumental.voice_probability`
      is 0.72 and the caption already says `VocalType: Vocal`, but the instrument head's
      "voice" is only 7% (synthesizer 33%). The instrument field contradicts the caption.
      Let the voice head feed the instrument list.
- [ ] Brass/woodwind score stem reads "acoustic guitar" (id 1582). IRMAS top-1 is guitar
      at 18%, but the wind classes together total 21% (sax 8, trumpet 7, clarinet 4,
      flute 2). Aggregate to instrument families before taking a top label. IRMAS also
      has no brass-section or synth class at all.
- [ ] Whole-file averaging is structurally wrong for long stems. The 37-minute brass stem
      is 35% active across 36 spans with different content, and one averaged label
      can't describe it. Run instrument analysis per active span or segment and tag the
      segments; Phase 4's `segment_analysis` table already exists for this.
- [ ] Evaluate better instrument models, measured against ground truth mira already has
      for free: stem filenames (BRASS_HORNS, STRINGS-HIGH, VOCALS, GTR, PADS…). Leads to
      check: CED-small (the content gate) is AudioSet-trained, and AudioSet has Brass
      instrument / Woodwind / Singing / Synthesizer classes, so check whether
      `content_gate` already stores those scores. The CLAP zero-shot idea is under "Not
      scheduled" below.
- [ ] BPM octave error: the DKP_70 loop analyzed as 146 (beat_this beats ~0.4s apart);
      its filename token says 70. Reconcile half/double time against an explicit
      filename BPM token.

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
- [ ] One-time observed crash at process exit during Phase 4 per-dimension-similarity
      testing: `libc++abi: terminating due to uncaught exception of type
      std::__1::system_error: recursive_mutex lock failed` — printed *after*
      "analyzed N files" and after all DB writes had already completed (confirmed via
      `mira stats` on the same DB immediately after: all 294 rows present, correct
      counts). Looks like a load-dependent teardown race in one of the vendored
      libraries' own static/thread-pool state (ONNX Runtime and Essentia both keep
      process-global state — OrtEnv.h documents one such prior bug in this exact area)
      rather than anything in the per-dimension-similarity code itself, which is
      synchronous DSP math and plain SQL writes with no threads of its own.

      **Stress-tested, did not reproduce**: 5x `mira analyze --force` back-to-back on the
      original 294-file Dark Pop library, then 5x more (1 plain + 4 `--force`) on a
      different, larger 304-file library (`Black Octopus Sound - Futuretone Drum & Bass
      Mayhem`) — 10 full analyze runs total, all exit code 0, no repeat. Given it won't
      reproduce under repeated stress on two different libraries, not chasing further
      without a real repro — flagged and left here for if it ever recurs with more
      information (a specific file, a specific run condition) to go on.
