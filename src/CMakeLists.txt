# The real mira app (Phase 1 onward — PRD §7). Built incrementally: today this is the
# scanner + SQLite storage; MIR, neural, and the JUCE UI attach to this same target in
# later phases.

# PRD §6: provenance records "mira version... lets a later model upgrade identify
# exactly which files need re-analysis." No release versioning exists yet, so the git
# commit is the only thing that actually identifies "which mira produced this row".
find_package(Git QUIET)
if(GIT_FOUND)
  execute_process(
    COMMAND ${GIT_EXECUTABLE} rev-parse --short HEAD
    WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
    OUTPUT_VARIABLE MIRA_GIT_HASH
    OUTPUT_STRIP_TRAILING_WHITESPACE
    ERROR_QUIET
  )
endif()
if(NOT MIRA_GIT_HASH)
  set(MIRA_GIT_HASH "unknown")
endif()

set(SQLITE_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/../vendor/sqlite-amalgamation)
set(SQLITECPP_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/../vendor/SQLiteCpp)
set(ESSENTIA_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/../vendor/essentia)
set(ONNXRUNTIME_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/../vendor/onnxruntime)
set(BEAT_THIS_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/../vendor/beat_this_cpp)
set(LIBKEYFINDER_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/../vendor/libkeyfinder)
set(VAMP_SDK_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/../vendor/vamp-plugin-sdk)
set(NNLS_CHROMA_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/../vendor/nnls-chroma)
set(BASICPITCH_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/../vendor/basicpitch.cpp)
set(KALDI_NATIVE_FBANK_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/../vendor/kaldi-native-fbank)
set(KISSFFT_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/../vendor/kissfft)
set(MODELS_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/../models)
set(SQLITE_VEC_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/../vendor/sqlite-vec)
set(TAXONOMY_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/../taxonomy)

if(NOT EXISTS ${SQLITE_ROOT}/sqlite3.c)
  message(FATAL_ERROR "SQLite amalgamation not found at ${SQLITE_ROOT} — run scripts/fetch-vendor.sh")
endif()
if(NOT EXISTS ${SQLITECPP_ROOT}/CMakeLists.txt)
  message(FATAL_ERROR "SQLiteCpp not found at ${SQLITECPP_ROOT} — run scripts/fetch-vendor.sh")
endif()
set(ESSENTIA_LIB ${ESSENTIA_ROOT}/build/src/libessentia.a)
if(NOT EXISTS ${ESSENTIA_LIB})
  message(FATAL_ERROR "libessentia.a not found — see spike/01_essentia_link for the build steps")
endif()
if(NOT EXISTS ${BEAT_THIS_ROOT}/CMakeLists.txt)
  message(FATAL_ERROR "beat_this_cpp not found at ${BEAT_THIS_ROOT} — run scripts/fetch-vendor.sh")
endif()
if(NOT EXISTS ${BEAT_THIS_ROOT}/onnx/beat_this.onnx)
  message(FATAL_ERROR "beat_this.onnx not found — expected committed in-tree in beat_this_cpp's own repo")
endif()
if(NOT EXISTS ${LIBKEYFINDER_ROOT}/CMakeLists.txt)
  message(FATAL_ERROR "libkeyfinder not found at ${LIBKEYFINDER_ROOT} — run scripts/fetch-vendor.sh")
endif()
if(NOT EXISTS ${VAMP_SDK_ROOT}/CMakeLists.txt)
  message(FATAL_ERROR "vamp-plugin-sdk not found at ${VAMP_SDK_ROOT} — run scripts/fetch-vendor.sh")
endif()
if(NOT EXISTS ${NNLS_CHROMA_ROOT}/Chordino.cpp)
  message(FATAL_ERROR "nnls-chroma not found at ${NNLS_CHROMA_ROOT} — run scripts/fetch-vendor.sh")
endif()
if(NOT EXISTS ${BASICPITCH_ROOT}/ort-model/model.onnx)
  message(FATAL_ERROR "basicpitch.cpp not found at ${BASICPITCH_ROOT} — run scripts/fetch-vendor.sh")
endif()
if(NOT EXISTS ${KALDI_NATIVE_FBANK_ROOT}/kaldi-native-fbank/csrc/online-feature.h)
  message(FATAL_ERROR "kaldi-native-fbank not found at ${KALDI_NATIVE_FBANK_ROOT} — run scripts/fetch-vendor.sh")
endif()
if(NOT EXISTS ${KISSFFT_ROOT}/kiss_fftr.c)
  message(FATAL_ERROR "kissfft not found at ${KISSFFT_ROOT} — run scripts/fetch-vendor.sh")
endif()
if(NOT EXISTS ${MODELS_ROOT}/feature-extractors/discogs-effnet/discogs-effnet-bsdynamic-1.onnx)
  message(FATAL_ERROR "discogs-effnet model not found — run scripts/fetch-vendor.sh")
endif()
if(NOT EXISTS ${MODELS_ROOT}/classification-heads/mtg_jamendo_moodtheme/mtg_jamendo_moodtheme-discogs-effnet-1.onnx)
  message(FATAL_ERROR "mtg_jamendo_moodtheme model not found — run scripts/fetch-vendor.sh")
endif()
if(NOT EXISTS ${MODELS_ROOT}/classification-heads/mtg_jamendo_instrument/mtg_jamendo_instrument-discogs-effnet-1.onnx)
  message(FATAL_ERROR "mtg_jamendo_instrument model not found — run scripts/fetch-vendor.sh")
endif()
if(NOT EXISTS ${MODELS_ROOT}/classification-heads/danceability/danceability-discogs-effnet-1.onnx)
  message(FATAL_ERROR "danceability model not found — run scripts/fetch-vendor.sh")
endif()
if(NOT EXISTS ${MODELS_ROOT}/classification-heads/irmas-predominant-instrument/irmas-predominant-instrument-1.onnx)
  message(FATAL_ERROR "irmas-predominant-instrument model not found — run lab/export_irmas_instrument_onnx.py (see TASKS.md Phase 2)")
endif()
if(NOT EXISTS ${MODELS_ROOT}/classification-heads/genre_discogs400/genre_discogs400-discogs-effnet-1.onnx)
  message(FATAL_ERROR "genre_discogs400 ONNX not found — run lab/'s tf2onnx conversion (see TASKS.md Phase 2)")
endif()
if(NOT EXISTS ${MODELS_ROOT}/classification-heads/voice_instrumental/voice_instrumental-discogs-effnet-1.onnx)
  message(FATAL_ERROR "voice_instrumental ONNX not found — run lab/'s tf2onnx conversion (see TASKS.md Phase 2)")
endif()
if(NOT EXISTS ${TAXONOMY_ROOT}/instrument-labels.yaml)
  message(FATAL_ERROR "taxonomy/instrument-labels.yaml not found — this is a committed, hand-authored file, not fetched; something's wrong with the checkout")
endif()
if(NOT EXISTS ${TAXONOMY_ROOT}/genre-labels.yaml)
  message(FATAL_ERROR "taxonomy/genre-labels.yaml not found — this is a committed, hand-authored file, not fetched; something's wrong with the checkout")
endif()
if(NOT EXISTS ${SQLITE_VEC_ROOT}/sqlite-vec.h)
  message(FATAL_ERROR "sqlite-vec.h not generated — run scripts/fetch-vendor.sh")
endif()
if(NOT EXISTS ${MODELS_ROOT}/content-gate/ced-small/model.onnx)
  message(FATAL_ERROR "CED-small model not found — run scripts/fetch-vendor.sh")
endif()

find_package(PkgConfig REQUIRED)
pkg_check_modules(EIGEN3 REQUIRED IMPORTED_TARGET eigen3)
pkg_check_modules(FFTW3F REQUIRED IMPORTED_TARGET fftw3f)
pkg_check_modules(TAGLIB REQUIRED IMPORTED_TARGET taglib)
pkg_check_modules(YAML REQUIRED IMPORTED_TARGET yaml-0.1)
pkg_check_modules(CHROMAPRINT REQUIRED IMPORTED_TARGET libchromaprint)
pkg_check_modules(SAMPLERATE REQUIRED IMPORTED_TARGET samplerate)
pkg_check_modules(AVCODEC REQUIRED IMPORTED_TARGET libavcodec)
pkg_check_modules(AVFORMAT REQUIRED IMPORTED_TARGET libavformat)
pkg_check_modules(AVUTIL REQUIRED IMPORTED_TARGET libavutil)
pkg_check_modules(SWRESAMPLE REQUIRED IMPORTED_TARGET libswresample)

# Our own vendored SQLite (3.53.4, PRD §7) as the SQLite::SQLite3 target SQLiteCpp
# expects — never Apple's libsqlite3 (built SQLITE_OMIT_LOAD_EXTENSION) and not
# SQLiteCpp's own bundled copy either, so there's exactly one SQLite version in the app.
add_library(mira_sqlite3 STATIC ${SQLITE_ROOT}/sqlite3.c)
target_include_directories(mira_sqlite3 PUBLIC ${SQLITE_ROOT})
target_compile_options(mira_sqlite3 PRIVATE
  -DSQLITE_ENABLE_LOAD_EXTENSION=1
  -DSQLITE_THREADSAFE=1
  -DSQLITE_ENABLE_COLUMN_METADATA=1
)
add_library(SQLite::SQLite3 ALIAS mira_sqlite3)

# sqlite-vec (PRD §6, §7: "no ANN index... brute-force KNN over the embedding, exact,
# not approximate" — spike/04_sqlite_vec proved this at 10k x 1280-dim). Statically
# linked, so per its own README it must be registered via sqlite3_auto_extension before
# any connection opens (main.cpp does this, once, at startup). vendor/sqlite-vec/ has a
# bare file named "version" at its root that collides with libc++'s <version> header if
# the whole directory is ever on a C++ translation unit's include path — copy just the
# one header actually needed into an isolated directory, same fix the spike used.
file(COPY ${SQLITE_VEC_ROOT}/sqlite-vec.h DESTINATION ${CMAKE_CURRENT_BINARY_DIR}/sqlite_vec_include)
add_library(mira_sqlite_vec STATIC ${SQLITE_VEC_ROOT}/sqlite-vec.c)
target_include_directories(mira_sqlite_vec PRIVATE ${SQLITE_VEC_ROOT})
target_link_libraries(mira_sqlite_vec PUBLIC mira_sqlite3)
target_compile_definitions(mira_sqlite_vec PRIVATE SQLITE_CORE SQLITE_VEC_STATIC)

set(SQLITECPP_INTERNAL_SQLITE OFF CACHE BOOL "" FORCE)
set(SQLITECPP_FIND_SQLITE OFF CACHE BOOL "" FORCE)
set(SQLITECPP_RUN_CPPLINT OFF CACHE BOOL "" FORCE)
set(SQLITECPP_RUN_CPPCHECK OFF CACHE BOOL "" FORCE)
set(SQLITECPP_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(SQLITECPP_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(SQLITECPP_INSTALL OFF CACHE BOOL "" FORCE)
add_subdirectory(${SQLITECPP_ROOT} sqlitecpp_build)

# beat_this_cpp (PRD §16.5) — its own CMakeLists.txt was patched during the Phase 0 spike
# (spike/README.md) to accept ONNXRUNTIME_ROOT directly, since the official prebuilt
# tarball's CMake package doesn't match the install-tree layout beat_this_cpp expects.
set(USE_SYSTEM_ONNXRUNTIME ON CACHE BOOL "" FORCE)
set(ONNXRUNTIME_ROOT ${ONNXRUNTIME_ROOT} CACHE PATH "" FORCE)
add_subdirectory(${BEAT_THIS_ROOT} beat_this_cpp_build)

# libKeyFinder (PRD §12b) — GPL-3.0, fine under mira's AGPL-3.0 (PRD header, §12 Q9).
set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
set(BUILD_TESTING OFF CACHE BOOL "" FORCE)
set(CMAKE_POSITION_INDEPENDENT_CODE ON CACHE BOOL "" FORCE)
add_subdirectory(${LIBKEYFINDER_ROOT} libkeyfinder_build)

# vamp-plugin-sdk (both vamp-sdk, which Chordino is written against, and vamp-hostsdk,
# whose PluginInputDomainAdapter/PluginBufferingAdapter drive it — PRD §12b). One local
# patch needed: its CMakeLists.txt has an example-plugins block outside the option()
# guard that's meant to contain it — see scripts/fetch-vendor.sh.
add_subdirectory(${VAMP_SDK_ROOT} vamp_plugin_sdk_build)

# nnls-chroma / Chordino (PRD §7, §12b) — GPL-2.0-or-later, fine under mira's AGPL-3.0.
# No CMakeLists.txt upstream (plain Makefile project); built directly here. Excludes
# plugins.cpp (Vamp dynamic-plugin registration — unused, we link Chordino directly) and
# chordextract.cpp (nnls-chroma's own CLI example; Chords.cpp is mira's equivalent).
add_library(nnls_chroma STATIC
  ${NNLS_CHROMA_ROOT}/chromamethods.cpp
  ${NNLS_CHROMA_ROOT}/NNLSBase.cpp
  ${NNLS_CHROMA_ROOT}/NNLSChroma.cpp
  ${NNLS_CHROMA_ROOT}/Chordino.cpp
  ${NNLS_CHROMA_ROOT}/Tuning.cpp
  ${NNLS_CHROMA_ROOT}/viterbi.cpp
  ${NNLS_CHROMA_ROOT}/nnls.c
)
target_include_directories(nnls_chroma PUBLIC ${NNLS_CHROMA_ROOT})
target_link_libraries(nnls_chroma PUBLIC vamp-sdk)
# chromamethods.cpp uses boost::tokenizer (header-only) — Homebrew's boost, not vendored.
find_package(Boost REQUIRED)
target_link_libraries(nnls_chroma PUBLIC Boost::boost)
# Must match nnls-chroma's own documented build invocation (top of Chordino.h/NNLSBase.h
# comments) exactly, and must be consistent across every TU that touches these Vamp
# types (including Chords.cpp) — it controls which internal namespace (_VampHost vs
# _VampPlugin) the plugin classes are wrapped in; a mismatch is an ODR violation, not
# just a style choice.
target_compile_definitions(nnls_chroma PUBLIC _VAMP_PLUGIN_IN_HOST_NAMESPACE=1)

# kaldi-native-fbank (Apache-2.0, csukuangfj/kaldi-native-fbank) — CED-small's content
# gate (PRD §2c) uses a kaldi-style fbank frontend, not Essentia's TensorflowInputMusiCNN
# (that's discogs-effnet's frontend, a different model with a different contract). This is
# the exact feature-extraction library sherpa-onnx itself links against for CED, vendored
# directly rather than hand-replicating kaldi's fbank math in Essentia. Compiled directly
# (not add_subdirectory'd — its own CMakeLists.txt pulls in Python bindings/tests by
# default), same pattern as nnls-chroma above. istft.cc/stft.cc (unused, ISTFT-only) and
# test-*.cc are excluded.
add_library(kaldi_native_fbank STATIC
  ${KALDI_NATIVE_FBANK_ROOT}/kaldi-native-fbank/csrc/feature-fbank.cc
  ${KALDI_NATIVE_FBANK_ROOT}/kaldi-native-fbank/csrc/feature-functions.cc
  ${KALDI_NATIVE_FBANK_ROOT}/kaldi-native-fbank/csrc/feature-mfcc.cc
  ${KALDI_NATIVE_FBANK_ROOT}/kaldi-native-fbank/csrc/feature-raw-audio-samples.cc
  ${KALDI_NATIVE_FBANK_ROOT}/kaldi-native-fbank/csrc/feature-window.cc
  ${KALDI_NATIVE_FBANK_ROOT}/kaldi-native-fbank/csrc/kaldi-math.cc
  ${KALDI_NATIVE_FBANK_ROOT}/kaldi-native-fbank/csrc/log.cc
  ${KALDI_NATIVE_FBANK_ROOT}/kaldi-native-fbank/csrc/mel-computations.cc
  ${KALDI_NATIVE_FBANK_ROOT}/kaldi-native-fbank/csrc/online-feature.cc
  ${KALDI_NATIVE_FBANK_ROOT}/kaldi-native-fbank/csrc/rfft.cc
  ${KALDI_NATIVE_FBANK_ROOT}/kaldi-native-fbank/csrc/whisper-feature.cc
  # kissfft (BSD-3-Clause) — rfft.cc's real-FFT backend. Upstream's own CMake
  # FetchContent-downloads this; vendored directly instead, same as everything else here.
  ${KISSFFT_ROOT}/kiss_fft.c
  ${KISSFFT_ROOT}/kiss_fftr.c
)
target_include_directories(kaldi_native_fbank PUBLIC ${KALDI_NATIVE_FBANK_ROOT})
target_include_directories(kaldi_native_fbank PRIVATE ${KISSFFT_ROOT})

add_executable(mira
  mira/main.cpp
  mira/db/Database.cpp
  mira/scan/Scanner.cpp
  mira/analyze/EssentiaEngine.cpp
  mira/analyze/AudioLoader.cpp
  mira/analyze/Router.cpp
  mira/analyze/ActiveRegions.cpp
  mira/analyze/Descriptors.cpp
  mira/analyze/Mir.cpp
  mira/analyze/Key.cpp
  mira/analyze/Chords.cpp
  mira/analyze/BasicPitchOrt.cpp
  mira/analyze/BasicPitchNotes.cpp
  mira/analyze/Transcription.cpp
  mira/analyze/OrtEnv.cpp
  mira/analyze/Embedding.cpp
  mira/analyze/ContentGate.cpp
  mira/analyze/ClassificationHead.cpp
  mira/analyze/MoodTheme.cpp
  mira/analyze/Instrument.cpp
  mira/analyze/Danceability.cpp
  mira/analyze/StemInstrument.cpp
  mira/analyze/Genre.cpp
  mira/analyze/VoiceInstrumental.cpp
  mira/taxonomy/Taxonomy.cpp
)

target_include_directories(mira PRIVATE
  ${ESSENTIA_ROOT}/src
  ${BEAT_THIS_ROOT}/Source
  ${LIBKEYFINDER_ROOT}/src
  ${NNLS_CHROMA_ROOT}
  ${ONNXRUNTIME_ROOT}/include
  ${CMAKE_CURRENT_BINARY_DIR}/sqlite_vec_include
)

target_compile_definitions(mira PRIVATE
  MIRA_BEAT_THIS_MODEL="${BEAT_THIS_ROOT}/onnx/beat_this.onnx"
  MIRA_BASIC_PITCH_MODEL="${BASICPITCH_ROOT}/ort-model/model.onnx"
  MIRA_EFFNET_MODEL="${MODELS_ROOT}/feature-extractors/discogs-effnet/discogs-effnet-bsdynamic-1.onnx"
  MIRA_MOODTHEME_MODEL="${MODELS_ROOT}/classification-heads/mtg_jamendo_moodtheme/mtg_jamendo_moodtheme-discogs-effnet-1.onnx"
  MIRA_INSTRUMENT_MODEL="${MODELS_ROOT}/classification-heads/mtg_jamendo_instrument/mtg_jamendo_instrument-discogs-effnet-1.onnx"
  MIRA_DANCEABILITY_MODEL="${MODELS_ROOT}/classification-heads/danceability/danceability-discogs-effnet-1.onnx"
  MIRA_IRMAS_INSTRUMENT_MODEL="${MODELS_ROOT}/classification-heads/irmas-predominant-instrument/irmas-predominant-instrument-1.onnx"
  MIRA_GENRE_MODEL="${MODELS_ROOT}/classification-heads/genre_discogs400/genre_discogs400-discogs-effnet-1.onnx"
  MIRA_VOICE_INSTRUMENTAL_MODEL="${MODELS_ROOT}/classification-heads/voice_instrumental/voice_instrumental-discogs-effnet-1.onnx"
  MIRA_INSTRUMENT_TAXONOMY="${TAXONOMY_ROOT}/instrument-labels.yaml"
  MIRA_GENRE_TAXONOMY="${TAXONOMY_ROOT}/genre-labels.yaml"
  MIRA_CED_MODEL="${MODELS_ROOT}/content-gate/ced-small/model.onnx"
  # Captured at configure time, not build time — re-run cmake to refresh after new
  # commits if you care about this being exact; "close enough" for a dev build.
  MIRA_GIT_HASH="${MIRA_GIT_HASH}"
  SQLITE_VEC_STATIC
  # main.cpp includes sqlite-vec.h directly (to register sqlite3_vec_init) — without
  # SQLITE_CORE it pulls in sqlite3ext.h's macro-redirected API instead of linking
  # directly against mira_sqlite3, and main.cpp/Database.cpp already do the latter.
  SQLITE_CORE
)

target_link_libraries(mira PRIVATE
  SQLiteCpp
  mira_sqlite3
  mira_sqlite_vec
  ${ESSENTIA_LIB}
  beat_this_api
  keyfinder
  nnls_chroma
  vamp-hostsdk
  kaldi_native_fbank
  PkgConfig::AVFORMAT
  PkgConfig::AVCODEC
  PkgConfig::SWRESAMPLE
  PkgConfig::AVUTIL
  PkgConfig::SAMPLERATE
  PkgConfig::TAGLIB
  PkgConfig::YAML
  PkgConfig::FFTW3F
  PkgConfig::CHROMAPRINT
  PkgConfig::EIGEN3
  "-framework CoreFoundation"
  "-framework Security"
)

set_target_properties(mira PROPERTIES
  CXX_STANDARD 20
  CXX_STANDARD_REQUIRED ON
  BUILD_RPATH "${ONNXRUNTIME_ROOT}/lib"
)
