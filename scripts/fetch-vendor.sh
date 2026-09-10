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

# Phase 1 — storage (PRD §6, §7). Not started yet.
# clone_if_missing SQLiteCpp https://github.com/SRombauts/SQLiteCpp.git

echo "== done =="
