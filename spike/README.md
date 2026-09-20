# Phase 0 spikes (PRD §9)

Each subdirectory is a standalone CMake project proving one risky assumption before
anything is built on top of it. Pass/fail only — not app code. All four spikes below
have passed as of 2026-09-10; nothing in PRD §10's risk table's mitigations were needed.

## 01_essentia_link — day 1-2, PASSED

Proves: Essentia C++ builds arm64, `--no-tensorflow`, static, and links from an
**external** CMake project (not the waf build tree itself) — the highest-risk item in §10.

```bash
cd ../../vendor/essentia
python3 waf configure --build-static --std=c++17 --fft=FFTW
python3 waf build -j8

cd ../../spike/01_essentia_link
cmake -S . -B build -G Ninja && cmake --build build
./build/spike_essentia_link <path/to/audio.wav>
```

```
loaded 627481 samples (14.2286 s)
bpm: 117.826  confidence: 1.22311
integrated loudness: -18.6326 LUFS
loudness range: 7.9506 LU
SPIKE OK — Essentia links and runs from an external CMake project.
```

**One fix required to build against current Homebrew ffmpeg (9.0.1):**
`src/essentia/utils/audiocontext.cpp` read `AVCodec::sample_fmts` directly — a field
FFmpeg removed in the 7.1 API migration (`avcodec_get_supported_config()` replaces it).
Essentia's `master` branch (cloned 2026-09-10) hadn't picked up the migration yet. Patched
in `vendor/essentia` (gitignored — vendor/ is fetched, not committed; re-apply if
re-cloning against a modern ffmpeg, or check whether upstream has since fixed it). Only
affects `AudioWriter`/`MonoWriter` (encoding) — irrelevant to mira's read-only pipeline,
but it's in the same translation unit as the loader so the whole static lib failed to link
without it.

## 02_onnx_parity — day 3, PASSED, exact match

Proves: the mel frontend (`FrameCutter` + `TensorflowInputMusiCNN`, params read directly
from `tensorflowpredicteffnetdiscogs.h` since that composite algorithm itself needs
`TensorflowPredict`/libtensorflow, unavailable in the no-TF build) plus ONNX Runtime C++
inference against `discogs-effnet-bsdynamic-1.onnx` agree with the Python reference to
within the PRD's ~1e-4 exit criterion.

```bash
# Python reference
cd ../../lab
uv run python3 parity_reference.py <audio.wav> /tmp/parity_py

# C++ side
cd ../spike/02_onnx_parity
cmake -S . -B build -G Ninja && cmake --build build
./build/spike_onnx_parity <audio.wav> ../../models/feature-extractors/discogs-effnet/discogs-effnet-bsdynamic-1.onnx /tmp/parity_cpp

# diff
cd ../../lab
uv run python3 compare_parity.py /tmp/parity_py /tmp/parity_cpp
```

Result on `flamenco.wav` (13 patches × 128 × 96 mel bands, 13 × 1280 embeddings):

```
mel patches: shape=(13, 128, 96)  max_abs=0  mean_abs=0  max_rel=0
embeddings : shape=(13, 1280)  max_abs=0  mean_abs=0  max_rel=0
PARITY OK — Python and C++ agree to < 1e-4
```

**Exact bitwise match**, not just within tolerance — same underlying Essentia C++ code and
the same ONNX Runtime 1.29.0 on CPU, deterministic on both sides.

Models downloaded from `essentia.upf.edu/models/` into `models/` (gitignored):
`feature-extractors/discogs-effnet/discogs-effnet-bsdynamic-1.{onnx,json}` (18 MB) and
`classification-heads/mtg_jamendo_moodtheme/mtg_jamendo_moodtheme-discogs-effnet-1.{onnx,json}`
(2.7 MB) — sizes match PRD §16.3 exactly.

ONNX Runtime: official prebuilt `onnxruntime-osx-arm64-1.29.0.tgz` extracted into
`vendor/onnxruntime/` (not Homebrew's 1.22.1 formula — PRD §16.4 prefers the official
package, and this also keeps the Python (`lab/`, onnxruntime 1.29.0) and C++ sides on the
same ORT version, which matters for a parity check).

## 03_dragout — day 4, PASSED

Proves: a minimal JUCE 9.0.2 app builds a real macOS `.app` bundle, and
`DragAndDropContainer::shouldDropFilesWhenDraggedExternally` compiles and links —
`onnx/beat_this_cpp`-adjacent GUI risk, founding requirement per PRD §2d.

```bash
cd ../../spike/03_dragout
cmake -S . -B build -G Ninja && cmake --build build
open "build/spike_dragout_artefacts/RelWithDebInfo/mira drag-out spike.app"
```

Built and launched cleanly (JUCE 9.0.2, cloned 2026-09-10). A window with a single
"drag me" tile opens; dragging it out drops `fixtures/flamenco.wav` onto whatever it's
released over (Finder, Ableton, Logic), copy not move (PRD §1: "no file ever moves").
**Manually confirmed working 2026-09-10** — the OS-level drag-out (NSDraggingItem under
the hood) lands a real file. This was the founding requirement most likely to force a
different GUI toolkit (PRD §2d) — it didn't.

Also bundles the day-5 `AudioThumbnailCache` persistence check (see below) as a
`--selftest` flag on the same binary, since it needed `juce_audio_utils` anyway.

## 04_sqlite_vec — day 5, PASSED

Proves: SQLite 3.53.4 amalgamation (vendored — PRD §7: never link macOS's `libsqlite3`,
it's built `SQLITE_OMIT_LOAD_EXTENSION`) + `sqlite-vec`, both statically linked into one
binary, work with 10k synthetic 1280-dim vectors (mira's real embedding size, not the
512-dim figure PRD §3 originally measured before the bsdynamic-embedding correction).

```bash
cd ../../spike/04_sqlite_vec
cmake -S . -B build -G Ninja && cmake --build build
./build/spike_sqlite_vec
```

```
inserting 10000 synthetic 1280-dim vectors...
insert: 357.383 ms (0.0357383 ms/vector)
top-20 query: 13.3135 ms, 20 results
closest match: rowid=5000 distance=0 (expected rowid=5000, distance=0)
SPIKE OK — sqlite-vec statically linked, KNN over 10k x 1280-dim vectors works.
```

13.3 ms for a top-20 KNN over 10k × 1280-dim — slower than PRD §3's original pure-numpy
figure (that measured raw brute-force cosine, not going through SQLite's virtual-table
machinery) but still well within interactive range. Confirms "no ANN index needed" holds
with the real embedding dimension, not just in the original 512-dim estimate.

**One source bug found and fixed in `beat_this_cpp`** (used for the day-5 `beat_this_cpp`
check, run separately — see below): `Source/beat_this_api.h` declared
`explicit BeatThis(const std::string&)` but `beat_this_api.cpp` defined
`BeatThis::BeatThis(const std::string&, bool use_dbn)` — a genuine header/impl mismatch,
not an ORT or platform issue. Fixed by adding the missing parameter (default `true`) to
the header declaration. Matches PRD §16.5's warning: "no CI, no releases... expect this
class of thing."

**Also: the official ORT tarball's CMake package doesn't match its own layout.**
`beat_this_cpp`'s `USE_SYSTEM_ONNXRUNTIME` path called `find_package(onnxruntime)`, whose
generated `onnxruntimeConfig.cmake` expects headers under `include/onnxruntime/` — but the
prebuilt `onnxruntime-osx-arm64-1.29.0.tgz` puts them directly under `include/`. Patched
`beat_this_cpp/CMakeLists.txt` to point at `-DONNXRUNTIME_ROOT=<path>` directly instead of
going through `find_package`. Same category of "nobody has run this exact combination
before" as the essentia and beat_this_cpp fixes above.

### `beat_this_cpp` — built and run standalone (not a CMake-driven spike subdirectory)

```bash
cd ../../vendor/beat_this_cpp
git submodule update --init Submodule/pocketfft Submodule/r8brain
git config -f .gitmodules submodule.Submodule/miniaudio.url https://github.com/mackron/miniaudio.git
git submodule update --init Submodule/miniaudio
cmake -S . -B build -G Ninja -DUSE_SYSTEM_ONNXRUNTIME=ON -DONNXRUNTIME_ROOT=../onnxruntime
cmake --build build
./build/beat_this_cpp onnx/beat_this.onnx <audio.wav> --calc-bpm
```

```
Loaded audio: 1254962 samples, 44100 Hz, 2 channels
Found 20 beats and 5 downbeats
Estimated BPM: 83.3
```

1.4s wall time for a 14.2s clip (~10x realtime). **Notably disagrees with Essentia's
RhythmExtractor2013 on the same file** (117.8 BPM vs 83.3 BPM) — this is *expected* per
PRD §5/§14.1: the two estimators' disagreement is itself the confidence signal, not a bug
to resolve by picking one.

### `AudioThumbnailCache` persistence — day 5, PASSED

Bundled into `03_dragout`'s binary as a `--selftest` flag (no separate CMake target
needed — it already links `juce_audio_utils`):

```bash
"spike/03_dragout/build/spike_dragout_artefacts/RelWithDebInfo/mira drag-out spike.app/Contents/MacOS/mira drag-out spike" --selftest
```

```
pass 1: scanned flamenco.wav, wrote cache (19688 bytes) to <tmp>/mira_thumbnail_cache_spike.dat
pass 2: fresh AudioThumbnailCache loaded from disk, thumbnail for flamenco.wav fully loaded within 500ms (cache hit, no full re-scan)
SPIKE OK — AudioThumbnailCache survives a relaunch.
```

Scans the fixture into a cache, writes the cache to a stream, then — with a **fresh**
`AudioThumbnailCache` instance, simulating a relaunch — reads it back and confirms the
thumbnail is immediately fully loaded (cache hit) rather than re-scanning the audio. This
is what matters at library scale (PRD §13: "the cache persists... which matters at 10k
files").

## Summary

All five day 1-5 exit criteria from PRD §9 are met, including the manual one:

| Day | Criterion | Result |
|---|---|---|
| 1-2 | Essentia C++ builds + links externally | ✅ PASS |
| 3 | ONNX embedding parity to ~1e-4 | ✅ PASS (exact match) |
| 4 | Drag-out builds/links, and the drag itself works | ✅ PASS (manually confirmed 2026-09-10) |
| 5 | `beat_this_cpp` on arm64 | ✅ PASS |
| 5 | SQLite + `sqlite-vec`, 10k × 1280-dim | ✅ PASS |
| 5 | `AudioThumbnailCache` survives relaunch | ✅ PASS |

**Phase 0 is complete.** Every item in PRD §10's risk table that Phase 0 was meant to
retire has been retired; none of the documented fallbacks were needed.

## 06_embedded_python — 2026-09-13, PASSED

Added after Phase 0, on the same pass/fail basis. Proves a standalone CPython + MLX can be
embedded in a signed `.app`, reach the GPU as a child process, and keep a valid code
signature — the unknown blocking [PACKAGING.md](../sa3-studio/PACKAGING.md) option B.

```bash
cd 06_embedded_python && ./build.sh
```

Needs no entitlements and no weakening of library validation. Found one trap: a bundled
interpreter writes `__pycache__` into the sealed bundle on first run and breaks its own
signature — fixed with `PYTHONDONTWRITEBYTECODE=1`, with a regression guard in `build.sh`.

Ad-hoc signed; real Developer ID notarisation is still untested (0 signing identities on
this machine). See [06_embedded_python/README.md](06_embedded_python/README.md).

Three real bugs found and fixed along the way (Essentia/ffmpeg 7.1 API removal,
`beat_this_cpp` header/impl mismatch, `beat_this_cpp`'s broken `find_package(onnxruntime)`
path) — all in vendored, gitignored code, documented above so they can be reapplied or
checked against upstream on a fresh clone.

## 08_stretch_latency — MIRA-BLOCKS.md step 3.1 (2026-09-20)

**Does `signalsmith-stretch` do what step 3 needs, and what does it cost in latency?**

The question that mattered was never "does it stretch". It was: a stretched take that comes
back late is a take that lands behind the grid it was stretched TO, which is the one failure
step 3.4 exists to prevent. At 44.1 kHz with `presetDefault` the answer is **120 ms at ratio
1.0** — about a third of a beat at 143 bpm — and it would have shipped invisibly.

The delay is **`inputLatency() * ratio + outputLatency()`**, exactly (both are 2646 samples).
Predicted against measured over five ratios, agreeing to within 0.5 ms — the residual is the
test click's own width. So the compensation is arithmetic, not a search.

Also settled here: **`signalsmith-linear` is a separate clone, not a submodule.** A
`--recursive` clone of signalsmith-stretch pulls only its demo tool and still leaves
`#include "signalsmith-linear/stft.h"` unresolvable. Found by compiling it, which was the
only way that was ever going to surface. And `SIGNALSMITH_USE_ACCELERATE` changes the output
not at all (identical to the sample across all five ratios) — it is a speed flag.
