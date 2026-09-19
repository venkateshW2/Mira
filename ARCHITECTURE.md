# ARCHITECTURE.md — how mira is put together

**Written 2026-09-19.** What the pieces are, how they talk to each other, and where audio
actually flows. Read [CLAUDE.md](CLAUDE.md) first for the map of every document; read this
one when you need to know *why the code is shaped this way* before changing it.

Every number here was checked against the tree or the live library on the date above. If
you change the shape of something, change this file in the same commit — a stale
architecture document is worse than none, because it is believed.

---

## 1. What mira is

Three things that share one library and one audio device:

| | what it does | lives in |
|---|---|---|
| **analyse** | listens to audio and writes down what it hears | `src/mira/analyze/` |
| **caption** | turns those measurements into words a model was trained on | `src/mira/caption/` |
| **generate + arrange** | prompts SA3, and arranges what comes back | `src/mira_ui/Source/`, `sa3-studio/` |

It runs entirely locally on Apple Silicon. No server, no cloud, no GPU assumption. The
library today holds **3,228 scanned files, 1,390 analysed**.

There are two binaries over one database:

- **`mira`** — the CLI (`src/mira/main.cpp`): `scan`, `analyze`, `caption`, `tag`,
  `similar`, `search`, `stats`.
- **`MIRA.app`** — the JUCE desktop app (`src/mira_ui/`).

**`mira_core`** is the static library both link. It holds `Database`, `CueDetection`,
`Groove`, `CaptionFields`, `Sa3Renderer`, `PathNormalise` — everything that is pure
arithmetic over stored JSON, with no audio decoding and no models. That boundary is the
reason the UI can run captioning inline instead of shelling out, and the reason the CLI and
the UI can never disagree about what a caption says.

---

## 2. The library

One SQLite file at `~/.mira/library.db`, WAL mode, with `sqlite-vec` for vector search.
`src/mira/db/Database.{h,cpp}` is the **only** surface that touches it.

### The tables that matter

```sql
files (id, path UNIQUE, sha256, mtime, size_bytes,
       content_type, content_type_source, group_id,
       active_ratio, active_spans,
       machine, human, provenance,          -- all three are JSON documents
       scanned_at, analyzed_at)

segments (id, group_id, file_id, start_seconds, end_seconds, human, source, created_at)
segment_analysis (segment_id, file_id, machine, analyzed_at)

ui_folder_roots  (id, path UNIQUE, added_at, scan_complete, display_name, group_id)
ui_folder_groups (id, name, added_at, category)
ui_collections / ui_collection_files
ui_settings (key, value)                     -- current_project, recent_projects,
                                             -- audio_device_state, lora_names, …
```

Four vector tables, each a `vec0` virtual table:

| table | dims | what it is |
|---|---|---|
| `vec_embeddings` | 1280 | Discogs-EffNet penultimate layer — "sounds like" |
| `vec_embeddings_dclap` | 512 | DCLAP joint audio/text space — search by description |
| `vec_timbre` | 13 | MFCC means — cheap timbral neighbours |
| `vec_spectrum` | 2 | centroid + rolloff — coarse brightness |

### Three rules the schema encodes

1. **`machine` is measured, `human` is asserted, and `human` always wins** (PRD §11). A
   re-analysis never overwrites a human field. This is why re-running analysis is always
   safe.
2. **Store everything, caption selectively** (convention 3). Raw numbers go into
   `files.machine` whether or not any word is derived from them. `onset_times` being stored
   is why the groove fix cost no re-analysis of 94 files.
3. **Never compare two paths with `==`** (convention 9). macOS hands back the same filename
   as different bytes depending on which API asked — `readdir` gives NFD, JUCE's directory
   walk gives NFC. Go through `mira::pathsEquivalent` / `Database::findByPath`
   ([PathNormalise.h](src/mira/db/PathNormalise.h)), which tries the exact bytes first and
   the other normalisation only on a miss. This hid for months and cost most of a day.

---

## 3. Analysis: audio in, JSON out

```
file on disk
   │
   ├─ mira::scan          (src/mira/scan/) — walk, hash, stat, insert a files row
   │
   └─ mira::analyze       (src/mira/main.cpp::runAnalyze)
         │
         ├─ AudioLoader   decode → mono + left + right float vectors at one rate
         ├─ ContentGate   CED-small: is this even audio worth measuring?
         ├─ ActiveRegions silence detection → active_spans, active_ratio
         ├─ Router        one-shot / loop / track / stem — decides what runs below
         │
         ├─ Descriptors   loudness, dynamics, spectral shape, MFCCs
         ├─ Mir           beat_this_cpp → beats, downbeats, grid stability
         ├─ Onsets        mira's own STFT onset detector (NOT Essentia's)
         ├─ Groove        swing / pocket / syncopation over those onsets
         ├─ Meter         bar length and bar spread
         ├─ Key           libkeyfinder
         ├─ Embedding     Discogs-EffNet 1280-d
         ├─ Genre / MoodTheme / Instrument / VoiceInstrumental / Danceability
         │                   — classification heads over that same embedding
         ├─ StemInstrument   for stems only
         ├─ Chords        opt-in (Chordino, +15 s/file)
         ├─ Transcription opt-in (Basic Pitch, +4 s/file)
         └─ DclapEmbedding opt-in (+27% per file)
                │
                └─→ files.machine (JSON) + the four vector tables
```

Models live in `models/` as ONNX and are loaded through `OrtEnv`:

```
feature-extractors/discogs-effnet/discogs-effnet-bsdynamic-1.onnx
classification-heads/{genre_discogs400, mtg_jamendo_moodtheme, mtg_jamendo_instrument,
                      irmas-predominant-instrument, voice_instrumental, danceability}
content-gate/ced-small
similarity-embeddings/dclap/{model_epoch_36, clap_text_model}
```

Everything else is vendored C++ in `vendor/`: `essentia`, `beat_this_cpp`,
`basicpitch.cpp`, `libkeyfinder`, `nnls-chroma`, `kaldi-native-fbank`, `kissfft`,
`onnxruntime`, `sqlite-vec`, `SQLiteCpp`, `JUCE`.

### Rules this pipeline encodes

- **Omit rather than guess** (convention 1). Every caption field has a confidence gate. An
  omitted field is the system working, not failing.
- **Never silently fall back** (convention 6). A failed model returns null *plus a recorded
  error*, never a substitute value. A heuristic that returned `1.0` on failure was
  indistinguishable from a real answer.
- **Measure the corpus, then set thresholds** (convention 2). Every bucket boundary in
  `CaptionFields.h` is a measured p33/p66 with the distribution in the comment above it.
- **Reject a second measurement of the same axis** (convention 7). `spectral_centroid`
  (r=0.91 with flatness), tempo drift (r=0.79 with jitter) and `flux_stddev` (r=0.71 with
  `flux_mean`) are all measured and all deliberately uncaptioned.
- **Segments only when they say something the file does not.** A single segment covering
  ≥95% of the file is not created at all; segment analysis runs only when there are ≥2.

Detail on every field: [ANALYSIS.md](ANALYSIS.md).

---

## 4. Captioning

`src/mira/caption/` is two steps, deliberately separate:

```
files.machine + files.human
        │
        ├─ CaptionFields   measurement → WORDS      (thresholds, gates, vocabularies)
        └─ Sa3Renderer     words → "trigger, Key: value, Key: value, …"
```

`CaptionFields` decides *what is true*. `Sa3Renderer` decides *how SA3 wants to hear it*.
Keeping them apart is what let the prompt format change without re-deriving any word.

The four human-only vocabularies (Material, World, Harmonic language, Signature) are fixed
lists in `TagVocabulary.h`, shared with `mira tag-folder` so the UI and the CLI cannot
offer different words. See [CAPTION-TAGGING.md](CAPTION-TAGGING.md).

**A caption word is only useful if it varies across the corpus** (convention 4). A word
every file carries is absorbed into the trigger and cannot be turned up or off at
generation time.

---

## 5. The app

`src/mira_ui/Source/`, ~27,000 lines. The big ones:

| file | lines | what it is |
|---|---|---|
| `Main.cpp` | 5,121 | app, main window, menu bar, window lifetimes, the shared audio device |
| `CanvasWindow.{h,cpp}` | 4,061 | the canvas — blocks, tracks, mixer, document, export |
| `GenerateWindow.{h,cpp}` | 3,750 | the generate pane: prompt, LoRAs, settings, takes |
| `WaveformView.{h,cpp}` | 2,708 | waveform, rulers, onsets, grids, selection |
| `FileTable.{h,cpp}` | 1,561 | the library table |
| `FolderTreeView.{h,cpp}` | 1,181 | roots, groups, projects |
| `CanvasEngine.{h,cpp}` | 773 | **the canvas mixer** — see §6 |
| `Export.{h,cpp}` | — | the only code that renders a take to a file |
| `Sa3Worker.{h,cpp}`, `Sa3WorkerHub.h` | — | the bridge to Python |

### Windows and their lifetimes

- **The canvas IS the project.** `File → New/Open Project` opens a canvas, not the old
  project window. A project is a `.mira` document in a folder of block folders.
- **The library window** can be closed without quitting; `Window → Library` brings it back.
  Closing it used to quit the app outright, killing a running generation.
- **The take pool** (`Window → Take Pool (generator v1)`) is the older per-project
  inference window, kept for the cue list and bulk take triage the canvas does not do.
- **One `AudioDeviceManager` for the whole app** (`MainComponent::sharedAudioDevice`),
  persisted in `ui_settings.audio_device_state`. Every window that makes sound joins it.
  A device per window meant a device per take preview.
- **One SA3 worker for the whole app** (`Sa3WorkerHub`). A worker holds the DiT and the
  decoder; a 30-second generation peaked at 11 GB on a 16 GB machine. Two windows each
  owning one would be two processes swapping against each other.

---

## 6. Audio flow — the part to get right

There are **four** paths audio takes through mira. They are deliberately different, and
confusing them has caused real bugs.

### 6.1 Analysis (offline, no device)

`AudioLoader` decodes the whole file into `std::vector<float>` at one rate and hands it to
the stages. **No `AudioDeviceManager`, no transport, no resampler.** Analysis never touches
the playback path.

### 6.2 Take preview (`WaveformView`, `TakeStack`)

One `WaveformView` moved between rows rather than one per row — it owns a transport, so a
waveform per take would be an audio device per take. Plays through the shared device via
`AudioTransportSource`, which resamples to the device rate through
`juce::ResamplingAudioSource`.

### 6.3 The canvas mixer (`CanvasEngine.{h,cpp}`)

This is the one worth reading carefully.

```
Block[]  (the document: file, lane, start, length, sourceOffset, contentSeconds,
          gainDb, fadeIn/fadeOut, fadeShape, muted, colour, name)
   │
   │  CanvasPlayer::rebuild()          — MESSAGE THREAD ONLY
   │    · opens readers (cached by path across rebuilds)
   │    · clamps each block to what actually SOUNDS
   │        sounding = min(contentSeconds or ∞, fileLength − sourceOffset)
   │        length   = min(block.length, sounding)
   │    · computes same-lane crossfades from the overlap
   │    · resolves everything to sample positions at 44,100
   ▼
Arrangement  (ReferenceCountedObject, IMMUTABLE once published)
   │   Voice[] { reader, lane, startSample, lengthSamples, sourceStartSample,
   │             fadeIn/OutSamples, fadeIn/OutShape, gain, rateRatio }
   ▼
CanvasAudioSource   — sums every voice overlapping the position
   │   · mute/solo as atomic BITMASKS (no rebuild to mute a lane)
   │   · per-lane gain, atomic
   │   · per-lane and master peak, per channel
   │   · per-voice Catmull-Rom resampling when rateRatio ≠ 1
   ▼
BufferingAudioSource   (TimeSliceThread "canvas file reader")
   ▼
AudioTransportSource   — resamples 44,100 → device rate
   ▼
AudioSourcePlayer → the shared AudioDeviceManager
```

**The timeline is always 44,100.** SA3 generates at 44.1 and nothing else, so the
arrangement is built at 44.1 regardless of what the device opened at, and the transport
does the one conversion. Building against the device rate meant sample positions and loop
points could disagree the moment the device changed. The toolbar says `44.1k → 48.0k` when
they differ, because an unadmitted resample is what makes "the canvas sounds different from
the preview" feel like a fault instead of a conversion.

Four bugs this path has already produced, all from one misunderstanding — that
`BufferingAudioSource`, not the source, owns the read position:

- **Every voice silently skipped.** It fills its buffer in chunks far larger than one device
  block (44,100 at a time) while the scratch buffer was `blockSize + 8`. Near-silence, with
  the odd small block getting through. Rendering is chunked now, and the skip is a
  `jassert`, not a `continue`.
- **Looping restarted every chunk.** It calls `setNextReadPosition(P)` with *linear*
  positions before every read, so an internal rewind at the out point restarted every chunk
  at the in point. Looping is a pure mapping now, as `AudioFormatReaderSource` does it.
- **The meter kept counting after a stop.** It pre-fills whether or not the transport runs,
  so the source kept producing peaks with nothing playing. The meter reads only while the
  transport is running.
- **Non-44.1 files played 8.8% slow.** `rateRatio` offset the read and then read *n*
  consecutive samples. Per-voice Catmull-Rom resampling now.

### 6.4 Export

Two renderers, for two different things:

| | what it renders | where |
|---|---|---|
| `mira::ui::renderTake` | one take + its stored trim/fade/gain | `Export.cpp` |
| `CanvasView::renderToFile` | the canvas, through its own mixer | `CanvasWindow.cpp` |

The canvas one calls `CanvasAudioSource::renderOffline` on the calling thread, so **the file
and the speakers come out of one mixer** — crossfades, fade shapes, per-block gain, mute,
solo and the resampler included. A second implementation would drift from the first the
moment either changed.

- **A track export is a solo**, using the lane mask the mixer already honours, restored by a
  scope guard on every path. A solo left latched after an export would silence the canvas
  and look like a playback bug.
- **Stems line up at zero.** "Export every track" renders each lane from `0` to
  `contentEnd()`, so every file is the same length and dropping them into a DAW at 0:00
  reproduces the arrangement exactly.
- **Native rate, always.** Export reads the source's own rate and writes that rate. It must
  never route through the playback path — `ResamplingAudioSource` is an interpolator plus a
  simple IIR low-pass, fine for auditioning, not a mastering SRC.

---

## 7. Generation — the bridge to Python

```
GenerateContent  (the prompt, LoRAs, settings, the buttons)
   │  JSON request over stdin
   ▼
Sa3Worker  (src/mira_ui/Source/Sa3Worker.cpp)
   │  one long-lived child process, replies matched by id
   ▼
sa3-studio/sa3_worker.py
   │  delegates sampling to …
   ▼
sa3-studio/stable-audio-3/optimized/mlx/scripts/sa3_gradio.py → sa3_mlx.py
   │  MLX on the GPU
   ▼
a .wav beside a .json recipe, in the block's folder
```

The worker is started on first use, not at launch: it costs a Python process and, on its
first generate, a ~44 s model load. An app opened to organise a library should pay neither.

### What the request carries

`prompt`, `negative_prompt`, `cfg`, `seconds`, `steps`, `seed`, `out`, `loras[]`
(`path`, `strength`, `steps` as a **2-tuple**, not a `"1-8"` string), and for audio input
either `init_audio` (audio-to-audio) or `inpaint_audio` + `inpaint_range`.

Every take gets a **`.json` sidecar** recording exactly that. It exists because this
information otherwise lives only in the log and dies with the session — and a generation you
cannot reproduce is a generation you cannot learn from.

### Three facts about SA3 that shape the UI

1. **The duration is a conditioning input, not a buffer size.**
   `secs_embedder(args.seconds)` goes into `cross_attn` *and* `global_cond`, and
   `apply_conditioner_lora` applies the LoRA's own delta to that same embedder. The model is
   *told* how long a piece to make, and the LoRA has opinions about it.

2. **LoRAs are trained on crops.** At `SAMPLES_PER_LATENT = 4096` and 44.1 kHz, a
   512-latent crop is **47.6 s** and a 320-latent crop is **29.7 s**. Ask far past that and
   the model runs out and pads with silence — measured across one project's 20 takes, single
   LoRAs filled 96–100% of their length up to 111 s while a 512+320 pair filled 79% at 84 s,
   72% at 136 s and **55% at 166 s**.

3. **Inpainting past the end is extension.** Init audio is zero-padded to the requested
   duration, so a range past the end of the audio generates a continuation.
   **But the kept region is not bit-exact.** `paste_back` preserves *latents*; the whole
   timeline is then decoded, and the source had to be encoded first, so the kept region takes
   a lossy round trip — −21.9 dB relative error after one extension, **−14.5 dB after three**.
   mira therefore never trusts the model to preserve anything: it writes the original samples
   back itself.

Everything the model does with a request: [sa3-studio/SA3-INFERENCE-AND-TRAINING.md](sa3-studio/SA3-INFERENCE-AND-TRAINING.md).
Everything the canvas does with it: [CANVAS.md](CANVAS.md).

---

## 8. Threads

| thread | what runs on it | rule |
|---|---|---|
| message | all UI, `CanvasPlayer::rebuild`, arrangement publishing | never blocks on audio or models |
| audio device | `CanvasAudioSource::getNextAudioBlock` | no allocation, no file open, no lock |
| `TimeSliceThread` "canvas file reader" | `BufferingAudioSource` read-ahead | the only thread that reads take files during playback |
| analysis worker | `mira analyze` stages | owns its own Essentia init |
| `Sa3Worker` reader ×2 | the child's stdout (protocol) and stderr (log) | stdout is pure protocol; logs go to stderr |

The handoff from message thread to audio thread is a `ReferenceCountedObjectPtr<Arrangement>`
swap. The old arrangement goes into a `retired` array and is freed on the message thread —
never in the audio callback.

---

## 9. The ten conventions

These are not style preferences. Each exists because breaking it caused a real bug. Full
text with the stories in [CLAUDE.md](CLAUDE.md).

1. Omit rather than guess.
2. Measure the corpus, then set thresholds. Never guess first.
3. Store everything, caption selectively.
4. A caption word is only useful if it varies across the corpus.
5. `human` always outranks machine.
6. Never silently fall back.
7. Reject a second measurement of the same axis.
8. Open the UI before claiming it works.
9. Never compare two file paths with `==`.
10. Measure before explaining.

Two more this project keeps re-learning, written down here because they have each cost a
day:

11. **A claim verified by reading code is not verified.** "Everything outside an inpaint
    range stays bit-exact" was true of the *latents* and false of the *audio*, and it sat in
    two documents for two days because nobody measured the output.
12. **State that outlives its action is a bug waiting to happen.** One `Extend` set an
    `initAudio` field that nothing cleared, and every generation afterwards was silently
    guided by the old block's audio. Scope state to the action that wanted it.

---

## 10. Build

```bash
cmake -B build -DCMAKE_BUILD_TYPE=RelWithDebInfo     # ONNXRUNTIME_ROOT defaults to vendor/
cmake --build build -j8                              # both targets
cmake --build build --target mira_ui -j8             # app only
cmake --build build --target mira -j8                # CLI only
```

- App: `build/src/mira_ui/mira_ui_artefacts/RelWithDebInfo/mira.app`
- CLI: `build/src/mira`
- Library: `~/.mira/library.db`

**A running MIRA holds the old binary.** After a rebuild it must be relaunched to pick up
changes — check `pgrep -f "MacOS/MIRA"` before assuming a change did not work.
