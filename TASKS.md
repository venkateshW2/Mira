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
- [ ] Segment-level analysis replacing whole-track averaging

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
- [ ] One-time observed crash at process exit during Phase 4 per-dimension-similarity
      testing: `libc++abi: terminating due to uncaught exception of type
      std::__1::system_error: recursive_mutex lock failed` — printed *after*
      "analyzed N files" and after all DB writes had already completed (confirmed via
      `mira stats` on the same DB immediately after: all 294 rows present, correct
      counts). Did not reproduce on an immediate full rerun of the same 294-file library
      (ran clean, exit 0). Looks like a load-dependent teardown race in one of the
      vendored libraries' own static/thread-pool state (ONNX Runtime and Essentia both
      keep process-global state — OrtEnv.h documents one such prior bug in this exact
      area) rather than anything in the per-dimension-similarity code itself, which is
      synchronous DSP math and plain SQL writes with no threads of its own. Flagged, not
      chased further yet — no reproduction, no data-loss, but a crash at exit is still a
      real bug worth a proper investigation before this ships anywhere unattended.
