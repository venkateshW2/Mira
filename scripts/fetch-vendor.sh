#!/usr/bin/env bash
# Fetches third-party C++ sources into vendor/ (gitignored — not committed).
# Re-run any time after a fresh clone. Each dependency is cloned shallow.
#
# Known local patches needed after a fresh clone (see spike/README.md for why):
#   - vendor/essentia/src/essentia/utils/audiocontext.cpp: FFmpeg 7.1+ removed
#     AVCodec::sample_fmts; patch to use avcodec_get_supported_config() instead.
#   - vendor/beat_this_cpp/Source/beat_this_api.h: constructor declaration missing
#     the `bool use_dbn` parameter that beat_this_api.cpp defines.
#   - vendor/beat_this_cpp/CMakeLists.txt: USE_SYSTEM_ONNXRUNTIME path needs
#     -DONNXRUNTIME_ROOT=<path>, not find_package(onnxruntime) (its CMake config
#     expects a different install layout than the official prebuilt tarball has).
#   - vendor/beat_this_cpp/Source/beat_this_api.cpp: BeatThis::Impl constructed its own
#     Ort::Env per instance (i.e. per file analyzed, every file by default — real crash
#     found once mira grew enough separate ONNX-backed analyzers that this Env churn
#     reliably corrupted memory; Ort::Env owns process-global state and repeated
#     construction/destruction within one process is not a safe pattern). Patched to a
#     function-local static Env (construct once for the process, never churn) instead of
#     a per-instance member — see the comment at that patch site for the full story.
#   - vendor/beat_this_cpp/Source/beat_this_api.{h,cpp}: added `process_audio_both()`,
#     which decodes BOTH postprocessors (madmom-compatible DBN and the minimal
#     peak-picker) from ONE model pass, plus the `decode_dbn`/`decode_minimal` helpers
#     and `Impl::logits` it is factored out of. mira needs both grids to run the
#     grid-stability cross-check (Mir.cpp): below 0.90 stability the minimal grid is
#     decoded too and the steadier one wins. Running the network twice to see both would
#     be pure waste. **Without this patch mira does not compile** -- Mir.cpp calls
#     process_audio_both directly.
#     Apply with:
#       git -C vendor/beat_this_cpp apply ../../scripts/vendor-patches/beat_this_cpp-process_audio_both.patch
# Check whether upstream has fixed any of these before re-patching.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
VENDOR="$ROOT/vendor"
mkdir -p "$VENDOR"
cd "$VENDOR"

clone_if_missing() {
  local name="$1" url="$2" ref="${3:-}"
  if [ -d "$name" ]; then
    echo "== $name already present, skipping (rm -rf vendor/$name to re-fetch) =="
    return
  fi
  echo "== cloning $name =="
  if [ -n "$ref" ]; then
    git clone --depth 1 --branch "$ref" "$url" "$name" || git clone --depth 1 "$url" "$name"
  else
    git clone --depth 1 "$url" "$name"
  fi
}

# MIRA-BLOCKS.md step 3 — signalsmith-stretch, the time-stretcher. MIT, header-only,
# and already trusted by the author. Needs --recursive: the repo carries a `cmd/util`
# submodule (only the command-line tool uses it, but a plain clone leaves the tree
# incomplete and the next person wondering why).
#
# mira includes ONLY `signalsmith-stretch.h` and `include/signalsmith-stretch/`. The
# `cmd/` and `web/` trees are the upstream demo apps and are not built.
if [ ! -f signalsmith-stretch/signalsmith-stretch.h ]; then
  echo "== cloning signalsmith-stretch =="
  git clone --depth 1 --recursive https://github.com/Signalsmith-Audio/signalsmith-stretch.git
else
  echo "== signalsmith-stretch already present, skipping =="
fi

# ...and the FFT library it includes, which is NOT a submodule of it. A --recursive clone
# of signalsmith-stretch pulls only `cmd/util` (its demo tool) and still leaves
# `#include "signalsmith-linear/stft.h"` unresolvable -- found by compiling it, which is
# the only way that fact was ever going to surface. MIT, header-only, same author.
if [ ! -f signalsmith-linear/stft.h ]; then
  echo "== cloning signalsmith-linear (signalsmith-stretch's FFT dependency) =="
  git clone --depth 1 https://github.com/Signalsmith-Audio/linear.git signalsmith-linear
else
  echo "== signalsmith-linear already present, skipping =="
fi

# Phase 0, day 1-2 — Essentia C++. Verified building arm64 --no-tensorflow static
# (spike/01_essentia_link). One local patch needed, see header comment above.
clone_if_missing essentia https://github.com/MTG/essentia.git

# Phase 0, day 3 — ONNX Runtime C++, official prebuilt (not Homebrew's formula —
# PRD §16.4 prefers it, and it keeps lab/'s Python ORT and the C++ side on the same
# version). Not a git clone: a release tarball.
if [ ! -f onnxruntime/lib/libonnxruntime.dylib ]; then
  echo "== fetching onnxruntime-osx-arm64-1.29.0 =="
  mkdir -p onnxruntime
  curl -sSL -o /tmp/ort.tgz "https://github.com/microsoft/onnxruntime/releases/download/v1.29.0/onnxruntime-osx-arm64-1.29.0.tgz"
  tar xzf /tmp/ort.tgz -C onnxruntime
  mv onnxruntime/onnxruntime-osx-arm64-1.29.0/* onnxruntime/
  rmdir onnxruntime/onnxruntime-osx-arm64-1.29.0
  rm /tmp/ort.tgz
else
  echo "== onnxruntime already present, skipping =="
fi

# Phase 0, day 4 — JUCE 9 (PRD §7, §13). Verified building + launching
# (spike/03_dragout); the actual OS drag gesture needs manual verification.
clone_if_missing JUCE https://github.com/juce-framework/JUCE.git 9.0.2

# Phase 0, day 5 — beat_this_cpp (PRD §7, §10 risk). Verified running arm64
# (spike/README.md). Needs its three submodules too, and one is SSH-only upstream.
clone_if_missing beat_this_cpp https://github.com/mosynthkey/beat_this_cpp.git
if [ -d beat_this_cpp ]; then
  cd beat_this_cpp
  git submodule update --init Submodule/pocketfft Submodule/r8brain
  git config -f .gitmodules submodule.Submodule/miniaudio.url https://github.com/mackron/miniaudio.git
  git submodule update --init Submodule/miniaudio
  cd "$VENDOR"
fi

# Phase 0, day 5 — sqlite-vec (PRD §6, §7). Verified with 10k x 1280-dim vectors
# (spike/04_sqlite_vec). Needs `make sqlite-vec.h` once (envsubst must be on PATH).
clone_if_missing sqlite-vec https://github.com/asg017/sqlite-vec.git
if [ -d sqlite-vec ] && [ ! -f sqlite-vec/sqlite-vec.h ]; then
  (cd sqlite-vec && make sqlite-vec.h)
fi

# Phase 0, day 5 — SQLite amalgamation, vendored (PRD §7: never link macOS's
# libsqlite3, it's built SQLITE_OMIT_LOAD_EXTENSION). Not a git clone: a source zip.
if [ ! -f sqlite-amalgamation/sqlite3.c ]; then
  echo "== fetching sqlite-amalgamation-3530400 (3.53.4) =="
  mkdir -p sqlite-amalgamation
  curl -sSL -o /tmp/sqlite.zip "https://www.sqlite.org/2026/sqlite-amalgamation-3530400.zip"
  unzip -q /tmp/sqlite.zip -d /tmp
  mv /tmp/sqlite-amalgamation-3530400/* sqlite-amalgamation/
  rmdir /tmp/sqlite-amalgamation-3530400
  rm /tmp/sqlite.zip
else
  echo "== sqlite-amalgamation already present, skipping =="
fi

# Phase 1 — key (PRD §12b). Verified against a real fixture (spike/README.md-style
# check, see TASKS.md) — gated on harmonic content via a spectral-flatness proxy.
clone_if_missing libkeyfinder https://github.com/mixxxdj/libkeyfinder.git

# Phase 1 — chords (PRD §7, §12b). nnls-chroma has no CMakeLists.txt (plain Makefile
# project); built directly in src/CMakeLists.txt. Needs vamp-plugin-sdk (below) too —
# ohollo/chord-extractor, an earlier guess at this dependency, is a Python wrapper, not
# usable from C++; the actual source is Mauch & Dixon's own repo.
clone_if_missing nnls-chroma https://github.com/c4dm/nnls-chroma.git

# Chordino's own Vamp::Plugin interface needs vamp-hostsdk's PluginInputDomainAdapter to
# get the FFT framing right (it declares FrequencyDomain input, not TimeDomain) and
# PluginBufferingAdapter for block-size negotiation — essentia's vendored copy only has
# the plugin-side SDK, not the host-side adapters, so this is a separate, complete copy.
# One local patch needed: its CMakeLists.txt has an example-plugins properties block
# outside the option(VAMPSDK_BUILD_EXAMPLE_PLUGINS) guard that's meant to contain it —
# harmless with the option OFF (default) until CMake actually reaches that dangling
# block, which it does unconditionally as written. Re-apply after a fresh clone, or
# check whether upstream has fixed it.
clone_if_missing vamp-plugin-sdk https://github.com/c4dm/vamp-plugin-sdk.git

clone_if_missing SQLiteCpp https://github.com/SRombauts/SQLiteCpp.git

# Phase 1 — note transcription (PRD §5, §12b). The nmp.onnx model (230,444 bytes,
# matching PRD §12b exactly) is committed in-tree in this repo at ort-model/model.onnx,
# not fetched separately. src/mira/analyze/BasicPitch{Ort,Notes}.cpp and Transcription.cpp
# adapt this repo's src/ort_inference.cpp and src/midi_notes.cpp (MIT) — see those files'
# header comments for exactly what changed and why (mostly: load the model from a file
# path instead of an embedded byte array, and drop the MIDI-serialization half, which
# needs libremidi, since mira stores note events directly).
clone_if_missing basicpitch.cpp https://github.com/sevagh/basicpitch.cpp.git

# Phase 2 — classification (PRD §2c). kaldi-native-fbank (Apache-2.0) is the exact
# feature-extraction library sherpa-onnx itself links against for CED-small's content
# gate — its frontend is kaldi-style fbank, not Essentia's TensorflowInputMusiCNN (that's
# discogs-effnet's frontend, a different model). Vendored rather than hand-replicating
# kaldi's fbank math; see src/mira/analyze/ContentGate.cpp for the exact FbankOptions,
# read directly from sherpa-onnx's own offline-stream.cc.
clone_if_missing kaldi-native-fbank https://github.com/csukuangfj/kaldi-native-fbank.git

# kaldi-native-fbank's rfft.cc needs kissfft (BSD-3-Clause) — upstream's own CMake
# FetchContent-downloads a pinned commit archive; vendored directly instead (same
# hand-picked-sources pattern as nnls-chroma), so latest-tag HEAD instead of that exact
# pinned commit, which is fine since kiss_fft.c/kiss_fftr.c are stable, rarely-changed code.
clone_if_missing kissfft https://github.com/mborgerding/kissfft.git

# Phase 2 — model weights (PRD §2c, §16.3), gitignored, into repo-root models/ (not
# vendor/ — this is the location spike/02_onnx_parity already used and proved bitwise
# parity against). Not git clones: direct downloads from the model's own host.
MODELS="$ROOT/models"
mkdir -p "$MODELS/feature-extractors/discogs-effnet" \
         "$MODELS/classification-heads/mtg_jamendo_moodtheme" \
         "$MODELS/classification-heads/mtg_jamendo_instrument" \
         "$MODELS/classification-heads/danceability" \
         "$MODELS/content-gate/ced-small"

fetch_if_missing() {
  local dest="$1" url="$2"
  if [ -f "$dest" ]; then
    echo "== $(basename "$dest") already present, skipping =="
    return
  fi
  echo "== fetching $(basename "$dest") =="
  curl -sSL -o "$dest" "$url"
}

# discogs-effnet-bsdynamic-1 — the embedding, mandatory input to every classification
# head (PRD §2c). Bitwise-parity-verified against the Python reference (spike/02_onnx_parity).
EFFNET_BASE="https://essentia.upf.edu/models/feature-extractors/discogs-effnet"
fetch_if_missing "$MODELS/feature-extractors/discogs-effnet/discogs-effnet-bsdynamic-1.onnx" \
  "$EFFNET_BASE/discogs-effnet-bsdynamic-1.onnx"
fetch_if_missing "$MODELS/feature-extractors/discogs-effnet/discogs-effnet-bsdynamic-1.json" \
  "$EFFNET_BASE/discogs-effnet-bsdynamic-1.json"

# mtg_jamendo_moodtheme — first classification head wired (PRD §2c, Phase 2 vertical slice).
MOODTHEME_BASE="https://essentia.upf.edu/models/classification-heads/mtg_jamendo_moodtheme"
fetch_if_missing "$MODELS/classification-heads/mtg_jamendo_moodtheme/mtg_jamendo_moodtheme-discogs-effnet-1.onnx" \
  "$MOODTHEME_BASE/mtg_jamendo_moodtheme-discogs-effnet-1.onnx"
fetch_if_missing "$MODELS/classification-heads/mtg_jamendo_moodtheme/mtg_jamendo_moodtheme-discogs-effnet-1.json" \
  "$MOODTHEME_BASE/mtg_jamendo_moodtheme-discogs-effnet-1.json"

# mtg_jamendo_instrument — same input/output contract as moodtheme (PRD §2c).
INSTRUMENT_BASE="https://essentia.upf.edu/models/classification-heads/mtg_jamendo_instrument"
fetch_if_missing "$MODELS/classification-heads/mtg_jamendo_instrument/mtg_jamendo_instrument-discogs-effnet-1.onnx" \
  "$INSTRUMENT_BASE/mtg_jamendo_instrument-discogs-effnet-1.onnx"
fetch_if_missing "$MODELS/classification-heads/mtg_jamendo_instrument/mtg_jamendo_instrument-discogs-effnet-1.json" \
  "$INSTRUMENT_BASE/mtg_jamendo_instrument-discogs-effnet-1.json"

# danceability — binary [danceable, not_danceable] softmax (PRD §2c). Distinct from
# Essentia's own DSP-based Danceability algorithm already used in Mir.cpp.
DANCEABILITY_BASE="https://essentia.upf.edu/models/classification-heads/danceability"
fetch_if_missing "$MODELS/classification-heads/danceability/danceability-discogs-effnet-1.onnx" \
  "$DANCEABILITY_BASE/danceability-discogs-effnet-1.onnx"
fetch_if_missing "$MODELS/classification-heads/danceability/danceability-discogs-effnet-1.json" \
  "$DANCEABILITY_BASE/danceability-discogs-effnet-1.json"

# CED-small — content gate, "is this even music?" (PRD §2c). Not from essentia.upf.edu
# like the two above — from k2-fsa/sherpa-onnx's own release, converted from
# github.com/RicherMans/CED. Using the fp32 model (86 MB), not the int8 one, to match
# every other model in the pipeline (no quantization anywhere else yet).
CED_TARBALL="/tmp/mira-ced-small-fetch.tar.bz2"
if [ ! -f "$MODELS/content-gate/ced-small/model.onnx" ]; then
  echo "== fetching CED-small =="
  curl -sSL -o "$CED_TARBALL" \
    "https://github.com/k2-fsa/sherpa-onnx/releases/download/audio-tagging-models/sherpa-onnx-ced-small-audio-tagging-2024-04-19.tar.bz2"
  TMP_EXTRACT="/tmp/mira-ced-small-extract"
  rm -rf "$TMP_EXTRACT" && mkdir -p "$TMP_EXTRACT"
  tar xjf "$CED_TARBALL" -C "$TMP_EXTRACT"
  cp "$TMP_EXTRACT"/*/model.onnx "$MODELS/content-gate/ced-small/model.onnx"
  cp "$TMP_EXTRACT"/*/class_labels_indices.csv "$MODELS/content-gate/ced-small/class_labels_indices.csv"
  rm -rf "$CED_TARBALL" "$TMP_EXTRACT"
else
  echo "== CED-small already present, skipping =="
fi

# --- Phase 5 — mira_ui typography (TASKS.md build-order step 2) ----------------
# IBM Plex Sans/Mono (SIL OFL 1.1, IBM/plex — permissive, fine under mira's AGPL-3.0),
# embedded into mira_ui via BinaryData (src/mira_ui/CMakeLists.txt), not a system-font
# dependency. Only the weights MiraLookAndFeel actually uses, not the full family.
if [ ! -f "$VENDOR/fonts/ibm-plex-sans/IBMPlexSans-Regular.ttf" ]; then
  echo "== fetching IBM Plex Sans/Mono =="
  mkdir -p "$VENDOR/fonts/ibm-plex-sans" "$VENDOR/fonts/ibm-plex-mono"
  for f in IBMPlexSans-Regular.ttf IBMPlexSans-Medium.ttf IBMPlexSans-SemiBold.ttf license.txt; do
    gh api repos/IBM/plex/contents/packages/plex-sans/fonts/complete/ttf/$f --jq .content \
      | base64 -d > "$VENDOR/fonts/ibm-plex-sans/$f"
  done
  for f in IBMPlexMono-Regular.ttf IBMPlexMono-Medium.ttf license.txt; do
    gh api repos/IBM/plex/contents/packages/plex-mono/fonts/complete/ttf/$f --jq .content \
      | base64 -d > "$VENDOR/fonts/ibm-plex-mono/$f"
  done
else
  echo "== IBM Plex fonts already present, skipping =="
fi

# --- lab/conversion_sources (Phase 2, PRD §16.3) --------------------------------
# One-time model-conversion inputs (gitignored, not committed — same "fetched, not
# authored here" pattern as vendor/ and models/). genre_discogs400 and voice_instrumental
# have no published ONNX (PRD §16.3), so their .pb graphs are converted via tf2onnx
# (lab/pyproject.toml). The IRMAS-predominant-instrument model is a genuinely different
# case: per-stem instrument recognition needs a model trained on isolated/predominant
# audio, not full mixes — see TASKS.md's Phase 2 section for why mtg_jamendo_instrument
# doesn't work for that. lab/export_irmas_instrument_onnx.py does the one-time torch->onnx
# export.
LAB_SOURCES="$ROOT/lab/conversion_sources"
mkdir -p "$LAB_SOURCES/genre_discogs400" "$LAB_SOURCES/voice_instrumental"

fetch_lab_source_if_missing() {
  local dest="$1" url="$2"
  if [ -f "$dest" ]; then
    echo "== $(basename "$dest") already present, skipping =="
    return
  fi
  echo "== fetching $(basename "$dest") =="
  curl -sSL -o "$dest" "$url"
}

GENRE_BASE="https://essentia.upf.edu/models/classification-heads/genre_discogs400"
fetch_lab_source_if_missing "$LAB_SOURCES/genre_discogs400/genre_discogs400-discogs-effnet-1.pb" \
  "$GENRE_BASE/genre_discogs400-discogs-effnet-1.pb"
fetch_lab_source_if_missing "$LAB_SOURCES/genre_discogs400/genre_discogs400-discogs-effnet-1.json" \
  "$GENRE_BASE/genre_discogs400-discogs-effnet-1.json"

VOICE_BASE="https://essentia.upf.edu/models/classification-heads/voice_instrumental"
fetch_lab_source_if_missing "$LAB_SOURCES/voice_instrumental/voice_instrumental-discogs-effnet-1.pb" \
  "$VOICE_BASE/voice_instrumental-discogs-effnet-1.pb"
fetch_lab_source_if_missing "$LAB_SOURCES/voice_instrumental/voice_instrumental-discogs-effnet-1.json" \
  "$VOICE_BASE/voice_instrumental-discogs-effnet-1.json"

# nii-yamagishilab/predominant-instrument-recognition (MIT) — full clone, not
# cherry-picked files: export_irmas_instrument_onnx.py imports several of its src/
# modules directly (SincConv, the custom ResNet variant, LDE pooling) rather than
# reimplementing them, so the whole src/ tree needs to be present, not just the
# checkpoint.
clone_if_missing() {
  local name="$1" url="$2"
  if [ -d "$LAB_SOURCES/$name" ]; then
    echo "== $name already present, skipping (rm -rf lab/conversion_sources/$name to re-fetch) =="
    return
  fi
  echo "== cloning $name =="
  git clone --depth 1 "$url" "$LAB_SOURCES/$name"
}
clone_if_missing irmas_predominant https://github.com/nii-yamagishilab/predominant-instrument-recognition.git

IRMAS_TARBALL="$LAB_SOURCES/irmas_predominant/pretrained/IRModels.tar.gz"
if [ -f "$IRMAS_TARBALL" ] && [ ! -d "$LAB_SOURCES/irmas_predominant/model_ckpt_tmp" ]; then
  echo "== extracting IRModels.tar.gz =="
  tar xzf "$IRMAS_TARBALL" -C "$LAB_SOURCES/irmas_predominant"
fi

echo "== done =="
